#pragma once

// The compiler's output: an immutable circuit plus everything that can be
// decided without knowing the sample rate.
//
// Two consequences worth stating, because they are the point of the type:
//
//   - There is no `Engine`, no `Network`, no `Z`, no DC solution and no mutable
//     state here. Preparing to run at a sample rate is the runtime's job.
//   - Parameter values in this object are the *compiled defaults*. The values a
//     control is currently at live in the runtime's engine, because changing one
//     re-derives the R-type transfer and that is execution state.
//
// The object is handed out as `shared_ptr<const CompiledCircuit>`, so a runtime
// can outlive the document it was compiled from without copying the netlist.

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/Diagnostic.h"
#include "compiler/NodeTable.h"
#include "compiler/ParameterTable.h"
#include "compiler/TopologyPlan.h"
#include "spice/Circuit.h"
#include "spice/Compiled.h"

namespace livewdf::compiler {

/// One nonlinear (root) device instance in the compiled circuit.
///
/// Linear elements are not listed: they are values in the netlist and ports in
/// the transfer, not devices with a law and a root. A future device registry can
/// grow this into the schema-plus-instance pair in TARGET-INTERFACES; Phase 1
/// keeps the instance data the solver actually addresses.
struct DeviceInstance {
    std::string type;  ///< stable token: "triode", "diode", "pentode", ...
    std::string name;  ///< component id, empty when the netlist did not name it
    RootKind kind = RootKind::Triode;
    int componentIndex = 0;  ///< index into the matching `Circuit` vector
    int rootPortBase = 0;    ///< first port in `TopologyPlan::rootLayout()`
    int rootPortCount = 0;
};

class CompiledCircuit {
public:
    CompiledCircuit(const CompiledCircuit&) = delete;
    CompiledCircuit& operator=(const CompiledCircuit&) = delete;

    const wdf::Circuit& netlist() const noexcept { return netlist_; }
    const wdf::Compiled& compiled() const noexcept { return compiled_; }
    const NodeTable& nodes() const noexcept { return nodes_; }
    const ParameterTable& parameters() const noexcept { return parameters_; }
    const TopologyPlan& topology() const noexcept { return topology_; }
    std::span<const DeviceInstance> devices() const noexcept { return devices_; }

    /// Size of the `step()` source-value vector: every ideal voltage source,
    /// then every ideal current source. This is the engine's layout, kept here
    /// so a caller never has to include an engine header to size a buffer.
    int sourceCount() const noexcept { return sourceCount_; }

    /// Stable content hash of the compile input, in lowercase hex. Two compiles
    /// of the same netlist produce the same revision; any structural or value
    /// change produces a different one. Used to tell a cached runtime whether it
    /// is stale, not as a cryptographic digest.
    std::string_view revision() const noexcept { return revision_; }

private:
    friend class Compiler;

    CompiledCircuit(wdf::Circuit netlist, wdf::Compiled compiled, NodeTable nodes,
                    ParameterTable parameters, TopologyPlan topology,
                    std::vector<DeviceInstance> devices, std::string revision)
        : netlist_(std::move(netlist)),
          compiled_(std::move(compiled)),
          nodes_(std::move(nodes)),
          parameters_(std::move(parameters)),
          topology_(std::move(topology)),
          devices_(std::move(devices)),
          revision_(std::move(revision)),
          sourceCount_(static_cast<int>(netlist_.src.size() + netlist_.isrc.size())) {}

    wdf::Circuit netlist_;
    wdf::Compiled compiled_;
    NodeTable nodes_;
    ParameterTable parameters_;
    TopologyPlan topology_;
    std::vector<DeviceInstance> devices_;
    std::string revision_;
    int sourceCount_ = 0;
};

struct CompileResult {
    std::shared_ptr<const CompiledCircuit> circuit;
    std::vector<Diagnostic> diagnostics;

    bool ok() const noexcept { return circuit != nullptr; }
};

}  // namespace livewdf::compiler
