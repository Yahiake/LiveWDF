#include "wdf/Engine.h"

#include <chrono>
#include <ctime>
#include <numbers>

namespace wdf {
namespace {

double profileSeconds() noexcept {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
    return std::numeric_limits<double>::quiet_NaN();
#else
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// The two worker-owned engines start at their DC point with the same controls.
// Compare the existing Fast Live solver to exactly the plan being published.
template<size_t blocks = 32>
bool winsPlaybackScreen(Engine& candidate, Engine& reference, size_t input, int output,
                        bool referenceCompiled, std::chrono::steady_clock::time_point stop,
                        bool testOverload = false) {
    constexpr size_t frames = 128;
    using Clock = std::chrono::steady_clock;
    std::array<std::array<double, blocks>, 2> elapsed{};
    std::array<std::array<double, frames>, 2> audio{};
    std::array<double, frames> drive{};
    auto sources = reference.dcValues();
    const double bias = sources[input];
    const double rate = reference.fs();
    std::array<Engine*, 2> engines{&reference, &candidate};
    candidate.useCircuitPlan(true);
    reference.useCircuitPlan(referenceCompiled);
    const double secondsPerBlock = frames / rate;
    int misses[2]{};
    for (size_t b = 0; b < blocks; ++b) {
        for (size_t i = 0; i < frames; ++i) {
            const double time = (b * frames + i) / rate;
            // Quiet input, clipping, a decaying note, then silence.
            const double level = b < 8 ? .005 : b < 16 ? 1.2 :
                b < 24 ? 1.2 * std::exp(-6.0 * ((b - 16) + double(i) / frames) / 8.0) : 0.;
            const double phase = 2.0 * std::numbers::pi * time;
            if (testOverload) {
                // Exercise clip entry, sustained overload and recovery. An ill-conditioned
                // reference can first reject well after the initial transition.
                const double driveLevel = b < blocks/4 ? .01 : b < blocks/2 ? 38. :
                                          b < 3*blocks/4 ? 1.5 : .0001;
                drive[i] = bias + driveLevel * (.6 * std::sin(997. * phase) + .34 * std::sin(6393. * phase) +
                                                .098 * std::sin(19103. * phase));
            } else
                drive[i] = bias + level * (.7 * std::sin(193. * phase) + .3 * std::sin(1327. * phase));
        }
        for (size_t order = 0; order < 2; ++order) {
            const size_t mode = (b + order) % 2;
            Engine& engine = *engines[mode];
            const double start = profileSeconds();
            for (size_t i = 0; i < frames; ++i) {
                sources[input] = drive[i];
                audio[mode][i] = engine.step(sources, Engine::kDefaultMaxIterations,
                                             Engine::kFastLiveTolerance)[output];
                if (engine.lastStateRejected() || !std::isfinite(audio[mode][i]) ||
                    ((i % 16) == 15 && Clock::now() >= stop))
                    return false;
            }
            elapsed[mode][b] = profileSeconds() - start;
            if (!(elapsed[mode][b] > 0.0))
                return false;
            misses[mode] += elapsed[mode][b] > secondsPerBlock;
            if (Clock::now() >= stop)
                return false;
        }
        for (size_t i = 0; i < frames; ++i)
            if (std::abs(audio[1][i] - audio[0][i]) > 2e-6 + 1e-5 * std::abs(audio[0][i]))
                return false;
    }
    for (auto& times : elapsed)
        std::sort(times.begin(), times.end());
    // Require a clear median win without buying it with worse callback tails.
    return elapsed[1][blocks / 2] < .90 * elapsed[0][blocks / 2] &&
           elapsed[1][blocks - 2] <= 1.05 * elapsed[0][blocks - 2] &&
           elapsed[1].back() <= 1.10 * elapsed[0].back() && misses[1] <= misses[0];
}
} // namespace

bool Engine::prepareAffineNodePreview(size_t inputSource, int outputNode) {
    if (inputSource >= dcValues_.size() || outputNode < 0 || outputNode >= N_) {
        affineNodes_ = {};
        return false;
    }
    if (!buildAffineNodePreview())
        return false;
    Engine candidate(circuit_, fs_, gridCurrent_), reference(circuit_, fs_, gridCurrent_);
    // Exercise private engines so screening never consumes a live sample or
    // leaves a fallback factorization from the overload test attached.
    if (reference.Z_ != Z_ || reference.Xe_ != Xe_ || reference.Xr_ != Xr_) {
        affineNodes_ = {};
        return false;
    }
    for (Engine* engine : {&candidate, &reference}) {
        engine->prepareCircuitPlan();
        if (affineRoot_.rows.empty())
            engine->affineRoot_ = {};
        if (rootFactors_.empty())
            engine->rootFactors_.clear();
        if (driveCoefficients_.empty())
            engine->driveCoefficients_.clear();
    }
    candidate.affineNodes_ = std::move(affineNodes_);
    candidate.affineNodes_.revision = candidate.linearRebuilds();
    candidate.useLoadedJunctionPreview(true);
    const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    const bool passed = winsPlaybackScreen<96>(candidate, reference, inputSource, outputNode, true, stop, true) &&
                        candidate.affineNodePreviewHits() > 0;
    if (passed) {
        affineNodes_ = std::move(candidate.affineNodes_);
        affineNodes_.revision = linearRebuilds();
    } else {
        affineNodes_ = {};
    }
    return passed;
}

bool Engine::LiveTransferSetup::screenCircuitPlan(LiveTransferSetup& peer, size_t inputSource, int outputNode) {
    if (!prepared_ || !peer.prepared_)
        return false;
    auto& candidate = *prepared_;
    auto& reference = *peer.prepared_;
    const bool eligible = !candidate.rootFactors_.empty() || candidate.hasLinearDrivePlan() ||
                          candidate.hasAffineRootPlan();
    if (eligible && maskCount_ == peer.maskCount_ &&
        std::equal(affineMasks_.begin(), affineMasks_.begin() + maskCount_, peer.affineMasks_.begin()) &&
        rate_ == peer.rate_ && candidate.net_.eG == reference.net_.eG &&
        inputSource < candidate.dcValues().size() && candidate.dcValues() == reference.dcValues() &&
        outputNode >= 0 && outputNode < candidate.N_ && outputNode < reference.N_) {
        const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
        if (candidate.hasAffineRootPlan()) {
            // Pay for a direct candidate only if it beats the already prepared
            // nonlinear predictor. Compare both from the same physical reset.
            auto direct = std::move(reference.affineRoot_);
            const auto initialFactors = reference.rootFactors_;
            const auto initialPivots = reference.rootPivots_;
            if (winsPlaybackScreen(candidate, reference, inputSource, outputNode, true, stop))
                reference.affineRoot_ = std::move(direct);
            else
                candidate.affineRoot_ = {};
            for (auto* engine : {&candidate, &reference}) {
                engine->rootFactors_ = initialFactors;
                engine->rootPivots_ = initialPivots;
                engine->reset();
            }
        }
        if (winsPlaybackScreen(candidate, reference, inputSource, outputNode, false, stop)) {
            // Screening can refactor the candidate at a hot sample. Publish
            // the same initial tangent to both newly installed channels.
            candidate.rootFactors_ = reference.rootFactors_;
            candidate.rootPivots_ = reference.rootPivots_;
            candidate.circuitPlanRetry_ = reference.circuitPlanRetry_;
            return true;
        }
    }

    for (auto* engine : {&candidate, &reference}) {
        engine->driveCoefficients_.clear();
        engine->driveInputs_.clear();
        engine->affineRoot_ = {};
        engine->rootFactors_.clear();
        engine->circuitResidualColumns_.clear();
        engine->rootPivots_.clear();
    }
    return false;
}

} // namespace wdf
