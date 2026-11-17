#pragma once

// Level 5 Phase 1d: Tree-based Wave Digital Filter (WDF) Executor.
//
// Translates a reduced SPQR Tree (from `circuit/spqr/Reduce.h`) into an adapted,
// compiled, cache-local WDF scattering tree. Evaluates series and parallel adaptors
// using canonical 1-multiplier Fettweis scattering.
//
// Unlike the dense R-type engine (which multiplies an N x P dense matrix `Xe * w`
// on every sample), this tree executor evaluates strictly local adaptor updates:
//   - Series: 1 multiply, 3 additions
//   - Parallel: 1 multiply, 4 additions
//   - Capacitors / Inductors: 1 memory store
// Pre-flattened in post-order and pre-order traversal arrays to eliminate all recursion,
// branching in the inner loop, and heap allocations during `step()`.

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "circuit/spqr/Reduce.h"
#include "circuit/tree/TreeIR.h"
#include "spice/Net.h"

namespace wdf {

/// Node in the compiled tree executor.
struct ExecNode {
    NodeKind kind = NodeKind::Leaf;
    LeafKind leaf = LeafKind::None;
    std::string name;

    int child0 = -1;
    int child1 = -1;
    int parent = -1;

    double R = 1.0;       ///< Port resistance (Ohms)
    double G = 1.0;       ///< Port conductance (Siemens)
    double gamma = 0.0;   ///< Canonical 1-multiplier reflection factor
    double a = 0.0;       ///< Incident wave
    double b = 0.0;       ///< Reflected wave
    double state = 0.0;   ///< Reactive state (incident wave from previous sample)
    double value = 0.0;   ///< Underlying component value (Ohms, Farads, Henrys)
    double polarity = 1.0;///< Orientation sign for node voltage probe
};

/// Compact bytecode op for series/parallel adaptor wave scattering.
struct AdaptorOp {
    bool isParallel = false;
    int self = -1;
    int child0 = -1;
    int child1 = -1;
    double gamma = 0.0;
};

/// Reactive state operation for capacitor/inductor.
struct ReactiveOp {
    int idx = -1;
    bool isInductor = false;
};

class TreeExecutor {
public:
    /// Construct from a `Circuit` description and sampling rate.
    TreeExecutor(const Circuit& c, double fs, const std::string& outElement = "")
        : fs_(fs), T_(1.0 / fs), outElementName_(outElement) {
        std::map<std::string, double> values;
        std::vector<NetEdge> edges;

        for (const auto& s : c.src) {
            std::string name = s.name.empty() ? "V1" : s.name;
            edges.push_back({s.a, s.b, LeafKind::VoltageSource, name, true});
            values[name] = s.dc;
        }
        for (const auto& r : c.res) {
            edges.push_back({r.a, r.b, LeafKind::Resistor, r.name, false});
            values[r.name] = r.value;
        }
        for (const auto& cap : c.cap) {
            edges.push_back({cap.a, cap.b, LeafKind::Capacitor, cap.name, false});
            values[cap.name] = cap.value;
        }
        for (const auto& ind : c.ind) {
            edges.push_back({ind.a, ind.b, LeafKind::Inductor, ind.name, false});
            values[ind.name] = ind.value;
        }

        compileFromEdges(edges, values);
    }

    /// Construct directly from netlist edges and component values.
    TreeExecutor(const std::vector<NetEdge>& edges, const std::map<std::string, double>& values,
                 double fs, const std::string& outElement = "")
        : fs_(fs), T_(1.0 / fs), outElementName_(outElement) {
        compileFromEdges(edges, values);
    }

    /// Reset all incident, reflected, and reactive states to zero.
    void reset() noexcept {
        for (auto& n : nodes_) {
            n.a = 0.0;
            n.b = 0.0;
            n.state = 0.0;
        }
    }

    /// Advance one sample with the given input source value.
    /// Returns the probed output element voltage.
    double step(double sourceValue) noexcept {
        // Phase 1a: Set reactive reflected waves
        for (const auto& op : reactiveOps_) {
            ExecNode& n = nodes_[static_cast<size_t>(op.idx)];
            n.b = op.isInductor ? -n.state : n.state;
        }

        // Phase 1b: Upward wave propagation through adaptors (post-order)
        for (const auto& op : adaptorUpOps_) {
            ExecNode& n = nodes_[static_cast<size_t>(op.self)];
            const double b0 = nodes_[static_cast<size_t>(op.child0)].b;
            const double b1 = nodes_[static_cast<size_t>(op.child1)].b;
            if (op.isParallel) {
                n.b = b1 + op.gamma * (b0 - b1);
            } else {
                n.b = -(b0 + b1);
            }
        }

        // Phase 2: Root Reflection at voltage source
        if (topIdx_ >= 0) {
            ExecNode& top = nodes_[static_cast<size_t>(topIdx_)];
            top.a = 2.0 * sourceValue - top.b;
        }

        // Phase 3: Downward wave propagation through adaptors (pre-order)
        for (const auto& op : adaptorDownOps_) {
            const ExecNode& n = nodes_[static_cast<size_t>(op.self)];
            ExecNode& c0 = nodes_[static_cast<size_t>(op.child0)];
            ExecNode& c1 = nodes_[static_cast<size_t>(op.child1)];
            if (op.isParallel) {
                const double bDiff = c1.b - c0.b;
                const double b2 = n.b - c1.b + n.a;
                c0.a = b2 + bDiff;
                c1.a = b2;
            } else {
                const double sum = -n.b + n.a;
                c0.a = c0.b - op.gamma * sum;
                c1.a = -(n.a + c0.a);
            }
        }

        // Phase 4: State updates
        for (const auto& op : reactiveOps_) {
            ExecNode& n = nodes_[static_cast<size_t>(op.idx)];
            n.state = n.a;
        }

        return (outIdx_ >= 0) ? elementVoltage(outIdx_) : 0.0;
    }

    /// Voltage across an element: v = (a + b) / 2
    double elementVoltage(int idx) const noexcept {
        const ExecNode& n = nodes_[static_cast<size_t>(idx)];
        return n.polarity * (n.a + n.b) * 0.5;
    }

    /// Voltage across an element by name.
    double elementVoltage(const std::string& name) const {
        const auto it = leafMap_.find(name);
        if (it == leafMap_.end())
            throw std::runtime_error("element not found: " + name);
        return elementVoltage(it->second);
    }

    /// Current through an element: i = (a - b) / (2 * R)
    double elementCurrent(const std::string& name) const {
        const auto it = leafMap_.find(name);
        if (it == leafMap_.end())
            throw std::runtime_error("element not found: " + name);
        const ExecNode& n = nodes_[static_cast<size_t>(it->second)];
        return (n.a - n.b) * (0.5 * n.G);
    }

    /// Dynamically update a leaf resistor value and propagate through the tree
    /// in O(depth) = O(log N) operations without rebuilding the tree.
    bool setResistance(const std::string& name, double ohms) noexcept {
        auto it = leafMap_.find(name);
        if (it == leafMap_.end() || !(ohms > 0.0))
            return false;
        const int idx = it->second;
        ExecNode& leaf = nodes_[static_cast<size_t>(idx)];
        if (leaf.value == ohms)
            return true;
        leaf.value = ohms;
        leaf.R = (ohms > 1e-12) ? ohms : 1e-12;
        leaf.G = 1.0 / leaf.R;

        // Propagate up along parent chain
        for (int p = leaf.parent; p >= 0; p = nodes_[static_cast<size_t>(p)].parent) {
            ExecNode& n = nodes_[static_cast<size_t>(p)];
            if (n.kind == NodeKind::Series) {
                const auto& c0 = nodes_[static_cast<size_t>(n.child0)];
                const auto& c1 = nodes_[static_cast<size_t>(n.child1)];
                n.R = c0.R + c1.R;
                n.G = 1.0 / n.R;
                n.gamma = c0.R / n.R;
            } else if (n.kind == NodeKind::Parallel) {
                const auto& c0 = nodes_[static_cast<size_t>(n.child0)];
                const auto& c1 = nodes_[static_cast<size_t>(n.child1)];
                n.G = c0.G + c1.G;
                n.R = 1.0 / n.G;
                n.gamma = c0.G / n.G;
            }
            for (auto& op : adaptorUpOps_) {
                if (op.self == p) {
                    op.gamma = n.gamma;
                    break;
                }
            }
            for (auto& op : adaptorDownOps_) {
                if (op.self == p) {
                    op.gamma = n.gamma;
                    break;
                }
            }
        }
        return true;
    }

    bool isPureTree() const noexcept { return pureTree_; }
    double sampleRate() const noexcept { return fs_; }
    size_t nodeCount() const noexcept { return nodes_.size(); }
    size_t stateCount() const noexcept { return reactiveNodes_.size(); }
    const Tree& tree() const noexcept { return tree_; }

private:
    void compileFromEdges(const std::vector<NetEdge>& edges, const std::map<std::string, double>& values) {
        tree_ = reduceToTree(edges);
        if (tree_.root < 0 || tree_.nodes.empty())
            throw std::runtime_error("failed to reduce circuit to tree");

        const auto& rootNode = tree_.nodes[static_cast<size_t>(tree_.root)];
        for (const auto& n : tree_.nodes) {
            if (n.kind == NodeKind::Rigid) {
                pureTree_ = false;
                throw std::runtime_error("circuit is not strictly series-parallel reducible (contains rigid component)");
            }
        }
        pureTree_ = true;

        nodes_.resize(tree_.nodes.size());
        for (size_t i = 0; i < tree_.nodes.size(); ++i) {
            const auto& tn = tree_.nodes[i];
            ExecNode& en = nodes_[i];
            en.kind = tn.kind;
            en.leaf = tn.leaf;
            en.name = tn.name;
            if (tn.children.size() >= 1)
                en.child0 = tn.children[0];
            if (tn.children.size() >= 2)
                en.child1 = tn.children[1];

            if (en.kind == NodeKind::Leaf) {
                leafMap_[en.name] = static_cast<int>(i);
                const auto valIt = values.find(en.name);
                if (valIt != values.end())
                    en.value = valIt->second;
            }
        }

        // Set parent links
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].child0 >= 0)
                nodes_[static_cast<size_t>(nodes_[i].child0)].parent = static_cast<int>(i);
            if (nodes_[i].child1 >= 0)
                nodes_[static_cast<size_t>(nodes_[i].child1)].parent = static_cast<int>(i);
        }

        // Compute element polarity: each series adaptor flips sign (v1 + v2 = -v3)
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].kind == NodeKind::Leaf) {
                int seriesCount = 0;
                for (int p = nodes_[i].parent; p >= 0; p = nodes_[static_cast<size_t>(p)].parent) {
                    if (nodes_[static_cast<size_t>(p)].kind == NodeKind::Series) {
                        ++seriesCount;
                    }
                }
                double pol = (seriesCount % 2 == 1) ? -1.0 : 1.0;
                for (const auto& e : edges) {
                    if (e.name == nodes_[i].name) {
                        if (e.a == "0" || e.a == kGroundName)
                            pol = -pol;
                        break;
                    }
                }
                nodes_[i].polarity = pol;
            }
        }

        // The top adaptor is root's child
        topIdx_ = (rootNode.children.empty()) ? -1 : rootNode.children[0];

        // Build traversal schedules
        upOrder_.clear();
        if (topIdx_ >= 0)
            buildPostOrder(topIdx_);

        downOrder_ = upOrder_;
        std::reverse(downOrder_.begin(), downOrder_.end());

        // Find reactive state elements (C and L)
        reactiveNodes_.clear();
        for (int idx : upOrder_) {
            const auto& n = nodes_[static_cast<size_t>(idx)];
            if (n.kind == NodeKind::Leaf && (n.leaf == LeafKind::Capacitor || n.leaf == LeafKind::Inductor))
                reactiveNodes_.push_back(idx);
        }

        // Calculate port impedances & reflection coefficients
        calcImpedances();

        // Compile optimized flat ops
        adaptorUpOps_.clear();
        for (int idx : upOrder_) {
            const auto& n = nodes_[static_cast<size_t>(idx)];
            if (n.kind == NodeKind::Series || n.kind == NodeKind::Parallel) {
                AdaptorOp op;
                op.isParallel = (n.kind == NodeKind::Parallel);
                op.self = idx;
                op.child0 = n.child0;
                op.child1 = n.child1;
                op.gamma = n.gamma;
                adaptorUpOps_.push_back(op);
            }
        }
        adaptorDownOps_ = adaptorUpOps_;
        std::reverse(adaptorDownOps_.begin(), adaptorDownOps_.end());

        reactiveOps_.clear();
        for (int idx : reactiveNodes_) {
            const auto& n = nodes_[static_cast<size_t>(idx)];
            ReactiveOp rop;
            rop.idx = idx;
            rop.isInductor = (n.leaf == LeafKind::Inductor);
            reactiveOps_.push_back(rop);
        }

        // Output probe index
        if (!outElementName_.empty()) {
            const auto it = leafMap_.find(outElementName_);
            if (it != leafMap_.end())
                outIdx_ = it->second;
        } else if (!reactiveNodes_.empty()) {
            outIdx_ = reactiveNodes_.front();
        }
    }

    void buildPostOrder(int idx) {
        const ExecNode& n = nodes_[static_cast<size_t>(idx)];
        if (n.child0 >= 0)
            buildPostOrder(n.child0);
        if (n.child1 >= 0)
            buildPostOrder(n.child1);
        upOrder_.push_back(idx);
    }

    void calcImpedances() {
        for (int i : upOrder_) {
            ExecNode& n = nodes_[static_cast<size_t>(i)];
            if (n.kind == NodeKind::Leaf) {
                if (n.leaf == LeafKind::Resistor) {
                    n.R = (n.value > 1e-12) ? n.value : 1e-12;
                    n.G = 1.0 / n.R;
                } else if (n.leaf == LeafKind::Capacitor) {
                    // Z_C = T / (2 * C)
                    const double cVal = (n.value > 1e-15) ? n.value : 1e-15;
                    n.R = T_ / (2.0 * cVal);
                    n.G = 1.0 / n.R;
                } else if (n.leaf == LeafKind::Inductor) {
                    // Z_L = 2 * L / T
                    const double lVal = (n.value > 1e-15) ? n.value : 1e-15;
                    n.R = (2.0 * lVal) / T_;
                    n.G = 1.0 / n.R;
                } else if (n.leaf == LeafKind::VoltageSource) {
                    n.R = 1e-9;
                    n.G = 1.0 / n.R;
                }
            } else if (n.kind == NodeKind::Series) {
                const auto& c0 = nodes_[static_cast<size_t>(n.child0)];
                const auto& c1 = nodes_[static_cast<size_t>(n.child1)];
                n.R = c0.R + c1.R;
                n.G = 1.0 / n.R;
                n.gamma = c0.R / n.R;
            } else if (n.kind == NodeKind::Parallel) {
                const auto& c0 = nodes_[static_cast<size_t>(n.child0)];
                const auto& c1 = nodes_[static_cast<size_t>(n.child1)];
                n.G = c0.G + c1.G;
                n.R = 1.0 / n.G;
                n.gamma = c0.G / n.G;
            }
        }
    }

    double fs_ = 48000.0;
    double T_ = 1.0 / 48000.0;
    bool pureTree_ = false;

    Tree tree_;
    std::vector<ExecNode> nodes_;
    int topIdx_ = -1;
    int outIdx_ = -1;
    std::string outElementName_;

    std::vector<int> upOrder_;
    std::vector<int> downOrder_;
    std::vector<int> reactiveNodes_;
    std::map<std::string, int> leafMap_;

    std::vector<AdaptorOp> adaptorUpOps_;
    std::vector<AdaptorOp> adaptorDownOps_;
    std::vector<ReactiveOp> reactiveOps_;
};

}  // namespace wdf
