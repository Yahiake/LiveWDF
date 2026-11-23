#pragma once

// Level 3 of the engine: the R-type transfer, DC operating point, and parameters.
//
// The network's structure never changes -- ports, ideal-source potentials and
// the node set all come from the netlist topology -- so `Xe`/`Xr`/`Z` are built
// once and re-derived only when a component *value* moves.
//
// The actual algorithms live in focused modular headers:
//   - `wdf/compile/DenseTransfer.h`: R-type transfer build and column-compaction
//   - `wdf/compile/DenseParameters.h`: parameter updates (pots, rheostats, resistors)
//   - `wdf/compile/DenseDc.h`: DC operating point and network bias solver
//
// This file is the public facade for Level 3 and forwards to these units.

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "wdf/EngineSolve.h"
#include "wdf/compile/AdjustableTransfer.h"
#include "wdf/compile/DenseDc.h"
#include "wdf/compile/DenseParameters.h"
#include "wdf/compile/DenseTransfer.h"

namespace wdf {

class EngineCompile : public EngineSolve {
public:
    // ------------------------------------------------------------------ //
    // Component values as parameters
    // ------------------------------------------------------------------ //

    /// The netlist's resistors, in element-port order.
    const std::vector<Res>& resistors() const { return circuit_.res; }

    /// Element-port index of the resistor named `name`, or -1.
    int resIndexByName(const std::string& name) const {
        return resIndexByNameCore(name);
    }

    /// How many times the R-type transfer has been re-derived.
    long long linearRebuilds() const { return linearRebuilds_; }

    /// Set resistor `idx` to `ohms` and re-derive the R-type transfer.
    bool setResistance(int idx, double ohms) {
        auto pCtx = makeParametersContext();
        return engine::DenseParameters::setResistance(pCtx, idx, ohms, [this] { return rebuildLinear(); });
    }

    bool setResistance(const std::string& name, double ohms) {
        const int idx = resIndexByName(name);
        return idx >= 0 && setResistance(idx, ohms);
    }

    /// Set multiple resistors and rebuild once (~25 us total instead of 25 us per resistor).
    bool setResistances(const std::vector<std::pair<std::string, double>>& updates) {
        bool any = false;
        auto pCtx = makeParametersContext();
        for (const auto& [name, ohms] : updates) {
            const int idx = resIndexByName(name);
            if (idx >= 0 && (ohms > 0.0) &&
                circuit_.res[static_cast<size_t>(idx)].value != ohms) {
                engine::DenseParameters::setResistorValue(pCtx, idx, ohms);
                any = true;
            }
        }
        return any ? rebuildLinear() : true;
    }

    /// Set multiple resistors by index and rebuild once (zero allocation, no string lookup).
    template <typename Container>
    bool setResistancesByIndex(const Container& updates) {
        auto pCtx = makeParametersContext();
        return engine::DenseParameters::setResistancesByIndex(pCtx, updates, [this] { return rebuildLinear(); });
    }

    /// The netlist's potentiometers.
    const std::vector<Pot>& pots() const { return circuit_.pots; }

    /// Index of the potentiometer named `name`, or -1.
    int potIndexByName(const std::string& name) const {
        for (int i = 0; i < static_cast<int>(circuit_.pots.size()); ++i)
            if (circuit_.pots[static_cast<size_t>(i)].name == name)
                return i;
        return -1;
    }

    /// Set potentiometer `name` to position `pos` in [0, 1].
    bool setPotPosition(const std::string& name, double pos) {
        const int idx = potIndexByName(name);
        return idx >= 0 && setPotPosition(idx, pos);
    }

    bool setPotPosition(int potIdx, double pos) {
        auto pCtx = makeParametersContext();
        return engine::DenseParameters::setPotPosition(pCtx, potIdx, pos, [this] { return rebuildLinear(); });
    }

    /// Apply a batch of parameter moves and re-derive the transfer at most once.
    bool setParametersByIndex(const std::vector<std::pair<int, double>>& pots,
                              const std::vector<std::pair<int, double>>& varRes,
                              const std::vector<std::pair<int, double>>& resistors) {
        auto pCtx = makeParametersContext();
        return engine::DenseParameters::setParametersByIndex(pCtx, pots, varRes, resistors, [this] { return rebuildLinear(); });
    }

    /// Prepare the transfer for sample-timed changes to this circuit's pots
    /// and rheostats. Call after loading the circuit and applying its controls.
    void prepareLiveControls() {
        if (adjustableTransfer_.ready())
            return;
        adjustableResistors_.clear();
        for (const auto* indices : {&potResA_, &potResB_, &varResResIndex_})
            for (const int index : *indices)
                if (index >= 0)
                    adjustableResistors_.push_back(index);
        std::sort(adjustableResistors_.begin(), adjustableResistors_.end());
        adjustableResistors_.erase(std::unique(adjustableResistors_.begin(), adjustableResistors_.end()),
                                   adjustableResistors_.end());
        previousPots_.reserve(circuit_.pots.size());
        previousRheostats_.reserve(circuit_.varRes.size());
        previousResistors_.reserve(adjustableResistors_.size());
        prepareAdjustableTransfer();
    }

    bool hasAdjustedLiveTransfer() const noexcept { return adjustableTransfer_.active(); }
    long long liveControlRevision() const noexcept { return liveControlRevision_; }

    /// Apply the same taper and resistor values as setParametersByIndex.
    /// A failed update leaves both the old settings and the old transfer live.
    bool setLiveControls(const std::vector<std::pair<int, double>>& pots,
                         const std::vector<std::pair<int, double>>& rheostats) {
        if (pots.empty() && rheostats.empty())
            return true;
        if (!adjustableTransfer_.ready() || pots.size() > circuit_.pots.size() ||
            rheostats.size() > circuit_.varRes.size())
            return false;
        previousPots_.clear();
        previousRheostats_.clear();
        previousResistors_.clear();
        for (const int index : adjustableResistors_)
            previousResistors_.emplace_back(index, circuit_.res[index].value);
        for (const auto& setting : pots) {
            const int index = setting.first;
            if (index < 0 || static_cast<size_t>(index) >= circuit_.pots.size())
                return false;
            previousPots_.emplace_back(index, circuit_.pots[index].position);
        }
        for (const auto& setting : rheostats) {
            const int index = setting.first;
            if (index < 0 || static_cast<size_t>(index) >= circuit_.varRes.size())
                return false;
            previousRheostats_.emplace_back(index, circuit_.varRes[index].position);
        }
        auto ctx = makeParametersContext();
        static const std::vector<std::pair<int, double>> noFixedResistors;
        bool transferFailed = false;
        const auto update = [this, &transferFailed] {
            transferFailed = !adjustableTransfer_.update(net_.eG, Z_);
            if (transferFailed)
                return false;
            compileZred();
            ++liveControlRevision_;
            return true;
        };
        if (engine::DenseParameters::setParametersByIndex(ctx, pots, rheostats, noFixedResistors, update))
            return true;
        if (transferFailed)
            engine::DenseParameters::setParametersByIndex(
                ctx, previousPots_, previousRheostats_, previousResistors_, update);
        return false;
    }

    /// The netlist's rheostats / variable resistors.
    const std::vector<VarRes>& varRes() const { return circuit_.varRes; }

    /// Index of the variable resistor named `name`, or -1.
    int varResIndexByName(const std::string& name) const {
        for (int i = 0; i < static_cast<int>(circuit_.varRes.size()); ++i)
            if (circuit_.varRes[static_cast<size_t>(i)].name == name)
                return i;
        return -1;
    }

    /// Set variable resistor `name` to position `pos` in [0, 1].
    bool setVarResPosition(const std::string& name, double pos) {
        const int idx = varResIndexByName(name);
        return idx >= 0 && setVarResPosition(idx, pos);
    }

    bool setVarResPosition(int vrIdx, double pos) {
        auto pCtx = makeParametersContext();
        return engine::DenseParameters::setVarResPosition(pCtx, vrIdx, pos, [this] { return rebuildLinear(); });
    }

    /// Re-derive `Xe_`/`Xr_`/`Z_` from the current `net_.eG`.
    bool rebuild() { return rebuildLinear(); }

    /// DC through the root machinery only: capacitors open, element waves zero.
    bool solveDc(std::vector<double>& node, std::vector<double>& rootV) const {
        return engine::DenseDc::solveDc(*this, node, rootV);
    }

    /// Resistor-only DC bias solver.
    bool solveNetworkBias(std::vector<double>& nodeBias) const {
        return engine::DenseDc::solveNetworkBias(*this, nodeBias);
    }

    /// One DC pass at a given source scale, with SPICE's gmin.
    bool solveDcAt(std::vector<double>& node, std::vector<double>& rootV, double sourceScale,
                   double gminOverride, double tol,
                   const std::vector<double>* warmStart) const {
        return engine::DenseDc::solveDcAt(*this, node, rootV, sourceScale, gminOverride, tol, warmStart);
    }

protected:
    using EngineSolve::EngineSolve;

    engine::TransferContext makeTransferContext() {
        return engine::TransferContext{
            net_, N_, P_, K_, nIsrc_, nLive_, Xe_, Xr_, Xis_, Z_,
            linearRebuilds_, linY_, linRhs_, linXe_, linXr_,
            xeRowOffsets_, xeCols_, xeVals_,
            xrRowOffsets_, xrCols_, xrVals_,
            rootLayout_.nodeCol, networkFactors_
        };
    }

    engine::ParametersContext makeParametersContext() {
        return engine::ParametersContext{
            circuit_, comp_, net_, potResA_, potResB_, varResResIndex_
        };
    }

    void setResistorValue(int idx, double ohms) {
        auto pCtx = makeParametersContext();
        engine::DenseParameters::setResistorValue(pCtx, idx, ohms);
    }

    /// The one place `Xe_`/`Xr_`/`Z_` are produced.
    bool rebuildLinear() {
        auto tCtx = makeTransferContext();
        if (!engine::DenseTransfer::rebuildLinear(tCtx))
            return false;
        refreshSparseTransfer();
        refreshRootGroups();
        if (!adjustableResistors_.empty())
            prepareAdjustableTransfer();
        return true;
    }

    void prepareAdjustableTransfer() {
        adjustableTransfer_.prepare(net_, linXe_, N_, Xr_, Z_, adjustableResistors_);
    }

    engine::AdjustableTransfer adjustableTransfer_;
    std::vector<int> adjustableResistors_;
    std::vector<std::pair<int, double>> previousPots_, previousRheostats_, previousResistors_;
    long long linearRebuilds_ = 0;
    long long liveControlRevision_ = 0;
};

}  // namespace wdf
