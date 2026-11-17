#pragma once

// Netlist -> `Tree`: the series/parallel reduction that turns a component
// graph into a scattering tree.
//
// Provenance: the algorithm is a direct translation of `reduce_to_spqr_tree`,
// `find_series_node`, `contract_series`, `find_parallel_pair`,
// `contract_parallel`, `build_rigid_component` and `reroot` in wdf_compiler's
// `compiler/modules/netlist_converter.jai` (MIT, Chowdhury-DSP). It is
// translated, not copied: Jai's dynamic arrays and `it_index` loops became
// `std::vector` and index loops, and the parts we do not need (op-amp
// pinning, the `.meta root:rigid` override, the WDF-string emitter) were
// left behind. `tests/test_spqr_tree.cpp` checks the rendered result against
// the prebuilt exe, which is the oracle.
//
// Why this shape:
//
//  * The input is a list of edges by *node name*, not a `Circuit`. The
//    reduction is a pure graph algorithm; deciding which of our `Circuit`'s
//    element arrays to flatten, and in what order, is the caller's job. That
//    also lets the tests feed it the oracle's own fixtures unchanged.
//  * Reduction order depends on edge order (the parallel search scans edges
//    in insertion order, the series search scans nodes in id order), so the
//    order of `edges` is part of the contract. The tests use netlist order,
//    which is the order the oracle uses.
//
// Complexity: each contraction deactivates one edge or eliminates one node,
// so the loop runs O(E) times; the searches inside are O(E) and O(N). Fine
// for a compile-time pass -- the whole point is that none of this runs per
// sample.

#include <string>
#include <utility>
#include <vector>

#include "circuit/tree/TreeIR.h"

namespace wdf {

/// One netlist component as the reduction sees it: two terminals, named.
/// The terminals may be written in either order; the graph is undirected.
struct NetEdge {
    std::string a, b;
    LeafKind leaf = LeafKind::None;
    /// Netlist designator, kept verbatim so it comes back out in the render.
    std::string name;
    /// Marked `; root` in the netlist. The tree is rerooted on the first such
    /// component, which is what makes a source the tree's reflector.
    bool isRoot = false;
};

namespace detail {

/// Mutable state of one reduction. Kept in a struct because the phases share
/// it and the free functions below read far better than nested lambdas.
struct ReduceState {
    /// An undirected component edge. `spqr` is the tree node that currently
    /// stands for everything collapsed into this edge.
    struct Redge {
        int n1 = 0;
        int n2 = 0;
        int spqr = -1;
        bool active = true;
        /// Survived a contraction: it stands for two or more components and
        /// carries no admittance of its own.
        bool virt = false;
    };

    std::vector<TreeNode> nodes;
    /// Parent of each node, parallel to `nodes`. Only `reroot` reads it.
    std::vector<int> parent;
    std::vector<Redge> edges;
    /// node id -> incident edge ids, in insertion order.
    std::vector<std::vector<int>> adj;
    /// Nodes that cannot be eliminated by series contraction (e.g. device terminals).
    std::vector<bool> preserved;
};

/// Swap-with-last erase, matching Jai's `array_unordered_remove_by_value`.
/// Order-sensitive on purpose: the oracle's adjacency lists evolve this way,
/// and adjacency order decides which branch becomes `children[0]`.
inline void eraseOnce(std::vector<int>& v, int value) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != value)
            continue;
        v[i] = v.back();
        v.pop_back();
        return;
    }
}

/// Append a composite node and return its index.
inline int addComposite(ReduceState& s, NodeKind kind, std::vector<int> children) {
    TreeNode n;
    n.kind = kind;
    n.children = std::move(children);
    s.nodes.push_back(std::move(n));
    s.parent.push_back(-1);
    return static_cast<int>(s.nodes.size()) - 1;
}

/// First pair of active edges spanning the same two nodes, in edge order.
inline void findParallelPair(const ReduceState& s, int& e1, int& e2) {
    e1 = e2 = -1;
    for (size_t i = 0; i < s.edges.size(); ++i) {
        if (!s.edges[i].active)
            continue;
        for (size_t j = i + 1; j < s.edges.size(); ++j) {
            if (!s.edges[j].active)
                continue;
            // Only protect Custom device ports if preserved nodes were explicitly specified
            if (!s.preserved.empty()) {
                const auto& leafI = s.nodes[static_cast<size_t>(s.edges[i].spqr)].leaf;
                const auto& leafJ = s.nodes[static_cast<size_t>(s.edges[j].spqr)].leaf;
                if (leafI == LeafKind::Custom || leafJ == LeafKind::Custom)
                    continue;
            }
            const auto& a = s.edges[i];
            const auto& b = s.edges[j];
            const bool same = (a.n1 == b.n1 && a.n2 == b.n2) ||
                              (a.n1 == b.n2 && a.n2 == b.n1);
            if (same) {
                e1 = static_cast<int>(i);
                e2 = static_cast<int>(j);
                return;
            }
        }
    }
}

/// Collapse the parallel pair into one edge carrying a `Parallel` node.
inline void contractParallel(ReduceState& s, int e1, int e2) {
    const int composite =
        addComposite(s, NodeKind::Parallel, {s.edges[e1].spqr, s.edges[e2].spqr});
    s.parent[s.edges[e1].spqr] = composite;
    s.parent[s.edges[e2].spqr] = composite;

    s.edges[e1].spqr = composite;
    s.edges[e1].virt = true;
    s.edges[e2].active = false;
    // Adjacency is untouched: both nodes keep their degree, and the survivor
    // still spans exactly the same pair.
}

/// First node with exactly two active edges, in node id order.
///
/// A node with one active edge is not a candidate -- that is what lets the
/// search terminate with a single edge still standing. A node with three or
/// more is skipped outright rather than contracted.
inline void findSeriesNode(const ReduceState& s, int& node, int& e1, int& e2) {
    node = e1 = e2 = -1;
    for (size_t nd = 0; nd < s.adj.size(); ++nd) {
        if (nd < s.preserved.size() && s.preserved[nd])
            continue;
        int act[2] = {-1, -1};
        int count = 0;
        bool overflow = false;
        for (const int eid : s.adj[nd]) {
            if (!s.edges[eid].active)
                continue;
            if (count == 2) {
                overflow = true;
                break;
            }
            act[count++] = eid;
        }
        if (overflow)
            continue;
        if (count == 2) {
            node = static_cast<int>(nd);
            e1 = act[0];
            e2 = act[1];
            return;
        }
    }
}

/// Collapse a degree-2 node: the two branches meet in series through it, so
/// they can be joined into one edge and the node can go.
inline void contractSeries(ReduceState& s, int node, int e1, int e2) {
    const int other1 = (s.edges[e1].n1 == node) ? s.edges[e1].n2 : s.edges[e1].n1;
    const int other2 = (s.edges[e2].n1 == node) ? s.edges[e2].n2 : s.edges[e2].n1;

    const int composite = addComposite(
        s, NodeKind::Series, {s.edges[e1].spqr, s.edges[e2].spqr});
    s.parent[s.edges[e1].spqr] = composite;
    s.parent[s.edges[e2].spqr] = composite;

    // e1 becomes the synthetic edge; e2 is consumed by it.
    s.edges[e1].n1 = other1;
    s.edges[e1].n2 = other2;
    s.edges[e1].spqr = composite;
    s.edges[e1].virt = true;

    eraseOnce(s.adj[node], e1);
    s.adj[other2].push_back(e1);

    s.edges[e2].active = false;
    eraseOnce(s.adj[node], e2);
    // e2 may also sit in adj[other2]; leave it. A stale entry costs one
    // inactive skip and keeps adjacency order identical to the oracle's.
}

/// The graph stopped reducing: wrap what is left in a `Rigid` node.
///
/// This is the branch a bridge or a multi-terminal device ends up on. Its
/// local vertices are numbered in first-use order and recorded in
/// `rigidVertices`; finding the datum among them is the R-type assembly's
/// (plan D3) job, since only the caller knows which node name is ground.
inline int buildRigidComponent(ReduceState& s) {
    std::vector<int> vertexMap;  // local -> global

    const int rigid = addComposite(s, NodeKind::Rigid, {});

    auto localVertex = [&vertexMap](int global) {
        for (size_t i = 0; i < vertexMap.size(); ++i)
            if (vertexMap[i] == global)
                return static_cast<int>(i);
        vertexMap.push_back(global);
        return static_cast<int>(vertexMap.size()) - 1;
    };

    for (const auto& e : s.edges) {
        if (!e.active)
            continue;
        const int v1 = localVertex(e.n1);
        const int v2 = localVertex(e.n2);
        s.nodes[rigid].children.push_back(e.spqr);
        s.parent[e.spqr] = rigid;
        s.nodes[rigid].rigidEdges.push_back({e.spqr, v1, v2, e.virt});
    }
    s.nodes[rigid].rigidVertices = std::move(vertexMap);
    return rigid;
}

/// Turn the reduction's chosen root into the component marked `; root`, by
/// walking up to it and inverting every edge along the way.
///
/// Returns `root` unchanged when nothing is marked, which is what the oracle
/// does. The inverted parent pointers are not kept afterwards (the `Tree`
/// walks children), so a `newRoot` whose parent still points sideways is
/// harmless; `reroot` clears it anyway.
inline int reroot(ReduceState& s, int root, const std::vector<bool>& marked) {
    int newRoot = -1;
    for (size_t i = 0; i < marked.size(); ++i) {
        if (marked[i]) {
            newRoot = static_cast<int>(i);
            break;
        }
    }
    if (newRoot < 0)
        return root;

    std::vector<int> path;  // from the target up to the current root
    for (int cur = newRoot; cur != -1; cur = s.parent[cur])
        path.push_back(cur);

    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const int child = path[i];
        const int par = path[i + 1];

        eraseOnce(s.nodes[par].children, child);
        // Prepend, not append. The reference does the reverse of what you'd
        // expect from its own `array_add`, and the prebuilt exe agrees with
        // the source on this: appending mirrors the sibling order of every
        // fixture. `Series`/`Parallel` are commutative so the physics does
        // not care, but the rendered tree does.
        s.nodes[child].children.insert(s.nodes[child].children.begin(), par);
        s.parent[par] = child;
        if (i + 2 < path.size())
            s.parent[child] = path[i + 2];

        // The branch that moved off a rigid node has to stop being one of its
        // edges, or the R-type assembly would read a vertex that is no longer
        // inside it.
        if (s.nodes[par].kind == NodeKind::Rigid) {
            for (auto& re : s.nodes[par].rigidEdges) {
                if (re.child == child) {
                    re.child = -1;
                    break;
                }
            }
        }
    }
    s.parent[newRoot] = -1;
    return newRoot;
}

}  // namespace detail

/// Reduce `edges` to a scattering tree.
///
/// The contract on `edges`: at least one entry, and ordered -- see the file
/// comment. Components that could not be reduced stay reachable from the root
/// through a single `Rigid` node rather than being dropped.
inline Tree reduceToTree(const std::vector<NetEdge>& edges,
                         const std::vector<std::string>& preservedNodes = {}) {
    detail::ReduceState s;
    std::vector<bool> marked;  // node id -> `; root` (leaves only, see below)
    std::vector<std::string> nodeNames;

    auto nodeId = [&nodeNames](const std::string& nm) {
        for (size_t i = 0; i < nodeNames.size(); ++i)
            if (nodeNames[i] == nm)
                return static_cast<int>(i);
        nodeNames.push_back(nm);
        return static_cast<int>(nodeNames.size()) - 1;
    };

    for (const auto& e : edges) {
        // Second terminal first, matching the oracle's `parse_netlist`: node
        // ids decide which degree-2 node the series search finds first, so
        // registering them the other way round can change the tree.
        const int n2 = nodeId(e.b);
        const int n1 = nodeId(e.a);
        if (s.adj.size() < nodeNames.size())
            s.adj.resize(nodeNames.size());

        const int leaf = static_cast<int>(s.nodes.size());
        TreeNode n;
        n.kind = NodeKind::Leaf;
        n.leaf = e.leaf;
        n.name = e.name;
        s.nodes.push_back(std::move(n));
        s.parent.push_back(-1);
        marked.push_back(e.isRoot);

        const int eid = static_cast<int>(s.edges.size());
        s.edges.push_back({n1, n2, leaf, true, false});
        s.adj[n1].push_back(eid);
        s.adj[n2].push_back(eid);
    }

    s.preserved.assign(nodeNames.size(), false);
    for (const auto& p : preservedNodes) {
        for (size_t i = 0; i < nodeNames.size(); ++i) {
            if (nodeNames[i] == p) {
                s.preserved[i] = true;
                break;
            }
        }
    }

    for (;;) {
        int e1, e2;
        detail::findParallelPair(s, e1, e2);
        if (e1 >= 0) {
            detail::contractParallel(s, e1, e2);
            continue;
        }
        int node, s1, s2;
        detail::findSeriesNode(s, node, s1, s2);
        if (node >= 0) {
            detail::contractSeries(s, node, s1, s2);
            continue;
        }
        break;
    }

    // The surviving edge already stands for the whole circuit; its node is a
    // two-branch junction that has to be flattened into a real root.
    int remaining = -1;
    int count = 0;
    for (size_t i = 0; i < s.edges.size(); ++i) {
        if (s.edges[i].active) {
            remaining = static_cast<int>(i);
            ++count;
        }
    }

    int root;
    if (count != 1) {
        root = detail::buildRigidComponent(s);
    } else {
        const int junction = s.edges[remaining].spqr;
        const auto& jn = s.nodes[static_cast<size_t>(junction)];
        if (jn.children.size() < 2) {
            // A single component that never contracted: it is already the
            // whole tree, and there is no junction to flatten away.
            root = junction;
        } else {
            const int elementRoot = jn.children[1];
            const int firstChild = jn.children[0];

            s.nodes[static_cast<size_t>(elementRoot)].children.push_back(firstChild);
            s.parent[static_cast<size_t>(elementRoot)] = -1;
            s.parent[static_cast<size_t>(firstChild)] = elementRoot;

            // The junction is the node the last contraction made, so it is
            // unreachable from `elementRoot` and safe to drop.
            if (junction + 1 == static_cast<int>(s.nodes.size())) {
                s.nodes.pop_back();
                s.parent.pop_back();
            }
            root = elementRoot;
        }
    }

    root = detail::reroot(s, root, marked);

    Tree tree;
    tree.nodes = std::move(s.nodes);
    tree.root = root;
    tree.nodeNames = std::move(nodeNames);
    return tree;
}

}  // namespace wdf
