// Nets from geometry: see the note in `SchematicNets.h`.
//
// The rules are Celine's (`Schematic::extractNets` and its `joinPointsLyingOnWires`),
// and LiveSPICE agrees with them: coincident points, wire joins, points lying
// along a wire, and terminals naming nets. The one thing that changes with our
// document model is that a `SchematicWire` is a routed *polyline* rather than one
// segment, so a wire contributes the segments between its consecutive points, and
// every point of one polyline is on one net.

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "format/Document.h"
#include "format/PartLibrary.h"
#include "format/SchematicNets.h"

namespace wdf {

namespace {

/// Disjoint-set over *points*, not over parts or wires: what the drawing decides
/// is which points are the same place, and a net is a class of points.
class DisjointSet {
public:
    int add()
    {
        parent.push_back(static_cast<int>(parent.size()));
        return static_cast<int>(parent.size()) - 1;
    }

    int find(int i) const
    {
        while (parent[static_cast<size_t>(i)] != i)
            i = parent[static_cast<size_t>(i)];
        return i;
    }

    void unite(int a, int b)
    {
        const int ra = find(a), rb = find(b);
        if (ra != rb)
            parent[static_cast<size_t>(rb)] = ra;
    }

    int size() const { return static_cast<int>(parent.size()); }

private:
    std::vector<int> parent;
};

/// Grid points key a map, and the order is **row first** -- Celine's `PointLess`.
/// Net numbering is assigned in this order, which is what keeps a net's number
/// from shuffling when an unrelated part is added below it: adding something
/// lower down the sheet does not renumber the nets above.
struct PointLess {
    bool operator()(const std::pair<int, int>& a, const std::pair<int, int>& b) const noexcept
    {
        return a.second != b.second ? a.second < b.second : a.first < b.first;
    }
};

using Point = std::pair<int, int>;

/// An integer grid point from a drawn one. Placements are floats because the
/// editor may one day place off-grid; the *drawing* joins on whole squares, so
/// the honest reading is the nearest one.
int gridOf(float v) { return static_cast<int>(std::lround(v)); }

} // namespace

// ============================================================================
// Derivation
// ============================================================================

DerivedNets deriveNets(const std::vector<SchematicComponent>& components,
                       const std::vector<SchematicWire>& wires,
                       const std::string& groundName,
                       const std::string& inputName,
                       const std::string& outputName)
{
    DerivedNets result;
    result.netOfPin.resize(components.size());
    result.netOfWire.assign(wires.size(), -1);

    DisjointSet sets;
    std::map<Point, int, PointLess> pointIds;

    const auto idOf = [&](Point p) {
        const auto it = pointIds.find(p);
        if (it != pointIds.end())
            return it->second;

        const int id = sets.add();
        pointIds.emplace(p, id);
        return id;
    };

    // Where every pin sits. The palette is asked once per pin for the answer it
    // owns: two pins are on one net exactly when the palette says they are at
    // one point. A type the palette does not know contributes no pins -- the
    // alternative, defaulting a pin to the part's centre or to ground, is what
    // connected a part to something it was never drawn on.
    for (std::size_t e = 0; e < components.size(); ++e) {
        const int pinCount = static_cast<int>(livewdf::editor::pinFrame(components[e].type).size());
        auto& row = result.netOfPin[e];
        row.resize(static_cast<std::size_t>(pinCount), -1);

        for (int pin = 0; pin < pinCount; ++pin) {
            const livewdf::editor::PinOffsetSpec at = livewdf::editor::pinAt(components[e], pin);
            row[static_cast<std::size_t>(pin)] = idOf({at.x, at.y});
        }
    }

    // Every wire endpoint, and every bend of a routed polyline: one polyline is
    // one drawn line, so all its points share a net. A wire with no points owns
    // nothing, which is why `netOfWire` starts at -1.
    std::vector<std::vector<Point>> wirePoints(wires.size());
    std::vector<std::vector<int>> wirePointIds(wires.size());
    for (std::size_t w = 0; w < wires.size(); ++w) {
        wirePoints[w].reserve(wires[w].points.size());
        for (const auto& pt : wires[w].points) {
            const Point p{gridOf(pt.x), gridOf(pt.y)};
            wirePoints[w].push_back(p);
            wirePointIds[w].push_back(idOf(p));
        }

        for (std::size_t s = 1; s < wirePointIds[w].size(); ++s)
            sets.unite(wirePointIds[w][0], wirePointIds[w][s]);
    }

    // A pin or a wire end landing *along* another wire is joined to it -- a T-
    // junction needs no dot. Indexed by row and column first for the reason
    // Celine gives: unindexed, extraction is quadratic in the sheet.
    {
        std::map<int, std::vector<std::pair<Point, int>>> byColumn, byRow;
        for (const auto& [point, id] : pointIds) {
            byColumn[point.first].emplace_back(point, id);
            byRow[point.second].emplace_back(point, id);
        }

        for (std::size_t w = 0; w < wires.size(); ++w) {
            const auto& pts = wirePoints[w];
            if (pts.empty())
                continue;

            const int wireSet = wirePointIds[w][0];

            const auto joinAlong = [&](Point a, Point b) {
                // Two ifs and no else: a zero-length segment is both vertical
                // and horizontal, and Celine keeps it in both indexes so that
                // dropping it from one cannot lose a connection.
                if (a.first == b.first) {
                    const int lo = std::min(a.second, b.second);
                    const int hi = std::max(a.second, b.second);
                    if (const auto column = byColumn.find(a.first); column != byColumn.end())
                        for (const auto& [candidate, id] : column->second)
                            if (candidate.second >= lo && candidate.second <= hi)
                                sets.unite(wireSet, id);
                }

                if (a.second == b.second) {
                    const int lo = std::min(a.first, b.first);
                    const int hi = std::max(a.first, b.first);
                    if (const auto row = byRow.find(a.second); row != byRow.end())
                        for (const auto& [candidate, id] : row->second)
                            if (candidate.first >= lo && candidate.first <= hi)
                                sets.unite(wireSet, id);
                }
            };

            for (std::size_t s = 0; s + 1 < pts.size(); ++s)
                joinAlong(pts[s], pts[s + 1]);
        }
    }

    // Collapse the roots into dense net indices, assigned in a stable order --
    // by point position, Celine's -- so net numbering does not shuffle when an
    // unrelated part is added.
    std::map<int, int> netOfRoot;
    std::vector<int> netOfPoint(static_cast<std::size_t>(sets.size()), -1);

    const auto netIndexOf = [&](int pointId) {
        const int root = sets.find(pointId);
        const auto it = netOfRoot.find(root);
        if (it != netOfRoot.end())
            return it->second;

        const int net = static_cast<int>(netOfRoot.size());
        netOfRoot.emplace(root, net);
        return net;
    };

    // Assigned in the map's order, which is by point position: net numbering
    // does not shuffle when an unrelated part is added.
    for (const auto& entry : pointIds)
        netOfPoint[static_cast<std::size_t>(entry.second)] = netIndexOf(entry.second);

    for (std::size_t e = 0; e < components.size(); ++e)
        for (auto& slot : result.netOfPin[e])
            if (slot >= 0)
                slot = netOfPoint[static_cast<std::size_t>(slot)];

    for (std::size_t w = 0; w < wires.size(); ++w)
        if (!wirePointIds[w].empty())
            result.netOfWire[w] = netOfPoint[static_cast<std::size_t>(wirePointIds[w][0])];

    result.netNames.assign(netOfRoot.size(), std::string());

    // **Terminals name their net; nothing else does.** A net with a ground
    // symbol drawn on it is ground whatever else shares it, and two ground
    // symbols on two unconnected groups both name their net "0" -- which is not
    // a name clash and not a bug: `Circuit` maps nodes by name, so the two
    // become one node when the circuit is built, and grounding a pin in two
    // places connects them without drawing a wire across the sheet.
    //
    // In priority order rather than drawing order, for the reason Celine states:
    // letting the last terminal win meant an Output placed after its Ground
    // named the shared net "out", leaving the sheet with no ground at all, and
    // the builder's own "output wired to ground" check -- which reads the name
    // -- never firing.
    //
    // Pin 0 of a terminal is the net it names. A terminal has one pin today; if
    // one ever gains a second, the second is not the signal's net -- LiveSPICE's
    // two-terminal `Input` is a source whose *cathode* is not the input.
    //
    // A supply names nothing. A rail or a source feeds the net it is drawn
    // onto; its own name labels *it*, for the inspector and the equations
    // (LiveSPICE's `Rail` adds "i"+Name as an unknown current, never a net).
    // Naming a net after a supply is what the first editor did with a stored
    // pin table, and it is why a part could be named onto a net it was drawn
    // nowhere near.
    // A sheet that left a terminal name blank still has a ground, an input and
    // an output: the fallbacks `Document.h` offers. Naming is the last step, so
    // blanking a name cannot change what is connected.
    const std::vector<std::pair<std::string, std::string>> namingOrder = {
        {"gnd", groundName.empty() ? std::string("0") : groundName},
        {"in", inputName.empty() ? std::string("in") : inputName},
        {"out", outputName.empty() ? std::string("out") : outputName},
    };

    for (const auto& [terminalType, name] : namingOrder)
        for (std::size_t e = 0; e < components.size(); ++e) {
            if (components[e].type != terminalType || result.netOfPin[e].empty())
                continue;

            const int net = result.netOfPin[e][0];
            if (net < 0)
                continue;

            auto& slot = result.netNames[static_cast<std::size_t>(net)];
            if (slot.empty())
                slot = name;
        }

    // Anything still unnamed gets a number. The numbers are per net, in net
    // order, so a sheet reads the same way every time it is opened.
    int autoNumber = 1;
    for (auto& name : result.netNames)
        if (name.empty())
            name = "n" + std::to_string(autoNumber++);

    // Nets with only one pin on them: a part left dangling, or a wire that
    // stops short. The editor points at these instead of letting the solver
    // refuse over a matrix that has no reference to ground in it. A net whose
    // only other thing is a wire that goes nowhere is still dangling -- the
    // wire is drawn, it connects nothing.
    std::vector<int> connectionCount(result.netNames.size(), 0);
    for (const auto& row : result.netOfPin)
        for (const int net : row)
            if (net >= 0 && net < static_cast<int>(connectionCount.size()))
                ++connectionCount[static_cast<std::size_t>(net)];

    for (std::size_t net = 0; net < connectionCount.size(); ++net)
        if (connectionCount[net] == 1)
            result.danglingNets.push_back(static_cast<int>(net));

    return result;
}

DerivedNets deriveNets(const LiveWDFDocument& doc)
{
    // **Ground keeps the document's name; input and output are named by the
    // parts.** `groundNode` is the same vocabulary `toCircuit()` reads to decide
    // which net *is* ground, so naming the `gnd` symbols anything else would put
    // the two accounts one edit apart -- the drift this project keeps being
    // told off for.
    //
    // `inputNode`/`outputNode` are not that: in the v1/v2 format they were *the
    // net the terminal part drove*, written into the file because the terminal
    // named its net. Under a geometry-only format the terminal names its net by
    // being drawn, so those strings are leftovers of a format that no longer
    // exists, and using them as names would be worse than ignoring them: a file
    // whose leftover output name is "n1" would claim whichever net the drawing
    // happens to number first, silently wiring the output to something else.
    // They remain the fallback `inputNet()`/`outputNet()` offer for a sheet
    // that has no terminal part -- and the loader says when that fallback is
    // what is being used, because it is a name the drawing does not make.
    return deriveNets(doc.components, doc.wires, doc.groundNode, "in", "out");
}

} // namespace wdf
