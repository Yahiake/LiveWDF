#pragma once

// The compiled circuit's free-node table.
//
// Ground is not a node: the engine represents it as -1 in every port, and the
// node vector a `step()` returns is indexed by these names. The order is the
// compiled order (sorted representative names), which is also the engine's
// `nodeNames()` order -- the index a diagnostic reports and the index a caller
// uses to read a voltage are the same number.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace livewdf::compiler {

class NodeTable {
public:
    NodeTable() = default;

    explicit NodeTable(std::vector<std::string> names) : names_(std::move(names)) {}

    std::span<const std::string> names() const noexcept { return names_; }

    /// Column of `name`, or -1 when the name is not a free node. A net held by
    /// an ideal source is not free; callers that need its voltage ask the source,
    /// exactly as they do against `Engine::nodeIndex`.
    ///
    /// A linear scan, deliberately: a circuit has tens of nodes, not thousands,
    /// and this lookup must not allocate.
    int index(std::string_view name) const noexcept {
        for (int i = 0; i < static_cast<int>(names_.size()); ++i)
            if (names_[static_cast<std::size_t>(i)] == name)
                return i;
        return -1;
    }

    std::size_t size() const noexcept { return names_.size(); }

private:
    std::vector<std::string> names_;
};

}  // namespace livewdf::compiler
