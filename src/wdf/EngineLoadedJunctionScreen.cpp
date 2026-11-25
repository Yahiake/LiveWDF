#include "wdf/Engine.h"

#include <chrono>

namespace wdf {

// This probe nominates op-amps on their linear branch and transistors whose
// base-emitter voltage barely moves. The same conditions are checked again
// on each actual sample, including signals absent from the probe.
void EngineStep::screenJunctionDevices(size_t inputSource, std::vector<bool>& linearAmps,
                                       std::vector<bool>& buffers) const {
    if (linearAmps.empty() && buffers.empty()) return;
    if (inputSource >= dcValues_.size()) {
        const auto candidate = std::find(dcValues_.begin(), dcValues_.end(), 0.0);
        if (candidate == dcValues_.end()) return;
        inputSource = static_cast<size_t>(candidate - dcValues_.begin());
    }
    Engine probe(circuit_, fs_);
    if (probe.Z_ != Z_) return;
    std::fill(linearAmps.begin(), linearAmps.end(), true);
    std::fill(buffers.begin(), buffers.end(), true);
    auto source = dcValues_;
    for (int n = 0; n < 512; ++n) {
        const double t = n/fs_;
        const double pulse = n < 192 ? std::sin(t*1730.0) : .55*std::sin(t*4100.) + .45*std::sin(t*31700.);
        source[inputSource] = dcValues_[inputSource] + 1.2*pulse;
        probe.step(source, 60, 1e-7);
        if (!probe.lastConverged() || probe.lastStateRejected()) {
            std::fill(linearAmps.begin(), linearAmps.end(), false);
            std::fill(buffers.begin(), buffers.end(), false);
            return;
        }
        for (const auto& d : rootLayout_.devices) {
            const int r = d.portBase, ci = d.compIndex;
            if (d.kind == RootKind::OpAmp) {
                const auto& a = opAmps_[ci];
                const double v = probe.opAmpVp_[ci];
                if (v < a.railLow() + .05 || v > a.railHigh() - .05)
                    linearAmps[ci] = false;
            } else if (d.kind == RootKind::Bjt) {
                const double sign = bjts_[ci].pnp ? -1.0 : 1.0;
                const double vbe = sign * probe.vv_[r + 1];
                if (std::abs(vbe - sign * vv_[r + 1]) > .010 || vbe - sign * probe.vv_[r] > .10)
                    buffers[ci] = false;
            }
        }
    }
}
bool EngineStep::screenJunctionBenefit(size_t inputSource, std::span<std::uint8_t> crossedCutoffs) {
    if (inputSource >= dcValues_.size()) {
        const auto candidate = std::find(dcValues_.begin(), dcValues_.end(), 0.0);
        if (candidate == dcValues_.end()) return false;
        inputSource = static_cast<size_t>(candidate - dcValues_.begin());
    }
    Engine reference(circuit_, fs_), preview(circuit_, fs_);
    if (reference.Z_ != Z_ || preview.Z_ != Z_) return false;
    reference.prepareCircuitPlan();
    reference.useCircuitPlan(true);
    preview.prepareCircuitPlan();
    preview.useCircuitPlan(true);
    preview.loadedJunction_ = std::move(loadedJunction_);
    preview.loadedJunction_.revision = preview.linearRebuilds();
    preview.loadedJunction_.controlRevision = preview.liveControlRevision();
    preview.useLoadedJunctionPreview(true);

    constexpr int blocks = 24, frames = 32;
    std::array<double, blocks> ratios{};
    std::vector<double> signal(N_), difference(N_);
    const auto initial = reference.node_;
    auto source = dcValues_;
    bool valid = true;
    std::array<std::vector<double>, 2> samples{
        std::vector<double>(size_t(N_)*frames), std::vector<double>(size_t(N_)*frames)};
    for (int b = 0; b < blocks; ++b) {
        std::array<double, frames> input{};
        for (int i = 0; i < frames; ++i) {
            const double t = (b*frames+i)/fs_;
            const double a = b < 5 ? .008 : 1.2;
            input[i] = b > 18 ? 0.0 : a*(.6*std::sin(2300.*t) + .4*std::sin(38700.*t));
        }
        double times[2];
        // Alternate the timed order without charging waveform comparison to a path.
        for (int pass = 0; pass < 2; ++pass) {
            const int mode = (b+pass)%2;
            Engine& engine = mode ? preview : reference;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < frames; ++i) {
                source[inputSource] = dcValues_[inputSource] + input[i];
                const auto& nodes = engine.step(source, kDefaultMaxIterations, kFastLiveTolerance);
                valid &= !engine.lastStateRejected();
                std::copy_n(nodes.begin(), N_, samples[mode].data()+size_t(i)*N_);
            }
            times[mode] = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        }
        ratios[b] = times[1] / std::max(1e-12, times[0]);
        if (!crossedCutoffs.empty()) {
            // Reconstruct the reference terminals outside both timed regions.
            const auto voltage = [&](int port, int frame) {
                const auto& term = rootPortInfo_[port];
                const auto node = [&](int n) { return n == N_ ? 0.0 : samples[0][size_t(frame)*N_ + n]; };
                return node(term.a) - node(term.b) + net_.roffAff[port].eval(source);
            };
            for (int i = 0; i < frames; ++i) {
                source[inputSource] = dcValues_[inputSource] + input[i];
                for (const auto& guard : preview.loadedJunction_.reverseCutoffs) {
                    const double vbc = voltage(guard.port + 1, i) - voltage(guard.port, i);
                    if ((bjts_[guard.device].pnp ? -vbc : vbc) > guard.maxVbc)
                        crossedCutoffs[guard.device] = 1;
                }
            }
        }
        for (int j = 0; j < N_; ++j)
            for (int i = 0; i < frames; ++i) {
                const size_t k = size_t(i)*N_+j;
                const double err = samples[1][k] - samples[0][k];
                const double ac = samples[0][k] - initial[j];
                signal[j] += ac*ac;
                difference[j] += err*err;
            }
    }
    for (int j = 0; j < N_; ++j)
        if (!(difference[j] <= std::max(.015*.015*blocks*frames, .1*.1*signal[j])))
            valid = false;
    std::sort(ratios.begin(), ratios.end());
    loadedJunction_ = std::move(preview.loadedJunction_);
    loadedJunction_.seed = true;
    loadedJunction_.cooldown = loadedJunction_.misses = 0;
    loadedJunction_.revision = linearRebuilds();
    loadedJunction_.controlRevision = liveControlRevision();
    return valid && preview.loadedJunctionHits() > blocks*frames*3/5 &&
           ratios[blocks/2] < .92 && ratios[blocks*3/4] < 1.0;
}
} // namespace wdf
