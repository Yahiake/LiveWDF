#pragma once

// The runtime contract: one sample in, one node-voltage view out.
//
// This is the seam the plugin, the editor and the validation tools share. It is
// deliberately narrow:
//
//   - `step()` advances exactly one sample and returns a view of the engine's
//     node vector. It performs no allocation and copies nothing; the view is
//     valid until the next `step()` or `reset()`.
//   - parameters are handles resolved at load/prepare, never strings in the
//     audio loop;
//   - diagnostics are snapshots for probes and validation, not a second audio
//     path.
//
// `makeDenseRuntime()` is the only factory in Phase 1. It wraps the existing
// dense R-type engine one-for-one; the engine remains the behavioral reference.

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "compiler/CompiledCircuit.h"
#include "compiler/Diagnostic.h"
#include "compiler/NodeTable.h"
#include "compiler/ParameterTable.h"

namespace livewdf::runtime {

struct RuntimeConfig {
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    /// Experimental runtime active-set partition. Off by default: the static
    /// block solve is the reference behavior.
    bool enableRuntimeBlockSolver = false;
};

struct OperatingPoint {
    std::vector<double> nodeVolts;     ///< indexed like `NodeTable`
    std::vector<double> rootCurrents;  ///< indexed like `RootLayout` ports
    bool converged = false;
};

/// Per-sample and cumulative counters from the dense engine.
///
/// The DC pass does not currently report an iteration count or residual, so
/// this snapshot has none: reporting a zero would claim a measurement the
/// engine does not make.
struct RuntimeDiagnostics {
    bool lastConverged = false;
    int lastIterations = 0;
    int lastDeviceEvaluations = 0;
    double lastResidual = 0.0;
    bool lastKeptBest = false;
    bool lastStateRejected = false;
    long long stateRejections = 0;

    /// The static block solve fell back to the joint Newton on the last sample.
    bool lastBlockFallback = false;
    long long blockFallbacks = 0;

    /// The experimental runtime partition drove the last sample.
    bool lastBlockUsed = false;

    /// How many times the linear transfer has been re-derived (a pot or
    /// resistor move). A no-op control write must not increment this.
    long long linearRebuilds = 0;
};

enum class ParameterUpdateKind {
    /// Value/metadata only; safe anywhere.
    Cheap,
    /// Re-derives the R-type transfer (~25 us class today). The caller decides
    /// whether that is safe at its call site.
    LinearRebuild,
};

class Runtime {
public:
    virtual ~Runtime() = default;

    virtual const compiler::CompiledCircuit& circuit() const noexcept = 0;
    virtual const compiler::NodeTable& nodes() const noexcept = 0;
    virtual const compiler::ParameterTable& parameters() const noexcept = 0;

    /// Advance exactly one sample. `sourceValues` is laid out as the netlist's
    /// voltage sources followed by its current sources, matching
    /// `CompiledCircuit::sourceCount()`. The returned view is valid until the
    /// next step/reset.
    virtual std::span<const double> step(std::span<const double> sourceValues) = 0;

    /// Apply a batch of parameter moves. All handles are validated before any
    /// value is written; an invalid handle or value rejects the whole batch and
    /// leaves the runtime unchanged.
    ///
    /// The batch re-derives the linear transfer at most once. It does not
    /// allocate after construction. Topology changes are never applied here.
    virtual bool setParameters(std::span<const compiler::ParameterValue> values) = 0;

    /// Cost class of a move of `handle`. Every parameter in Phase 1 is a
    /// `LinearRebuild`; `Cheap` is kept for metadata-only controls.
    virtual ParameterUpdateKind parameterUpdateKind(
        compiler::ParameterHandle handle) const noexcept = 0;

    /// The value a handle is currently at. The compiled default is
    /// `parameters().value(handle)`; this is the live value.
    virtual double parameterValue(compiler::ParameterHandle handle) const noexcept = 0;

    /// All live values, indexed by `ParameterHandle::value`.
    virtual std::span<const double> parameterValues() const noexcept = 0;

    /// Recompute the DC operating point and seed reactive state, exactly as the
    /// current `Engine::reset()` does. Not a cheap "clear buffers" call: sample 1
    /// must reproduce the operating point after it.
    virtual void reset() = 0;

    /// Solve (or re-solve) the DC operating point without disturbing reactive
    /// state. Used by the editor's tables and by validation.
    virtual OperatingPoint operatingPoint() const = 0;

    virtual RuntimeDiagnostics diagnostics() const noexcept = 0;

    virtual double sampleRate() const noexcept = 0;
};

/// Build the dense runtime. Throws `std::invalid_argument` for a null circuit
/// or an unusable config, and `std::runtime_error` when the linear network is
/// singular (the same condition under which `Engine` refuses to construct).
std::unique_ptr<Runtime> makeDenseRuntime(
    std::shared_ptr<const compiler::CompiledCircuit> circuit,
    const RuntimeConfig& config);

}  // namespace livewdf::runtime
