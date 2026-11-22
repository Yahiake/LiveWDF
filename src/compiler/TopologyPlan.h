#pragma once

// Sample-rate-independent topology: root-port layout plus candidate stages.
//
// This is deliberately weaker than "the solve partition". The real Newton
// partition depends on the R-type impedance `Z`, which depends on the sample
// rate and the current component values; it belongs to the runtime. What lives
// here is everything that is true of the compiled netlist alone:
//
//   - the root-port layout (the single source of truth for port order);
//   - the directed signal-flow stages, as *candidate* co-solved groups.
//
// A caller can use the candidates for diagnostics, display, or a future
// partition heuristic, but must not assume they are the blocks the dense engine
// will actually solve with.

#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "circuit/RootLayout.h"
#include "circuit/SignalFlow.h"
#include "spice/Circuit.h"
#include "spice/Compiled.h"

namespace livewdf::compiler {

using RootLayout = wdf::RootLayout;
using RootKind = wdf::RootKind;

struct RootGroupSpec {
    RootKind kind = RootKind::Triode;
    /// Root-port indices owned by the devices in this group, in device order.
    std::vector<int> rootPorts;
    /// Indices into `RootLayout::devices` / the compiled root-device order.
    std::vector<int> deviceIndices;
    /// True when this device kind has a scalar closed form *and* the group is a
    /// singleton. This is a capability flag, not a promise: the runtime still
    /// checks `Z` before choosing the closed form.
    bool scalarClosedForm = false;
};

class TopologyPlan {
public:
    TopologyPlan() = default;

    TopologyPlan(RootLayout layout, std::vector<RootGroupSpec> candidateGroups)
        : layout_(std::move(layout)), candidates_(std::move(candidateGroups)) {}

    const RootLayout& rootLayout() const noexcept { return layout_; }

    std::span<const RootGroupSpec> candidateGroups() const noexcept { return candidates_; }

    bool hasCandidateStages() const noexcept { return !candidates_.empty(); }

private:
    RootLayout layout_;
    std::vector<RootGroupSpec> candidates_;
};

namespace detail {

/// Device kinds with a scalar closed form in the dense solver. A pentode and a
/// BJT have two live ports, an op-amp is solved as a three-port VCVS, and a
/// multi-device stage is never scalar regardless of its devices.
inline bool hasScalarClosedForm(RootKind kind) noexcept {
    switch (kind) {
    case RootKind::Triode:
    case RootKind::Diode:
    case RootKind::ControlledRes:
    case RootKind::Jfet:
    case RootKind::Mosfet:
        return true;
    case RootKind::Pentode:
    case RootKind::Bjt:
    case RootKind::OpAmp:
        return false;
    }
    return false;
}

}  // namespace detail

/// Build the candidate topology from the compiled netlist and its root layout.
///
/// The signal-flow analysis emits active elements in root-device order (its
/// contract, kept in `analyzeSignalFlow`), so a `FlowStage::devices` entry is
/// also an index into `RootLayout::devices`.
inline TopologyPlan buildTopologyPlan(const wdf::Circuit& circuit,
                                      const RootLayout& layout) {
    const wdf::FlowGraph flow = wdf::analyzeSignalFlow(circuit);
    std::vector<RootGroupSpec> groups;
    groups.reserve(flow.stages.size());

    const int deviceCount = static_cast<int>(layout.devices.size());
    for (const auto& stage : flow.stages) {
        RootGroupSpec group;
        bool first = true;
        bool allSameKind = true;
        for (int deviceIndex : stage.devices) {
            if (deviceIndex < 0 || deviceIndex >= deviceCount)
                continue;
            const auto& device = layout.devices[static_cast<std::size_t>(deviceIndex)];
            if (first) {
                group.kind = device.kind;
                first = false;
            } else if (device.kind != group.kind) {
                allSameKind = false;
            }

            group.deviceIndices.push_back(deviceIndex);
            for (int q = 0; q < device.numPorts; ++q)
                group.rootPorts.push_back(device.portBase + q);
        }

        if (group.deviceIndices.empty())
            continue;

        group.scalarClosedForm = allSameKind && group.deviceIndices.size() == 1 &&
                                 detail::hasScalarClosedForm(group.kind);

        groups.push_back(std::move(group));
    }

    return TopologyPlan(layout, std::move(groups));
}

}  // namespace livewdf::compiler
