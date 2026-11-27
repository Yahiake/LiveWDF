#include "SchematicModel.h"
#include "format/PartLibrary.h"
#include "format/SchematicNets.h"
#include "SymbolArtwork.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace livewdf::editor {

namespace {

/// Keys a grid point for a std::map, in a fixed order. The net extractor lives
/// in `format/SchematicNets.h` now -- see `extractNets()` -- so this file only
/// needs the ordering, for the junction dots the renderer draws.
struct PointLess {
    bool operator()(const std::pair<int, int>& a, const std::pair<int, int>& b) const noexcept {
        return a.second != b.second ? a.second < b.second : a.first < b.first;
    }
};

/// Half the box a pin's drawn dot covers, in grid units. The pin is part of the
/// symbol's visible shape, so it stays selectable even when the artwork's own
/// paths do not reach it. Kept in sync with the 0.4-square box `partBounds`
/// already uses for pins.
constexpr float kPinDotRadius = 0.2f;

bool pinHitTest(const wdf::SchematicComponent& part, float px, float py, float reach) {
    const auto offsets = Schematic::pinOffsets(part);
    const float radius = kPinDotRadius + reach;

    for (int pin = 0; pin < static_cast<int>(offsets.size()); ++pin) {
        const auto p = Schematic::pinPosition(part, pin);
        const float dx = px - static_cast<float>(p.x);
        const float dy = py - static_cast<float>(p.y);

        if (dx * dx + dy * dy <= radius * radius)
            return true;
    }

    return false;
}

bool pinTouchesBox(const wdf::SchematicComponent& part, juce::Rectangle<float> box) {
    const auto offsets = Schematic::pinOffsets(part);

    for (int pin = 0; pin < static_cast<int>(offsets.size()); ++pin) {
        const auto p = Schematic::pinPosition(part, pin);
        const auto dot = juce::Rectangle<float>(kPinDotRadius * 2.0f, kPinDotRadius * 2.0f)
                             .withCentre(juce::Point<float>(static_cast<float>(p.x),
                                                            static_cast<float>(p.y)));

        if (box.intersects(dot))
            return true;
    }

    return false;
}

// The wire-along join, the union-find over pin points and wire ends, and the
// numbering of nets are all in `format/SchematicNets.h` now -- `extractNets()`
// below just calls `wdf::deriveNets`. Keeping a second copy of that machinery
// here is how the editor got a different connectivity from the one the reader
// and the converter were given, and nothing here disagrees with anything there
// any more: there is one answer to "what is connected to what".
//
// **There is no net-naming machinery here any more** -- no `isGeneratedNetName`,
// no `sanitizeNodeName`, no `sourceNodeName`, and nothing that ranks a stored
// pin name above another. All of it existed to decide which name a net should
// carry, because the document stored names and they disagreed with each other
// and with the drawing. A net's name now comes from the terminals *drawn* on it
// (`format/SchematicNets.h`), and a supply -- rail, `src`, `isrc` -- names
// nothing, which is what Celine's PSU and LiveSPICE's `Rail` do: the part's own
// name goes into the equations as an unknown, never into the net it feeds. So
// there is nothing left to rank, and the file's `node` property is display, not
// connectivity.

}  // namespace

// ============================================================================
// Parts
// ============================================================================

// The pin frame and the pin's absolute grid point come from `format/PartLibrary.h`
// -- the one table the reader, the converter and the drawing all ask. There is
// deliberately no second derivation of a part's geometry here: a pin the editor
// computes into one square and the converter computes into another is a part
// wired to nothing, and no test that only looks at the drawing sees it. Celine
// has one `ElementGeometry` table for the same reason; so do we now.
std::vector<PinOffset> Schematic::pinOffsets(const std::string& type) {
    std::vector<PinOffset> offsets;
    for (const auto& o : livewdf::editor::pinFrame(type))
        offsets.push_back({o.x, o.y});
    return offsets;
}

std::vector<PinOffset> Schematic::pinOffsets(const wdf::SchematicComponent& part) {
    std::vector<PinOffset> offsets;
    for (const auto& o : livewdf::editor::pinFrame(part))
        offsets.push_back({o.x, o.y});
    return offsets;
}

PinOffset Schematic::pinPosition(const wdf::SchematicComponent& part, int pinIndex) {
    // `pinAt` already asks the palette for the frame and applies the part's own
    // mirror and rotation, in Celine's order, and returns the grid point -- the
    // question the renderer, the hit test and the net extractor all want.
    const PinOffsetSpec p = livewdf::editor::pinAt(part, pinIndex);
    return {p.x, p.y};
}

juce::Rectangle<float> Schematic::partBounds(const wdf::SchematicComponent& part) {
    // **Céline's `SymbolPainter::elementBounds`, ported.** See the header for why
    // this is the symbol's own box and not a box drawn around it.
    const float cx = static_cast<float>(part.visual.x);
    const float cy = static_cast<float>(part.visual.y);

    bool first = true;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;

    const auto include = [&](juce::Rectangle<float> box) {
        if (first) {
            minX = box.getX(); minY = box.getY();
            maxX = box.getRight(); maxY = box.getBottom();
            first = false;
            return;
        }

        minX = juce::jmin(minX, box.getX());
        minY = juce::jmin(minY, box.getY());
        maxX = juce::jmax(maxX, box.getRight());
        maxY = juce::jmax(maxY, box.getBottom());
    };

    // The artwork, in the part's own frame. Mirror, then rotate -- the same
    // order `pinPosition` uses and the same order the drawing uses, so the box
    // and the symbol turn together.
    if (const auto* artwork = artworkForPart(part)) {
        if (!artwork->isEmpty()) {
            auto box = artwork->bounds;

            if (part.visual.flipped)
                box.setX(-box.getRight());

            for (int turn = 0; turn < ((part.visual.rotation / 90) & 3); ++turn)
                box = {-box.getBottom(), box.getX(), box.getHeight(), box.getWidth()};

            include(box.translated(cx, cy));
        }
    }

    // The pins, so a part is always grabbable by its terminals even if its
    // artwork is missing entirely. (Céline's rule, and its reason.)
    const auto offsets = pinOffsets(part);
    for (int pin = 0; pin < static_cast<int>(offsets.size()); ++pin) {
        const auto p = pinPosition(part, pin);
        include(juce::Rectangle<float>(0.4f, 0.4f)
                    .withCentre(juce::Point<float>(static_cast<float>(p.x),
                                                  static_cast<float>(p.y))));
    }

    if (first)
        include({cx - 0.5f, cy - 0.5f, 1.0f, 1.0f});

    juce::Rectangle<float> box(minX, minY, maxX - minX, maxY - minY);

    // A floor, because some symbols are genuinely thin: a switch is a line and
    // two dots, which measures 0.4 of a square across. Tracing that exactly
    // would be correct and unusable. One rule for every part rather than a
    // nudge per type, so it cannot drift from the artwork. (Céline's rule.)
    constexpr float minimumSpan = 1.2f;

    if (box.getWidth() < minimumSpan)
        box = box.withWidth(minimumSpan).withX(box.getCentreX() - minimumSpan * 0.5f);

    if (box.getHeight() < minimumSpan)
        box = box.withHeight(minimumSpan).withY(box.getCentreY() - minimumSpan * 0.5f);

    return box;
}

bool Schematic::hitTestPart(const wdf::SchematicComponent& part, float px, float py,
                            float reach) const {
    // **The symbol's own shape, not the box around it.** `partBounds` is still
    // the right answer for framing and for enclosing selections, but using it to
    // decide what a click landed on made every part claim the empty corners of
    // its box -- and for a valve or a transformer, a good deal of empty space
    // inside it too.
    if (const auto* artwork = artworkForPart(part); artwork != nullptr && !artwork->isEmpty()) {
        if (artworkHitTest(*artwork, part, px, py, reach))
            return true;
    } else if (partBounds(part).contains(px, py)) {
        // A type with no artwork still has to be selectable. This is the old
        // box, and only this fallback uses it for picking.
        return true;
    }

    // Pin dots are drawn by the canvas rather than by the SVG, but they are part
    // of the symbol the user sees and aims at.
    return pinHitTest(part, px, py, reach);
}

bool Schematic::partTouchesBox(const wdf::SchematicComponent& part,
                               juce::Rectangle<float> box) const {
    if (box.isEmpty())
        return false;

    if (const auto* artwork = artworkForPart(part); artwork != nullptr && !artwork->isEmpty()) {
        if (artworkIntersectsBox(*artwork, part, box))
            return true;
    } else if (partBounds(part).intersects(box)) {
        return true;
    }

    return pinTouchesBox(part, box);
}

int Schematic::indexOfPart(const std::string& id) const noexcept {
    for (size_t i = 0; i < parts_.size(); ++i)
        if (parts_[i].id == id)
            return static_cast<int>(i);
    return -1;
}

wdf::SchematicComponent* Schematic::findPart(const std::string& id) noexcept {
    for (auto& p : parts_)
        if (p.id == id)
            return &p;
    return nullptr;
}

const wdf::SchematicComponent* Schematic::findPart(const std::string& id) const noexcept {
    for (const auto& p : parts_)
        if (p.id == id)
            return &p;
    return nullptr;
}

std::string Schematic::addPart(const std::string& type, int x, int y) {
    wdf::SchematicComponent part =
        makeComponent(type, parts_, static_cast<float>(x), static_cast<float>(y));
    if (part.type.empty())
        return {};
    return addPart(std::move(part));
}

std::string Schematic::addPart(wdf::SchematicComponent part) {
    // Ids are strings in this format, so uniqueness is by string. A document
    // loaded from a file keeps its ids; a new part gets one from the library.
    std::set<std::string> used;
    for (const auto& p : parts_)
        used.insert(p.id);

    // A blank or colliding id gets a fresh one, so a hand-edited file cannot
    // produce two parts that `findPart` cannot tell apart.
    if (part.id.empty() || used.count(part.id))
        part.id = uniqueId(part.type, parts_);

    const std::string id = part.id;
    parts_.push_back(std::move(part));
    return id;
}

int Schematic::removePart(const std::string& id) {
    const int index = indexOfPart(id);
    if (index < 0)
        return 0;

    parts_.erase(parts_.begin() + index);

    // Wires that are now attached to nothing are dropped with it. Without this,
    // deleting a resistor leaves its two wires hanging in space, connected to
    // each other and to nothing else -- which reads as a track left behind on a
    // layout that the user believes they cleared.
    const auto gone = [](const Wire& w) {
        // A wire survives if it still has length. Whether a pin is on it is
        // decided by the net extractor, not here; this only removes degenerate
        // leftovers.
        return w.ax == w.bx && w.ay == w.by;
    };

    const auto before = wires_.size();
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(), gone), wires_.end());
    return static_cast<int>(before - wires_.size());
}

std::string Schematic::partAt(float px, float py, float reach) const {
    // Newest first, so the part drawn last -- the one on top -- is the one you
    // get when two overlap.
    for (size_t i = parts_.size(); i > 0; --i)
        if (hitTestPart(parts_[i - 1], px, py, reach))
            return parts_[i - 1].id;
    return "";
}

std::pair<std::string, int> Schematic::pinNear(int px, int py, int reach) const {
    std::string bestPart;
    int bestPin = -1;
    long bestDistance = static_cast<long>(reach) * reach + 1;

    for (const auto& part : parts_) {
        const int pinCount = static_cast<int>(pinOffsets(part).size());
        for (int pin = 0; pin < pinCount; ++pin) {
            const auto p = pinPosition(part, pin);
            const long dx = px - p.x, dy = py - p.y;
            const long d = dx * dx + dy * dy;
            if (d < bestDistance) {
                bestDistance = d;
                bestPart = part.id;
                bestPin = pin;
            }
        }
    }

    return {bestPart, bestPin};
}

// ============================================================================
// Wires
// ============================================================================

std::vector<int> Schematic::addWire(int ax, int ay, int bx, int by) {
    if (ax == bx && ay == by)
        return {};

    if (ax == bx || ay == by) {
        wires_.push_back({nextWireId_++, ax, ay, bx, by});
        return {wires_.back().id};
    }

    // A diagonal drag becomes an L, across then down: two segments meeting at a
    // corner, joined by the net extractor because they share an endpoint. Two
    // ids as well, so either can be deleted on its own when the corner went the
    // wrong way round.
    const int cornerX = bx, cornerY = ay;
    wires_.push_back({nextWireId_++, ax, ay, cornerX, cornerY});
    wires_.push_back({nextWireId_++, cornerX, cornerY, bx, by});
    return {wires_[wires_.size() - 2].id, wires_.back().id};
}

const Wire* Schematic::findWire(int id) const noexcept {
    const auto it = std::find_if(wires_.begin(), wires_.end(),
                                 [id](const Wire& w) { return w.id == id; });
    return it == wires_.end() ? nullptr : &(*it);
}

int Schematic::wireNear(float px, float py, float reach) const {
    float best = reach;
    int found = -1;

    for (const auto& wire : wires_) {
        const juce::Line<float> line(static_cast<float>(wire.ax),
                                     static_cast<float>(wire.ay),
                                     static_cast<float>(wire.bx),
                                     static_cast<float>(wire.by));

        juce::Point<float> nearest;
        const auto distance = line.getDistanceFromPoint({px, py}, nearest);

        // `<=`, so of two wires crossing at the same distance the later one
        // wins -- the one drawn on top, which is the one you were aiming at.
        if (distance <= best) {
            best = distance;
            found = wire.id;
        }
    }

    return found;
}

bool Schematic::removeWire(int id) {
    const auto it = std::find_if(wires_.begin(), wires_.end(),
                                 [id](const Wire& w) { return w.id == id; });
    if (it == wires_.end())
        return false;
    wires_.erase(it);
    return true;
}

void Schematic::moveWire(int id, int dx, int dy) {
    for (auto& wire : wires_)
        if (wire.id == id) {
            wire.ax += dx;
            wire.ay += dy;
            wire.bx += dx;
            wire.by += dy;
            return;
        }
}

bool Schematic::resizeWireEnd(int id, int end, int px, int py) {
    for (auto& wire : wires_) {
        if (wire.id != id)
            continue;

        const int anchorX = end == 0 ? wire.bx : wire.ax;
        const int anchorY = end == 0 ? wire.by : wire.ay;
        int& movingX = end == 0 ? wire.ax : wire.bx;
        int& movingY = end == 0 ? wire.ay : wire.by;

        // Only the coordinate running along the wire comes from the pointer; the
        // other stays on the anchor, which keeps the wire axis-aligned however
        // far off the line the mouse strays.
        const int wantX = wire.isVertical() ? anchorX : px;
        const int wantY = wire.isVertical() ? py : anchorY;

        if (wantX == anchorX && wantY == anchorY)
            return false;   // landing on the anchor: a zero-length wire

        if (wantX == movingX && wantY == movingY)
            return false;

        movingX = wantX;
        movingY = wantY;
        return true;
    }
    return false;
}

int Schematic::removeWiresAt(int px, int py) {
    const auto before = wires_.size();
    for (size_t i = wires_.size(); i > 0; --i)
        if (wires_[i - 1].contains(px, py))
            wires_.erase(wires_.begin() + static_cast<long>(i - 1));
    return static_cast<int>(before - wires_.size());
}

int Schematic::mergeCollinearWires(int preferredId) {
    const auto before = wires_.size();

    const auto joinable = [](const Wire& x, const Wire& y) {
        if (x.isVertical() && y.isVertical() && x.ax == y.ax) {
            const int lo = std::max(std::min(x.ay, x.by), std::min(y.ay, y.by));
            const int hi = std::min(std::max(x.ay, x.by), std::max(y.ay, y.by));
            return lo <= hi;
        }
        if (x.isHorizontal() && y.isHorizontal() && x.ay == y.ay) {
            const int lo = std::max(std::min(x.ax, x.bx), std::min(y.ax, y.bx));
            const int hi = std::min(std::max(x.ax, x.bx), std::max(y.ax, y.bx));
            return lo <= hi;
        }
        return false;
    };

    // Connectivity is mediated by *points*: extractNets registers pins and wire
    // ends, and joins a wire to every registered point lying on it. So merging
    // two collinear wires deletes the ends where they met, and if that was the
    // only thing registering the point, every other wire through it comes loose.
    // A wire merely passing through does not register one -- so a four-way node
    // would come apart while looking identical. (This is Celine's hard-won
    // explanation, and it is why merging is gated rather than unconditional.)
    const auto seamSurvives = [this](int px, int py, const Wire& x, const Wire& y) {
        for (const auto& part : parts_)
            for (int pin = 0; pin < static_cast<int>(pinOffsets(part).size()); ++pin) {
                const auto p = pinPosition(part, pin);
                if (p.x == px && p.y == py)
                    return true;
            }
        for (const auto& other : wires_)
            if (&other != &x && &other != &y
                && ((other.ax == px && other.ay == py) || (other.bx == px && other.by == py)))
                return true;
        return false;
    };

    const auto somethingRunsThrough = [this](int px, int py, const Wire& x, const Wire& y) {
        for (const auto& other : wires_)
            if (&other != &x && &other != &y && other.contains(px, py))
                return true;
        return false;
    };

    for (bool merged = true; merged;) {
        merged = false;

        for (size_t i = 0; i < wires_.size() && !merged; ++i) {
            for (size_t j = i + 1; j < wires_.size(); ++j) {
                if (!joinable(wires_[i], wires_[j]))
                    continue;

                const auto& first = wires_[i];
                const auto& second = wires_[j];

                const int lowX = std::min(std::min(first.ax, first.bx),
                                          std::min(second.ax, second.bx));
                const int lowY = std::min(std::min(first.ay, first.by),
                                          std::min(second.ay, second.by));
                const int highX = std::max(std::max(first.ax, first.bx),
                                           std::max(second.ax, second.bx));
                const int highY = std::max(std::max(first.ay, first.by),
                                           std::max(second.ay, second.by));

                // Leave the pair alone if dissolving where they meet would
                // strand something that reached them through it.
                bool stranding = false;
                for (const auto e : {std::make_pair(first.ax, first.ay),
                                     std::make_pair(first.bx, first.by),
                                     std::make_pair(second.ax, second.ay),
                                     std::make_pair(second.bx, second.by)}) {
                    const bool onSpan = (e.first == lowX && e.second == lowY)
                                     || (e.first == highX && e.second == highY);
                    if (onSpan)
                        continue;
                    if (!seamSurvives(e.first, e.second, first, second)
                        && somethingRunsThrough(e.first, e.second, first, second))
                        stranding = true;
                }
                if (stranding)
                    continue;

                const int keep = (wires_[j].id == preferredId) ? wires_[j].id : wires_[i].id;
                wires_[i].id = keep;
                wires_[i].ax = lowX;  wires_[i].ay = lowY;
                wires_[i].bx = highX; wires_[i].by = highY;
                wires_.erase(wires_.begin() + static_cast<long>(j));

                merged = true;
                break;
            }
        }
    }

    return static_cast<int>(before - wires_.size());
}

std::vector<std::pair<int, int>> Schematic::findJunctions() const {
    std::map<std::pair<int, int>, int, PointLess> endCount;
    for (const auto& wire : wires_) {
        ++endCount[{wire.ax, wire.ay}];
        ++endCount[{wire.bx, wire.by}];
    }

    std::map<int, std::vector<const Wire*>> verticalsByX, horizontalsByY;
    for (const auto& wire : wires_) {
        if (wire.isVertical())
            verticalsByX[wire.ax].push_back(&wire);
        if (wire.isHorizontal())
            horizontalsByY[wire.ay].push_back(&wire);
    }

    std::vector<std::pair<int, int>> junctions;

    for (const auto& [point, count] : endCount) {
        if (count >= 3) {
            junctions.push_back(point);
            continue;
        }

        const auto at = point;
        const auto touches = [&at](const std::vector<const Wire*>& candidates) {
            for (const auto* wire : candidates)
                if (!((wire->ax == at.first && wire->ay == at.second)
                      || (wire->bx == at.first && wire->by == at.second))
                    && wire->contains(at.first, at.second))
                    return true;
            return false;
        };

        bool found = false;
        if (const auto column = verticalsByX.find(point.first); column != verticalsByX.end())
            found = touches(column->second);
        if (!found)
            if (const auto row = horizontalsByY.find(point.second); row != horizontalsByY.end())
                found = touches(row->second);

        if (found)
            junctions.push_back(point);
    }

    return junctions;
}

// ============================================================================
// Connectivity
// ============================================================================

std::vector<std::pair<int, int>> Schematic::wireEnds() const {
    std::vector<std::pair<int, int>> ends;
    ends.reserve(wires_.size() * 2);
    for (const auto& w : wires_) {
        ends.push_back({w.ax, w.ay});
        ends.push_back({w.bx, w.by});
    }
    return ends;
}

NetList Schematic::extractNets() const {
    // **One answer to "what is connected to what". The same call the reader
    // makes when it loads a file** -- `DocumentIO.cpp` runs this over the parts
    // and wires it just read. That is the whole point of this function existing:
    // a sheet being edited and the document that sheet serializes to must not
    // disagree about which pin is on which net, and they used to, because there
    // were two union-finds -- one that joined a wire's endpoints and pins
    // landing along it, and another that did not, and a part wired to nothing is
    // drawn exactly like a part wired to something.
    //
    // The rules (coincident points join, a wire joins its ends, a pin landing
    // *along* a wire joins it, nets are numbered in Celine's row-first order, a
    // net's name comes from the terminals drawn on it) live in
    // `format/SchematicNets.h` and are stated there once. Nothing here repeats
    // them, and nothing here ranks a stored name: a supply -- rail, `src`,
    // `isrc` -- names nothing, so the sheet cannot grow two supplies that claim
    // one net, which is what made the converter build a circuit that then
    // refused to solve.
    NetList result;

    // The editor keeps a wire as one axis-aligned segment, for dragging and
    // hit-testing; the derivation wants the document's routed polyline. One
    // segment is a polyline of two points, and the two accounts agree because
    // they are derived from each other, not copied from each other.
    std::vector<wdf::SchematicWire> routed;
    routed.reserve(wires_.size());
    for (const auto& wire : wires_) {
        wdf::SchematicWire drawn;
        drawn.points.push_back({static_cast<float>(wire.ax), static_cast<float>(wire.ay)});
        drawn.points.push_back({static_cast<float>(wire.bx), static_cast<float>(wire.by)});
        routed.push_back(drawn);
    }

    // Ground keeps the document's own name, so naming and grounding cannot
    // disagree. Input and output are named by the terminal *parts*, not by the
    // document's `inputNode`/`outputNode`: those are leftovers of the format
    // that used to name nets, and reusing them as names would let a stale "n1"
    // claim whichever net the drawing numbers first -- silently merging two
    // nets into one.
    const wdf::DerivedNets nets = wdf::deriveNets(parts_, routed, groundNode_, "in", "out");

    result.netOfPart = std::move(nets.netOfPin);
    result.netNames = std::move(nets.netNames);
    result.netOfWire = std::move(nets.netOfWire);
    result.danglingNets = std::move(nets.danglingNets);
    return result;
}

ProbeReadout Schematic::probeReadout(const std::string& partId) const {
    ProbeReadout readout;
    const auto* part = findPart(partId);
    if (!part || part->type != "scope")
        return readout;

    const auto nets = extractNets();
    const int index = indexOfPart(partId);
    if (index < 0)
        return readout;

    const auto& pins = nets.netOfPart[static_cast<size_t>(index)];

    // A row is sized to the part's own pin count, so a part the palette has no
    // pins for has no signal to report either -- inventing one at the part's
    // centre is what connected a part to something it was never drawn on.
    if (pins.size() < 2)
        return readout;

    // Pin 0 is the signal, pin 1 the reference. A reference that is ground (net
    // name "0") is reported as empty, which the reader treats as "against
    // ground" rather than "dangling".
    if (pins[0] >= 0)
        readout.node = nets.netNames[static_cast<size_t>(pins[0])];
    if (pins[1] >= 0)
        readout.reference = nets.netNames[static_cast<size_t>(pins[1])];
    if (readout.reference == "0")
        readout.reference.clear();

    // **A reference that is wired to nothing is ground.**
    //
    // The probe is a voltmeter: one lead on the node you want to know about,
    // and the other lead on the circuit's ground. A person placing a meter on a
    // wire wants to know the voltage *against ground*, so the second lead is
    // optional -- unwired means ground, not "dangling".
    //
    // The reference net is the probe's own, alone, when nothing else is on it:
    // then it is unwired.
    if (!readout.reference.empty() && pins[1] >= 0) {
        int onNet = 0;
        for (const auto& partPins : nets.netOfPart)
            for (const int net : partPins)
                if (net == pins[1])
                    ++onNet;
        if (onNet <= 1)
            readout.reference.clear();
    }

    return readout;
}

// ============================================================================
// Whole-sheet
// ============================================================================

void Schematic::clear() {
    parts_.clear();
    wires_.clear();
    nextPartId_ = 1;
    nextWireId_ = 1;
    name_ = "Untitled Circuit";
    category_ = "Custom";
    author_ = "LiveWDF User";
    description_ = "";
    allowPreviewInputPad_ = false;
    previewPadDb_ = 0.;
    inputNode_ = "in";
    outputNode_ = "out";
    groundNode_ = "0";
    potOrder_.clear();
    switchOrder_.clear();
}

juce::Rectangle<float> Schematic::contentBounds() const noexcept {
    juce::Rectangle<float> bounds;
    bool first = true;

    const auto include = [&](juce::Rectangle<float> box) {
        bounds = first ? box : bounds.getUnion(box);
        first = false;
    };

    for (const auto& part : parts_)
        include(partBounds(part));

    for (const auto& wire : wires_) {
        const float x0 = static_cast<float>(std::min(wire.ax, wire.bx));
        const float x1 = static_cast<float>(std::max(wire.ax, wire.bx));
        const float y0 = static_cast<float>(std::min(wire.ay, wire.by));
        const float y1 = static_cast<float>(std::max(wire.ay, wire.by));
        include(juce::Rectangle<float>(x0, y0, x1 - x0, y1 - y0).expanded(1.0f));
    }

    return first ? juce::Rectangle<float>{} : bounds;
}

// ============================================================================
// Persistence
// ============================================================================

void Schematic::fromDocument(const wdf::LiveWDFDocument& doc) {
    clear();
    name_ = doc.name;
    category_ = doc.category;
    author_ = doc.author;
    description_ = doc.description;
    allowPreviewInputPad_ = doc.allowPreviewInputPad;
    previewPadDb_ = doc.previewPadDb;
    inputNode_ = doc.inputNode;
    outputNode_ = doc.outputNode;
    groundNode_ = doc.groundNode;
    potOrder_ = doc.potOrder;
    switchOrder_ = doc.switchOrder;

    for (const auto& comp : doc.components) {
        wdf::SchematicComponent part = comp;
        // The sheet is a grid, so a saved placement that drifted is read back at
        // the square it was meant to be on. Not a repair of connectivity: a pin
        // half a square off is a pin on nothing, and that is what derivation will
        // say.
        part.visual.x = std::round(part.visual.x);
        part.visual.y = std::round(part.visual.y);

        // **There is no rail `node` back-fill here any more.** A rail names
        // nothing (Celine's PSU and LiveSPICE's `Rail` both do this: their own
        // name becomes an unknown current in the equations, never the net they
        // feed), so there is no name to recover for it -- the rail drives
        // whatever net it is drawn onto, and the drawing is right below.
        parts_.push_back(std::move(part));
    }

    // **One wire of the file is one drawn line**, so it becomes the editor's
    // segments one segment at a time: a routed polyline of n points is n-1
    // segments that the user can select and drag individually, and the union of
    // them joins the same points the polyline joined. A file that writes a
    // polyline of n points and the editor reads it as one segment is the drift
    // class -- and the version-1/2 files that grouped *every* wire on a net into
    // one such polyline are refused at load for that reason, so any multi-point
    // polyline here is one the format means.
    for (const auto& routed : doc.wires) {
        for (size_t s = 1; s < routed.points.size(); ++s) {
            const auto& a = routed.points[s - 1];
            const auto& b = routed.points[s];
            addWire(static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)),
                    static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)));
        }
    }
}

wdf::LiveWDFDocument Schematic::toDocument() const {
    wdf::LiveWDFDocument doc;
    doc.name = name_;
    doc.category = category_;
    doc.author = author_;
    doc.description = description_;
    doc.allowPreviewInputPad = allowPreviewInputPad_;
    doc.previewPadDb = previewPadDb_;
    doc.inputNode = inputNode_;
    doc.outputNode = outputNode_;
    doc.groundNode = groundNode_;
    doc.components = parts_;

    // **The drawing is written, and nothing else about connectivity.** Not one
    // pin is renamed onto a net it was not drawn on, not one net is handed a
    // name the sheet does not draw, not one rail property is rewritten to agree
    // with the net the rail happens to touch. The v1/v2 format stored a
    // `map<pin, netName>` per part and a net name per wire, so the editor of
    // that format had ~200 lines here whose whole job was to decide which of
    // several stored names a net should keep -- user names outranking generated
    // ones, "is this pin's name a leftover from its birth", "is two nets sharing
    // one name a bug or the user's intent", de-duplicating names by renaming
    // them. Every one of those questions stops existing once connectivity is the
    // drawing: there is one name per net and it comes from the terminals drawn
    // on it, and a sheet that says two conflicting things about one net is
    // refused at load rather than reconciled here. That is also why the
    // editor used to be able to make a circuit the converter refused: the
    // drawing was right, the pins were not, and nothing downstream could tell
    // which account to believe.
    const NetList nets = extractNets();

    // `SchematicComponent::pins` is a cache the reader fills at load and the
    // writer never serialises -- but a document built *here* is handed straight
    // to `toCircuit()`, which reads it, so it has to be filled, and the only
    // honest fill is the same derivation the drawing just went through. That is
    // the whole point: the circuit the editor is running and the circuit the
    // saved file compiles to come from one connectivity, not from two accounts
    // of it that agree by luck.
    //
    // Insert rather than overwrite: a part made by the palette may not have got
    // a pin name at all, and "the pin's name is missing" used to mean "so it is
    // ground", which is what invented a connection.
    for (size_t partIndex = 0; partIndex < doc.components.size(); ++partIndex) {
        const PartSpec* spec = livewdf::editor::findPart(doc.components[partIndex].type);
        if (!spec)
            continue;   // no pins means no placement; derivation gave this part an empty row too

        const auto& pins = livewdf::editor::partPins(doc.components[partIndex]);
        const auto& row = nets.netOfPart[partIndex];
        for (size_t pin = 0; pin < pins.size(); ++pin) {
            const int net = pin < row.size() ? row[pin] : -1;
            if (net < 0 || net >= static_cast<int>(nets.netNames.size()))
                continue;
            doc.components[partIndex].pins[pins[pin]] = nets.netNames[static_cast<size_t>(net)];
        }
    }

    // Write each wire as its own two-point polyline so routing and topology
    // round-trip losslessly. The in-memory document carries the wire's net name
    // (parallel to `DocumentIO::fromJson`), while `toJson` strips it to maintain
    // the geometry-only format contract.
    for (size_t i = 0; i < wires_.size(); ++i) {
        const auto& wire = wires_[i];
        wdf::SchematicWire w;
        const int netIndex = i < nets.netOfWire.size() ? nets.netOfWire[i] : -1;
        if (netIndex >= 0 && netIndex < static_cast<int>(nets.netNames.size()))
            w.net = nets.netNames[static_cast<size_t>(netIndex)];
        w.points.push_back({static_cast<float>(wire.ax), static_cast<float>(wire.ay)});
        w.points.push_back({static_cast<float>(wire.bx), static_cast<float>(wire.by)});
        doc.wires.push_back(std::move(w));
    }

    // **The document's io names say what the drawing says**, so a sheet saved
    // once stops carrying a v1/v2 leftover name the next load would otherwise
    // use as the name of a net -- a stale "n1" used to claim whichever net the
    // drawing happened to number first, silently merging two nets into one.
    // `inputNet()`/`outputNet()` answer from the terminal parts' derived pins,
    // and these two fields are the fallback they offer for a sheet that has no
    // terminal part at all, which the loader says so.
    doc.inputNode = doc.inputNet();
    doc.outputNode = doc.outputNet();
    doc.potOrder = potOrder_;
    doc.switchOrder = switchOrder_;

    return doc;
}

}  // namespace livewdf::editor
