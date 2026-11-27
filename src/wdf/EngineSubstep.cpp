#include "wdf/Engine.h"

// A sample the solver misses is re-run on engines at two (and four) times the
// rate with the input interpolated linearly, then handed back. Every reactive
// state is a wave s = v + R*i; halving the step halves a capacitor's port
// resistance and doubles an inductor's, so the state moves between rates in
// closed form from the port voltage and the state alone.

namespace wdf {

void Engine::prepareSubsteps(int depth) {
    half_.reset();
    if (depth <= 0 || !bucketBrigades_.empty() || !rmsDetectors_.empty() || powerSagEnabled_)
        return;
    auto half = std::make_shared<Engine>(circuit_, 2. * fs_, gridCurrent_);
    if (half->capState_.size() != capState_.size() || half->indState_.size() != indState_.size() ||
        half->K_ != K_ || half->N_ != N_ || half->eoff_.size() != eoff_.size())
        return;
    half->prepareSubsteps(depth - 1);
    lastSources_ = dcValues();
    for (const auto& source : comp_.isrc)
        lastSources_.push_back(source.second.dc);
    midSources_ = lastSources_;
    lastEoff_ = eoff_;
    half_ = std::move(half);
}

void Engine::rescaleWaves(const std::vector<double>& node, const std::vector<double>& eoff, double capScale,
                          const std::vector<double>& caps, const std::vector<double>& inductors,
                          std::vector<double>& capsOut, std::vector<double>& inductorsOut) const {
    const auto cap = [&](int k, double v) { capsOut[k] = v + capScale * (caps[k] - v); };
    const auto ind = [&](int k, double v) { inductorsOut[k] = v + (inductors[k] - v) / capScale; };
    for (const auto& u : capBoth_) cap(u.capIdx, node[u.epA] - node[u.epB] + eoff[u.portIdx]);
    for (const auto& u : capToGnd_) cap(u.capIdx, node[u.ep] + eoff[u.portIdx]);
    for (const auto& u : capFromGnd_) cap(u.capIdx, eoff[u.portIdx] - node[u.ep]);
    for (const auto& u : capZero_) cap(u.capIdx, eoff[u.portIdx]);
    for (const auto& u : indBoth_) ind(u.indIdx, node[u.epA] - node[u.epB] + eoff[u.portIdx]);
    for (const auto& u : indToGnd_) ind(u.indIdx, node[u.ep] + eoff[u.portIdx]);
    for (const auto& u : indFromGnd_) ind(u.indIdx, eoff[u.portIdx] - node[u.ep]);
    for (const auto& u : indZero_) ind(u.indIdx, eoff[u.portIdx]);
}

void Engine::substep(const double* sources, size_t count, int maxIter, double tol) {
    Engine& half = *half_;
    const auto samePosition = [](const auto& a, const auto& b) { return a.position == b.position; };
    if (count > lastSources_.size() || powerSagEnabled_ || hasAdjustedLiveTransfer() ||
        !std::equal(circuit_.pots.begin(), circuit_.pots.end(), half.circuit_.pots.begin(), samePosition) ||
        !std::equal(circuit_.varRes.begin(), circuit_.varRes.end(), half.circuit_.varRes.begin(), samePosition)) {
        ++failedSubsteps_;
        return;
    }
    // The held state is the previous sample's; give it to the half-rate engine.
    rescaleWaves(node_, lastEoff_, .5, capState_, indState_, half.capState_, half.indState_);
    half.vv_ = half.vvPrev_ = half.vvPrev2_ = vv_;
    half.ii_ = ii_;
    half.node_ = node_;
    half.eoff_ = lastEoff_;
    half.lastSources_ = lastSources_;
    half.opAmpVp_ = opAmpVp_;
    half.ctrlResValues_ = ctrlResValues_;
    for (size_t p = 0; p < photoresistors_.size(); ++p)
        half.photoresistors_[p].illumination = photoresistors_[p].illumination;
    half.triodeDomainPin_ = triodeDomainPin_;
    if (half.gridLag_.size() == gridLag_.size())
        half.gridLag_ = gridLag_;
    half.consecutiveStateRejects_ = 0;

    for (size_t i = 0; i < count; ++i)
        midSources_[i] = .5 * (lastSources_[i] + sources[i]);
    for (const double* values : {static_cast<const double*>(midSources_.data()), sources}) {
        half.step(values, count, maxIter, tol);
        newtonIters_ += half.newtonIters_;
        deviceEvals_ += half.deviceEvals_;
        if (half.lastStateRejected_) {
            ++failedSubsteps_;
            return;
        }
    }

    half.rescaleWaves(half.node_, half.eoff_, 2., half.capState_, half.indState_, capState_, indState_);
    vvPrev2_ = vvPrev_;
    vv_ = vvPrev_ = half.vv_;
    ii_ = half.ii_;
    node_ = half.node_;
    opAmpVp_ = half.opAmpVp_;
    ctrlResValues_ = half.ctrlResValues_;
    for (size_t p = 0; p < photoresistors_.size(); ++p)
        photoresistors_[p].illumination = half.photoresistors_[p].illumination;
    triodeDomainPin_ = half.triodeDomainPin_;
    if (half.gridLag_.size() == gridLag_.size())
        gridLag_ = half.gridLag_;
    lastConverged_ = true;
    lastStateRejected_ = false;
    --stateRejections_;
    consecutiveStateRejects_ = 0;
    ++substeps_;
}

}  // namespace wdf
