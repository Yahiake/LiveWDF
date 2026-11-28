#pragma once

#include "wdf/LoadedJunctionPlan.h"

// Reset seeds the reactive states from the DC operating point. Step advances
// one sample using the prepared root solver and its fallback.

#include "wdf/EngineCompile.h"

#include <optional>

namespace wdf {

class EngineStep : public EngineCompile {
public:
    bool hasCompiledOptical() const noexcept { return sharedLamps_; }
    void useCompiledOptical(bool enabled) noexcept { compiledOptical_ = enabled && sharedLamps_; }

    /// Seed the reactive states so sample 1 reproduces the DC operating point.
    void reset() {
        prepareHeldRails();
        resetPreviewState();
        std::vector<double> node, rootV;
        const bool haveDc =
            solveDc(node, rootV) && node.size() >= static_cast<size_t>(N_) &&
            rootV.size() >= static_cast<size_t>(K_);
        if (haveDc && (!photoresistors_.empty() || !vcas_.empty() || !otas_.empty())) {
            // Seed the controllers from their DC operating point, then settle
            // the signal path at those initial control settings.
            for (auto& photo : photoresistors_)
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
        saveRecoveryState(haveDc && K_ > 0);
        resetSolverHistory();
        if (!haveDc) {
            // An oscillator has no stable DC point. Run it silently for two
            // seconds so its coupling capacitors start where the running circuit has them.
            // Stop before a stalled settle re-anchors: without a DC point that re-anchor is this reset again.
            const std::vector<double> sources = dcValues_;
            for (long n = static_cast<long>(2.0 / T_); n > 0 && consecutiveStateRejects_ < 8; --n)
                step(sources);
        }
    }

    static constexpr int kDefaultMaxIterations = 60;
    static constexpr double kDefaultTolerance = 1e-9;
    static constexpr double kFastLiveTolerance = 1e-7;

    /// Prepare the nonlinear root preview at the current operating point off the audio thread.
    bool prepareLoadedJunctionPreview(size_t inputSource = size_t(-1), bool screenBenefit = true);
    void useLoadedJunctionPreview(bool enabled) noexcept {
        if (loadedJunctionEnabled_ != enabled) loadedJunction_.seed = true;
        loadedJunctionEnabled_ = enabled;
    }
    long long affineNodePreviewHits() const noexcept { return affineNodeHits_; }
    bool hasLoadedJunctionPreview() const noexcept;
    long long loadedJunctionAttempts() const noexcept { return loadedJunctionAttempts_; }
    long long loadedJunctionHits() const noexcept { return loadedJunctionHits_; }

    void prepareCircuitPlan();
    void useCircuitPlan(bool enabled) noexcept { circuitPlanEnabled_ = enabled; }
    void usePreviewPentodes(bool enabled) noexcept {
        for (size_t i = 0; i < pentodes_.size(); ++i) {
            const auto& card = comp_.pent[i].model.reefman;
            pentodes_[i].reefman.quadric = enabled && card.previewQuadric ? card.previewQuadric : card.quadric;
        }
    }
    void useTriodePreview(bool enabled) noexcept { triodePreviewEnabled_ = enabled; }
    void keepBlockAnswer(bool enabled) noexcept { keepBlockAnswer_ = enabled; }
    void useSlowLag(bool enabled) noexcept { slowLagEnabled_ = enabled; }
    void prepareSlowLag();
    void prepareLampReplay();
    void stepLampReplay();
    void foldLamp();
    /// Make this clone stage 1 (the blocks before the first power-tube block) or
    /// stage 2 (the rest) of a two-core Preview. False when the partition has no such cut.
    bool preparePipelineStage(int stage);
    bool pipelinePrepared() const noexcept {
        return pipe_.stage && pipe_.revision == linearRebuilds() && pipe_.lagView == slowLagActive() && !lamp_.period;
    }
    bool pipelineReady() const noexcept { return pipelinePrepared() && !pipe_.paused; }
    bool pipelinePaused() const noexcept { return pipe_.paused; }
    /// Solve the whole circuit again; `resumePipeline` restarts from this clone's own state.
    void pausePipeline() noexcept { pipe_.paused = true; }
    void resumePipeline() noexcept {
        const size_t n = pipe_.inPorts.size();
        for (size_t k = 0; k < n; ++k) {
            pipe_.input[k] = ii_[static_cast<size_t>(pipe_.inPorts[k])];
            pipe_.input[n + k] = vv_[static_cast<size_t>(pipe_.inPorts[k])];
        }
        pipe_.paused = false;
    }
    void stopPipeline() noexcept { pipe_.stage = 0; }
    /// The other stage's port currents, then its port voltages.
    double* pipelineInput() noexcept { return pipe_.input.data(); }
    size_t pipelineInputSize() const noexcept { return pipe_.input.size(); }
    size_t pipelineOutputSize() const noexcept { return 2 * pipe_.ownPorts.size(); }
    void pipelineOutput(double* out) const noexcept {
        const size_t n = pipe_.ownPorts.size();
        for (size_t k = 0; k < n; ++k) {
            out[k] = ii_[static_cast<size_t>(pipe_.ownPorts[k])];
            out[n + k] = vv_[static_cast<size_t>(pipe_.ownPorts[k])];
        }
    }
    void copyStateFrom(const EngineStep& other) {
        capState_ = other.capState_;
        indState_ = other.indState_;
        vv_ = other.vv_;
        ii_ = other.ii_;
        vvPrev_ = other.vvPrev_;
        vvPrev2_ = other.vvPrev2_;
        triodeDomainPin_ = other.triodeDomainPin_;
    }
    bool slowLagActive() const noexcept {
        return (loadedJunctionEnabled_ || slowLagEnabled_) && lagCount_ > 1 && lagRevision_ == linearRebuilds() && blockCount_ > 1 &&
               !fineBlockSolver_;
    }
    long long triodePreviewHits() const noexcept { return triodePreviewHits_; }
    bool circuitPlanPrepared() const noexcept { return circuitPlanRevision_ == linearRebuilds(); }
    bool hasLinearDrivePlan() const noexcept {
        return circuitPlanRevision_ == linearRebuilds() && !driveCoefficients_.empty();
    }
    bool hasPackedDrivePlan() const noexcept {
        return packedDrive_.revision == linearRebuilds();
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
    // A held capacitor's current is what the elements at its `a` node and the other held capacitors there carry into
    // that node; the explicit step is stable because T/RC stays far below one on supply filter chains.
    struct HeldRail {
        size_t source;
        double farads, volts, current = 0.0, total = 0.0;
        std::vector<std::pair<int, double>> ports = {}, rails = {};  // index, +1 where it carries current into the node
    };
    std::vector<HeldRail> heldRails_;
    std::vector<double> heldSources_;

    void prepareHeldRails() {
        heldRails_.clear();
        for (size_t s = 0; s < circuit_.src.size(); ++s)
            if (circuit_.src[s].heldFarads > 0.0)
                heldRails_.push_back({s, circuit_.src[s].heldFarads, circuit_.src[s].dc});
        const int nRes = static_cast<int>(circuit_.res.size()), nCap = static_cast<int>(circuit_.cap.size());
        for (auto& h : heldRails_) {
            const auto& node = circuit_.src[h.source].a;
            const auto attach = [&](auto& list, int index, const std::string& a, const std::string& b) {
                if (a != b && (a == node || b == node)) list.emplace_back(index, b == node ? 1.0 : -1.0);
            };
            for (int k = 0; k < nRes; ++k) attach(h.ports, k, circuit_.res[size_t(k)].a, circuit_.res[size_t(k)].b);
            for (int k = 0; k < nCap; ++k) attach(h.ports, nRes + k, circuit_.cap[size_t(k)].a, circuit_.cap[size_t(k)].b);
            for (int k = 0; k < static_cast<int>(circuit_.ind.size()); ++k)
                if (std::ranges::none_of(net_.coupledGroups, [&](const auto& g) { return std::ranges::count(g.indIndices, k); }))
                    attach(h.ports, nRes + nCap + k, circuit_.ind[size_t(k)].a, circuit_.ind[size_t(k)].b);
            for (size_t t = 0; t < heldRails_.size(); ++t)
                if (&heldRails_[t] != &h)
                    attach(h.rails, int(t), circuit_.src[heldRails_[t].source].a, circuit_.src[heldRails_[t].source].b);
        }
    }

    void stepHeldRails() noexcept {
        for (auto& h : heldRails_) {
            h.current = 0.0;
            for (const auto& [j, sign] : h.ports) {
                const auto u = static_cast<size_t>(j);
                double v = (net_.epA[u] >= 0 ? node_[size_t(net_.epA[u])] : 0.0) -
                           (net_.epB[u] >= 0 ? node_[size_t(net_.epB[u])] : 0.0) + eoff_[u];
                if (net_.capOf[u] >= 0) v -= capState_[size_t(net_.capOf[u])];
                if (net_.indOf[u] >= 0) v += indState_[size_t(net_.indOf[u])];
                h.current += sign * net_.eG[u] * v;
            }
            h.total = h.current;
        }
        // Stacked capacitors pass their current on; one pass per capacitor in the deepest stack.
        for (size_t pass = 1; pass < heldRails_.size(); ++pass)
            for (auto& h : heldRails_) {
                h.total = h.current;
                for (const auto& [t, sign] : h.rails) h.total += sign * heldRails_[size_t(t)].total;
            }
        for (auto& h : heldRails_) h.volts += T_ * h.total / h.farads;
    }

    void resetPreviewState() {
        resetAffineMotion();
        loadedJunction_.seed = true;
        loadedJunction_.cooldown = loadedJunction_.misses = 0;
        circuitPlanCooldown_ = 0;
        affineRootCooldown_ = 0;
        affineRootRetry_ = 8;
        circuitPlanRetry_ = 8;
        resetControlledDevices();
    }

    void resetSolverHistory() {
        // A reset starts a new solve history: the runtime gate's evidence from
        // the old history would describe a different operating point.
        runtimeGateCooldown_ = runtimeBlockEnabled_ ? 1 : 0;
        blockFallbackCooldown_ = blockFallbackCooldownHoldoff_ = 0;
        triodePreviewCooldown_ = 0;
        jointLastIters_ = 0;
        jointLastTrials_ = 0;
        runtimeBlockUsed_ = false;
        runtimeBlockCount_ = 0;
        runtimePartition_ = nullptr;
    }

    // For a fixed netlist and sample rate these DC states depend only on
    // element conductances and the static amplifier law. Devices whose DC
    // laws depend on live control, tube domain or supply state use reset().
    struct RecoveryState {
        bool valid = false, hasRoot = false;
        std::vector<double> conductance, caps, inductors, volts, currents, poles;
        std::vector<unsigned char> softRails;
    } recovery_;

    bool staticRecoveryEligible() const noexcept {
        return K_ > 0 && nTri_ == 0 && nCtrl_ == 0 && !powerSagEnabled_ &&
               bucketBrigades_.empty() && rmsDetectors_.empty();
    }

    bool hasRecoveryState() const noexcept {
        return recovery_.valid && staticRecoveryEligible() && recovery_.conductance == net_.eG &&
               std::ranges::equal(recovery_.softRails, opAmps_, {}, {}, &OpAmpModel::softRails);
    }

    void saveRecoveryState(bool hasRoot) {
        if (!staticRecoveryEligible())
            return;
        recovery_.conductance = net_.eG;
        recovery_.softRails.resize(opAmps_.size());
        for (size_t i = 0; i < opAmps_.size(); ++i)
            recovery_.softRails[i] = opAmps_[i].softRails;
        recovery_.caps = capState_;
        recovery_.inductors = indState_;
        recovery_.volts = vv_;
        recovery_.currents = ii_;
        recovery_.poles = opAmpVp_;
        recovery_.hasRoot = hasRoot;
        recovery_.valid = true;
    }

    void recoverOperatingPoint() {
        if (!loadedJunctionEnabled_ || !hasRecoveryState()) {
            reset();
            return;
        }
        resetPreviewState();
        capState_ = recovery_.caps;
        indState_ = recovery_.inductors;
        // With only a network-bias seed, reset() preserves the live root history.
        if (recovery_.hasRoot) {
            ii_ = recovery_.currents;
            opAmpVp_ = recovery_.poles;
            vv_ = vvPrev_ = vvPrev2_ = recovery_.volts;
        }
        resetSolverHistory();
    }

    struct AffineRootPlan {
        struct CurrentRow {
            int port, pole;
            double conductance, differential, poleGain;
        };
        std::vector<CurrentRow> rows;
        std::vector<double> inverse, impedance, drive, current;
    };
    struct AffineRootScratch {
        explicit AffineRootScratch(int n) :
            full(n*n), baseTranspose(n*n), comparison(n*n), factors(n*n), column(n), pivots(n) {}
        std::vector<double> full, baseTranspose, comparison, factors, column;
        std::vector<int> pivots;
    };
    struct AffineMotion {
        explicit AffineMotion(int n) : scratch(n) {}
        AffineRootPlan root;
        AffineRootScratch scratch;
        long long revision = -1;
        bool certified = false;
        int cooldown = 0;
    };
    struct AffineNodePlan {
        long long revision = -1;
        struct Amp { int port, model; double impedanceBound; };
        std::vector<Amp> amps;
        std::vector<double> coefficients, inputs, outputs, residualBound;
        std::optional<AffineMotion> motion;
    };
    AffineNodePlan affineNodes_;
    long long affineNodeHits_ = 0;
    bool tryTriodePreview() noexcept;
    int triodePreviewCooldown_ = 0;
    long long triodePreviewHits_ = 0;

    bool buildAffineNodePreview();
    void swapAffineNodePreview(EngineStep& prepared) noexcept;
    bool tryAffineNodes(const double* sources, size_t count, double tolerance) noexcept;
    const std::vector<double>& finishStep(const double* sourceValues, size_t sourceCount,
                                          std::chrono::steady_clock::time_point phaseTime);
    void swapLoadedJunctionPreview(EngineStep& prepared) noexcept;
    LoadedJunctionPlan loadedJunction_;
    long long loadedJunctionAttempts_ = 0, loadedJunctionHits_ = 0;
    bool tryLoadedJunctionRoot() noexcept;
    bool buildLoadedJunctionPlan(const std::vector<bool>& linearAmps, const std::vector<bool>& buffers,
                                 bool omitReverse, bool& negativeReverse, std::span<const std::uint8_t> retained = {});
    bool screenJunctionBenefit(size_t inputSource, std::span<std::uint8_t> crossedCutoffs = {});
    void screenJunctionDevices(size_t inputSource, std::vector<bool>& linearAmps,
                               std::vector<bool>& bufferTransistors) const;

    bool factorAffineRoot(AffineRootPlan&, AffineRootScratch&) const noexcept;
    bool buildAffineRoot(AffineRootPlan& result) const;
    void prepareAffineMotion();
    void resetAffineMotion() noexcept;
    bool tryAffineMotion(double tolerance) noexcept;
    void prepareAffineRoot();
    bool opAmpsOnLinearBranch(const std::vector<double>& voltage) const noexcept;
    bool tryAffineRoot(AffineRootPlan&, double tolerance) noexcept;
    bool circuitRootContinuous() const noexcept;
    void acceptCircuitRoot(double error) noexcept;
    void commitCompiledRoot(double error) noexcept;
    AffineRootPlan affineRoot_;
    std::vector<double> certifiedResistance_;
    long long affineRootAttempts_ = 0, affineRootHits_ = 0;
    int affineRootCooldown_ = 0, affineRootRetry_ = 8;
    bool factorCircuitTangent(const std::vector<double>& jacobian) noexcept;
    bool tryCompiledRoot(double tolerance) noexcept;
    bool applyCompiledDrive(const double* sources, size_t count) noexcept;
    struct PackedDrive {
        long long revision = -1;
        std::vector<detail::DrivePanel> panels;
        std::vector<int> nodes;
        std::vector<double> values;
        std::vector<SparseRowN> remaining;
    };
    PackedDrive packedDrive_;
    void preparePackedDrive();
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

    static double latchedPhotoVolts(PhotoresistorState& state, const PhotoresistorControl& control,
                                    double volts) {
        if (control.strikeVolts <= 0.0)
            return volts;
        state.lit = volts >= control.strikeVolts || (state.lit && volts > control.extinguishVolts);
        return state.lit ? control.referenceVolts : 0.0;
    }

    double photoTargetConductance(PhotoresistorState& state,
                                  const std::vector<double>& nodes,
                                  const double* sources, size_t sourceCount) const {
        const auto& ctrl = circuit_.ctrlRes[state.index];
        return ctrl.photo->targetConductance(
            latchedPhotoVolts(state, *ctrl.photo, photoSenseVoltage(state, nodes, sources, sourceCount)),
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
