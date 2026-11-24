#pragma once

// Level 2 of the engine: the nonlinear root.
//
// Every nonlinear device in the network sits behind a root port -- three per
// triode, one per diode -- so solving the circuit is solving
// `v - v_oc + Z*i(v) = 0` for the K device currents. This file holds that solve
// and nothing else: the device laws, the residual and Jacobian, the reduced
// system the Newton step factors, and the dense solver it factors it with. It
// also holds the exact closed-form root that replaces the whole loop whenever it
// can: one scalar closed form per device, applied group by group in dependency
// order. The two paths differ only in how they produce `ii_`, so
// `EngineStep::step()` is written once against both.

#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "circuit/SignalFlow.h"
#include "wdf/DenseSolve.h"
#include "wdf/EngineCore.h"
#include "wdf/solver/RootResidual.h"
#include "wdf/solver/RootJacobian.h"
#include "wdf/solver/RootClosedForms.h"
#include "wdf/solver/RootNewton.h"
#include "wdf/solver/RootBlockSolve.h"
#include "wdf/solver/RootGroups.h"
#include "wdf/solver/RootLinearPlan.h"

namespace wdf {

class EngineSolve : public EngineCore {
    friend class engine::DenseDc;
public:
    /// Root currents and their Jacobian at root-port voltages `v`. Allocates, so
    /// it is for setup only; `currentsInto` is the audio-loop version.
    ///
    /// **A `v` that is not the root-port voltages is refused, not read.** The
    /// device laws read `v[o]`, `v[o+1]` and `v[o+2]` for each device's ports,
    /// so a short `v` would be read past its end -- an access violation, not a
    /// wrong answer. A caller that has no root voltages (because the DC solve
    /// failed and left them empty) gets zeros back, which is the truth: no
    /// voltages, no currents.
    void currents(const std::vector<double>& v, std::vector<double>& i, std::vector<double>& J) const {
        i.assign(static_cast<size_t>(K_), 0.0);
        J.assign(static_cast<size_t>(K_) * K_, 0.0);
        if (static_cast<int>(v.size()) < K_)
            return;
        currentsInto(v, i, J);
    }

    /// Allocation-free root currents: `i` and `J` must already be sized.
    void currentsInto(const std::vector<double>& v, std::vector<double>& i,
                      std::vector<double>& J) const {
        const double* LIVEWDF_RESTRICT vData = v.data();
        double* LIVEWDF_RESTRICT iData = i.data();
        double* LIVEWDF_RESTRICT jData = J.data();
        for (const auto& dev : rootDevices_)
            currentsForDevice(dev, vData, iData, jData);
    }

    /// One device's currents and Jacobian into the full K-wide arrays.
    ///
    /// The root solve needs the joint arrays, but a block solve needs to evaluate
    /// only the devices in the block. This is the single definition of each device
    /// law; `currentsInto` is the loop over it. The local Jacobian block is cleared
    /// first, so a block call cannot inherit a stale derivative from the previous
    /// iterate or from another block.
    void currentsForDevice(const RootDevice& dev, const double* vData,
                           double* iData, double* jData) const {
        solver::RootResidual::currentsForDevice(
            dev, comp_, ctrlResValues_.data(), pentodes_.data(), bjts_.data(),
            opAmps_.data(), opAmpVp_.data(), opAmpAlpha_.data(), opAmpMaxDv_.data(),
            staticMode_, triodeDomainPin_.data(), Z_.data(), K_, vData, iData, jData, deviceEvals_);
    }

    /// The joint residual of the last `step()`'s solution, evaluated now if the
    /// closed form left it unevaluated.
    double evaluateResidual() {
        if (std::isnan(lastResidual_)) {
            const long long stepEvals = deviceEvals_;
            for (const auto& dev : rootDevices_)
                currentsForDevice(dev, vv_.data(), iv_.data(), nullptr);
            deviceEvals_ = stepEvals;
            residualInto(vv_, vOc_, Z_, iv_, F_);
            lastResidual_ = solver::RootNewton::maxAbs(F_, K_);
        }
        return lastResidual_;
    }

    /// Whether the exact closed-form root can drive this circuit: the root
    /// partition exists and contains no device kind the closed forms cannot
    /// express. Every block is then either a scalar singleton (triode with its
    /// grid line, diode, controlled resistor, JFET, MOSFET) or an anti-parallel
    /// diode pair reduced to one scalar port equation. Blocks that couple
    /// several nonlinear devices are not exact; they stay on the block or joint
    /// Newton.
    bool allowsExplicitRoot() const {
        return groupCount_ > 0 && nPent_ == 0 && nBjt_ == 0 && nOpAmp_ == 0;
    }

    /// Whether the exact closed-form root is currently driving the solve:
    /// requested, the partition succeeded, and *every* block is exact
    /// (`allClosedFormBlocks_`, from `buildRootOrder`). When false,
    /// `blockRoots()` may still be driving a general block Newton; if neither,
    /// the circuit runs the joint Newton. A live transfer correction can
    /// bypass the compiled partition until the next full transfer build.
    bool explicitRoots() const { return explicitRootsActive_; }

    /// Whether the general block Newton is currently driving the solve.
    /// Any multi-stage circuit with at least 2 partitioned blocks uses
    /// block decomposition (solving sequential blocks with folded coupling),
    /// falling back to the joint Newton only if the residual check misses.
    /// A single block is still worth the block path when it contains affine
    /// devices that can be eliminated: the reduced nonlinear core can be a
    /// scalar closed form even though the full joint system is not.
    bool blockRoots() const {
        if (!explicitRootsAllowed_ || groupCount_ <= 0 || explicitRootsActive_)
            return false;
        // Two blocks with only one scalar block is neutral on joint 3x3; stand down,
        // unless the other block is an exact closed-form anti-parallel diode pair
        // or has an affine-device reduction.
        if (blockCount_ == 2 && scalarBlockCount_ <= 1 && nBjt_ == 0 &&
            !hasAntiParallelBlock_ && !hasLinearReduction_)
            return false;
        if (blockCount_ == 1)
            return hasLinearReduction_ || hasAntiParallelBlock_ || hasMonotoneClassAB() ||
                   solver::RootBlockSolve::hasDirectSingleton(
                       newtonBlocks_.front(), rootDevices_, comp_, opAmps_, Z_, K_);
        return blockCount_ >= 2;
    }

    /// Enable or disable the experimental runtime active-set partition.
    /// Enabling precomputes the mask table here, off the audio path; disabling
    /// drops it again. Never call this from the audio callback.
    bool setRuntimeBlockSolver(bool on) {
        runtimeBlockEnabled_ = on;
        runtimeBlockCount_ = 0;
        runtimeBlockUsed_ = false;
        runtimeGateCooldown_ = on ? 1 : 0;
        jointLastIters_ = 0;
        jointLastTrials_ = 0;
        runtimePartition_ = nullptr;
        if (on) {
            prepareRuntimePartitions();
        } else {
            runtimePartitions_.clear();
        }
        return runtimeBlockEnabled_;
    }

    /// Whether the experimental runtime partition is eligible to drive this
    /// sample. `step()` selects the partition immediately before this test; a
    /// single runtime block is not worth the block overhead.
    bool runtimeBlockRoots() const {
        return runtimeBlockEnabled_ && groupCount_ > 0 &&
               runtimeBlockCount_ > 1;
    }

    /// Enable or disable the experimental relaxed stage partition. When on,
    /// non-scalar root blocks may be split along directed signal-flow stages
    /// and solved with residual-verified block Gauss-Seidel sweeps. This is an
    /// experiment: the joint Newton remains the fallback and the reference.
    bool setFineBlockSolver(bool on) {
        if (fineBlockSolver_ == on)
            return fineBlockSolver_;
        fineBlockSolver_ = on;
        refreshRootGroups();
        return fineBlockSolver_;
    }

    /// Turn the exact closed-form root on/off. Returns whether the engine now
    /// matches the request: disabling always succeeds, enabling only does for the
    /// circuits `allowsExplicitRoot()` selects. `rebuild()` -- and so any pot move
    /// -- re-derives the partition from the current `Z_`. Disabling also turns
    /// off `blockRoots()`: the joint Newton is the reference path.
    bool setExplicitRoots(bool on) {
        explicitRootsAllowed_ = on;
        refreshRootGroups();
        return explicitRootsActive_ == on;
    }

protected:
    /// Each level only forwards construction down the chain; the work happens in
    /// `EngineCore` (topology) and `Engine` (transfer, buffers, seeding).
    using EngineCore::EngineCore;

    solver::NewtonContext makeNewtonContext() {
        return solver::NewtonContext{
            vv_, ii_, J_, F_, vOc_, Z_, Zred_,
            loadedJunctionEnabled_ && sharedNewtonColumns_.hasPackedResidual() ? &sharedNewtonColumns_ : nullptr,
            loadedJunctionEnabled_ ? &triodeDomainTransfer_ : nullptr,
            vvPrev_, vvPrev2_, predUp_, predDown_, stepUp_, stepDown_, redCol_,
            redToActive_, activeRedCol_, Mr_, rN_, dv_, vt_, ft_, Jt_, iv_,
            bestV_, bestI_, gridJunctionLimiters_, junctionLimiters_, rootLayout_,
            bjts_, rootDevices_, comp_, ctrlResValues_, pentodes_,
            opAmps_, opAmpVp_, opAmpAlpha_, opAmpMaxDv_, triodeDomainPin_,
            staticMode_, K_, nRed_, keepBestOn_, &prof_, profileOn_,
            newtonIters_, lineTrials_, limitHits_, gridLimitHits_, deviceEvals_,
            jointLastIters_, jointLastTrials_, bestFn_, keptBest_, lastConverged_, lastResidual_, loadedJunctionEnabled_
        };
    }

    solver::BlockSolveContext makeBlockSolveContext() {
        return solver::BlockSolveContext{
            blockBase_, blockI_, blockJ_, blockF_, blockV_, blockVOld_,
            blockDv_, blockM_, blockRhs_,
            vv_, ii_, iv_, J_, F_, vOc_, Z_, Zred_, vvPrev_, vvPrev2_,
            stepUp_, stepDown_, redCol_,
            K_, nRed_,
            rootDevices_, comp_, bjts_, ctrlResValues_, pentodes_,
            opAmps_, opAmpVp_, opAmpAlpha_, opAmpMaxDv_, triodeDomainPin_,
            grpDiode_, junctionLimiters_, gridJunctionLimiters_,
            staticMode_, &prof_, profileOn_,
            newtonIters_, deviceEvals_, limitHits_, gridLimitHits_,
            lastBlockFallback_, lastBlockResidual_, blockFallbacks_,
            runtimeBlockFallbacks_, runtimeBlockUsed_, lastConverged_,
            lastResidual_, keptBest_, jfetSolveFailed_, mosfetSolveFailed_,
            jfetFallbacks_, mosfetFallbacks_, gridLag_,
            mixedAffineEnabled_, mixedAffineAttempts_, mixedAffineAccepted_
        };
    }

    solver::RootGroupsContext makeRootGroupsContext() {
        return solver::RootGroupsContext{
            rootDevices_, circuit_, comp_, ctrlResValues_, Z_,
            K_, nTri_, nD_, nCtrl_, nPent_, nJfet_, nMosfet_, nBjt_, nOpAmp_,
            explicitRootsAllowed_, runtimeBlockEnabled_, fineBlockSolver_,
            explicitRootsActive_, allClosedFormBlocks_, hasAntiParallelBlock_,
            blockCount_, scalarBlockCount_, groupCount_,
            runtimeBlockCount_, runtimeBlockUsed_,
            groupOrder_, grpIndeg_, grpQueue_, grpDiode_,
            newtonBlocks_, blockOf_, blockReach_, blockAdj_,
            runtimeBlocks_, runtimeOrder_, runtimePartitions_, runtimePartition_
        };
    }

    /// Run every exponential junction's port through its own limiter, in
    /// place: `v[e]` holds the proposed voltage, `vOld[e]` the iterate it moved
    /// from. Diodes and BJT base-emitter junctions use `DiodeModel::limit`;
    /// pentode control grids use pedalkernel's `Pentode::limitGrid` (the same
    /// `pnjlim` rule, with the pentode's own `grid_is` / `grid_vt`). Returns
    /// how many ports were clamped, for the caller's diagnostics.
    ///
    /// It lives here rather than in the device because it needs the whole port
    /// vector to be about iterates at all, and here rather than in one solver
    /// because both of them (`solveDc`'s dense Newton and the sample loop's
    /// multiport one) need exactly the same rule.
    int limitDiodePorts(double* v, const double* vOld) const {
        return solver::RootResidual::limitDiodePorts(
            v, vOld, junctionLimiters_.data(), junctionLimiters_.size(),
            gridJunctionLimiters_.data(), gridJunctionLimiters_.size(), gridLimitHits_);
    }

    int limitBjtJunctions(double* v, const double* vOld) const {
        return solver::RootResidual::limitBjtJunctions(v, vOld, rootLayout_, bjts_.data());
    }

    /// F = v - vOc + Z*i, written into `F`.
    void residualInto(const std::vector<double>& v, const std::vector<double>& vOc,
                      const std::vector<double>& Z, const std::vector<double>& i,
                      std::vector<double>& F) const {
        solver::RootResidual::residualInto(
            v.data(), vOc.data(), Z.data(), i.data(), F.data(), redCol_.data(), nRed_, K_);
    }

    std::vector<double> residual(const std::vector<double>& v, const std::vector<double>& vOc,
                                 const std::vector<double>& Z, const std::vector<double>& i) const {
        std::vector<double> F(static_cast<size_t>(K_));
        residualInto(v, vOc, Z, i, F);
        return F;
    }

    /// A = I + Z*J, written into `A`.
    void jacobianInto(const std::vector<double>& Z, const std::vector<double>& J,
                      std::vector<double>& A) const {
        solver::RootResidual::jacobianInto(
            Z.data(), J.data(), A.data(), rootDevices_.data(), rootDevices_.size(), K_);
    }

    std::vector<double> jacobian(const std::vector<double>& Z, const std::vector<double>& J) const {
        std::vector<double> A(static_cast<size_t>(K_) * K_, 0.0);
        jacobianInto(Z, J, A);
        return A;
    }

    /// Assemble the reduced Newton system. See `reducedFinish` for the algebra:
    /// with u = J*dv the K x K step (I + Z J) dv = -F is *equivalent* to the
    /// system (I + J Z) u = -J F (Sylvester) on the active rows: a passive
    /// port's row of J is zero, so its u is zero identically. This writes
    /// M = I + (J Z) restricted to the active rows into `Mr_` and -J F into
    /// `rN_`, indexed by `redCol_`.
    void reducedAssemble(const std::vector<double>& Z, const std::vector<double>& J,
                         const std::vector<double>& F) {
        solver::RootJacobian::reducedAssemble(
            Z.data(), J.data(), F.data(), redCol_.data(), nRed_, K_,
            rootDevices_.data(), rootDevices_.size(), Mr_.data(), rN_.data());
    }

    /// Overload with active-set reduction: `nAct` active ports (<= nRed_).
    /// Uses `activeRedCol_` and `redToActive_` mapping built by the caller.
    void reducedAssemble(const std::vector<double>& Z, const std::vector<double>& J,
                         const std::vector<double>& F, int nAct) {
        const int* colOf = (nAct == nRed_) ? redCol_.data() : activeRedCol_.data();
        solver::RootJacobian::reducedAssembleActive(
            Z.data(), J.data(), F.data(), colOf, redToActive_.data(), nAct, K_,
            rootDevices_.data(), rootDevices_.size(), Mr_.data(), rN_.data());
    }

    /// Solve the system assembled by `reducedAssemble` and expand u = J*dv back
    /// to the Newton step dv = -F - Z u.
    bool reducedFinish(const std::vector<double>& Z, const std::vector<double>& F,
                       std::vector<double>& dv) {
        return solver::RootJacobian::reducedFinish(
            Mr_.data(), rN_.data(), nRed_, Z.data(), F.data(), redCol_.data(), K_, dv.data());
    }

    /// Overload with active-set reduction: `nAct` active ports.
    bool reducedFinish(const std::vector<double>& Z, const std::vector<double>& F,
                       std::vector<double>& dv, int nAct) {
        const int* colOf = (nAct == nRed_) ? redCol_.data() : activeRedCol_.data();
        return solver::RootJacobian::reducedFinish(
            Mr_.data(), rN_.data(), nAct, Z.data(), F.data(), colOf, K_, dv.data());
    }

    /// The exact closed form, applied to every group in dependency order. A group
    /// is one device, and each kind has a scalar closed form: a quadric
    /// triode's plate current is the relevant root of a scalar quadratic, a
    /// diode's is one `wrightOmega` call. Before a group's root is taken, the
    /// currents of the groups already solved are folded into its open-circuit
    /// voltages -- that fold is the residual's coupling term. Because the order is
    /// topological under `coupled`, those are the only unknowns a group's
    /// equations mention, except a later group's grid current, which is
    /// predicted from the last two samples and re-solved once where it missed;
    /// without grid current the sequence is exact.
    ///
    /// With one device the fold loop is empty and this reproduces the previous
    /// single-device closed form bit for bit.
    ///
    /// A group is a device, but the group order is not the root-port order.
    /// This maps a group index to the `RootDevice` it owns without repeating
    /// the offset arithmetic in every solve function.
    const RootDevice& groupRootDevice(int g) const noexcept {
        return rootDevices_[static_cast<size_t>(g)];
    }

    BlockView staticBlockView() const noexcept {
        return {&newtonBlocks_, &groupOrder_, blockCount_};
    }

    BlockView runtimeBlockView() const noexcept {
        if (runtimePartition_)
            return {&runtimePartition_->blocks, &runtimePartition_->order,
                    runtimePartition_->count};
        return {&runtimeBlocks_, &runtimeOrder_, runtimeBlockCount_};
    }

    /// The block's final port voltages after a scalar closed form. A block
    /// Newton already has them, but a scalar solve only writes currents; the
    /// voltages are needed for the full-residual check and for `step()`'s
    /// state commit.
    void settleBlockVoltages(int b, int s, bool relaxed = false) {
        settleBlockVoltages(staticBlockView(), b, s, relaxed);
    }

    void settleBlockVoltages(const BlockView& view, int b, int s, bool relaxed = false) {
        solver::RootClosedForms::settleBlockVoltages(
            view, b, s, relaxed, Z_.data(), vOc_.data(), ii_.data(), vv_.data(),
            rootDevices_.data(), K_);
    }

    void solveRootGroups(int maxIter, double tol) {
        // The grid passes get their own context so the plain pass keeps its
        // context in registers.
        if (!gridLag_.empty()) {
            auto lagCtx = makeBlockSolveContext();
            solver::RootBlockSolve::predictLaterGrids(lagCtx);
        }
        auto ctx = makeBlockSolveContext();
        solver::RootBlockSolve::solveRootGroups(ctx, staticBlockView(), maxIter, tol);
        if (!gridLag_.empty()) {
            auto lagCtx = makeBlockSolveContext();
            bool failed = false;
            const bool resolved = solver::RootBlockSolve::resolveMissedGrids(
                lagCtx, staticBlockView(), maxIter, tol, failed);
            if (resolved) {
                // The grid-current prediction makes the closed-form pass
                // inexact. Verify the actual system before accepting it; if
                // the prediction/resolve was not self-consistent, seed the
                // joint Newton from the closed-form result and solve the
                // original equations.
                auto checkCtx = makeNewtonContext();
                const bool pinChanged = solver::RootClosedForms::pinTriodeDomains(
                    staticBlockView(), Z_.data(), vOc_.data(), ii_.data(),
                    triodeDomainPin_.data(), comp_, rootDevices_.data(), K_);
                solver::RootNewton::currentsOnlyInto(checkCtx, vv_, ii_);
                solver::RootNewton::residualInto(
                    checkCtx, vv_, vOc_, Z_, ii_, F_);
                const double r = solver::RootNewton::maxAbs(F_, K_);
                if (failed || pinChanged || !(r <= tol)) {
                    solveRootsNewtonSeeded(maxIter, tol);
                    return;
                }
                lastConverged_ = true;
                lastResidual_ = r;
                vvPrev_ = vv_;
            }
        }
    }

    bool hasMonotoneClassAB() const noexcept {
        // A lone op-amp has ports out, pos, neg and only output current.
        // Z[3] <= Z[6] gives negative feedback and Z[0] a passive load line.
        return K_ == 3 && nOpAmp_ == 1 && opAmps_.front().hasClassABOutput() &&
               !opAmps_.front().softRails && Z_[0] >= 0.0 && Z_[3] <= Z_[6];
    }

    /// Solve the root in block order, using the scalar closed form for a
    /// singleton and the block Newton otherwise.
    ///
    /// This is the non-scalar completion of the partition. The block graph is
    /// acyclic after `buildRootOrder` merges strongly connected devices, so a
    /// block's equations only read currents from blocks already solved; the
    /// sequence is exact for that graph. The graph is built from the static
    /// port impedance, however, and a device can enter a regime the static
    /// model did not predict. The full residual is therefore evaluated after
    /// the pass and any failure is handed to the joint Newton. If every block
    /// solved but the full residual missed, `solveRootsNewtonSeeded` starts from
    /// the block result; if a block's own local solve failed, the ordinary
    /// `solveRootsNewton` is used instead, starting from its usual extrapolated
    /// prediction. Both joint paths retry from the previous sample's root if
    /// their first pass does not converge. The fast path is an optimization only
    /// when the check passes; it is never allowed to define a different circuit.
    void solveRootBlocks(int maxIter, double tol) {
        if (hasMonotoneClassAB()) {
            lastBlockFallback_ = false;
            const auto result = solver::RootClosedForms::solveClassABOutput(
                opAmps_.front(), vOc_[0], vOc_[1], vOc_[2], Z_[0], Z_[3], Z_[6],
                opAmpVp_[0], opAmpAlpha_[0], opAmpMaxDv_[0], staticMode_, maxIter,
                std::max(tol, 1e-9), deviceEvals_);
            if (result.ok) {
                vv_[0] = vOc_[0] - Z_[0] * result.loadCurrent;
                vv_[1] = vOc_[1] - Z_[3] * result.loadCurrent;
                vv_[2] = vOc_[2] - Z_[6] * result.loadCurrent;
                ii_[0] = result.modelCurrent;
                ii_[1] = ii_[2] = 0.0;
                lastConverged_ = true;
                lastResidual_ = result.residual;
                lastBlockResidual_ = result.residual;
                keptBest_ = false;
                vvPrev2_ = vvPrev_;
                vvPrev_ = vv_;
                return;
            }
            lastBlockFallback_ = true;
            ++blockFallbacks_;
            solveRootsNewton(maxIter, tol);
            return;
        }
        auto ctx = makeBlockSolveContext();
        auto newtonCtx = makeNewtonContext();
        solver::RootBlockSolve::solveRootBlocks(
            ctx, newtonCtx, staticBlockView(), maxIter, tol,
            fineBlockSolver_ ? 6 : 0);
    }

    /// Runtime active-set variant of `solveRootBlocks`.
    void solveRootBlocksRuntime(int maxIter, double tol) {
        auto ctx = makeBlockSolveContext();
        auto newtonCtx = makeNewtonContext();
        solver::RootBlockSolve::solveRootBlocksRuntime(ctx, newtonCtx, runtimeBlockView(), maxIter, tol);
    }

    /// Port `r`'s open-circuit voltage with every live column of the `s` blocks
    /// already solved folded in. The solve's own fold (`NewtonBlock::predFold`)
    /// also reads a later block's internal-grid current, predicted.
    double foldPort(int r, int s) const {
        return foldPort(staticBlockView(), r, s);
    }

    double foldPort(const BlockView& view, int r, int s) const {
        return solver::RootClosedForms::foldPort(view, r, s, Z_.data(), vOc_.data(), ii_.data(), K_);
    }

    double foldPortAll(int r, int currentBlock) const {
        return foldPortAll(staticBlockView(), r, currentBlock);
    }

    double foldPortAll(const BlockView& view, int r, int currentBlock) const {
        return solver::RootClosedForms::foldPortAll(
            view, r, currentBlock, Z_.data(), vOc_.data(), ii_.data(), K_);
    }

    /// One quadric triode with its grid line in exact closed form.
    void solveTriodeGroup(int g, int s, bool relaxed = false, int b = 0) {
        solver::RootClosedForms::solveTriodeGroup(
            g, s, relaxed, b, staticBlockView(), Z_.data(), vOc_.data(), ii_.data(),
            triodeDomainPin_.data(), comp_, rootDevices_.data(), K_);
    }

    /// One diode in exact scalar closed form via Wright Omega.
    void solveDiodeGroup(int g, int s, bool relaxed = false, int b = 0) {
        solver::RootClosedForms::solveDiodeGroup(
            g, s, relaxed, b, staticBlockView(), Z_.data(), vOc_.data(), ii_.data(),
            grpDiode_.data(), rootDevices_.data(), K_);
    }

    /// Symmetrical anti-parallel diode pair in exact closed form via dual Wright Omega.
    void solveAntiParallelDiodeGroup(int b, int s, bool relaxed = false) {
        solveAntiParallelDiodeGroup(staticBlockView(), b, s, relaxed);
    }

    void solveAntiParallelDiodeGroup(const BlockView& view, int b, int s, bool relaxed = false) {
        solver::RootClosedForms::solveAntiParallelDiodeGroup(
            view, b, s, relaxed, Z_.data(), vOc_.data(), ii_.data(), vv_.data(),
            rootDevices_.data(), K_);
    }

    /// One controlled resistor in exact closed form.
    void solveCtrlResGroup(int g, int s, bool relaxed = false, int b = 0) {
        solver::RootClosedForms::solveCtrlResGroup(
            g, s, relaxed, b, staticBlockView(), Z_.data(), vOc_.data(), ii_.data(),
            ctrlResValues_.data(), rootDevices_.data(), K_);
    }

    /// One general JFET in scalar closed form / bracketed reference.
    void solveJfetGroup(int g, int s, bool relaxed = false, int b = 0) {
        solver::RootClosedForms::solveJfetGroup(
            g, s, relaxed, b, staticBlockView(), Z_.data(), vOc_.data(), ii_.data(),
            comp_, rootDevices_.data(), K_, jfetFallbacks_, jfetSolveFailed_);
    }

    /// One general n-channel MOSFET in scalar closed form / bracketed reference.
    void solveMosfetGroup(int g, int s, bool relaxed = false, int b = 0) {
        solver::RootClosedForms::solveMosfetGroup(
            g, s, relaxed, b, staticBlockView(), Z_.data(), vOc_.data(), ii_.data(),
            comp_, rootDevices_.data(), K_, mosfetFallbacks_, mosfetSolveFailed_);
    }

    /// One operational amplifier stage in Harold Black negative feedback closed form.
    void solveOpAmpGroup(int g, int s, bool relaxed = false, int b = 0) {
        solver::RootClosedForms::solveOpAmpGroup(
            g, s, relaxed, b, staticBlockView(), Z_.data(), vOc_.data(), ii_.data(),
            opAmps_.data(), opAmpVp_.data(), opAmpAlpha_.data(), opAmpMaxDv_.data(),
            staticMode_, rootDevices_.data(), K_);
    }

    /// Solve one block by Newton on its own live columns.
    ///
    /// The block is a small reduced system of the same form as the global one:
    /// `u` is the correction to the block's currents, `M = I + J Z` over the
    /// block's active rows and live columns, and `dv = -F - Z u`. The difference
    /// is that the open-circuit voltages are already folded through the blocks
    /// solved before this one, and the currents of blocks solved later are zero.
    /// That is exact whenever the block graph is acyclic for the currents that
    /// actually flow; `solveRootBlocks` checks the full residual afterwards.
    ///
    /// The device Jacobian is evaluated by `currentsForDevice`, the same code the
    /// joint Newton uses, so the block path cannot drift from the device laws.
    bool solveBlockNewton(int b, int s, int maxIter, double tol, bool relaxed = false) {
        return solveBlockNewton(staticBlockView(), b, s, maxIter, tol, relaxed);
    }

    bool solveBlockNewton(const BlockView& view, int b, int s, int maxIter,
                          double tol, bool relaxed = false) {
        auto ctx = makeBlockSolveContext();
        return solver::RootBlockSolve::solveBlockNewton(view, b, s, maxIter, tol, relaxed, ctx);
    }

    /// Re-derive the root partition and whether the closed form is active. Called
    /// from `EngineCompile::rebuildLinear` -- the single choke point where `Z_`
    /// (and so the coupling) can move -- and from `setExplicitRoots`, so a pot
    /// move cannot leave the solve against a stale partition.
    void refreshRootGroups() {
        auto ctx = makeRootGroupsContext();
        solver::RootGroups::refreshRootGroups(ctx);
        prepareLinearReductions();
        compileZred();
    }

    /// Break a large coupled Newton block into one block per nonlinear device.
    ///
    /// This is not called by the stage experiment: per-device splitting was
    /// measured at ~92 iterations/sample on the Mark IIC+ poweramp (the four
    /// output pentodes are one strongly coupled SCC), so the stage partition
    /// stays at the SCC level. Kept as the reference implementation for a
    /// future partitioner that can prove a weaker coupling graph.
    ///
    /// The original SCC is usually the whole output stage; the per-device
    /// blocks are then coupled only through the (linear) network, so repeated
    /// block Gauss-Seidel sweeps can reach the same fixed point as the joint
    /// Newton.
    ///
    /// Blocks with affine devices are left alone: the linear elimination is a
    /// stronger reduction than splitting, and mixing the two would rebuild the
    /// op-amp coupling as a relaxation. Anti-parallel pairs keep their closed
    /// form for the same reason.
    ///
    /// Intentional: this reallocates and recompiles `blockZ`/`predFold`. It
    /// runs at topology or switch time, never per sample.
    void splitLargeBlocks() {
        const int nGroups = static_cast<int>(rootDevices_.size());
        if (!fineBlockSolver_ || nGroups <= 0)
            return;
        if (static_cast<int>(newtonBlocks_.size()) != nGroups)
            return;

        std::vector<NewtonBlock> old;
        old.swap(newtonBlocks_);
        newtonBlocks_.resize(static_cast<size_t>(nGroups));
        for (auto& b : newtonBlocks_) {
            b.devices.clear();
            b.ports.clear();
            b.portDevice.clear();
            b.clearLinearReduction();
        }
        std::fill(blockOf_.begin(), blockOf_.end(), -1);

        int nb = 0;
        // `newtonBlocks_` is sized to the device count, but only the first
        // `blockCount_` entries are live. Trailing entries are empty scratch;
        // iterating them would append empty blocks past the end of the vector
        // when the live block is split one-device-per-block.
        for (int bi = 0; bi < blockCount_; ++bi) {
            auto& ob = old[static_cast<size_t>(bi)];
            bool hasAffine = false;
            for (int di : ob.devices) {
                const DeviceKind kind = rootDevices_[static_cast<size_t>(di)].kind;
                if (kind == DeviceKind::OpAmp || kind == DeviceKind::ControlledRes) {
                    hasAffine = true;
                    break;
                }
            }

            const bool split = !hasAffine && !ob.isAntiParallelDiodePair &&
                               ob.devices.size() > 1 && ob.ports.size() > 8;
            if (!split) {
                newtonBlocks_[static_cast<size_t>(nb)] = std::move(ob);
                for (int di : newtonBlocks_[static_cast<size_t>(nb)].devices)
                    blockOf_[static_cast<size_t>(di)] = nb;
                ++nb;
                continue;
            }

            for (int di : ob.devices) {
                NewtonBlock& block = newtonBlocks_[static_cast<size_t>(nb)];
                const RootDevice& dev = rootDevices_[static_cast<size_t>(di)];
                block.devices.push_back(di);
                for (int rr = 0; rr < activePortCount(dev); ++rr) {
                    block.ports.push_back(activePort(dev, rr));
                    block.portDevice.push_back(di);
                }
                blockOf_[static_cast<size_t>(di)] = nb;
                ++nb;
            }
        }

        blockCount_ = nb;
        groupCount_ = nb;
        for (int s = 0; s < nb; ++s)
            groupOrder_[static_cast<size_t>(s)] = s;

        scalarBlockCount_ = 0;
        int closedBlockCount = 0;
        hasAntiParallelBlock_ = false;
        for (int b = 0; b < nb; ++b) {
            const NewtonBlock& block = newtonBlocks_[static_cast<size_t>(b)];
            const bool scalar = isScalarBlock(block, rootDevices_.data());
            if (scalar)
                ++scalarBlockCount_;
            if (scalar || block.isAntiParallelDiodePair)
                ++closedBlockCount;
            if (block.isAntiParallelDiodePair)
                hasAntiParallelBlock_ = true;
        }
        allClosedFormBlocks_ = (closedBlockCount == nb);

        solver::RootGroups::compileBlockZAndPredFold(
            newtonBlocks_, groupOrder_.data(), blockCount_, rootDevices_.data(),
            Z_.data(), K_);
    }

    /// Build the affine-device elimination for every block that admits one.
    /// This is a topology-time operation: `Z_`, the device models, and the
    /// block partition are fixed between pot moves.
    void prepareLinearReductions() {
        hasLinearReduction_ = false;
        for (auto& block : newtonBlocks_) {
            if (solver::RootLinearPlan::build(
                    block, rootDevices_.data(), Z_.data(), K_, comp_, opAmps_,
                    ctrlResValues_, fs_))
                hasLinearReduction_ = true;
        }
    }

    /// Build the runtime partition. This changes only which devices are grouped
    /// and in what order; device laws, `Z`, state, and the full-residual fallback
    /// are unchanged.
    bool buildRuntimeBlocks() {
        auto ctx = makeRootGroupsContext();
        return solver::RootGroups::buildRuntimeBlocks(ctx);
    }

    /// Reserve the per-partition storage once. `refreshRootGroups()` can run
    /// from the audio thread on a pot move, so re-preparing an already built
    /// table must reuse these vectors rather than destroy and reallocate them.
    void ensureRuntimePartitionStorage(RuntimePartition& p, int nGroups) {
        solver::RootGroups::ensureRuntimePartitionStorage(p, nGroups);
    }

    /// Precompute the runtime partition.
    void prepareRuntimePartitions() {
        auto ctx = makeRootGroupsContext();
        solver::RootGroups::prepareRuntimePartitions(ctx);
    }

    /// Pick the precomputed runtime partition.
    bool selectRuntimeRootOrder() {
        auto ctx = makeRootGroupsContext();
        return solver::RootGroups::selectRuntimeRootOrder(ctx);
    }

    /// A group's root ports.
    int groupPorts(int g) const noexcept { return groupRootDevice(g).numPorts; }

    /// A group's first root port.
    int groupBase(int g) const noexcept { return groupRootDevice(g).portBase; }

    /// Build the Newton blocks and their dependency order.
    bool buildRootOrder(bool allowSignalFlowPruning = true) {
        auto ctx = makeRootGroupsContext();
        return solver::RootGroups::buildRootOrder(ctx, allowSignalFlowPruning);
    }

    /// Multiport Newton for the device currents.
    void solveRootsNewton(int maxIter, double tol) {
        auto ctx = makeNewtonContext();
        solver::RootNewton::solveRootsNewton(ctx, maxIter, tol, staticBlockView(), vvPrev_, vvPrev2_);
    }

    /// Multiport Newton seeded from the block solve's result.
    void solveRootsNewtonSeeded(int maxIter, double tol) {
        auto ctx = makeNewtonContext();
        solver::RootNewton::solveRootsNewtonSeeded(ctx, maxIter, tol, staticBlockView(), vvPrev_, vvPrev2_);
    }

    /// One pass of `solveRootsNewton`, with or without the extrapolated seed.
    void solveRootsNewtonPass(int maxIter, double tol, bool usePrediction,
                              bool useCurrentSeed = false) {
        auto ctx = makeNewtonContext();
        solver::RootNewton::solveRootsNewtonPass(ctx, maxIter, tol, usePrediction, useCurrentSeed);
    }

};

}  // namespace wdf
