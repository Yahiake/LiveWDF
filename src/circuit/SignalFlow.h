#pragma once

// Directed Signal Flow Graph analysis for circuit stage partitioning.
//
// Identifies active devices (triodes, pentodes, BJTs, JFETs, MOSFETs, op-amps,
// diodes) and determines directed signal dependencies between them by
// traversing passive signal paths while strictly excluding power and DC rail
// nodes (GND, B+, VCC, etc.).
//
// Strongly Connected Components (SCCs) define co-solved stages (active devices
// with mutual feedback, such as shared-cathode pairs, long-tailed pairs,
// anti-parallel clippers, or active feedback loops).
//
// Feedforward cascades (e.g. multi-stage preamps like Mesa Mark IIC+,
// Marshall JCM800, Fender Bassman) have no directed reverse signal path;
// their SCCs are single-device stages evaluated in topological order.
//
// Cleanroom design inspired by directed flow principles in circuit analysis.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "circuit/RootLayout.h"
#include "spice/Circuit.h"

namespace wdf {

/// An active element in the circuit graph.
struct FlowElement {
    RootKind kind = RootKind::Triode;
    int index = 0;              ///< Device index matching rootDevices_ order (0..nGroups-1)
    int compIndex = 0;          ///< Index in Circuit's component vector (e.g. tri[compIndex])
    std::string name;
    std::vector<std::string> inPins;
    std::vector<std::string> outPins;
    bool isBarrierInput = false;///< e.g. OpAmp inverting input acts as summing barrier
};

/// A partition stage: active devices that are mutually coupled (an SCC).
struct FlowStage {
    std::vector<int> devices;   ///< Indices into `FlowGraph::elements`
    bool hasFeedback = false;   ///< True if multi-device or device has self-feedback
};

/// Result of directed signal flow analysis.
struct FlowGraph {
    int numElements = 0;
    std::vector<FlowElement> elements;
    std::unordered_set<std::string> railNodes;
    std::vector<std::vector<int>> flowAdj;  ///< i -> j means i drives j
    std::vector<FlowStage> stages;          ///< Topologically ordered stages
    std::vector<int> stageOfDevice;         ///< device index -> stage index
};

namespace detail {

inline bool isRailName(const std::string& name) {
    if (name.empty())
        return false;
    if (name == "0" || name == "gnd" || name == "GND" || name == "ground")
        return true;
    std::string lower;
    lower.reserve(name.size());
    for (char c : name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower == "vcc" || lower == "vdd" || lower == "vee" ||
           lower == "vss" || lower == "b+" || lower == "bplus" ||
           lower == "rail" || lower.rfind("rail_", 0) == 0;
}

inline std::unordered_set<std::string> findRailNodes(const Circuit& c) {
    std::unordered_set<std::string> rails;
    rails.insert("0");
    rails.insert("gnd");
    rails.insert("GND");
    rails.insert("ground");

    auto isGroundNode = [&](const std::string& name) {
        return name == "0" || name == "gnd" || name == "GND" || name == "ground";
    };

    // Any node with explicit rail naming
    for (const auto& s : c.src) {
        if (isRailName(s.a)) rails.insert(s.a);
        if (isRailName(s.b)) rails.insert(s.b);
        // DC voltage sources referenced to ground define rail potentials
        if (isGroundNode(s.b)) {
            if (std::abs(s.dc) > 1e-6)
                rails.insert(s.a);
        } else if (isGroundNode(s.a)) {
            if (std::abs(s.dc) > 1e-6)
                rails.insert(s.b);
        }
    }
    for (const auto& r : c.res) {
        if (isRailName(r.a)) rails.insert(r.a);
        if (isRailName(r.b)) rails.insert(r.b);
    }
    for (const auto& cap : c.cap) {
        if (isRailName(cap.a)) rails.insert(cap.a);
        if (isRailName(cap.b)) rails.insert(cap.b);
    }

    // Iteratively discover decoupled power supply rails:
    // A node with a large bypass capacitor (C >= 0.5 uF) to ground or a rail,
    // AND connected to an active rail via a resistor or DC supply,
    // is an AC virtual ground / power supply rail.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& s : c.src) {
            if (std::abs(s.dc) > 1e-6) {
                if (rails.count(s.b) && !rails.count(s.a)) {
                    rails.insert(s.a);
                    changed = true;
                } else if (rails.count(s.a) && !rails.count(s.b)) {
                    rails.insert(s.b);
                    changed = true;
                }
            }
        }
        for (const auto& cap : c.cap) {
            if (cap.value < 0.5e-6) continue;
            std::string node;
            if (isGroundNode(cap.b) || rails.count(cap.b)) node = cap.a;
            else if (isGroundNode(cap.a) || rails.count(cap.a)) node = cap.b;
            if (node.empty() || rails.count(node) > 0) continue;

            for (const auto& r : c.res) {
                if ((r.a == node && rails.count(r.b)) ||
                    (r.b == node && rails.count(r.a))) {
                    rails.insert(node);
                    changed = true;
                    break;
                }
            }
        }
    }

    return rails;
}

}  // namespace detail

/// Analyze the directed signal flow of a Circuit.
/// Matches the exact device order of `RootLayout` / `EngineCore::rootDevices_`.
inline FlowGraph analyzeSignalFlow(const Circuit& c) {
    FlowGraph fg;
    fg.railNodes = detail::findRailNodes(c);

    auto isRail = [&](const std::string& node) -> bool {
        return fg.railNodes.count(node) > 0 || detail::isRailName(node);
    };

    // 1. Collect active elements in the exact order established by buildRootLayout:
    //    triodes, diodes, controlled resistors, pentodes, jfets, mosfets, bjts, opamps.
    int elementIdx = 0;

    for (int t = 0; t < static_cast<int>(c.tri.size()); ++t) {
        const auto& ct = c.tri[static_cast<size_t>(t)];
        FlowElement el;
        el.kind = RootKind::Triode;
        el.index = elementIdx++;
        el.compIndex = t;
        el.name = ct.name.empty() ? ("V" + std::to_string(t)) : ct.name;
        el.inPins = {ct.g};
        el.outPins = {ct.p};
        el.isBarrierInput = true;
        if (!isRail(ct.k)) {
            el.outPins.push_back(ct.k);
            if (isRail(ct.g)) {
                el.inPins.push_back(ct.k); // grounded grid: cathode is the driven input
                el.isBarrierInput = false;
            }
        }
        fg.elements.push_back(std::move(el));
    }

    for (int d = 0; d < static_cast<int>(c.diode.size()); ++d) {
        const auto& cd = c.diode[static_cast<size_t>(d)];
        FlowElement el;
        el.kind = RootKind::Diode;
        el.index = elementIdx++;
        el.compIndex = d;
        el.name = cd.name.empty() ? ("D" + std::to_string(d)) : cd.name;
        el.inPins = {cd.a, cd.b};
        el.outPins = {cd.a, cd.b};
        fg.elements.push_back(std::move(el));
    }

    for (int cr = 0; cr < static_cast<int>(c.ctrlRes.size()); ++cr) {
        const auto& ccr = c.ctrlRes[static_cast<size_t>(cr)];
        FlowElement el;
        el.kind = RootKind::ControlledRes;
        el.index = elementIdx++;
        el.compIndex = cr;
        el.name = ccr.name.empty() ? ("CR" + std::to_string(cr)) : ccr.name;
        el.inPins = {ccr.a, ccr.b};
        el.outPins = {ccr.a, ccr.b};
        fg.elements.push_back(std::move(el));
    }

    for (int p = 0; p < static_cast<int>(c.pent.size()); ++p) {
        const auto& cp = c.pent[static_cast<size_t>(p)];
        FlowElement el;
        el.kind = RootKind::Pentode;
        el.index = elementIdx++;
        el.compIndex = p;
        el.name = cp.name.empty() ? ("P" + std::to_string(p)) : cp.name;
        el.inPins = {cp.g};
        el.outPins = {cp.p, cp.s};
        el.isBarrierInput = true;
        if (!isRail(cp.k)) {
            el.outPins.push_back(cp.k);
            if (isRail(cp.g)) {
                el.inPins.push_back(cp.k);
                el.isBarrierInput = false;
            }
        }
        fg.elements.push_back(std::move(el));
    }

    for (int j = 0; j < static_cast<int>(c.jfet.size()); ++j) {
        const auto& cj = c.jfet[static_cast<size_t>(j)];
        FlowElement el;
        el.kind = RootKind::Jfet;
        el.index = elementIdx++;
        el.compIndex = j;
        el.name = cj.name.empty() ? ("J" + std::to_string(j)) : cj.name;
        el.inPins = {cj.g};
        el.outPins = {cj.d};
        el.isBarrierInput = true;
        if (!isRail(cj.s)) {
            el.outPins.push_back(cj.s);
            if (isRail(cj.g)) {
                el.inPins.push_back(cj.s); // common-gate
                el.isBarrierInput = false;
            }
        }
        fg.elements.push_back(std::move(el));
    }

    for (int m = 0; m < static_cast<int>(c.mosfet.size()); ++m) {
        const auto& cm = c.mosfet[static_cast<size_t>(m)];
        FlowElement el;
        el.kind = RootKind::Mosfet;
        el.index = elementIdx++;
        el.compIndex = m;
        el.name = cm.name.empty() ? ("M" + std::to_string(m)) : cm.name;
        el.inPins = {cm.g};
        el.outPins = {cm.d};
        el.isBarrierInput = true;
        if (!isRail(cm.s)) {
            el.outPins.push_back(cm.s);
            if (isRail(cm.g)) {
                el.inPins.push_back(cm.s); // common-gate
                el.isBarrierInput = false;
            }
        }
        fg.elements.push_back(std::move(el));
    }

    for (int b = 0; b < static_cast<int>(c.bjt.size()); ++b) {
        const auto& cb = c.bjt[static_cast<size_t>(b)];
        FlowElement el;
        el.kind = RootKind::Bjt;
        el.index = elementIdx++;
        el.compIndex = b;
        el.name = cb.name.empty() ? ("Q" + std::to_string(b)) : cb.name;
        el.inPins = {cb.b};
        el.outPins = {cb.c};
        el.isBarrierInput = true;
        if (!isRail(cb.e)) {
            el.outPins.push_back(cb.e);
            if (isRail(cb.b)) {
                el.inPins.push_back(cb.e); // common-base: emitter is the driven input
                el.isBarrierInput = false;
            }
        }
        fg.elements.push_back(std::move(el));
    }

    for (int a = 0; a < static_cast<int>(c.opamp.size()); ++a) {
        const auto& ca = c.opamp[static_cast<size_t>(a)];
        FlowElement el;
        el.kind = RootKind::OpAmp;
        el.index = elementIdx++;
        el.compIndex = a;
        el.name = ca.name.empty() ? ("U" + std::to_string(a)) : ca.name;
        el.inPins.clear();
        if (!isRail(ca.pos) && ca.pos != ca.out)
            el.inPins.push_back(ca.pos);
        if (!isRail(ca.neg) && ca.neg != ca.out)
            el.inPins.push_back(ca.neg);
        el.outPins = {ca.out};
        el.isBarrierInput = true;
        fg.elements.push_back(std::move(el));
    }

    fg.numElements = elementIdx;
    fg.flowAdj.assign(static_cast<size_t>(fg.numElements), {});

    if (fg.numElements == 0)
        return fg;

    // 2. Build passive signal adjacency graph:
    // Exclude all rail nodes so power supply rails never leak reverse signal paths.
    std::unordered_map<std::string, std::vector<std::string>> passiveAdj;
    auto addPassiveEdge = [&](const std::string& u, const std::string& v) {
        if (u.empty() || v.empty() || u == v)
            return;
        if (isRail(u) || isRail(v))
            return;
        passiveAdj[u].push_back(v);
        passiveAdj[v].push_back(u);
    };

    for (const auto& r : c.res) addPassiveEdge(r.a, r.b);
    for (const auto& cap : c.cap) addPassiveEdge(cap.a, cap.b);
    for (const auto& ind : c.ind) addPassiveEdge(ind.a, ind.b);
    for (const auto& vr : c.varRes) addPassiveEdge(vr.a, vr.b);
    for (const auto& pot : c.pots) {
        addPassiveEdge(pot.n1, pot.wiper);
        addPassiveEdge(pot.wiper, pot.n2);
        addPassiveEdge(pot.n1, pot.n2);
    }
    // Coupled inductors / transformers:
    for (const auto& m : c.mutual) {
        std::vector<std::pair<std::string, std::string>> indPorts;
        for (const auto& iname : m.indNames) {
            for (const auto& ind : c.ind) {
                if (ind.name == iname) {
                    indPorts.push_back({ind.a, ind.b});
                    break;
                }
            }
        }
        for (size_t p1 = 0; p1 < indPorts.size(); ++p1) {
            for (size_t p2 = p1 + 1; p2 < indPorts.size(); ++p2) {
                addPassiveEdge(indPorts[p1].first, indPorts[p2].first);
                addPassiveEdge(indPorts[p1].first, indPorts[p2].second);
                addPassiveEdge(indPorts[p1].second, indPorts[p2].first);
                addPassiveEdge(indPorts[p1].second, indPorts[p2].second);
            }
        }
    }

    // 3. For each active element i, find which active elements j are reachable:
    for (int i = 0; i < fg.numElements; ++i) {
        const auto& elemI = fg.elements[static_cast<size_t>(i)];

        // Output nodes of OTHER active amplifying elements are blocked (prevents traversing
        // backwards through an active output into an upstream stage).
        // Diodes and controlled resistors are non-unilateral passive junctions;
        // they must never block signal propagation through the passive network.
        // Barrier inputs (op-amp inverting inputs) of other elements are also blocked.
        std::unordered_set<std::string> blocked;
        // Output nodes of other active amplifying elements are barriers:
        for (int j = 0; j < fg.numElements; ++j) {
            if (i == j)
                continue;
            const auto& elemJ = fg.elements[static_cast<size_t>(j)];
            if (elemJ.kind == RootKind::Diode || elemJ.kind == RootKind::ControlledRes)
                continue;
            for (const auto& outPin : elemJ.outPins) {
                if (!isRail(outPin))
                    blocked.insert(outPin);
            }
        }
        // Barrier inputs of ALL active elements (op-amp inputs, grids, gates)
        // are high-impedance inputs. Signal enters them, but does not conduct
        // backwards through them into preceding stages.
        for (int j = 0; j < fg.numElements; ++j) {
            const auto& elemJ = fg.elements[static_cast<size_t>(j)];
            if (elemJ.isBarrierInput) {
                for (const auto& inPin : elemJ.inPins) {
                    if (!isRail(inPin))
                        blocked.insert(inPin);
                }
            }
        }
        // VCVS buffer outputs are also unilateral barriers:
        for (const auto& v : c.vcvs) {
            if (!isRail(v.outA))
                blocked.insert(v.outA);
        }

        // BFS from element i's output pins
        std::unordered_set<std::string> visited;
        std::queue<std::string> q;

        for (const auto& outPin : elemI.outPins) {
            if (!isRail(outPin) && !visited.count(outPin)) {
                visited.insert(outPin);
                q.push(outPin);
            }
        }

        while (!q.empty()) {
            std::string curr = q.front();
            q.pop();

            // Forward traversal across linear VCVS buffers:
            for (const auto& v : c.vcvs) {
                if (curr == v.inA || (!isRail(v.inB) && curr == v.inB)) {
                    if (!isRail(v.outA) && !visited.count(v.outA)) {
                        visited.insert(v.outA);
                        q.push(v.outA);
                    }
                }
            }

            auto it = passiveAdj.find(curr);
            if (it == passiveAdj.end())
                continue;

            for (const auto& next : it->second) {
                if (visited.count(next))
                    continue;
                visited.insert(next);
                // Do not expand through blocked nodes
                if (blocked.count(next))
                    continue;
                q.push(next);
            }
        }

        // Check which other elements j receive signal from element i
        for (int j = 0; j < fg.numElements; ++j) {
            if (i == j)
                continue;
            const auto& elemJ = fg.elements[static_cast<size_t>(j)];

            // Direct terminal sharing outside rails (e.g. shared cathode)
            bool sharedTerminal = false;
            for (const auto& pI : elemI.outPins) {
                if (isRail(pI)) continue;
                for (const auto& pJ : elemJ.outPins) {
                    if (!isRail(pJ) && pI == pJ) {
                        sharedTerminal = true;
                        break;
                    }
                }
                if (sharedTerminal) break;
            }

            if (sharedTerminal) {
                fg.flowAdj[static_cast<size_t>(i)].push_back(j);
                continue;
            }

            // Path through passive network to element j's input
            bool inputReached = false;
            for (const auto& inPin : elemJ.inPins) {
                if (visited.count(inPin)) {
                    inputReached = true;
                    break;
                }
            }
            if (inputReached) {
                fg.flowAdj[static_cast<size_t>(i)].push_back(j);
            }
        }
    }

    // 4. Tarjan's Strongly Connected Components (SCC) algorithm:
    int n = fg.numElements;
    int indexCounter = 0;
    std::vector<int> index(static_cast<size_t>(n), -1);
    std::vector<int> lowlink(static_cast<size_t>(n), -1);
    std::vector<bool> onStack(static_cast<size_t>(n), false);
    std::vector<int> stack;
    std::vector<std::vector<int>> sccs;

    auto strongConnect = [&](auto& self, int u) -> void {
        index[static_cast<size_t>(u)] = indexCounter;
        lowlink[static_cast<size_t>(u)] = indexCounter;
        ++indexCounter;
        stack.push_back(u);
        onStack[static_cast<size_t>(u)] = true;

        for (int v : fg.flowAdj[static_cast<size_t>(u)]) {
            if (index[static_cast<size_t>(v)] == -1) {
                self(self, v);
                lowlink[static_cast<size_t>(u)] =
                    std::min(lowlink[static_cast<size_t>(u)], lowlink[static_cast<size_t>(v)]);
            } else if (onStack[static_cast<size_t>(v)]) {
                lowlink[static_cast<size_t>(u)] =
                    std::min(lowlink[static_cast<size_t>(u)], index[static_cast<size_t>(v)]);
            }
        }

        if (lowlink[static_cast<size_t>(u)] == index[static_cast<size_t>(u)]) {
            std::vector<int> scc;
            for (;;) {
                int w = stack.back();
                stack.pop_back();
                onStack[static_cast<size_t>(w)] = false;
                scc.push_back(w);
                if (w == u)
                    break;
            }
            sccs.push_back(std::move(scc));
        }
    };

    for (int u = 0; u < n; ++u) {
        if (index[static_cast<size_t>(u)] == -1)
            strongConnect(strongConnect, u);
    }

    // 5. Build stage DAG of SCCs and topologically sort:
    int numSccs = static_cast<int>(sccs.size());
    std::vector<int> sccOf(static_cast<size_t>(n), -1);
    for (int s = 0; s < numSccs; ++s) {
        for (int dev : sccs[static_cast<size_t>(s)])
            sccOf[static_cast<size_t>(dev)] = s;
    }

    std::vector<std::vector<int>> dagAdj(static_cast<size_t>(numSccs));
    std::vector<int> inDegree(static_cast<size_t>(numSccs), 0);

    for (int u = 0; u < n; ++u) {
        int su = sccOf[static_cast<size_t>(u)];
        for (int v : fg.flowAdj[static_cast<size_t>(u)]) {
            int sv = sccOf[static_cast<size_t>(v)];
            if (su != sv) {
                // Edge su -> sv (su drives sv)
                if (std::find(dagAdj[static_cast<size_t>(su)].begin(),
                              dagAdj[static_cast<size_t>(su)].end(), sv) ==
                    dagAdj[static_cast<size_t>(su)].end()) {
                    dagAdj[static_cast<size_t>(su)].push_back(sv);
                    ++inDegree[static_cast<size_t>(sv)];
                }
            }
        }
    }

    // Kahn's algorithm for topological sort:
    std::queue<int> topoQ;
    for (int s = 0; s < numSccs; ++s) {
        if (inDegree[static_cast<size_t>(s)] == 0)
            topoQ.push(s);
    }

    std::vector<int> topoOrder;
    while (!topoQ.empty()) {
        int u = topoQ.front();
        topoQ.pop();
        topoOrder.push_back(u);

        for (int v : dagAdj[static_cast<size_t>(u)]) {
            if (--inDegree[static_cast<size_t>(v)] == 0)
                topoQ.push(v);
        }
    }

    // In case of disconnected or tie DAG, ensure all SCCs are included
    if (static_cast<int>(topoOrder.size()) < numSccs) {
        for (int s = 0; s < numSccs; ++s) {
            if (std::find(topoOrder.begin(), topoOrder.end(), s) == topoOrder.end())
                topoOrder.push_back(s);
        }
    }

    fg.stages.resize(static_cast<size_t>(numSccs));
    fg.stageOfDevice.assign(static_cast<size_t>(n), -1);

    for (size_t sIdx = 0; sIdx < topoOrder.size(); ++sIdx) {
        int originalScc = topoOrder[sIdx];
        FlowStage stage;
        stage.devices = std::move(sccs[static_cast<size_t>(originalScc)]);
        // Sort devices within stage for deterministic order
        std::sort(stage.devices.begin(), stage.devices.end());
        stage.hasFeedback = (stage.devices.size() > 1);
        for (int dev : stage.devices)
            fg.stageOfDevice[static_cast<size_t>(dev)] = static_cast<int>(sIdx);
        fg.stages[sIdx] = std::move(stage);
    }

    return fg;
}

}  // namespace wdf
