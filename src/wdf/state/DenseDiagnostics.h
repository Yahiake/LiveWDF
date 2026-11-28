#pragma once

#include "wdf/DenseTopology.h"

namespace wdf::state {

class DenseDiagnostics {
public:
    mutable long long newtonIters_ = 0;
    mutable long long deviceEvals_ = 0;
    mutable long long lineTrials_ = 0;
    mutable long long limitHits_ = 0;
    mutable long long gridLimitHits_ = 0;
    mutable long long knotStepChoices_ = 0;
    mutable long long knotTriggeredSolves_ = 0;
    mutable long long knotConvergedSolves_ = 0;
    mutable long long knotConvergedIterations_ = 0;

    mutable long long jfetFallbacks_ = 0;
    bool jfetSolveFailed_ = false;
    mutable long long mosfetFallbacks_ = 0;
    bool mosfetSolveFailed_ = false;
    mutable long long opAmpSatFallbacks_ = 0;

    bool profileOn_ = false;
    PhaseProfile prof_{};

    void resetPhaseProfile() { prof_ = PhaseProfile{}; }
    void resetDiagnostics() {
        newtonIters_ = 0;
        deviceEvals_ = 0;
        lineTrials_ = 0;
        limitHits_ = 0;
        gridLimitHits_ = 0;
        jfetFallbacks_ = 0;
        jfetSolveFailed_ = false;
        mosfetFallbacks_ = 0;
        mosfetSolveFailed_ = false;
        opAmpSatFallbacks_ = 0;
        prof_ = PhaseProfile{};
    }
};

}  // namespace wdf::state
