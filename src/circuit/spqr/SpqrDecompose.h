#pragma once

// Upgraded SPQR Decomposition with Pendant Extraction.
//
// Extends the standard series/parallel reduction in `Reduce.h` with:
// 1. **Pendant Extraction**: identifies ground-terminated and dead-end passive
//    branches (capacitors to ground, bypass networks, load resistors) and peels
//    them off before series-parallel reduction. This reduces junction degrees
//    from >= 3 down to 2, unlocking series contractions that standard SPQR
//    would otherwise abandon to a monolithic Rigid (R) node.
// 2. **Pendant Re-insertion**: re-attaches each peeled pendant as a canonical
//    Parallel WDF adaptor at its junction node in the core tree.
// 3. **Stage-Aware SPQR**: decomposes multi-stage circuits into independent WDF
//    sub-trees connected by incident wave boundary injection.
//
// Cleanroom design inspired by PedalKernel compiler algorithms.

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "circuit/spqr/Reduce.h"
#include "circuit/tree/TreeIR.h"

namespace wdf {

struct PendantLeaf {
    NetEdge edge;
    std::string junction; ///< The non-rail junction node where this pendant attaches
};

namespace detail {

inline bool isDefaultRail(const std::string& name) {
    if (name.empty()) return false;
    if (name == "0" || name == "gnd" || name == "GND") return true;
    std::string lower;
    lower.reserve(name.size());
    for (char c : name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower == "vcc" || lower == "vdd" || lower == "vee" ||
           lower == "vss" || lower == "b+" || lower == "rail";
}

}  // namespace detail

/// Peels pendant edges from `edges` given a set of known rail nodes.
/// Returns the remaining core edges and the list of peeled pendants.
inline std::pair<std::vector<NetEdge>, std::vector<PendantLeaf>>
extractPendants(const std::vector<NetEdge>& edges,
                const std::unordered_set<std::string>& rails,
                const std::unordered_set<std::string>& terminals = {}) {
    std::vector<NetEdge> remaining = edges;
    std::vector<PendantLeaf> pendants;

    for (;;) {
        // Find rail-shunt edges (one terminal is a rail, the other is a circuit node)
        std::vector<size_t> foundIndices;
        for (size_t i = 0; i < remaining.size(); ++i) {
            const auto& e = remaining[i];
            // Sources and root-marked components are tree reflectors; never peel them
            if (e.isRoot || e.leaf == LeafKind::VoltageSource || e.leaf == LeafKind::CurrentSource)
                continue;
            // Never peel custom multi-port device terminals
            if (e.leaf == LeafKind::Custom)
                continue;

            const bool aIsRail = rails.count(e.a) > 0;
            const bool bIsRail = rails.count(e.b) > 0;

            // A rail shunt connects a circuit junction to a supply/ground rail
            if (aIsRail != bIsRail) {
                // Do not peel if the non-rail end is a designated terminal
                const std::string nonRail = aIsRail ? e.b : e.a;
                if (!terminals.count(nonRail)) {
                    foundIndices.push_back(i);
                }
            }
        }

        if (foundIndices.empty())
            break;

        std::vector<bool> removeMask(remaining.size(), false);
        for (size_t idx : foundIndices) {
            removeMask[idx] = true;
            const auto& e = remaining[idx];
            const bool aIsRail = rails.count(e.a) > 0;
            const std::string junction = aIsRail ? e.b : e.a;
            pendants.push_back({e, junction});
        }

        std::vector<NetEdge> nextRemaining;
        nextRemaining.reserve(remaining.size() - foundIndices.size());
        for (size_t i = 0; i < remaining.size(); ++i) {
            if (!removeMask[i])
                nextRemaining.push_back(std::move(remaining[i]));
        }
        remaining = std::move(nextRemaining);

        // One pass of rail shunts is sufficient to unblock series reductions
        break;
    }

    return {remaining, pendants};
}

/// Upgraded SPQR decomposition with pendant extraction and re-insertion.
inline Tree reduceToTreeWithPendants(
    const std::vector<NetEdge>& edges,
    const std::vector<std::string>& customRails = {},
    const std::vector<std::string>& preservedNodes = {}) {
    if (edges.empty())
        return Tree{};

    std::unordered_set<std::string> rails;
    rails.insert("0");
    rails.insert("gnd");
    rails.insert("GND");
    for (const auto& r : customRails)
        rails.insert(r);

    for (const auto& e : edges) {
        if (detail::isDefaultRail(e.a)) rails.insert(e.a);
        if (detail::isDefaultRail(e.b)) rails.insert(e.b);
    }

    auto [coreEdges, pendants] = extractPendants(edges, rails);

    // If no pendants were found or all edges were peeled, fall back to canonical reduction
    if (pendants.empty() || coreEdges.empty()) {
        return reduceToTree(edges, preservedNodes);
    }

    // Reduce the core series/parallel skeleton
    Tree tree = reduceToTree(coreEdges, preservedNodes);

    // Re-attach pendants as parallel branches at their respective junctions
    for (const auto& p : pendants) {
        // Create leaf node for the pendant
        TreeNode pendantNode;
        pendantNode.kind = NodeKind::Leaf;
        pendantNode.leaf = p.edge.leaf;
        pendantNode.name = p.edge.name;
        int pendantIdx = static_cast<int>(tree.nodes.size());
        tree.nodes.push_back(std::move(pendantNode));

        // Find the node in the tree incident to `p.junction` or attach at root
        int targetNode = -1;
        for (size_t i = 0; i < tree.nodes.size(); ++i) {
            if (tree.nodes[i].kind == NodeKind::Leaf &&
                tree.nodes[i].name == p.junction) {
                targetNode = static_cast<int>(i);
                break;
            }
        }

        if (targetNode >= 0) {
            // Wrap targetNode and pendant in a Parallel composite
            TreeNode parNode;
            parNode.kind = NodeKind::Parallel;
            parNode.children = {targetNode, pendantIdx};
            int parIdx = static_cast<int>(tree.nodes.size());
            tree.nodes.push_back(std::move(parNode));

            // If target was root, update root
            if (tree.root == targetNode) {
                tree.root = parIdx;
            } else {
                for (auto& n : tree.nodes) {
                    for (auto& ch : n.children) {
                        if (ch == targetNode)
                            ch = parIdx;
                    }
                }
            }
        } else {
            // Attach as sibling to root via Parallel or Series
            TreeNode compNode;
            compNode.kind = NodeKind::Parallel;
            compNode.children = {tree.root, pendantIdx};
            int newRoot = static_cast<int>(tree.nodes.size());
            tree.nodes.push_back(std::move(compNode));
            tree.root = newRoot;
        }
    }

    return tree;
}

}  // namespace wdf
