#include "runtime/dense/DenseRuntime.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace livewdf::runtime {

namespace {

bool usableConfig(const RuntimeConfig& config) noexcept {
    return std::isfinite(config.sampleRate) && config.sampleRate > 0.0 &&
           config.maxBlockSize > 0;
}

double checkedSampleRate(const RuntimeConfig& config) {
    if (!usableConfig(config))
        throw std::invalid_argument("DenseRuntime: invalid RuntimeConfig");
    return config.sampleRate;
}

}  // namespace

namespace dense {

DenseRuntime::DenseRuntime(std::shared_ptr<const compiler::CompiledCircuit> circuit,
                           const RuntimeConfig& config)
    : circuit_(std::move(circuit)),
      engine_(circuit_->netlist(), checkedSampleRate(config)),
      sampleRate_(config.sampleRate) {
    if (config.enableRuntimeBlockSolver)
        engine_.setRuntimeBlockSolver(true);

    const auto& table = circuit_->parameters();
    parameterValues_.assign(table.size(), 0.0);
    potUpdates_.reserve(8);
    varResUpdates_.reserve(8);
    resistorUpdates_.reserve(32);
    refreshParameterValues();
}

std::span<const double> DenseRuntime::step(std::span<const double> sourceValues) {
    const auto& nodes = engine_.step(sourceValues.data(), sourceValues.size());
    return {nodes.data(), nodes.size()};
}

bool DenseRuntime::setParameters(std::span<const compiler::ParameterValue> values) {
    const auto& table = circuit_->parameters();

    // Validate the whole batch first: a rejected handle must not leave half the
    // controls moved.
    for (const auto& update : values) {
        if (!update.handle ||
            update.handle.value >= static_cast<int>(table.size()) ||
            !std::isfinite(update.value))
            return false;
        const auto kind = table.kind(update.handle);
        if (kind == compiler::ParameterKind::Resistance && !(update.value > 0.0))
            return false;
    }

    potUpdates_.clear();
    varResUpdates_.clear();
    resistorUpdates_.clear();

    for (const auto& update : values) {
        const auto& entry = table.entry(update.handle);
        switch (entry.kind) {
        case compiler::ParameterKind::PotPosition:
            potUpdates_.emplace_back(entry.componentIndex, update.value);
            break;
        case compiler::ParameterKind::VarResPosition:
            varResUpdates_.emplace_back(entry.componentIndex, update.value);
            break;
        case compiler::ParameterKind::Resistance:
            resistorUpdates_.emplace_back(entry.componentIndex, update.value);
            break;
        }
    }

    if (!engine_.setParametersByIndex(potUpdates_, varResUpdates_, resistorUpdates_))
        return false;

    refreshParameterValues();
    return true;
}

ParameterUpdateKind DenseRuntime::parameterUpdateKind(
    compiler::ParameterHandle handle) const noexcept {
    // Every current parameter changes a conductance, which re-derives the
    // transfer. `Cheap` has no instance yet; it exists for metadata-only
    // controls (rail enable, legal limits) rather than as a claim about today.
    if (!validHandle(handle))
        return ParameterUpdateKind::Cheap;
    return ParameterUpdateKind::LinearRebuild;
}

double DenseRuntime::parameterValue(compiler::ParameterHandle handle) const noexcept {
    if (!validHandle(handle))
        return std::numeric_limits<double>::quiet_NaN();
    return parameterValues_[static_cast<std::size_t>(handle.value)];
}

OperatingPoint DenseRuntime::operatingPoint() const {
    OperatingPoint op;
    op.nodeVolts.assign(static_cast<std::size_t>(engine_.nodeCount()), 0.0);
    op.rootCurrents.assign(static_cast<std::size_t>(engine_.rootCount()), 0.0);

    std::vector<double> rootVolts;
    op.converged = engine_.solveDc(op.nodeVolts, rootVolts);
    if (!rootVolts.empty()) {
        std::vector<double> unusedJacobian;
        engine_.currents(rootVolts, op.rootCurrents, unusedJacobian);
    }
    return op;
}

RuntimeDiagnostics DenseRuntime::diagnostics() const noexcept {
    RuntimeDiagnostics d;
    d.lastConverged = engine_.lastConverged();
    d.lastIterations = static_cast<int>(engine_.newtonIterations());
    d.lastDeviceEvaluations = static_cast<int>(engine_.deviceEvaluations());
    d.lastResidual = engine_.lastResidual();
    d.lastKeptBest = engine_.lastKeptBest();
    d.lastStateRejected = engine_.lastStateRejected();
    d.stateRejections = engine_.stateRejectCount();
    d.lastBlockFallback = engine_.lastBlockFallback();
    d.blockFallbacks = engine_.blockFallbackCount();
    d.lastBlockUsed = engine_.lastRuntimeBlockUsed();
    d.linearRebuilds = engine_.linearRebuilds();
    return d;
}

void DenseRuntime::refreshParameterValues() {
    const auto& table = circuit_->parameters();
    const auto& resistors = engine_.resistors();
    const auto& pots = engine_.pots();
    const auto& varRes = engine_.varRes();

    for (int i = 0; i < static_cast<int>(table.size()); ++i) {
        const auto handle = compiler::ParameterHandle{i};
        const auto& entry = table.entry(handle);
        double value = 0.0;
        switch (entry.kind) {
        case compiler::ParameterKind::Resistance:
            value = resistors[static_cast<std::size_t>(entry.componentIndex)].value;
            break;
        case compiler::ParameterKind::PotPosition:
            value = pots[static_cast<std::size_t>(entry.componentIndex)].position;
            break;
        case compiler::ParameterKind::VarResPosition:
            value = varRes[static_cast<std::size_t>(entry.componentIndex)].position;
            break;
        }
        parameterValues_[static_cast<std::size_t>(i)] = value;
    }
}

}  // namespace dense

std::unique_ptr<Runtime> makeDenseRuntime(
    std::shared_ptr<const compiler::CompiledCircuit> circuit,
    const RuntimeConfig& config) {
    if (!circuit)
        throw std::invalid_argument("makeDenseRuntime: compiled circuit is null");
    if (!usableConfig(config))
        throw std::invalid_argument("makeDenseRuntime: invalid RuntimeConfig");
    return std::make_unique<dense::DenseRuntime>(std::move(circuit), config);
}

}  // namespace livewdf::runtime
