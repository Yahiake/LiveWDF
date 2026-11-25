#include "wdf/Engine.h"

#include <algorithm>
#include <tuple>

namespace wdf {

bool Engine::captureLiveTransferSetup(LiveTransferSetup& setup, bool planOnly) const noexcept {
    if (setup.prepared_ || hasAdjustedLiveTransfer() == planOnly || !explicitRootsAllowed_ || runtimeBlockEnabled_ ||
        fineBlockSolver_ || powerSagEnabled_ ||
        setup.circuit_.res.size() != circuit_.res.size() ||
        setup.circuit_.pots.size() != circuit_.pots.size() ||
        setup.circuit_.varRes.size() != circuit_.varRes.size())
        return false;

    for (size_t i = 0; i < circuit_.res.size(); ++i)
        setup.circuit_.res[i].value = circuit_.res[i].value;
    for (size_t i = 0; i < circuit_.pots.size(); ++i)
        setup.circuit_.pots[i].position = circuit_.pots[i].position;
    for (size_t i = 0; i < circuit_.varRes.size(); ++i)
        setup.circuit_.varRes[i].position = circuit_.varRes[i].position;
    setup.owner_ = this;
    setup.planOnly_ = planOnly;
    setup.maskCount_ = 0;
    setup.affineRevision_ = mixedAffineRevision();
    if (planOnly && blockCount_ == 1) {
        for (const auto& plan : newtonBlocks_.front().alternateLinearSlopes)
            setup.affineMasks_[setup.maskCount_++] = plan.linearSlopeMask;
    }
    setup.rate_ = fs_;
    setup.gridCurrent_ = gridCurrent_;
    setup.linearRevision_ = linearRebuilds();
    setup.controlRevision_ = liveControlRevision();
    return true;
}

Engine::LiveTransferSetup Engine::LiveTransferSetup::makePeer() {
    if (!baseCircuit_)
        baseCircuit_ = std::make_shared<BaseCircuitIdentity>();
    LiveTransferSetup peer(circuit_);
    peer.baseCircuit_ = baseCircuit_;
    return peer;
}

void Engine::LiveTransferSetup::build(size_t inputSource, bool prepareAffine) {
    prepared_ = std::make_unique<Engine>(circuit_, rate_, gridCurrent_);
    if (!planOnly_)
        prepared_->prepareLiveControls();
    if (planOnly_ || prepareAffine)
        prepared_->prepareCircuitPlan();
    if (prepareAffine)
        prepared_->prepareMixedAffineReduction(inputSource);
    else if (maskCount_ && prepared_->canPrepareMixedAffineReduction(inputSource))
        prepared_->compileMixedAffinePlans(std::span(affineMasks_).first(maskCount_));
}

void Engine::LiveTransferSetup::prepareCompiledPreview(size_t inputSource, int outputNode, bool allowOutputOnly,
                                                       double requestedPeak, bool allowPad) {
    if (!prepared_)
        return;
    compiledPreview_ = true;
    // Performance screening may leave the worker at the end of its test signal.
    // Compile each channel at its physical DC point. Live histories stay on the recipient.
    prepared_->reset();
    prepared_->prepareCompiledPreview(inputSource, outputNode, allowOutputOnly, requestedPeak, allowPad);
}

bool Engine::LiveTransferSetup::prepareCompiledPreviewPair(LiveTransferSetup& peer, size_t inputSource,
                                                           int outputNode, bool allowOutputOnly,
                                                           double requestedPeak, bool allowPad) {
    const bool fresh = !compiledPreview_ && !peer.compiledPreview_;
    prepareCompiledPreview(inputSource, outputNode, allowOutputOnly, requestedPeak, allowPad);
    const auto sameValues = [](const auto& a, const auto& b, auto field) {
        return std::ranges::equal(a, b, {}, field, field);
    };
    // Only these three sets of values can differ between snapshots of a shared
    // base circuit. Model cards, wiring and the other components came from the same copy.
    if (fresh && prepared_ && prepared_->bipolarPreview_ && peer.prepared_ &&
        baseCircuit_ && baseCircuit_ == peer.baseCircuit_ &&
        rate_ == peer.rate_ && gridCurrent_ == peer.gridCurrent_ && planOnly_ == peer.planOnly_ &&
        maskCount_ == peer.maskCount_ && affineMasks_ == peer.affineMasks_ &&
        sameValues(circuit_.res, peer.circuit_.res, &Res::value) &&
        sameValues(circuit_.pots, peer.circuit_.pots, &Pot::position) &&
        sameValues(circuit_.varRes, peer.circuit_.varRes, &VarRes::position)) {
        peer.prepared_->reset();
        peer.prepared_->copyBipolarPreviewFrom(*prepared_);
        peer.compiledPreview_ = true;
        return true;
    }
    peer.prepareCompiledPreview(inputSource, outputNode, allowOutputOnly, requestedPeak, allowPad);
    return false;
}

void Engine::prepareCompiledPreview(size_t inputSource, int outputNode, bool allowOutputOnly,
                                    double requestedPeak, bool allowPad) {
    if (prepareAffineNodePreview(inputSource, outputNode))
        return;
    if (allowOutputOnly && prepareBipolarPreview(inputSource, outputNode, requestedPeak, allowPad))
        return;
    prepareLoadedJunctionPreview(inputSource);
    sharedNewtonColumns_.prepare(Zred_, K_, nRed_, rootDevices_);
    triodeDomainTransfer_.prepare(Z_, K_, rootDevices_);
}

bool Engine::canInstallLiveTransferSetup(const LiveTransferSetup& setup) const noexcept {
    const auto& prepared = setup.prepared_;
    return setup.owner_ == this && prepared && (hasAdjustedLiveTransfer() != setup.planOnly_) &&
           (!setup.planOnly_ || setup.affineRevision_ == mixedAffineRevision()) &&
           setup.linearRevision_ == linearRebuilds() &&
           setup.controlRevision_ == liveControlRevision() &&
           explicitRootsAllowed_ && !runtimeBlockEnabled_ && !fineBlockSolver_ &&
           !powerSagEnabled_ &&
           std::equal(opAmps_.begin(), opAmps_.end(), prepared->opAmps_.begin(),
                      prepared->opAmps_.end(), [](const auto& a, const auto& b) {
                          return a.softRails == b.softRails &&
                                 a.outputResistanceEmbedded == b.outputResistanceEmbedded;
                      }) &&
           prepared->net_.epA == net_.epA &&
           prepared->net_.epB == net_.epB && prepared->net_.rp == net_.rp &&
           prepared->net_.eG == net_.eG && prepared->net_.liveCols == net_.liveCols;
}

bool Engine::installLiveTransferSetup(LiveTransferSetup& setup) noexcept {
    if (!canInstallLiveTransferSetup(setup))
        return false;

    releaseBipolarPreview();
    setup.owner_ = nullptr;
    auto& preparedEngine = *setup.prepared_;
    if (preparedEngine.hasRecoveryState())
        std::swap(recovery_, preparedEngine.recovery_);
    driveCoefficients_.swap(preparedEngine.driveCoefficients_);
    driveInputs_.swap(preparedEngine.driveInputs_);
    std::swap(affineRoot_, preparedEngine.affineRoot_);
    certifiedResistance_.swap(preparedEngine.certifiedResistance_);
    affineRootCooldown_ = 0;
    affineRootRetry_ = 8;
    rootFactors_.swap(preparedEngine.rootFactors_);
    circuitResidualColumns_.swap(preparedEngine.circuitResidualColumns_);
    rootPivots_.swap(preparedEngine.rootPivots_);
    circuitPlanCooldown_ = 0;
    circuitPlanRetry_ = preparedEngine.circuitPlanRetry_;
    const auto installPreview = [&] {
        if (setup.compiledPreview_) {
            std::swap(sharedNewtonColumns_, preparedEngine.sharedNewtonColumns_);
            std::swap(triodeDomainTransfer_, preparedEngine.triodeDomainTransfer_);
            swapAffineNodePreview(preparedEngine);
            adoptBipolarPreview(preparedEngine);
            swapLoadedJunctionPreview(preparedEngine);
        }
    };
    if (setup.planOnly_) {
        circuitPlanRevision_ = linearRebuilds();
        installPreview();
        return true;
    }

    // The old transfer goes back to the control side through setup. Live
    // capacitor/inductor waves, device histories and sample scratch stay put.
    const auto transfer = [](Engine& e) {
        return std::tie(
            e.net_.elems, e.Xe_, e.Xr_, e.Z_, e.Xis_,
            e.linY_, e.linRhs_, e.linXe_, e.linXr_, e.networkFactors_,
            e.xeRowOffsets_, e.xeCols_, e.xeVals_, e.xeRows1_, e.xeRows2_, e.xeRowsN_,
            e.xrRowOffsets_, e.xrCols_, e.xrVals_, e.xrRows1_, e.xrRows2_, e.xrRowsN_,
            e.isrcRows_, e.rootPanels_, e.rootPanelValues_, e.adjustableTransfer_,
            e.Zred_, e.newtonBlocks_, e.groupOrder_, e.grpDiode_, e.blockOf_,
            e.blockCount_, e.groupCount_, e.explicitRootsActive_,
            e.allClosedFormBlocks_, e.scalarBlockCount_, e.hasAntiParallelBlock_,
            e.hasLinearReduction_);
    };
    auto live = transfer(*this);
    auto prepared = transfer(*setup.prepared_);
    live.swap(prepared);
    if (!setup.compiledPreview_) {
        sharedNewtonColumns_.refresh(Zred_, K_, nRed_);
        triodeDomainTransfer_.refresh(Z_, K_);
    }
    ++linearRebuilds_;
    circuitPlanRevision_ = preparedEngine.circuitPlanPrepared() ? linearRebuilds() : -1;
    installPreview();
    return true;
}

}  // namespace wdf
