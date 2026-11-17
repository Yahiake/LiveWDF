#pragma once

// The structural IR that a netlist's series/parallel reduction produces.
//
// Provenance: this mirrors `SPQR_Node` in wdf_compiler's
// `compiler/modules/netlist_converter.jai` (MIT, Chowdhury-DSP), read as a
// *specification* rather than copied. The four node kinds are the same four
// because the algorithm that makes them has exactly those four cases:
//
//   LEAF     one netlist component
//   SERIES   a degree-2 node was contracted: two branches share the current
//   PARALLEL two branches span the same two nodes: they share the voltage
//   RIGID    neither applies any more (a bridge, a multi-terminal device)
//
// The structural oracle is the prebuilt `netlist_converter_standalone` exe;
// `tests/test_spqr_tree.cpp` renders fixtures with both and compares.
//
// Why these types live apart from the reduction that builds them: the
// reduction runs once at compile time, while an executor (plan step E) walks
// this IR once per sample. That is the same compile/execute split that put
// `Network` in `src/circuit/` and the loops in `src/wdf/`.

#include <string>
#include <vector>

namespace wdf {

/// What a node of the reduced tree is. Everything except `Leaf` is a shape
/// the reduction contracted into.
enum class NodeKind { Leaf, Series, Parallel, Rigid };

/// The component behind a `Leaf`.
enum class LeafKind {
    None,
    VoltageSource,
    CurrentSource,
    Resistor,
    Capacitor,
    Inductor,
    Custom,
};

/// One edge of a `Rigid` node's internal graph, in that node's *local*
/// vertex numbering. `child` indexes the owning `Tree::nodes` and is `-1`
/// once rerooting has detached that branch (see `RigidEdge` in Reduce.h).
struct RigidEdge {
    int child = -1;
    int v1 = 0;
    int v2 = 0;
    /// The edge is the synthetic remnant of a contraction rather than an
    /// original component; it carries no admittance of its own.
    bool virt = false;
};

/// One node of the reduced tree; index into the owning `Tree::nodes`.
struct TreeNode {
    NodeKind kind = NodeKind::Leaf;
    LeafKind leaf = LeafKind::None;
    /// Netlist designator (`R1`) for a `Leaf`; empty for a composite.
    std::string name;

    /// Children, in the order the reduction attached them. Empty for a leaf.
    ///
    /// Sibling order is *not* semantic -- `Series` and `Parallel` are
    /// commutative -- but it is reproduced from the oracle in the tests, so
    /// treat it as part of the observable contract rather than shuffling it.
    std::vector<int> children;

    /// `Rigid` only: the edges of the un-reducible subgraph, in the same
    /// order as `children`. Empty otherwise.
    std::vector<RigidEdge> rigidEdges;

    /// `Rigid` only: local vertex -> global node id. Local vertices are
    /// numbered in first-use order, so locating the datum among them is the
    /// consumer's job. Empty otherwise.
    std::vector<int> rigidVertices;
};

/// A reduced netlist: the forest plus the index of the node to start at.
/// Every other node is reachable from `root`; a `Rigid` reduction that left
/// nothing to contract still yields a `Rigid` root rather than an empty tree.
struct Tree {
    std::vector<TreeNode> nodes;
    int root = -1;
    std::vector<std::string> nodeNames;
};

inline const char* kindName(NodeKind k) {
    switch (k) {
        case NodeKind::Series:   return "SERIES";
        case NodeKind::Parallel: return "PARALLEL";
        case NodeKind::Rigid:    return "RIGID";
        case NodeKind::Leaf:     break;
    }
    return "LEAF";
}

namespace detail {

/// Emit one subtree the way the oracle's `-print_wdf` dump does: `prefix`
/// then a branch glyph, then the child's label. A child's own prefix extends
/// `prefix` by `│   ` (it is not the last sibling) or four spaces (it is).
inline void renderSubtree(const std::vector<TreeNode>& nodes, int idx,
                          const std::string& prefix, bool last, std::string& out,
                          std::vector<bool>& visited) {
    if (idx < 0 || idx >= static_cast<int>(nodes.size()) || visited[static_cast<size_t>(idx)])
        return;
    visited[static_cast<size_t>(idx)] = true;

    const TreeNode& n = nodes[static_cast<size_t>(idx)];
    out += prefix;
    out += last ? "└── " : "├── ";
    out += n.kind == NodeKind::Leaf ? n.name : kindName(n.kind);
    out += '\n';

    const std::string childPrefix = prefix + (last ? "    " : "│   ");
    for (size_t i = 0; i < n.children.size(); ++i)
        renderSubtree(nodes, n.children[i], childPrefix,
                      i + 1 == n.children.size(), out, visited);
}

}  // namespace detail

/// Render `tree` exactly as the oracle does, so the two strings can be
/// compared directly. The root is printed bare (no prefix, no branch glyph);
/// everything else hangs off it.
inline std::string renderTree(const Tree& tree) {
    std::string out;
    if (tree.root < 0 || tree.nodes.empty())
        return out;

    const TreeNode& r = tree.nodes[static_cast<size_t>(tree.root)];
    out += r.kind == NodeKind::Leaf ? r.name : kindName(r.kind);
    out += '\n';

    std::vector<bool> visited(tree.nodes.size(), false);
    visited[static_cast<size_t>(tree.root)] = true;

    for (size_t i = 0; i < r.children.size(); ++i)
        detail::renderSubtree(tree.nodes, r.children[i], "",
                              i + 1 == r.children.size(), out, visited);
    return out;
}

}  // namespace wdf
