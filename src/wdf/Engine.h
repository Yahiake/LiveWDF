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

namespace wdf {

class Engine : public EngineStep {
public:
    /// Compile `circuit` and prepare to run at `fs` Hz. Throws if the network is
    /// singular. Runs the transfer build, sizes every buffer, and seeds the
    /// reactive states from the DC operating point, so the first `step()` already
    /// reproduces the operating point. `gridCurrent = false` builds every triode
    /// with the plate-only law.
    Engine(const Circuit& circuit, double fs, bool gridCurrent = true)
        : EngineStep(circuit, fs, gridCurrent) {
        if (!rebuildLinear() &&
            (!embedOpAmpOutputResistance() || !rebuildLinear()))
            throw std::runtime_error("rootTransfer failed (singular linear network)");
        allocateBuffers();
        reset();
    }

    /// Prepare at most five affine slope patterns for a coupled root. Call
    /// outside the audio callback. A separate engine measures the patterns,
    /// so preparing a plan never changes this engine's audio state. A change
    /// to the linear network discards the plans along with the root partition.
    size_t prepareMixedAffineReduction(size_t inputSource, double peakVolts = 0.36);

    long long mixedAffineAttempts() const { return mixedAffineAttempts_; }
    long long mixedAffineAccepted() const { return mixedAffineAccepted_; }

private:
    std::vector<std::pair<unsigned, int>> measureAffineSlopes(size_t inputSource, double peakVolts);
};

}  // namespace wdf
