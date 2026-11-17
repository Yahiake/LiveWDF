#pragma once

// Dense runtime: the existing R-type engine behind the `Runtime` contract.
//
// This header and its .cpp are the only runtime files that include `wdf/Engine.h`.
// Consumers include `runtime/Runtime.h`; if they need a concrete type they use
// the factory.
//
// Phase 1 is a one-for-one adapter. The private `ExecutionPlan` in
// TARGET-INTERFACES (Network, transfer, block partition) stays inside `Engine`
// until Phase 2; the important part today is that callers see `Runtime` and that
// the solve dispatch and the protected relaxation/fallback region are untouched.

#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "runtime/Runtime.h"
#include "wdf/Engine.h"

namespace livewdf::runtime::dense {

class DenseRuntime final : public Runtime {
public:
    DenseRuntime(std::shared_ptr<const compiler::CompiledCircuit> circuit,
                 const RuntimeConfig& config);

    const compiler::CompiledCircuit& circuit() const noexcept override { return *circuit_; }
    const compiler::NodeTable& nodes() const noexcept override { return circuit_->nodes(); }
    const compiler::ParameterTable& parameters() const noexcept override {
        return circuit_->parameters();
    }

    std::span<const double> step(std::span<const double> sourceValues) override;

    bool setParameters(std::span<const compiler::ParameterValue> values) override;

    ParameterUpdateKind parameterUpdateKind(
        compiler::ParameterHandle handle) const noexcept override;

    double parameterValue(compiler::ParameterHandle handle) const noexcept override;

    std::span<const double> parameterValues() const noexcept override {
        return parameterValues_;
    }

    void reset() override { engine_.reset(); }

    OperatingPoint operatingPoint() const override;

    RuntimeDiagnostics diagnostics() const noexcept override;

    double sampleRate() const noexcept override { return sampleRate_; }

private:
    bool validHandle(compiler::ParameterHandle handle) const noexcept {
        return handle.value >= 0 &&
               handle.value < static_cast<int>(parameterValues_.size());
    }

    void refreshParameterValues();

    std::shared_ptr<const compiler::CompiledCircuit> circuit_;
    wdf::Engine engine_;
    double sampleRate_ = 48000.0;

    std::vector<double> parameterValues_;

    // Per-update scratch, sized here and reused: a parameter move must not
    // allocate. Indices refer to the matching `Circuit` vectors.
    std::vector<std::pair<int, double>> potUpdates_;
    std::vector<std::pair<int, double>> varResUpdates_;
    std::vector<std::pair<int, double>> resistorUpdates_;
};

}  // namespace livewdf::runtime::dense
