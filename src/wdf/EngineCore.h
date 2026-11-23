#pragma once

// Engine core: the compiled topology and every buffer the engine owns.
//
// The engine has four genuinely different jobs -- holding state, solving the
// nonlinear root, deriving the R-type transfer, and running one sample -- and a
// single file that holds all four cannot be read end to end. So `Engine` is
// assembled as a small chain of focused classes: this file (`EngineCore`),
// `EngineSolve`, `EngineCompile`, `EngineStep`. The chain is an implementation
// detail; `Engine` remains the one public type and re-exports all of it.
//
// `EngineCore` is the only level that *owns* state. The levels above reach it
// through the protected members at the bottom of this class, which is what makes
// the split pure code motion: nothing moved but its address in the file.
//
// One sample `n`:
//
//     u        = Xe * (b - eoff)          node voltages from the element waves
//     v_oc[r]  = v_r(u) + roff[r]         open-circuit root port voltages
//     Z[r][c]  = response of root port r to a unit current into root port c
//     solve    v - v_oc + Z*i(v) = 0      multiport Newton for the device currents
//     node     = u - Xr * i
//
// `eoff` / `roff` carry the ideal sources. Their *structure* never changes, so
// `Xe`/`Xr`/`Z` are built once at construction and only the offsets are
// re-derived per sample -- and even those are an affine map (see `AffineOffset`
// below), so a step costs a short dot product rather than a rebuild.
//
// Everything the audio loop touches is allocated before the first sample.
// `EngineStep::step()` performs no allocation.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "circuit/Network.h"
#include "circuit/RootLayout.h"
#include "devices/Diode.h"
#include "devices/PowerSupply.h"
#include "spice/Net.h"
#include "wdf/DenseTopology.h"
#include "wdf/state/DenseDiagnostics.h"
#include "wdf/state/DenseState.h"

namespace wdf {

namespace engine {
class DenseDc;
}

/// Level 1 of the engine: topology and state.
class EngineCore : public state::DenseState, public state::DenseDiagnostics {
    friend class engine::DenseDc;
public:
    // ------------------------------------------------------------------ //
    // Topology: what the netlist compiled to. Fixed for the engine's life --
    // only component *values* are parameters, never ports or nodes.
    // ------------------------------------------------------------------ //
    int nodeCount() const { return N_; }
    int rootCount() const { return K_; }
    int elementCount() const { return P_; }
    int sourceCount() const { return nsrc_; }
    int capCount() const { return nCaps_; }
    int inductorCount() const { return nInds_; }
    int currentSourceCount() const { return nIsrc_; }
    int controlledResistorCount() const { return nCtrl_; }
    int pentodeCount() const { return nPent_; }
    int jfetCount() const { return nJfet_; }
    int mosfetCount() const { return nMosfet_; }
    int bjtCount() const { return nBjt_; }
    int opAmpCount() const { return nOpAmp_; }
    /// How many JFET groups have needed the independent bracketed fallback.
    long long jfetFallbackCount() const { return jfetFallbacks_; }
    /// How many MOSFET groups have needed the independent bracketed fallback.
    long long mosfetFallbackCount() const { return mosfetFallbacks_; }
    /// The single source of truth for root-port order and active rows.
    const RootLayout& rootLayout() const { return rootLayout_; }
    int vcvsCount() const { return nVCVS_; }
    const std::vector<double>& controlledResistanceValues() const { return ctrlResValues_; }
    int controlledResistorIndexByName(const std::string& name) const {
        for (int i = 0; i < nCtrl_; ++i)
            if (ctrlResNames_[static_cast<size_t>(i)] == name)
                return i;
        return -1;
    }
    void setControlledResistance(int idx, double ohms) {
        if (idx >= 0 && idx < nCtrl_ && ohms > 0.0)
            ctrlResValues_[static_cast<size_t>(idx)] = ohms;
    }
    bool setControlledResistance(const std::string& name, double ohms) {
        const int idx = controlledResistorIndexByName(name);
        if (idx < 0 || !(ohms > 0.0))
            return false;
        ctrlResValues_[static_cast<size_t>(idx)] = ohms;
        return true;
    }
    double fs() const { return fs_; }
    double T() const { return T_; }
    const Compiled& compiled() const { return comp_; }
    const std::vector<std::string>& nodeNames() const { return comp_.nodes; }
    int nodeIndex(const std::string& name) const {
        const auto it = comp_.index.find(name);
        return it == comp_.index.end() ? -1 : it->second;
    }
    const std::vector<double>& dcValues() const { return dcValues_; }

    // ------------------------------------------------------------------ //
    // Dynamic Power Supply Sag & Asymmetric Soft Rails
    // ------------------------------------------------------------------ //
    void enableOpAmpRailSag(size_t opAmpIndex, double impedance = 100.0, double filterCap = 47.0e-6,
                            RectifierType rectifier = RectifierType::SolidState) {
        if (opAmpIndex < opAmps_.size()) {
            powerSagEnabled_ = true;
            if (opAmpPsuPos_.size() != opAmps_.size()) {
                opAmpPsuPos_.resize(opAmps_.size());
                opAmpPsuNeg_.resize(opAmps_.size());
                opAmpSagActive_.assign(opAmps_.size(), false);
                opAmpNominalRailPos_.resize(opAmps_.size());
                opAmpNominalRailNeg_.resize(opAmps_.size());
                for (size_t i = 0; i < opAmps_.size(); ++i) {
                    opAmpNominalRailPos_[i] = opAmps_[i].vRailPos;
                    opAmpNominalRailNeg_[i] = opAmps_[i].vRailNeg;
                }
            }
            opAmpSagActive_[opAmpIndex] = true;
            opAmpPsuPos_[opAmpIndex] = PowerSupply(opAmpNominalRailPos_[opAmpIndex], impedance, filterCap, rectifier, fs_);
            opAmpPsuNeg_[opAmpIndex] = PowerSupply(opAmpNominalRailNeg_[opAmpIndex], impedance, filterCap, rectifier, fs_);
        }
    }

    void enableAllOpAmpRailSag(double impedance = 100.0, double filterCap = 47.0e-6,
                               RectifierType rectifier = RectifierType::SolidState) {
        for (size_t i = 0; i < opAmps_.size(); ++i) {
            enableOpAmpRailSag(i, impedance, filterCap, rectifier);
        }
    }

    void disableOpAmpRailSag(size_t opAmpIndex) {
        if (opAmpIndex < opAmpSagActive_.size()) {
            opAmpSagActive_[opAmpIndex] = false;
            if (opAmpIndex < opAmps_.size() && opAmpIndex < opAmpNominalRailPos_.size()) {
                opAmps_[opAmpIndex].setVRails(opAmpNominalRailPos_[opAmpIndex], opAmpNominalRailNeg_[opAmpIndex]);
            }
            bool anyActive = false;
            for (bool a : opAmpSagActive_) {
                if (a) {
                    anyActive = true;
                    break;
                }
            }
            powerSagEnabled_ = anyActive;
        }
    }

    void disableAllOpAmpRailSag() {
        for (size_t i = 0; i < opAmpSagActive_.size(); ++i) {
            opAmpSagActive_[i] = false;
            if (i < opAmps_.size() && i < opAmpNominalRailPos_.size()) {
                opAmps_[i].setVRails(opAmpNominalRailPos_[i], opAmpNominalRailNeg_[i]);
            }
        }
        powerSagEnabled_ = false;
    }

    bool isOpAmpRailSagEnabled(size_t opAmpIndex) const {
        return (opAmpIndex < opAmpSagActive_.size()) && opAmpSagActive_[opAmpIndex];
    }

    const PowerSupply* opAmpPowerSupplyPos(size_t opAmpIndex) const {
        return (opAmpIndex < opAmpPsuPos_.size() && opAmpSagActive_[opAmpIndex]) ? &opAmpPsuPos_[opAmpIndex] : nullptr;
    }

    const PowerSupply* opAmpPowerSupplyNeg(size_t opAmpIndex) const {
        return (opAmpIndex < opAmpPsuNeg_.size() && opAmpSagActive_[opAmpIndex]) ? &opAmpPsuNeg_[opAmpIndex] : nullptr;
    }

    void setOpAmpSoftRails(size_t opAmpIndex, bool soft = true) {
        if (opAmpIndex < opAmps_.size()) {
            opAmps_[opAmpIndex].softRails = soft;
        }
    }

    void setAllOpAmpSoftRails(bool soft = true) {
        for (auto& oa : opAmps_) {
            oa.softRails = soft;
        }
    }

    bool isOpAmpSoftRails(size_t opAmpIndex) const {
        return (opAmpIndex < opAmps_.size()) && opAmps_[opAmpIndex].softRails;
    }

    // ------------------------------------------------------------------ //
    // The derived R-type transfer. Built once by `EngineCompile::rebuildLinear`,
    // re-derived on a pot move; these are read-only views for oracles and tools.
    // ------------------------------------------------------------------ //
    const std::vector<std::array<int, 2>>& rootPorts() const { return net_.rp; }
    const std::vector<double>& rootImpedance() const { return Z_; }
    /// The element-to-node transfer, **column-compacted**: `nLive_ <= P_` columns,
    /// row-major, column `t` belonging to element port `liveElementPorts()[t]`.
    /// The dropped columns are the ones whose `w` entry is identically zero, so
    /// `Xe * w == XeLive * wLive` exactly. Kept compact rather than duplicated
    /// dense, because the only reader is a diagnostic.
    const std::vector<double>& Xe() const { return Xe_; }
    /// Column `t` of `Xe()` is the transfer of element port `liveElementPorts()[t]`.
    const std::vector<int>& liveElementPorts() const { return net_.liveCols; }
    const std::vector<double>& Xr() const { return Xr_; }
    const std::vector<double>& elementG() const { return net_.eG; }
    const std::vector<int>& elementPortA() const { return net_.epA; }
    const std::vector<int>& elementPortB() const { return net_.epB; }
    const std::vector<int>& capOf() const { return net_.capOf; }
    const std::vector<int>& indOf() const { return net_.indOf; }
    const std::vector<AffineOffset>& elementOffsets() const { return net_.eoffAff; }
    const std::vector<AffineOffset>& rootOffsets() const { return net_.roffAff; }

    // ------------------------------------------------------------------ //
    // Reactive state: the previous incident wave of each storage element.
    // ------------------------------------------------------------------ //
    const std::vector<double>& capacitorStates() const { return capState_; }
    const std::vector<double>& inductorStates() const { return indState_; }
    void setCapacitorState(size_t k, double a) noexcept {
        if (k < capState_.size())
            capState_[k] = a;
    }
    double getCapacitorState(size_t k) const noexcept {
        return k < capState_.size() ? capState_[k] : 0.0;
    }
    void setInductorState(size_t k, double a) noexcept {
        if (k < indState_.size())
            indState_[k] = a;
    }
    double getInductorState(size_t k) const noexcept {
        return k < indState_.size() ? indState_[k] : 0.0;
    }

    /// Diagnostics: how much work the last `step()` did.
    long long newtonIterations() const { return newtonIters_; }
    long long deviceEvaluations() const { return deviceEvals_; }
    long long lineSearchTrials() const { return lineTrials_; }
    long long stepLimitHits() const { return limitHits_; }

    using PhaseProfile = wdf::PhaseProfile;
    void setProfile(bool on) { profileOn_ = on; }
    bool profile() const { return profileOn_; }
    const PhaseProfile& phaseProfile() const { return prof_; }
    void resetPhaseProfile() { prof_ = PhaseProfile{}; }

    /// Per-port voltage limits, all in volts.
    struct Limits {
        double predPlateUp = 50.0, predPlateDown = 1000.0;
        double predGridUp = 50.0, predGridDown = 50.0;
        double stepPlateUp = 500.0, stepPlateDown = 1000.0;
        double stepGridUp = 50.0, stepGridDown = 50.0;
        double predDiodeUp = 5.0, predDiodeDown = 5.0;
        double stepDiodeUp = 5.0, stepDiodeDown = 5.0;
        // Sanity bound for an *accepted* node voltage. Most modelled rails sit
        // under 500 V, but a pulled Lead circuit amplified by a cranked Master
        // has a genuine converged fixed point around 8.3 kV at its GEQ grid
        // node (the `mark iic+ test` document: residual 3e-11, node -8302 V).
        // A 2 kV bound rejected that *true* operating point, which turned
        // every sample into a reject-and-restore and pegged the CPU. Runaway
        // iterates were observed at 8.2e6 V, so 30 kV still rejects runaway
        // by two and a half orders of magnitude while accepting the real one.
        double maxNodeVolts = 30000.0;
    };
    void setLimits(const Limits& l) {
        lim_ = l;
        updatePortLimits();
    }
    const Limits& limits() const { return lim_; }

    /// How many pentode grid ports the `pnjlim` limiter moved on the last
    /// sample. Diagnostic only: the accepted root is unchanged, but a high
    /// count means the limiter is compressing Newton steps.
    long long gridLimitHits() const { return gridLimitHits_; }
    /// Did the last `step()` reach the residual tolerance?
    bool lastConverged() const { return lastConverged_; }
    /// Max |residual| (volts) at the accepted point of the last `step()`, NaN
    /// after the closed form until `evaluateResidual()`.
    double lastResidual() const { return lastResidual_; }
    /// Did the last `step()` finish by restoring an earlier iterate with a lower residual?
    bool lastKeptBest() const { return keptBest_; }
    /// Was the last `step()` rejected because its state was non-finite or outside
    /// the plausible voltage range? The previous accepted state was restored.
    bool lastStateRejected() const { return lastStateRejected_; }
    /// Total rejected samples since construction (diagnostic only).
    long long stateRejectCount() const { return stateRejections_; }

    /// The largest node voltage magnitude seen by the last `step()` and which
    /// node it was. Recorded by the state guard's own scan, so this is free at
    /// the call site. An index of -1 means the sample was not reconstructed.
    int lastMaxNodeIndex() const { return lastMaxNodeIndex_; }
    double lastMaxNodeVolts() const { return lastMaxNodeVolts_; }

    /// When `lastStateRejected()` is true, the first node that failed the
    /// guard. Diagnostic only: a rejected solve is restored rather than used.
    int lastRejectNodeIndex() const { return lastRejectNodeIndex_; }
    double lastRejectNodeVolts() const { return lastRejectNodeVolts_; }

    /// Diagnostics for the block pass: its size, whether the last sample had to
    /// fall back to the joint Newton, and how many samples have done so.
    int rootBlockCount() const { return blockCount_; }
    bool fineBlockSolver() const { return fineBlockSolver_; }
    bool lastBlockFallback() const { return lastBlockFallback_; }
    long long blockFallbackCount() const { return blockFallbacks_; }
    double lastBlockResidual() const { return lastBlockResidual_; }
    int rootBlockDeviceCount(int s) const {
        return static_cast<int>(newtonBlocks_[static_cast<size_t>(groupOrder_[static_cast<size_t>(s)])].devices.size());
    }
    int rootBlockDevice(int s, int j) const {
        return newtonBlocks_[static_cast<size_t>(groupOrder_[static_cast<size_t>(s)])].devices[static_cast<size_t>(j)];
    }
    int rootBlockPortCount(int s) const {
        return static_cast<int>(newtonBlocks_[static_cast<size_t>(groupOrder_[static_cast<size_t>(s)])].ports.size());
    }
    wdf::DeviceKind rootDeviceKind(int idx) const {
        return rootDevices_[static_cast<size_t>(idx)].kind;
    }
    int rootDeviceComp(int idx) const {
        return static_cast<int>(rootDevices_[static_cast<size_t>(idx)].compIndex);
    }
    int rootDevicePortCount(int idx) const {
        return rootDevices_[static_cast<size_t>(idx)].numPorts;
    }

    /// Experimental runtime active-set block partition. The static partition
    /// above is derived once from the complete coupling graph; this one selects
    /// a precomputed partition from the grid columns that can actually conduct
    /// at the current point. Disabled by default, so the reference solver is
    /// unchanged.
    bool runtimeBlockSolver() const { return runtimeBlockEnabled_; }
    int runtimeBlockCount() const { return runtimeBlockCount_; }
    bool lastRuntimeBlockUsed() const { return runtimeBlockUsed_; }
    long long runtimeBlockFallbackCount() const { return runtimeBlockFallbacks_; }
    int runtimeBlockDeviceCount(int s) const {
        if (s < 0 || s >= runtimeBlockCount_)
            return 0;
        const auto& blocks = runtimePartition_ ? runtimePartition_->blocks
                                               : runtimeBlocks_;
        const auto& order = runtimePartition_ ? runtimePartition_->order
                                              : runtimeOrder_;
        return static_cast<int>(
            blocks[static_cast<size_t>(order[static_cast<size_t>(s)])]
                .devices.size());
    }
    int runtimeBlockDevice(int s, int j) const {
        if (s < 0 || s >= runtimeBlockCount_)
            return -1;
        const auto& blocks = runtimePartition_ ? runtimePartition_->blocks
                                               : runtimeBlocks_;
        const auto& order = runtimePartition_ ? runtimePartition_->order
                                              : runtimeOrder_;
        const auto& devices =
            blocks[static_cast<size_t>(order[static_cast<size_t>(s)])].devices;
        if (j < 0 || j >= static_cast<int>(devices.size()))
            return -1;
        return devices[static_cast<size_t>(j)];
    }
    int runtimeBlockPortCount(int s) const {
        if (s < 0 || s >= runtimeBlockCount_)
            return 0;
        const auto& blocks = runtimePartition_ ? runtimePartition_->blocks
                                               : runtimeBlocks_;
        const auto& order = runtimePartition_ ? runtimePartition_->order
                                              : runtimeOrder_;
        return static_cast<int>(
            blocks[static_cast<size_t>(order[static_cast<size_t>(s)])]
                .ports.size());
    }

    void setKeepBest(bool on) { keepBestOn_ = on; }
    bool keepBest() const { return keepBestOn_; }

    using DeviceKind = wdf::DeviceKind;
    using RootDevice = wdf::RootDevice;
    using JunctionLimiter = wdf::JunctionLimiter;
    using GridJunctionLimiter = wdf::GridJunctionLimiter;
    using NewtonBlock = wdf::NewtonBlock;
    using RuntimePartition = wdf::RuntimePartition;
    using BlockView = wdf::BlockView;
    static DeviceKind deviceKind(RootKind kind) noexcept { return wdf::deviceKind(kind); }

    void updatePortLimits() {
        if (K_ <= 0) return;
        predDown_.assign(static_cast<size_t>(K_), 50.0);
        predUp_.assign(static_cast<size_t>(K_), 50.0);
        stepDown_.assign(static_cast<size_t>(K_), 50.0);
        stepUp_.assign(static_cast<size_t>(K_), 50.0);

        for (const auto& dev : rootDevices_) {
            switch (dev.kind) {
            case DeviceKind::Triode: {
                const int o = dev.portBase;
                predUp_[static_cast<size_t>(o)] = lim_.predPlateUp;
                predDown_[static_cast<size_t>(o)] = lim_.predPlateDown;
                stepUp_[static_cast<size_t>(o)] = lim_.stepPlateUp;
                stepDown_[static_cast<size_t>(o)] = lim_.stepPlateDown;

                predUp_[static_cast<size_t>(o + 1)] = lim_.predGridUp;
                predDown_[static_cast<size_t>(o + 1)] = lim_.predGridDown;
                stepUp_[static_cast<size_t>(o + 1)] = lim_.stepGridUp;
                stepDown_[static_cast<size_t>(o + 1)] = lim_.stepGridDown;

                predUp_[static_cast<size_t>(o + 2)] = lim_.predGridUp;
                predDown_[static_cast<size_t>(o + 2)] = lim_.predGridDown;
                stepUp_[static_cast<size_t>(o + 2)] = lim_.stepGridUp;
                stepDown_[static_cast<size_t>(o + 2)] = lim_.stepGridDown;
                break;
            }
            case DeviceKind::Diode: {
                const int e = dev.portBase;
                predUp_[static_cast<size_t>(e)] = lim_.predDiodeUp;
                predDown_[static_cast<size_t>(e)] = lim_.predDiodeDown;
                stepUp_[static_cast<size_t>(e)] = lim_.stepDiodeUp;
                stepDown_[static_cast<size_t>(e)] = lim_.stepDiodeDown;
                break;
            }
            case DeviceKind::ControlledRes: {
                const int e = dev.portBase;
                predUp_[static_cast<size_t>(e)] = 0.0;
                predDown_[static_cast<size_t>(e)] = 0.0;
                stepUp_[static_cast<size_t>(e)] = std::numeric_limits<double>::infinity();
                stepDown_[static_cast<size_t>(e)] = std::numeric_limits<double>::infinity();
                break;
            }
            case DeviceKind::Pentode: {
                const int base = dev.portBase;
                predUp_[static_cast<size_t>(base)] = lim_.predPlateUp;
                predDown_[static_cast<size_t>(base)] = lim_.predPlateDown;
                stepUp_[static_cast<size_t>(base)] = lim_.stepPlateUp;
                stepDown_[static_cast<size_t>(base)] = lim_.stepPlateDown;

                // Grid port (position 1).
                predUp_[static_cast<size_t>(base + 1)] = lim_.predGridUp;
                predDown_[static_cast<size_t>(base + 1)] = lim_.predGridDown;
                stepUp_[static_cast<size_t>(base + 1)] = lim_.stepGridUp;
                stepDown_[static_cast<size_t>(base + 1)] = lim_.stepGridDown;

                // Screen port (position 2): high-voltage electrode, like the plate.
                predUp_[static_cast<size_t>(base + 2)] = lim_.predPlateUp;
                predDown_[static_cast<size_t>(base + 2)] = lim_.predPlateDown;
                stepUp_[static_cast<size_t>(base + 2)] = lim_.stepPlateUp;
                stepDown_[static_cast<size_t>(base + 2)] = lim_.stepPlateDown;
                break;
            }
            case DeviceKind::Jfet:
            case DeviceKind::Mosfet:
            case DeviceKind::Bjt: {
                const int base = dev.portBase;
                // The collector-emitter (or drain-source) port can swing like a plate;
                // the base-emitter (or gate-source) port is a control voltage and uses
                // the grid limits. These laws are polynomial, so these are
                // prediction/step guards, not junction clamps.
                predUp_[static_cast<size_t>(base)] = lim_.predPlateUp;
                predDown_[static_cast<size_t>(base)] = lim_.predPlateDown;
                stepUp_[static_cast<size_t>(base)] = lim_.stepPlateUp;
                stepDown_[static_cast<size_t>(base)] = lim_.stepPlateDown;

                predUp_[static_cast<size_t>(base + 1)] = lim_.predGridUp;
                predDown_[static_cast<size_t>(base + 1)] = lim_.predGridDown;
                stepUp_[static_cast<size_t>(base + 1)] = lim_.stepGridUp;
                stepDown_[static_cast<size_t>(base + 1)] = lim_.stepGridDown;
                break;
            }
            case DeviceKind::OpAmp: {
                const int o = dev.portBase;
                predUp_[static_cast<size_t>(o)] = lim_.predPlateUp;
                predDown_[static_cast<size_t>(o)] = lim_.predPlateDown;
                stepUp_[static_cast<size_t>(o)] = lim_.stepPlateUp;
                stepDown_[static_cast<size_t>(o)] = lim_.stepPlateDown;
                for (int q = 1; q < 3; ++q) {
                    predUp_[static_cast<size_t>(o + q)] = lim_.predGridUp;
                    predDown_[static_cast<size_t>(o + q)] = lim_.predGridDown;
                    stepUp_[static_cast<size_t>(o + q)] = lim_.stepGridUp;
                    stepDown_[static_cast<size_t>(o + q)] = lim_.stepGridDown;
                }
                break;
            }
            }
        }
    }

protected:
    /// Index of the resistor named `name` in `Circuit::res`, or -1. The
    /// allocation-free lookup behind `EngineCompile::resIndexByName`; a linear
    /// scan because a circuit has tens of resistors.
    int resIndexByNameCore(const std::string& name) const noexcept {
        for (int i = 0; i < static_cast<int>(circuit_.res.size()); ++i)
            if (circuit_.res[static_cast<std::size_t>(i)].name == name)
                return i;
        return -1;
    }

    /// Compile the netlist into ports, element conductances and root ports.
    EngineCore(const Circuit& circuit, double fs, bool gridCurrent)
        : circuit_(circuit), fs_(fs), T_(1.0 / fs) {
        // Diagnostic knob for A/B measurements of the block-fallback gate.
        // Default 16 samples (measured A/B sweep 0/2/4/8/16/32); zero disables
        // the cooldown entirely for control runs.
        if (const char* cd = std::getenv("LIVEWDF_BLOCK_COOLDOWN"))
            blockFallbackCooldownLimit_ = std::clamp(std::atoi(cd), 0, 64);
        comp_ = compile(circuit_, {});
        for (auto& t : comp_.tri)
            t.model.gridConducts = gridCurrent;
        N_ = comp_.N;
        nsrc_ = comp_.nsrc;
        dcValues_.resize(static_cast<size_t>(nsrc_));
        for (int i = 0; i < nsrc_; ++i)
            dcValues_[static_cast<size_t>(i)] = circuit_.src[static_cast<size_t>(i)].dc;

        nCaps_ = static_cast<int>(circuit_.cap.size());
        nInds_ = static_cast<int>(circuit_.ind.size());
        nIsrc_ = static_cast<int>(circuit_.isrc.size());
        nVCVS_ = static_cast<int>(circuit_.vcvs.size());
        nTri_ = static_cast<int>(comp_.tri.size());
        nD_ = static_cast<int>(comp_.diode.size());
        nCtrl_ = static_cast<int>(comp_.ctrlRes.size());
        nPent_ = static_cast<int>(comp_.pent.size());
        nJfet_ = static_cast<int>(comp_.jfet.size());
        nMosfet_ = static_cast<int>(comp_.mosfet.size());
        nBjt_ = static_cast<int>(comp_.bjt.size());
        nOpAmp_ = static_cast<int>(comp_.opamp.size());
        rootLayout_ = buildRootLayout(comp_);
        K_ = rootLayout_.K;
        ctrlResValues_.resize(static_cast<size_t>(nCtrl_));
        ctrlResNames_.resize(static_cast<size_t>(nCtrl_));
        for (int c = 0; c < nCtrl_; ++c) {
            ctrlResValues_[static_cast<size_t>(c)] = comp_.ctrlRes[static_cast<size_t>(c)].second;
            ctrlResNames_[static_cast<size_t>(c)] = circuit_.ctrlRes[static_cast<size_t>(c)].name;
        }
        photoresistors_.reserve(comp_.photoSensePorts.size());
        for (const auto& [index, port] : comp_.photoSensePorts) {
            const auto& photo = *circuit_.ctrlRes[index].photo;
            photoresistors_.push_back({
                index, port.a, port.b, makeAffine(port.off),
                -std::expm1(-T_ / photo.attackSeconds),
                -std::expm1(-T_ / photo.releaseSeconds),
            });
        }
        vcas_.reserve(comp_.vcaSensePorts.size());
        for (const auto& [index, port] : comp_.vcaSensePorts)
            vcas_.push_back({index, port.a, port.b, makeAffine(port.off)});
        for (const auto& bias : comp_.dbx200BiasPorts)
            dbx200Biases_.push_back({bias.pnp, bias.npn, {bias.port.a, bias.port.b, makeAffine(bias.port.off)}});
        otas_.reserve(comp_.otaSensePorts.size());
        for (const auto& [index, port] : comp_.otaSensePorts)
            otas_.push_back({index, {port.a, port.b, makeAffine(port.off)}});
        bucketBrigades_.reserve(comp_.bucketBrigades.size());
        for (const auto& bbd : comp_.bucketBrigades)
            bucketBrigades_.push_back({
                bbd.outputCurrentIndex,
                {bbd.input.a, bbd.input.b, makeAffine(bbd.input.off)},
                {bbd.clock.a, bbd.clock.b, makeAffine(bbd.clock.off)},
                BucketBrigadeState(bbd.model),
            });
        rmsDetectors_.reserve(comp_.rmsDetectors.size());
        for (const auto& detector : comp_.rmsDetectors)
            rmsDetectors_.push_back({
                detector.outputCurrentIndex,
                {detector.input.a, detector.input.b, makeAffine(detector.input.off)},
                RmsDetectorState(detector.model, T_),
            });
        // The resistor each control writes. Resolved once, here: a pot move on
        // the audio thread used to build "POT1_1" and scan the resistor list for
        // it, which allocates and is exactly the kind of work a parameter update
        // must not do. A missing backing resistor stays -1 and the move is
        // refused, as before.
        potResA_.assign(circuit_.pots.size(), -1);
        potResB_.assign(circuit_.pots.size(), -1);
        for (size_t i = 0; i < circuit_.pots.size(); ++i) {
            const auto& pot = circuit_.pots[i];
            potResA_[i] = resIndexByNameCore(pot.name + "_1");
            potResB_[i] = resIndexByNameCore(pot.name + "_2");
        }
        varResResIndex_.assign(circuit_.varRes.size(), -1);
        for (size_t i = 0; i < circuit_.varRes.size(); ++i)
            varResResIndex_[i] = resIndexByNameCore(circuit_.varRes[i].name);
        pentodes_.clear();
        for (const auto& cp : comp_.pent) {
            pentodes_.push_back(cp.model);
            pentodes_.back().ensureTable();
        }

        opAmps_.clear();
        for (const auto& oa : comp_.opamp) {
            opAmps_.push_back(oa.model);
        }
        // Per-op-amp runtime state for the three-port VCVS law: the internal
        // dominant-pole source voltage, the backward-Euler pole coefficient
        // (alpha = 2*pi*fp/fs with fp = GBW/A0) and the per-sample slew limit.
        // The source starts at zero; the step path updates it from the solved
        // root voltage.
        opAmpVp_.assign(static_cast<size_t>(nOpAmp_), 0.0);
        opAmpAlpha_.assign(static_cast<size_t>(nOpAmp_), 0.0);
        opAmpMaxDv_.assign(static_cast<size_t>(nOpAmp_), 0.0);
        for (int i = 0; i < nOpAmp_; ++i) {
            const OpAmpModel& oa = opAmps_[static_cast<size_t>(i)];
            opAmpAlpha_[static_cast<size_t>(i)] = OpAmpEngineLaw::alpha(oa, fs_);
            opAmpMaxDv_[static_cast<size_t>(i)] = OpAmpEngineLaw::maxDv(oa, fs_);
        }

        bjts_.clear();
        for (const auto& bj : comp_.bjt)
            bjts_.push_back(bj.model);

        // `rebuildLinear()` calls `refreshRootGroups()`, which asks the root
        // device table for group offsets. Build that table here, before the
        // first transfer, not in `allocateBuffers()`.
        buildRootDeviceTable();

        net_ = buildNetwork(circuit_, comp_, T_);
        P_ = static_cast<int>(net_.eG.size());
    }

    bool embedOpAmpOutputResistance() {
        bool embedded = false;
        for (auto& model : opAmps_) {
            if (model.hasClassABOutput() || !(model.outputImpedance > 0.0) ||
                !std::isfinite(model.outputImpedance))
                continue;
            model.outputResistanceEmbedded = true;
            embedded = true;
        }
        if (embedded) {
            net_ = buildNetwork(circuit_, comp_, T_, true);
            P_ = static_cast<int>(net_.eG.size());
        }
        return embedded;
    }

    void buildRootDeviceTable() {
        rootDevices_.clear();
        junctionLimiters_.clear();
        gridJunctionLimiters_.clear();
        for (const auto& dev : rootLayout_.devices) {
            rootDevices_.push_back({deviceKind(dev.kind), dev.portBase, dev.numPorts,
                                    dev.activeRowBase, dev.numActiveRows,
                                    static_cast<size_t>(dev.compIndex)});
            switch (dev.kind) {
            case RootKind::Triode: {
                const auto& tri = comp_.tri[static_cast<size_t>(dev.compIndex)].model;
                if (tri.gridConducts) {
                    gridJunctionLimiters_.push_back({dev.portBase + 2, nullptr, &tri});
                }
                break;
            }
            case RootKind::Diode:
                junctionLimiters_.push_back(
                    {dev.portBase, &comp_.diode[static_cast<size_t>(dev.compIndex)].second});
                break;
            case RootKind::Pentode:
                // The pentode grid is a bare exponential; its owner is
                // `Pentode::limitGrid`. The plate
                // needs no junction limiter.
                gridJunctionLimiters_.push_back(
                    {dev.portBase + 1, &pentodes_[static_cast<size_t>(dev.compIndex)], nullptr});
                break;
            case RootKind::Bjt: {
                // BJT has base-emitter junction at portBase + 1 (base-emitter voltage)
                const BjtModel& bjt = bjts_[static_cast<size_t>(dev.compIndex)];
                junctionLimiters_.push_back(
                    {dev.portBase + 1, &bjt.diode, bjt.pnp});
                break;
            }
            case RootKind::OpAmp: {
                // Op-amp: no junction limiters needed for basic model
                break;
            }
            case RootKind::ControlledRes:
            case RootKind::Jfet:
            case RootKind::Mosfet:
                break;
            }
        }
    }

    // Flat precompiled execution structures for zero-allocation, register-resident streaming in step()
    struct LiveColInfo {
        int portIdx;
        int capIdx;
        int indIdx;
    };
    std::vector<LiveColInfo> liveColInfo_;

    struct CapUpdateBoth {
        int epA;
        int epB;
        int portIdx;
        int capIdx;
    };
    struct CapUpdateOne {
        int ep;
        int portIdx;
        int capIdx;
    };
    struct CapUpdateZero {
        int portIdx;
        int capIdx;
    };
    std::vector<CapUpdateBoth> capBoth_;
    std::vector<CapUpdateOne> capToGnd_;
    std::vector<CapUpdateOne> capFromGnd_;
    std::vector<CapUpdateZero> capZero_;

    struct IndUpdateBoth {
        int epA;
        int epB;
        int portIdx;
        int indIdx;
    };
    struct IndUpdateOne {
        int ep;
        int portIdx;
        int indIdx;
    };
    struct IndUpdateZero {
        int portIdx;
        int indIdx;
    };
    std::vector<IndUpdateBoth> indBoth_;
    std::vector<IndUpdateOne> indToGnd_;
    std::vector<IndUpdateOne> indFromGnd_;
    std::vector<IndUpdateZero> indZero_;

    struct SparseRow1 {
        int node;
        int col;
        double val;
    };
    struct SparseRow2 {
        int node;
        int col0;
        int col1;
        double val0;
        double val1;
    };
    struct SparseRowN {
        int node;
        int start;
        int end;
    };
    static constexpr int rootPanelWidth = 8;
    struct RootPanel {
        int node;
        int colStart;
        int colCount;
        size_t valueStart;
    };
    std::vector<RootPanel> rootPanels_;
    std::vector<double> rootPanelValues_;
    std::vector<SparseRow1> xrRows1_;
    std::vector<SparseRow2> xrRows2_;
    std::vector<SparseRowN> xrRowsN_;

    std::vector<SparseRow1> xeRows1_;
    std::vector<SparseRow2> xeRows2_;
    std::vector<SparseRowN> xeRowsN_;

    struct IsrcRow {
        int node;
        int src;
        double val;
    };
    std::vector<IsrcRow> isrcRows_;

    void refreshSparseTransfer() {
        xeRows1_.clear();
        xeRows2_.clear();
        xeRowsN_.clear();
        if (N_ > 0 && !xeRowOffsets_.empty()) {
            for (int n = 0; n < N_; ++n) {
                const int start = xeRowOffsets_[static_cast<size_t>(n)];
                const int end = xeRowOffsets_[static_cast<size_t>(n + 1)];
                const int deg = end - start;
                if (deg == 1) {
                    xeRows1_.push_back({n, xeCols_[static_cast<size_t>(start)], xeVals_[static_cast<size_t>(start)]});
                } else if (deg == 2) {
                    xeRows2_.push_back({n,
                                        xeCols_[static_cast<size_t>(start)],
                                        xeCols_[static_cast<size_t>(start + 1)],
                                        xeVals_[static_cast<size_t>(start)],
                                        xeVals_[static_cast<size_t>(start + 1)]});
                } else if (deg > 2) {
                    xeRowsN_.push_back({n, start, end});
                }
            }
        }

        rootPanels_.clear();
        rootPanelValues_.clear();
        xrRows1_.clear();
        xrRows2_.clear();
        xrRowsN_.clear();
        if (N_ > 0 && !xrRowOffsets_.empty()) {
            for (int n = 0; n < N_; ++n) {
                const int start = xrRowOffsets_[static_cast<size_t>(n)];
                const int end = xrRowOffsets_[static_cast<size_t>(n + 1)];
                const int deg = end - start;
                // Adjacent nodes often depend on exactly the same root currents.
                // Store each coefficient column contiguously across those nodes
                // so the audio loop can accumulate independent voltages in SIMD.
                if (deg >= 4 && n + rootPanelWidth <= N_) {
                    bool shared = true;
                    for (int q = 1; q < rootPanelWidth && shared; ++q) {
                        const int first = xrRowOffsets_[n + q];
                        shared = xrRowOffsets_[n + q + 1] - first == deg &&
                                 std::equal(xrCols_.begin() + start, xrCols_.begin() + end,
                                            xrCols_.begin() + first);
                    }
                    if (shared) {
                        rootPanels_.push_back({n, start, deg, rootPanelValues_.size()});
                        for (int j = 0; j < deg; ++j)
                            for (int q = 0; q < rootPanelWidth; ++q)
                                rootPanelValues_.push_back(xrVals_[xrRowOffsets_[n + q] + j]);
                        n += rootPanelWidth - 1;
                        continue;
                    }
                }
                if (deg == 1) {
                    xrRows1_.push_back({n, xrCols_[static_cast<size_t>(start)], xrVals_[static_cast<size_t>(start)]});
                } else if (deg == 2) {
                    xrRows2_.push_back({n,
                                        xrCols_[static_cast<size_t>(start)],
                                        xrCols_[static_cast<size_t>(start + 1)],
                                        xrVals_[static_cast<size_t>(start)],
                                        xrVals_[static_cast<size_t>(start + 1)]});
                } else if (deg > 2) {
                    xrRowsN_.push_back({n, start, end});
                }
            }
        }

        // Each independent source touches only its own nodes, so the dense
        // N-by-nIsrc multiply in step() is almost entirely zeros. Keep only
        // the nonzero entries; this also leaves the generic dense Xis_ intact
        // for the transfer API.
        isrcRows_.clear();
        if (N_ > 0 && nIsrc_ > 0 &&
            Xis_.size() >= static_cast<size_t>(N_) * static_cast<size_t>(nIsrc_)) {
            for (int n = 0; n < N_; ++n) {
                const double* row =
                    &Xis_[static_cast<size_t>(n) * static_cast<size_t>(nIsrc_)];
                for (int k = 0; k < nIsrc_; ++k) {
                    const double v = row[static_cast<size_t>(k)];
                    if (v != 0.0)
                        isrcRows_.push_back({n, k, v});
                }
            }
        }
    }

    struct RootPortInfo {
        int a;
        int b;
    };
    std::vector<RootPortInfo> rootPortInfo_;

    struct CoupledOffset {
        int targetIdx;
        double c0;
        int nCoef;
        int idx0;
        int idx1;
        double c1;
        double c2;
    };
    std::vector<CoupledOffset> coupledOffsets_;

    struct CoupledRoot {
        int targetIdx;
        double c0;
        int nCoef;
        int idx0;
        int idx1;
        double c1;
        double c2;
    };
    std::vector<CoupledRoot> coupledRoots_;

    /// Size every working buffer. Runs after the transfer is built.
    void allocateBuffers() {
        b_.assign(P_, 0.0);
        w_.assign(static_cast<size_t>(nLive_), 0.0);
        eoff_.assign(P_, 0.0);
        for (int j = 0; j < P_; ++j)
            eoff_[static_cast<size_t>(j)] = net_.eoffAff[static_cast<size_t>(j)].c0;
        u_.assign(static_cast<size_t>(N_) + 1, 0.0);
        vOc_.assign(K_, 0.0);
        roff_.assign(K_, 0.0);
        for (size_t r = 0; r < net_.roffAff.size(); ++r)
            roff_[r] = net_.roffAff[r].c0;
        vv_.assign(K_, 0.0);
        vvSafe_.assign(K_, 0.0);
        triodeDomainPin_.assign(K_, 0);
        gridLag_.assign(comp_.tri.empty() || !comp_.tri[0].model.gridConducts ? 0 : static_cast<size_t>(3 * K_), 0.0);
        vvPrev_.assign(K_, 0.0);
        vvPrev2_.assign(K_, 0.0);
        ii_.assign(K_, 0.0);
        iiSafe_.assign(K_, 0.0);
        bestV_.assign(K_, 0.0);
        bestI_.assign(K_, 0.0);
        J_.assign(static_cast<size_t>(K_) * K_, 0.0);
        node_.assign(N_, 0.0);
        nodeSafe_.assign(N_, 0.0);
        F_.assign(K_, 0.0);
        dv_.assign(K_, 0.0);
        A_.assign(static_cast<size_t>(K_) * K_, 0.0);
        {
            // The root device table was built before the first transfer, because
            // `refreshRootGroups()` needs it. Only the reduced-system buffers
            // depend on the transfer and are sized here.
            nRed_ = rootLayout_.nRed;
            Mr_.assign(static_cast<size_t>(nRed_) * nRed_, 0.0);
            rN_.assign(static_cast<size_t>(nRed_), 0.0);
            redCol_ = rootLayout_.redCol;
            redRow_ = rootLayout_.redRow;
            blockBase_.assign(K_, 0.0);
            blockI_.assign(K_, 0.0);
            blockJ_.assign(static_cast<size_t>(K_) * K_, 0.0);
            blockF_.assign(K_, 0.0);
            blockV_.assign(K_, 0.0);
            blockVOld_.assign(K_, 0.0);
            blockDv_.assign(K_, 0.0);
            activeRedCol_.assign(static_cast<size_t>(nRed_), 0);
        redToActive_.assign(static_cast<size_t>(nRed_), -1);
        blockM_.assign(static_cast<size_t>(nRed_) * nRed_, 0.0);
        blockRhs_.assign(static_cast<size_t>(nRed_), 0.0);
        compileZred();
            updatePortLimits();
        }
        vt_.assign(K_, 0.0);
        ft_.assign(K_, 0.0);
        iv_.assign(K_, 0.0);
        Jt_.assign(static_cast<size_t>(K_) * K_, 0.0);
        capState_.assign(nCaps_, 0.0);
        indState_.assign(nInds_, 0.0);
        isCur_.assign(static_cast<size_t>(nIsrc_), 0.0);

        // Pre-compile execution tables for step() to eliminate pointer chasing and branch loops
        liveColInfo_.resize(static_cast<size_t>(nLive_));
        for (int t = 0; t < nLive_; ++t) {
            const int j = net_.liveCols[static_cast<size_t>(t)];
            liveColInfo_[static_cast<size_t>(t)] = {
                j,
                net_.capOf[static_cast<size_t>(j)],
                net_.indOf[static_cast<size_t>(j)]
            };
        }

        capBoth_.clear();
        capToGnd_.clear();
        capFromGnd_.clear();
        capZero_.clear();
        for (size_t j = 0; j < net_.capOf.size(); ++j) {
            const int c = net_.capOf[j];
            if (c >= 0) {
                const int a = net_.epA[j];
                const int b = net_.epB[j];
                const int pj = static_cast<int>(j);
                if (a >= 0 && b >= 0)
                    capBoth_.push_back({a, b, pj, c});
                else if (a >= 0)
                    capToGnd_.push_back({a, pj, c});
                else if (b >= 0)
                    capFromGnd_.push_back({b, pj, c});
                else
                    capZero_.push_back({pj, c});
            }
        }

        indBoth_.clear();
        indToGnd_.clear();
        indFromGnd_.clear();
        indZero_.clear();
        for (size_t j = 0; j < net_.indOf.size(); ++j) {
            const int l = net_.indOf[j];
            if (l >= 0) {
                const int a = net_.epA[j];
                const int b = net_.epB[j];
                const int pj = static_cast<int>(j);
                if (a >= 0 && b >= 0)
                    indBoth_.push_back({a, b, pj, l});
                else if (a >= 0)
                    indToGnd_.push_back({a, pj, l});
                else if (b >= 0)
                    indFromGnd_.push_back({b, pj, l});
                else
                    indZero_.push_back({pj, l});
            }
        }

        rootPortInfo_.resize(static_cast<size_t>(K_));
        for (int r = 0; r < K_; ++r) {
            const int a = net_.rp[static_cast<size_t>(r)][0];
            const int b = net_.rp[static_cast<size_t>(r)][1];
            rootPortInfo_[static_cast<size_t>(r)] = {
                a >= 0 ? a : N_,
                b >= 0 ? b : N_
            };
        }

        coupledOffsets_.clear();
        for (int j : net_.coupled) {
            const auto& aff = net_.eoffAff[static_cast<size_t>(j)];
            coupledOffsets_.push_back({
                j,
                aff.c0,
                aff.nCoef,
                aff.idx0,
                aff.idx1,
                aff.c1,
                aff.c2
            });
        }

        coupledRoots_.clear();
        for (int r : net_.coupledRoots) {
            const auto& aff = net_.roffAff[static_cast<size_t>(r)];
            coupledRoots_.push_back({
                r,
                aff.c0,
                aff.nCoef,
                aff.idx0,
                aff.idx1,
                aff.c1,
                aff.c2
            });
        }
        refreshSparseTransfer();
    }

    double acrossPort(size_t j, const std::vector<double>& vec) const {
        return (net_.epA[j] >= 0 ? vec[static_cast<size_t>(net_.epA[j])] : 0.0) -
               (net_.epB[j] >= 0 ? vec[static_cast<size_t>(net_.epB[j])] : 0.0);
    }
    static double acrossVec(const std::array<int, 2>& p, const std::vector<double>& v) {
        return (p[0] >= 0 ? v[static_cast<size_t>(p[0])] : 0.0) -
               (p[1] >= 0 ? v[static_cast<size_t>(p[1])] : 0.0);
    }
    static double acrossMat(const std::array<int, 2>& p, const Dense& m, int col) {
        return (p[0] >= 0 ? m[static_cast<size_t>(p[0])][static_cast<size_t>(col)] : 0.0) -
               (p[1] >= 0 ? m[static_cast<size_t>(p[1])][static_cast<size_t>(col)] : 0.0);
    }

    static double maxAbs(const std::vector<double>& v, int n) {
        double m = 0.0;
        for (int i = 0; i < n; ++i) {
            const double a = std::fabs(v[static_cast<size_t>(i)]);
            // A non-finite entry has to survive the reduction. `std::max` ranks
            // NaN below everything, so a residual array that is all NaN would
            // reduce to 0 and every caller below would report convergence and a
            // zero residual for a network that produced nothing. A residual the
            // network cannot represent is not a residual: propagate it, and the
            // line search and the convergence test both refuse it.
            if (std::isnan(a))
                return a;
            m = std::max(m, a);
        }
        return m;
    }

    void compileZred() {
        if (K_ <= 0 || nRed_ <= 0 || Z_.empty() || redCol_.empty())
            return;
        Zred_.assign(static_cast<size_t>(K_) * static_cast<size_t>(nRed_), 0.0);
        const double* zData = Z_.data();
        const int* rc = redCol_.data();
        double* zrData = Zred_.data();
        for (int r = 0; r < K_; ++r) {
            const double* zRow = zData + static_cast<size_t>(r) * K_;
            double* zrRow = zrData + static_cast<size_t>(r) * nRed_;
            for (int j = 0; j < nRed_; ++j) {
                zrRow[j] = zRow[rc[j]];
            }
        }
    }

    // ------------------------------------------------------------------ //
    // State.
    // ------------------------------------------------------------------ //
    Circuit circuit_;
    Compiled comp_;
    double fs_, T_;
    int N_ = 0, P_ = 0, K_ = 0, nCaps_ = 0, nInds_ = 0, nIsrc_ = 0, nsrc_ = 0, nVCVS_ = 0;
    int nTri_ = 0, nD_ = 0, nCtrl_ = 0, nPent_ = 0, nJfet_ = 0, nMosfet_ = 0, nBjt_ = 0, nOpAmp_ = 0;
    std::vector<double> ctrlResValues_;
    std::vector<std::string> ctrlResNames_;
    struct PhotoresistorState {
        size_t index;
        int sensePos, senseNeg;
        AffineOffset senseOffset;
        double attack, release;
    };
    std::vector<PhotoresistorState> photoresistors_;
    struct VcaState {
        size_t index;
        int control, reference;
        AffineOffset controlOffset;
    };
    std::vector<VcaState> vcas_;
    struct SensePort {
        int pos, neg;
        AffineOffset offset;

        double voltage(const std::vector<double>& nodes, const double* sources,
                       std::size_t sourceCount) const noexcept {
            return (pos >= 0 ? nodes[static_cast<std::size_t>(pos)] : 0.0) -
                   (neg >= 0 ? nodes[static_cast<std::size_t>(neg)] : 0.0) +
                   offset.eval(sources, sourceCount);
        }
    };
    struct Dbx200BiasState {
        size_t pnp, npn;
        SensePort bases;
    };
    std::vector<Dbx200BiasState> dbx200Biases_;
    struct OtaState {
        std::size_t index;
        SensePort bias;
    };
    std::vector<OtaState> otas_;
    struct BucketBrigadeConnection {
        std::size_t outputCurrentIndex;
        SensePort input, clock;
        BucketBrigadeState delay;
    };
    std::vector<BucketBrigadeConnection> bucketBrigades_;
    struct RmsDetectorConnection {
        std::size_t outputCurrentIndex;
        SensePort input;
        RmsDetectorState detector;
    };
    std::vector<RmsDetectorConnection> rmsDetectors_;
    /// Resistor indices a pot/rheostat writes, resolved in the constructor.
    std::vector<int> potResA_, potResB_, varResResIndex_;
    std::vector<Pentode> pentodes_;
    std::vector<OpAmpModel> opAmps_;
    // Runtime state for the op-amp three-port law. `opAmpVp_` is the internal
    // dominant-pole source voltage in port space (volts above `vee`).
    std::vector<double> opAmpVp_, opAmpAlpha_, opAmpMaxDv_;
    bool powerSagEnabled_ = false;
    std::vector<PowerSupply> opAmpPsuPos_;
    std::vector<PowerSupply> opAmpPsuNeg_;
    std::vector<bool> opAmpSagActive_;
    std::vector<double> opAmpNominalRailPos_;
    std::vector<double> opAmpNominalRailNeg_;
    // True while `solveDc` runs: reactive device states are at their operating
    // point. The op-amp uses its full open-loop DC gain with no slew clamp.
    mutable bool staticMode_ = false;
    std::vector<BjtModel> bjts_;
    FactoredTransfer networkFactors_;
    int nLive_ = 0;

    Network net_;

    std::vector<double> Xe_, Xr_, Z_, Xis_;
    std::vector<int> xeRowOffsets_, xeCols_;
    std::vector<double> xeVals_;
    std::vector<int> xrRowOffsets_, xrCols_;
    std::vector<double> xrVals_;
    std::vector<double> eoff_, u_, vOc_, roff_;
    std::vector<double> vv_, ii_, J_, vvPrev_, vvPrev2_;
    // Last state accepted by `step()`. A non-converged or non-physical iterate
    // is rejected and these are restored before the next sample, so a bad
    // topology-switch transient cannot accumulate in the reactive state.
    std::vector<double> vvSafe_, iiSafe_;
    std::vector<double> bestV_, bestI_;
    std::vector<double> F_, dv_, A_, vt_, ft_, iv_, Jt_;
    std::vector<double> Mr_, rN_;
    RootLayout rootLayout_;
    int nRed_ = 0;
    std::vector<int> redCol_, redRow_;
    std::vector<double> Zred_;
    std::vector<RootDevice> rootDevices_;
    std::vector<NewtonBlock> newtonBlocks_;
    /// Experimental runtime partition, selected per sample when enabled. It has
    /// the same shape as `newtonBlocks_` and its live ports follow the static
    /// model capability.
    std::vector<NewtonBlock> runtimeBlocks_;
    std::vector<int> runtimeOrder_;
    std::vector<RuntimePartition> runtimePartitions_;
    const RuntimePartition* runtimePartition_ = nullptr;
    std::vector<unsigned char> blockReach_, blockAdj_;
    std::vector<int> blockOf_;
    std::vector<double> blockBase_, blockI_, blockJ_, blockF_, blockV_, blockVOld_, blockDv_;
    std::vector<int> activeRedCol_;
    /// Mapping from static redCol index to dynamic active index, -1 if inactive
    std::vector<int> redToActive_;
    std::vector<double> blockM_, blockRhs_;
    int blockCount_ = 0;
    std::vector<JunctionLimiter> junctionLimiters_;
    std::vector<GridJunctionLimiter> gridJunctionLimiters_;
    std::vector<double> predDown_, predUp_, stepDown_, stepUp_;
    std::vector<double> dcValues_;
    mutable long long blockFallbacks_ = 0;
    mutable long long runtimeBlockFallbacks_ = 0;
    bool lastBlockFallback_ = false;
    double lastBlockResidual_ = 0.0;
    bool runtimeBlockEnabled_ = false;
    bool runtimeBlockUsed_ = false;
    int runtimeBlockCount_ = 0;
    int runtimeGateCooldown_ = 0;
    int blockFallbackCooldown_ = 0;  // joint samples after a static-block fallback
    int blockFallbackCooldownLimit_ = 16;  // env-overridable A/B knob (0..64)
    /// Experimental relaxed stage partition: non-scalar SCC blocks are split by
    /// the directed signal-flow analysis and solved with residual-verified
    /// Gauss-Seidel sweeps. Off by default; the production SCC + Schur path is
    /// faster on the measured pedals and the poweramp remains one global-feedback
    /// SCC. The joint Newton is always the fallback and reference.
    bool fineBlockSolver_ = false;
    long long jointLastIters_ = 0;
    long long jointLastTrials_ = 0;
    bool lastConverged_ = false;
    bool lastStateRejected_ = false;
    long long stateRejections_ = 0;
    int consecutiveStateRejects_ = 0;
    double lastResidual_ = 0.0;
    int lastMaxNodeIndex_ = -1;
    double lastMaxNodeVolts_ = 0.0;
    int lastRejectNodeIndex_ = -1;
    double lastRejectNodeVolts_ = 0.0;
    bool keptBest_ = false;
    bool keepBestOn_ = true;
    double bestFn_ = 0.0;
    Limits lim_{};

    /// Per-sample domain decision for each triode's plate port: 0 = quadric
    /// law, 1 = junction wire of the port impedance (`i = v/Z`), 2 = cold
    /// (no positive port impedance to project onto). The paper decides its
    /// domain state once after solving, on the *solved* plate voltage; the
    /// Newton rows must not re-decide at every iterate, since for an
    /// out-of-domain excursion of the quadric surface with a positive open
    /// plate voltage the fixed points of the two branches lie on opposite
    /// sides of `vpk = 0` and there is no fixed point of the branch-flipping
    /// law at all. Indexed by plate port.
    std::vector<unsigned char> triodeDomainPin_;
    std::vector<double> gridLag_;  // previous, predicted and first-pass grid currents

    std::vector<int> groupOrder_;
    std::vector<int> grpIndeg_;
    std::vector<int> grpQueue_;
    std::vector<DiodeModel> grpDiode_;
    int groupCount_ = 0;
    bool explicitRootsAllowed_ = true;
    bool explicitRootsActive_ = false;
    /// True when every Newton block is solved exactly: a closed-form scalar
    /// singleton, or an anti-parallel diode pair whose two port equations
    /// collapse to one scalar solve. `explicitRootsActive_` requires it, so the
    /// flag means "the whole solve is closed form", not "a partition exists".
    bool allClosedFormBlocks_ = false;
    /// Number of scalar-capable singleton blocks. The block Newton only pays
    /// when it can mix at least two of these cheap closed forms with local
    /// non-scalar work; the measured two-block case (one scalar, one
    /// non-scalar) and all-non-scalar cascades are neutral or slower, so they
    /// stay on the joint path.
    int scalarBlockCount_ = 0;
    bool hasAntiParallelBlock_ = false;
    /// True when at least one block has an affine-device reduction (op-amps or
    /// controlled resistors eliminated from the Newton unknowns). A single
    /// such block is worth the block path even though `blockCount_ == 1`.
    bool mixedAffineEnabled_ = true;
    long long mixedAffineAttempts_ = 0;
    long long mixedAffineAccepted_ = 0;
    bool hasLinearReduction_ = false;

    Dense linY_, linRhs_, linXe_, linXr_;
};

}  // namespace wdf

