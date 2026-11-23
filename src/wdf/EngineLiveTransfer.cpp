#include "wdf/Engine.h"

#include <tuple>

namespace wdf {

bool Engine::captureLiveTransferSetup(LiveTransferSetup& setup) const noexcept {
    if (setup.prepared_ || !hasAdjustedLiveTransfer() || !explicitRootsAllowed_ || runtimeBlockEnabled_ ||
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
    setup.rate_ = fs_;
    setup.gridCurrent_ = gridCurrent_;
    setup.linearRevision_ = linearRebuilds();
    setup.controlRevision_ = liveControlRevision();
    return true;
}

void Engine::LiveTransferSetup::build(size_t inputSource, bool prepareAffine) {
    prepared_ = std::make_unique<Engine>(circuit_, rate_, gridCurrent_);
    prepared_->prepareLiveControls();
    if (prepareAffine)
        prepared_->prepareMixedAffineReduction(inputSource);
}

bool Engine::canInstallLiveTransferSetup(const LiveTransferSetup& setup) const noexcept {
    const auto& prepared = setup.prepared_;
    return setup.owner_ == this && prepared && hasAdjustedLiveTransfer() &&
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
    ++linearRebuilds_;
    return true;
}

}  // namespace wdf
