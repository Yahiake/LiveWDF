#pragma once

// Time-domain WDF engine: R-type root network, bilinear reactive elements.
//
// This header is the public face of the engine and nothing else -- the whole
// implementation lives beside it in `src/wdf/`, one level per job:
//
//     EngineCore     topology, state, buffers, diagnostics   (level 1)
//     EngineSolve    the nonlinear root: Newton + closed form (level 2)
//     EngineCompile  the R-type transfer, DC, parameters      (level 3)
//     EngineStep     seeding and one sample                   (level 4)
//
// `Engine` is the single type a caller uses. The levels below it are an
// implementation detail of this file's organization, not a second API: they are
// public bases so every method above stays reachable on an `Engine`, and every
// level owns no state of its own but reaches the one that does through the
// protected members in `EngineCore`.
//
// The per-sample math, and why each piece is where it is, is documented at the
// top of `EngineCore.h`; the derivations are in `docs/PERFORMANCE.md`.

#include "wdf/EngineStep.h"

#include <array>
#include <memory>
#include <optional>
#include <span>

namespace wdf {

class Engine : public EngineStep {
    static constexpr size_t planLimit = 5;
public:
    /// Compile `circuit` and prepare to run at `fs` Hz. Throws if the network is
    /// singular. Runs the transfer build, sizes every buffer, and seeds the
    /// reactive states from the DC operating point, so the first `step()` already
    /// reproduces the operating point. `gridCurrent = false` builds every triode
    /// with the plate-only law.
    Engine(const Circuit& circuit, double fs, bool gridCurrent = true)
        : EngineStep(circuit, fs, gridCurrent), gridCurrent_(gridCurrent) {
        if (!rebuildLinear() &&
            (!embedOpAmpOutputResistance() || !rebuildLinear()))
            throw std::runtime_error("rootTransfer failed (singular linear network)");
        allocateBuffers();
        reset();
    }

    void reset() {
        if (bipolarPreview_) resetBipolarPreviewHistory();
        EngineStep::reset();
    }

    const std::vector<double>& step(const std::vector<double>& sources,
                                    int maxIter = kDefaultMaxIterations, double tol = kDefaultTolerance) {
        return step(sources.data(), sources.size(), maxIter, tol);
    }

    const std::vector<double>& step(const double* sources, size_t count,
                                    int maxIter = kDefaultMaxIterations, double tol = kDefaultTolerance) {
        if (bipolarPreview_) releaseBipolarPreview();
        return EngineStep::step(sources, count, maxIter, tol);
    }

    // A single-output approximation for static BJT/diode networks. Preparation
    // allocates and qualifies the fixed schematic away from the audio thread.
    bool prepareBipolarPreview(size_t inputSource, int outputNode);
    /// Off-thread preparation and playback screen against the current Fast solve.
    bool prepareAffineNodePreview(size_t inputSource, int outputNode);
    bool stepBipolarPreview(double inputVolts, double& outputVolts) noexcept;
    bool bipolarPreviewActive() const noexcept;
    void releaseBipolarPreview() noexcept;
    // The caller has validated the worker's topology and controls for this engine.
    void adoptBipolarPreview(Engine& prepared) noexcept;

    /// Prepare at most five affine slope patterns for a coupled root. Call
    /// outside the audio callback. A separate engine measures the patterns,
    /// so preparing a plan never changes this engine's audio state. A change
    /// to the linear network discards the plans along with the root partition.
    size_t prepareMixedAffineReduction(size_t inputSource, double peakVolts = 0.36);

    /// Snapshot setup data on the control side. The job can be built without
    /// touching the live engine and installed with a short buffer swap.
    class MixedAffineSetup {
    public:
        void build(std::optional<std::span<const unsigned>> cachedMasks = {});
        std::vector<unsigned> slopeMasks() const;
        double sampleRate() const noexcept { return rate_; }
        size_t inputSource() const noexcept { return input_; }
        std::span<const double> transferMatrix() const noexcept { return transfer_; }

    private:
        friend class Engine;
        const Engine* owner_ = nullptr;
        Circuit circuit_;
        double rate_ = 0.0;
        size_t input_ = 0;
        bool gridCurrent_ = true;
        long long revision_ = 0;
        std::vector<double> transfer_;
        std::vector<NewtonBlock> plans_;
    };

    std::optional<MixedAffineSetup> captureMixedAffineSetup(size_t inputSource) const;
    bool installMixedAffineSetup(MixedAffineSetup& setup);
    void useMixedAffineReduction(bool enabled) noexcept { mixedAffineEnabled_ = enabled; }

    long long mixedAffineRevision() const noexcept { return mixedAffineRevision_; }
    long long mixedAffineAttempts() const { return mixedAffineAttempts_; }
    long long mixedAffineAccepted() const { return mixedAffineAccepted_; }

    class LiveTransferSetup {
    public:
        explicit LiveTransferSetup(const Circuit& circuit) : circuit_(circuit) {}
        void build(size_t inputSource, bool prepareAffine);
        void prepareCompiledPreview(size_t inputSource, int outputNode, bool allowOutputOnly);
        /// Compare the prepared channels on the worker and drop unprofitable plans.
        bool screenCircuitPlan(LiveTransferSetup& peer, size_t inputSource, int outputNode);

    private:
        friend class Engine;
        const Engine* owner_ = nullptr;
        Circuit circuit_;
        double rate_ = 0.0;
        bool gridCurrent_ = true, planOnly_ = false, compiledPreview_ = false;
        std::array<unsigned, planLimit> affineMasks_{};
        size_t maskCount_ = 0;
        long long linearRevision_ = 0, controlRevision_ = 0, affineRevision_ = 0;
        std::unique_ptr<Engine> prepared_;
    };

    /// Copy live numeric settings into a setup created from this engine's
    /// original circuit. Build on a worker; installation keeps live DSP state.
    /// planOnly snapshots an unadjusted transfer and installs just its solver plan.
    bool captureLiveTransferSetup(LiveTransferSetup& setup, bool planOnly = false) const noexcept;
    bool canInstallLiveTransferSetup(const LiveTransferSetup& setup) const noexcept;
    bool installLiveTransferSetup(LiveTransferSetup& setup) noexcept;

private:
    long long mixedAffineRevision_ = 0;
    bool gridCurrent_;
    void resetBipolarPreviewHistory() noexcept;
    struct BipolarPreview;
    std::shared_ptr<BipolarPreview> bipolarPreview_;
    bool canPrepareMixedAffineReduction(size_t inputSource) const noexcept;
    void compileMixedAffinePlans(std::span<const unsigned> masks);
    std::vector<std::pair<unsigned, int>> measureAffineSlopes(size_t inputSource, double peakVolts);
};

}  // namespace wdf
