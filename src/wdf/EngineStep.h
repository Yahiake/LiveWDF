#pragma once

// Level 4 of the engine: one sample.
//
// `reset()` seeds the reactive states from the DC operating point, and `step()`
// advances exactly one sample. `step()` is written once against both root solvers:
// it builds the waves and the node voltages, hands the root currents to whichever
// solve is active (`EngineSolve::solveRootsNewton` or the exact
// `solveRootGroups`), then reconstructs the nodes and stores the new states.
// Neither path is special-cased anywhere else.
//
// Allocation-free: every buffer it touches is sized in `EngineCore`.

#include "wdf/EngineCompile.h"

namespace wdf {

class EngineStep : public EngineCompile {
public:
    bool hasCompiledOptical() const noexcept { return sharedLamps_; }
    void useCompiledOptical(bool enabled) noexcept { compiledOptical_ = enabled && sharedLamps_; }

    /// Seed the reactive states so sample 1 reproduces the DC operating point.
    void reset() {
        circuitPlanCooldown_ = 0;
        affineRootCooldown_ = 0;
        affineRootRetry_ = 8;
        circuitPlanRetry_ = 8;
        resetControlledDevices();
        std::vector<double> node, rootV;
        const bool haveDc =
            solveDc(node, rootV) && node.size() >= static_cast<size_t>(N_) &&
            rootV.size() >= static_cast<size_t>(K_);
        if (haveDc && (!photoresistors_.empty() || !vcas_.empty() || !otas_.empty())) {
            // Seed the controllers from their DC operating point, then settle
            // the signal path at those initial control settings.
            for (const auto& photo : photoresistors_)
                ctrlResValues_[photo.index] = 1.0 / photoTargetConductance(
                    photo, node, dcValues_.data(), dcValues_.size());
            updateVcas(node, dcValues_.data(), dcValues_.size());
            updateOtas(node, dcValues_.data(), dcValues_.size());
            std::vector<double> litNode, litRootV;
            if (solveDc(litNode, litRootV)) {
                node = std::move(litNode);
                rootV = std::move(litRootV);
            } else {
                resetControlledDevices();
            }
        }
        if (!haveDc) {
            std::vector<double> biasNode;
            if (solveNetworkBias(biasNode)) {
                node = std::move(biasNode);
                rootV.assign(static_cast<size_t>(K_), 0.0);
            } else {
                node.assign(static_cast<size_t>(N_), 0.0);
                rootV.assign(static_cast<size_t>(K_), 0.0);
            }
        } else if (bucketBrigades_.empty() && rmsDetectors_.empty()) {
            std::vector<double> biasNode;
            if (solveNetworkBias(biasNode)) {
                for (int n = 0; n < N_; ++n) {
                    if (std::abs(node[static_cast<size_t>(n)]) < 1e-9 &&
                        std::abs(biasNode[static_cast<size_t>(n)]) > 1e-3) {
                        node[static_cast<size_t>(n)] = biasNode[static_cast<size_t>(n)];
                    }
                }
            }
        }
        for (std::size_t i = 0; i < bucketBrigades_.size(); ++i) {
            auto& bbd = bucketBrigades_[i];
            const double input = haveDc ?
                bbd.input.voltage(node, dcValues_.data(), dcValues_.size()) :
                comp_.bucketBrigades[i].model.inputBias;
            bbd.delay.reset(input);
        }

        for (auto& rms : rmsDetectors_)
            rms.detector.reset(haveDc ? rms.input.voltage(node, dcValues_.data(), dcValues_.size())
                                     : 0.0);

        // The closed-form root groups use the previous sample's current as
        // their branch-continuity seed. Seed it from the DC operating point so
        // sample 1 starts on the same branch as `solveDc`, rather than on
        // whatever `ii_` happened to contain before reset.
        if (haveDc && K_ > 0) {
            std::vector<double> i, J;
            // The DC operating point uses the static op-amp law (no GBW
            // state, no slew). Compute the matching currents so the first
            // transient sample starts exactly on that branch.
            const bool savedStatic = staticMode_;
            staticMode_ = true;
            currents(rootV, i, J);
            staticMode_ = savedStatic;
            if (i.size() == static_cast<size_t>(K_)) {
                ii_ = i;
                // The internal source is the output port voltage plus the
                // drop across the output resistance: vSource = vOut - Ro*iOut.
                for (const auto& dev : rootDevices_) {
                    if (dev.kind != DeviceKind::OpAmp)
                        continue;
                    const size_t ci = dev.compIndex;
                    const int o = dev.portBase;
                    const auto& model = opAmps_[ci];
                    if (model.hasClassABOutput()) {
                        const double vDiff = rootV[static_cast<size_t>(o + 1)] -
                                             rootV[static_cast<size_t>(o + 2)];
                        opAmpVp_[ci] = model.evalStatic(vDiff);
                    } else {
                        opAmpVp_[ci] = model.sourceVoltageFromSolverCurrent(
                            rootV[static_cast<size_t>(o)], i[static_cast<size_t>(o)]);
                    }
                }
            }
            vv_ = rootV;
            vvPrev_ = rootV;
            vvPrev2_ = rootV;
        }
        if (powerSagEnabled_) {
            for (size_t i = 0; i < opAmpSagActive_.size(); ++i) {
                if (opAmpSagActive_[i]) {
                    opAmpPsuPos_[i].reset();
                    opAmpPsuNeg_[i].reset();
                    if (i < opAmps_.size() && i < opAmpNominalRailPos_.size()) {
                        opAmps_[i].setVRails(opAmpNominalRailPos_[i], opAmpNominalRailNeg_[i]);
                    }
                }
            }
        }
        for (int j = 0; j < P_; ++j) {
            if (net_.capOf[j] < 0)
                continue;
            const auto k = static_cast<size_t>(net_.capOf[j]);
            capState_[k] = circuit_.cap[k].initialVolts.value_or(
                acrossPort(static_cast<size_t>(j), node) +
                net_.eoffAff[static_cast<size_t>(j)].eval(dcValues_));
        }
        // Inductor state is the incident wave `a = i * 2L/T` (the port resistance
        // is 2L/T), or `a = R_L * i` for coupled groups. Seeding it is what makes
        // sample 1 reproduce the operating point instead of settling from zero over tau.
        const int nInd = static_cast<int>(comp_.ind.size());
        std::vector<double> iDc(static_cast<size_t>(nInd), 0.0);
        for (int j = 0; j < P_; ++j) {
            if (net_.indOf[j] < 0)
                continue;
            const double vPort =
                acrossPort(static_cast<size_t>(j), node) + net_.eoffAff[static_cast<size_t>(j)].eval(dcValues_);
            iDc[static_cast<size_t>(net_.indOf[j])] = vPort / kIndDcR;
        }
        for (int k = 0; k < nInd; ++k) {
            const double L = comp_.ind[static_cast<size_t>(k)].second;
            indState_[static_cast<size_t>(k)] = iDc[static_cast<size_t>(k)] * 2.0 * L / T_;
        }
        for (const auto& grp : net_.coupledGroups) {
            const size_t W = grp.indIndices.size();
            for (size_t r = 0; r < W; ++r) {
                double a = 0.0;
                for (size_t c = 0; c < W; ++c)
                    a += grp.R[r][c] * iDc[static_cast<size_t>(grp.indIndices[c])];
                indState_[static_cast<size_t>(grp.indIndices[r])] = a;
            }
        }
        // A reset starts a new solve history: the runtime gate's evidence from
        // the old history would describe a different operating point.
        runtimeGateCooldown_ = runtimeBlockEnabled_ ? 1 : 0;
        blockFallbackCooldown_ = 0;
        jointLastIters_ = 0;
        jointLastTrials_ = 0;
        runtimeBlockUsed_ = false;
        runtimeBlockCount_ = 0;
        runtimePartition_ = nullptr;
    }

    static constexpr int kDefaultMaxIterations = 60;
    static constexpr double kDefaultTolerance = 1e-9;
    static constexpr double kFastLiveTolerance = 1e-7;

    /// Compile at the current operating point outside the audio callback.
    void prepareCircuitPlan();
    void useCircuitPlan(bool enabled) noexcept { circuitPlanEnabled_ = enabled; }
    bool circuitPlanPrepared() const noexcept { return circuitPlanRevision_ == linearRebuilds(); }
    bool hasLinearDrivePlan() const noexcept {
        return circuitPlanRevision_ == linearRebuilds() && !driveCoefficients_.empty();
    }
    bool hasAffineRootPlan() const noexcept {
        return circuitPlanPrepared() && !hasAdjustedLiveTransfer() && !affineRoot_.rows.empty();
    }
    long long affineRootAttempts() const noexcept { return affineRootAttempts_; }
    long long affineRootHits() const noexcept { return affineRootHits_; }
    long long circuitPlanAttempts() const noexcept { return circuitPlanAttempts_; }
    long long circuitPlanHits() const noexcept { return circuitPlanHits_; }

    /// Advance one sample. Returns the node voltages (indexed like `nodeNames()`).
    const std::vector<double>& step(const std::vector<double>& sourceValues,
                                    int maxIter = kDefaultMaxIterations,
                                    double tol = kDefaultTolerance) {
        return step(sourceValues.data(), sourceValues.size(), maxIter, tol);
    }

    /// Same, for a caller that already holds the values as a pointer. The
    /// runtime facade uses this so its `step()` does not copy the source vector.
    // Keep the numeric path compiled once for the engine, plug-in, and audit.
    const std::vector<double>& step(const double* sourceValues, std::size_t sourceCount,
                                    int maxIter = kDefaultMaxIterations,
                                    double tol = kDefaultTolerance);

protected:
    struct AffineRootPlan {
        struct CurrentRow {
            int port, pole;
            double conductance, differential, poleGain;
        };
        std::vector<CurrentRow> rows;
        std::vector<double> inverse, impedance, drive, current;
    };
    void prepareAffineRoot();
    bool opAmpsOnLinearBranch(const std::vector<double>& voltage) const noexcept;
    bool tryAffineRoot(double tolerance) noexcept;
    bool circuitRootContinuous() const noexcept;
    void acceptCircuitRoot(double error) noexcept;
    AffineRootPlan affineRoot_;
    std::vector<double> certifiedResistance_;
    long long affineRootAttempts_ = 0, affineRootHits_ = 0;
    int affineRootCooldown_ = 0, affineRootRetry_ = 8;
    bool factorCircuitTangent(const std::vector<double>& jacobian) noexcept;
    bool tryCompiledRoot(double tolerance) noexcept;
    bool applyCompiledDrive(const double* sources, size_t count) noexcept;
    std::vector<double> driveCoefficients_, driveInputs_, rootFactors_, circuitResidualColumns_;
    std::vector<int> rootPivots_;
    long long circuitPlanRevision_ = -1, circuitPlanAttempts_ = 0, circuitPlanHits_ = 0;
    int circuitPlanCooldown_ = 0, circuitPlanRetry_ = 8;
    bool circuitPlanEnabled_ = false;
    void resetControlledDevices() {
        for (const auto& photo : photoresistors_)
            ctrlResValues_[photo.index] = circuit_.ctrlRes[photo.index].initialOhms;
        for (const auto& vca : vcas_)
            ctrlResValues_[vca.index] = circuit_.ctrlRes[vca.index].initialOhms;
        for (const auto& bias : dbx200Biases_) {
            comp_.dbx200Crossover[bias.pnp] = circuit_.ctrlRes[bias.pnp].dbx200Crossover;
            comp_.dbx200Crossover[bias.npn] = circuit_.ctrlRes[bias.npn].dbx200Crossover;
        }
        for (const auto& ota : otas_)
            ctrlResValues_[ota.index] = circuit_.ctrlRes[ota.index].initialOhms;
    }

    void updateOtas(const std::vector<double>& nodes, const double* sources,
                    std::size_t sourceCount) {
        // Hold the slow input bias current for the next solve. The signal
        // differential and its tanh current law are solved together.
        for (const auto& ota : otas_)
            ctrlResValues_[ota.index] =
                OtaControl::slopeResistance(ota.bias.voltage(nodes, sources, sourceCount));
    }

    void updateVcas(const std::vector<double>& nodes, const double* sources,
                    size_t sourceCount) {
        // The control comes from the previous accepted circuit sample. The
        // signal current itself is solved on the current sample as a root.
        for (const auto& vca : vcas_) {
            const double control =
                (vca.control >= 0 ? nodes[static_cast<size_t>(vca.control)] : 0.0) -
                (vca.reference >= 0 ? nodes[static_cast<size_t>(vca.reference)] : 0.0) +
                vca.controlOffset.eval(sources, sourceCount);
            const auto& ctrl = circuit_.ctrlRes[vca.index];
            ctrlResValues_[vca.index] = ctrl.initialOhms * ctrl.vca->initialGain() /
                                       ctrl.vca->normalizedGain(control);
        }
        for (const auto& bias : dbx200Biases_) {
            const double v = bias.bases.voltage(nodes, sources, sourceCount);
            const double scale = std::exp(std::clamp(v / Dbx200Bias::twiceThermalVolts, -20.0, 20.0));
            comp_.dbx200Crossover[bias.pnp] = circuit_.ctrlRes[bias.pnp].dbx200Crossover * scale;
            comp_.dbx200Crossover[bias.npn] = circuit_.ctrlRes[bias.npn].dbx200Crossover * scale;
        }
    }

    double photoSenseVoltage(const PhotoresistorState& state, const std::vector<double>& nodes,
                             const double* sources, size_t sourceCount) const {
        return (state.sensePos >= 0 ? nodes[static_cast<size_t>(state.sensePos)] : 0.0) -
            (state.senseNeg >= 0 ? nodes[static_cast<size_t>(state.senseNeg)] : 0.0) +
            state.senseOffset.eval(sources, sourceCount);
    }

    double photoTargetConductance(const PhotoresistorState& state,
                                  const std::vector<double>& nodes,
                                  const double* sources, size_t sourceCount) const {
        const auto& ctrl = circuit_.ctrlRes[state.index];
        return ctrl.photo->targetConductance(photoSenseVoltage(state, nodes, sources, sourceCount),
                                             ctrl.initialOhms);
    }

    /// A sample may only commit reactive state after a verified solve.
    /// `lastConverged_` is the solver's certification; a finite residual alone
    /// is not proof that the original system was solved. The explicit
    /// closed-form roots deliberately leave the residual unevaluated (NaN),
    /// but their converged flag is still mandatory.
    bool stateAcceptable() const {
        if (!lastConverged_)
            return false;
        if (std::isnan(lastResidual_))
            return explicitRootsActive_;
        return std::isfinite(lastResidual_) && lastResidual_ <= 1e12;
    }

    /// Forwards construction down the chain (see `EngineSolve`).
    using EngineCompile::EngineCompile;
};

}  // namespace wdf
