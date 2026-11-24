#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <limits>
#include <cstddef>
#include <cstring>

#include "wdf/DenseTopology.h"
#include "wdf/solver/RootResidual.h"
#include "wdf/solver/RootJacobian.h"
#include "wdf/solver/SharedNewtonColumns.h"
#include "wdf/solver/RootClosedForms.h"

#if defined(_MSC_VER)
#define LIVEWDF_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define LIVEWDF_RESTRICT __restrict__
#else
#define LIVEWDF_RESTRICT
#endif

namespace wdf::solver {

struct NewtonContext {
    // Buffers and topology references
    std::vector<double>& vv;
    std::vector<double>& ii;
    std::vector<double>& J;
    std::vector<double>& F;
    const std::vector<double>& vOc;
    const std::vector<double>& Z;
    const std::vector<double>& Zred;
    SharedNewtonColumns* sharedColumns;
    const std::vector<double>& vvPrev;
    const std::vector<double>& vvPrev2;
    const std::vector<double>& predUp;
    const std::vector<double>& predDown;
    const std::vector<double>& stepUp;
    const std::vector<double>& stepDown;
    const std::vector<int>& redCol;
    std::vector<int>& redToActive;
    std::vector<int>& activeRedCol;
    std::vector<double>& Mr;
    std::vector<double>& rN;
    std::vector<double>& dv;
    std::vector<double>& vt;
    std::vector<double>& ft;
    std::vector<double>& Jt;
    std::vector<double>& iv;
    std::vector<double>& bestV;
    std::vector<double>& bestI;
    const std::vector<GridJunctionLimiter>& gridJunctionLimiters;
    const std::vector<JunctionLimiter>& junctionLimiters;
    const RootLayout& rootLayout;
    const std::vector<BjtModel>& bjts;
    const std::vector<RootDevice>& rootDevices;
    const Compiled& comp;
    const std::vector<double>& ctrlResValues;
    const std::vector<Pentode>& pentodes;
    const std::vector<OpAmpModel>& opAmps;
    const std::vector<double>& opAmpVp;
    const std::vector<double>& opAmpAlpha;
    const std::vector<double>& opAmpMaxDv;
    const std::vector<unsigned char>& triodeDomainPin;
    bool staticMode = false;
    int K = 0;
    int nRed = 0;
    bool keepBestOn = true;
    PhaseProfile* prof = nullptr;
    bool profileOn = false;

    // Diagnostics and counters
    long long& newtonIters;
    long long& lineTrials;
    long long& limitHits;
    long long& gridLimitHits;
    long long& deviceEvals;
    long long& jointLastIters;
    long long& jointLastTrials;
    double& bestFn;
    bool& keptBest;
    bool& lastConverged;
    double& lastResidual;
    bool skipIdenticalRetry = false;
};

class RootNewton {
public:
    static inline double maxAbs(const std::vector<double>& v, int n) noexcept {
        double m = 0.0;
        const double* d = v.data();
        for (int i = 0; i < n; ++i)
            m = std::max(m, std::abs(d[i]));
        return m;
    }

    static inline void currentsInto(NewtonContext& ctx, const std::vector<double>& v,
                                    std::vector<double>& i, std::vector<double>& J) noexcept {
        const double* LIVEWDF_RESTRICT vData = v.data();
        double* LIVEWDF_RESTRICT iData = i.data();
        double* LIVEWDF_RESTRICT jData = J.data();
        for (const auto& dev : ctx.rootDevices) {
            RootResidual::currentsForDevice(
                dev, ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(), ctx.bjts.data(),
                ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(), ctx.opAmpMaxDv.data(),
                ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(), ctx.K, vData, iData, jData,
                ctx.deviceEvals);
        }
    }

    static inline void currentsOnlyInto(NewtonContext& ctx, const std::vector<double>& v,
                                        std::vector<double>& i) noexcept {
        const double* LIVEWDF_RESTRICT vData = v.data();
        double* LIVEWDF_RESTRICT iData = i.data();
        for (const auto& dev : ctx.rootDevices) {
            RootResidual::currentsForDevice(
                dev, ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(), ctx.bjts.data(),
                ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(), ctx.opAmpMaxDv.data(),
                ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(), ctx.K, vData, iData, nullptr,
                ctx.deviceEvals);
        }
    }

    static inline void residualInto(const NewtonContext& ctx, const std::vector<double>& v,
                                    const std::vector<double>& vOc, const std::vector<double>& Z,
                                    const std::vector<double>& i, std::vector<double>& F) noexcept {
        if (ctx.sharedColumns && ctx.sharedColumns->residual(
                v.data(), vOc.data(), i.data(), ctx.redCol.data(), ctx.K, F.data()))
            return;
        if (!ctx.Zred.empty()) {
            RootResidual::residualIntoFast(
                v.data(), vOc.data(), ctx.Zred.data(), i.data(), F.data(),
                ctx.redCol.data(), ctx.nRed, ctx.K);
        } else {
            RootResidual::residualInto(
                v.data(), vOc.data(), Z.data(), i.data(), F.data(),
                ctx.redCol.data(), ctx.nRed, ctx.K);
        }
    }

    static inline int limitDiodePorts(NewtonContext& ctx, double* v, const double* vOld) noexcept {
        return RootResidual::limitDiodePorts(
            v, vOld, ctx.junctionLimiters.data(), ctx.junctionLimiters.size(),
            ctx.gridJunctionLimiters.data(), ctx.gridJunctionLimiters.size(), ctx.gridLimitHits);
    }

    static inline int limitBjtJunctions(NewtonContext& ctx, double* v, const double* vOld) noexcept {
        return RootResidual::limitBjtJunctions(v, vOld, ctx.rootLayout, ctx.bjts.data());
    }

    // Select a trial just beyond the last hard-clamp boundary on a
    // rejected Newton step. The residual still certifies the trial using
    // the original device law.
    static inline double clampAwareStep(const NewtonContext& ctx, double rejected) noexcept {
        if (ctx.opAmps.empty())
            return rejected * .5;
        double boundary = 0.0;
        for (const RootDevice& dev : ctx.rootDevices) {
            if (dev.kind != DeviceKind::OpAmp)
                continue;
            const auto j = dev.compIndex;
            const OpAmpModel& amp = ctx.opAmps[j];
            const int o = dev.portBase;
            const double prev = ctx.opAmpVp[j];
            const double alpha = std::clamp(ctx.opAmpAlpha[j], 0.0, 1.0);
            const double gain = ctx.staticMode ? amp.openLoopGain : alpha * amp.openLoopGain / (1.0 + alpha);
            const double bias = ctx.staticMode ? 0.0 : prev / (1.0 + alpha);
            const double raw = bias + gain * (ctx.vv[o + 1] - ctx.vv[o + 2]);
            const double delta = gain * (ctx.dv[o + 1] - ctx.dv[o + 2]);
            if (delta == 0.0)
                continue;
            const double maxDv = ctx.opAmpMaxDv[j];
            double lo = amp.softRails ? -std::numeric_limits<double>::infinity() : amp.railLow();
            double hi = amp.softRails ? std::numeric_limits<double>::infinity() : amp.railHigh();
            if (!ctx.staticMode) {
                lo = std::max(lo, prev - maxDv);
                hi = std::min(hi, prev + maxDv);
            }
            if (lo >= hi)
                continue;
            for (double limit : {lo, hi}) {
                const double hit = (limit - raw) / delta;
                if (hit > boundary && hit < rejected * .95)
                    boundary = hit;
            }
        }
        if (boundary < 1e-6)
            return rejected * .5;
        return std::min(boundary * 1.01, rejected * .95);
    }

    static double predict(double last, double older, double down, double up) noexcept {
        return last + std::clamp(last - older, -down, up);
    }

    static bool predictionChangesSeed(const NewtonContext& ctx) noexcept {
        for (int r = 0; r < ctx.K; ++r) {
            const double seed = predict(ctx.vvPrev[r], ctx.vvPrev2[r], ctx.predDown[r], ctx.predUp[r]);
            if (std::memcmp(&seed, &ctx.vvPrev[r], sizeof(seed)) != 0)
                return true;
        }
        return false;
    }

    /// One pass of `solveRootsNewton`, with or without the extrapolated seed.
    static inline void solveRootsNewtonPass(NewtonContext& ctx, int maxIter, double tol,
                                           bool usePrediction, bool useCurrentSeed = false) noexcept {
        const long long jointIt0 = ctx.newtonIters;
        const long long jointTr0 = ctx.lineTrials;
        using clk = std::chrono::steady_clock;
        const auto tick = [&] { return ctx.profileOn ? clk::now() : clk::time_point{}; };
        clk::time_point tPhase = tick();
        const auto tock = [&](double& acc) {
            if (ctx.profileOn)
                acc += std::chrono::duration<double>(clk::now() - tPhase).count();
        };


        const double* LIVEWDF_RESTRICT vPrev = ctx.vvPrev.data();
        const double* LIVEWDF_RESTRICT vPrev2 = ctx.vvPrev2.data();
        double* LIVEWDF_RESTRICT vCur = ctx.vv.data();
        const double* LIVEWDF_RESTRICT pUp = ctx.predUp.data();
        const double* LIVEWDF_RESTRICT pDown = ctx.predDown.data();

        if (!useCurrentSeed) {
            if (!usePrediction) {
                std::memcpy(vCur, vPrev, static_cast<size_t>(ctx.K) * sizeof(double));
            } else {
                for (int r = 0; r < ctx.K; ++r) {
                    vCur[r] = predict(vPrev[r], vPrev2[r], pDown[r], pUp[r]);
                }
            }
        }

        for (const auto& gl : ctx.gridJunctionLimiters) {
            const int r = gl.port;
            bool limited = false;
            ctx.vv[static_cast<size_t>(r)] =
                gl.limitGrid(ctx.vvPrev[static_cast<size_t>(r)],
                             ctx.vv[static_cast<size_t>(r)], limited);
        }
        if (ctx.prof)
            tock(ctx.prof->predict);

        bool curValid = false;
        bool reached = false;
        ctx.bestFn = std::numeric_limits<double>::infinity();
        double base = 0.0;
        for (int it = 0; it < maxIter; ++it) {
            ++ctx.newtonIters;
            if (!curValid) {
                tPhase = tick();
                currentsInto(ctx, ctx.vv, ctx.ii, ctx.J);
                residualInto(ctx, ctx.vv, ctx.vOc, ctx.Z, ctx.ii, ctx.F);
                if (ctx.prof)
                    tock(ctx.prof->device);
                curValid = true;
                base = maxAbs(ctx.F, ctx.K);
            }
            if (base < ctx.bestFn) {
                ctx.bestFn = base;
                ctx.bestV = ctx.vv;
                ctx.bestI = ctx.ii;
            }
            if (base < tol) {
                reached = true;
                break;
            }

            tPhase = tick();
            const auto assembleFull = [&] {
                if (!ctx.Zred.empty()) {
                    RootJacobian::reducedAssembleFast(
                        ctx.Zred.data(), ctx.J.data(), ctx.F.data(), ctx.nRed, ctx.K,
                        ctx.rootDevices.data(), ctx.rootDevices.size(), ctx.Mr.data(), ctx.rN.data());
                } else {
                    RootJacobian::reducedAssemble(
                        ctx.Z.data(), ctx.J.data(), ctx.F.data(), ctx.redCol.data(), ctx.nRed, ctx.K,
                        ctx.rootDevices.data(), ctx.rootDevices.size(), ctx.Mr.data(), ctx.rN.data());
                }
            };
            if (ctx.sharedColumns)
                ctx.sharedColumns->assemble(ctx.J.data(), ctx.F.data(), ctx.K,
                                            ctx.redCol.data(), ctx.rootDevices);
            else
                assembleFull();
            if (ctx.prof)
                tock(ctx.prof->jacobian);

            tPhase = tick();
            bool solved = ctx.sharedColumns && ctx.sharedColumns->finish(ctx.F.data(), ctx.K, ctx.dv.data());
            if (!solved) {
                if (ctx.sharedColumns)
                    assembleFull();
                solved = !ctx.Zred.empty()
                    ? RootJacobian::reducedFinishFast(ctx.Mr.data(), ctx.rN.data(), ctx.nRed,
                          ctx.Zred.data(), ctx.F.data(), ctx.K, ctx.dv.data())
                    : RootJacobian::reducedFinish(ctx.Mr.data(), ctx.rN.data(), ctx.nRed,
                          ctx.Z.data(), ctx.F.data(), ctx.redCol.data(), ctx.K, ctx.dv.data());
            }
            if (ctx.prof)
                tock(ctx.prof->lu);
            if (!solved)
                break;

            double scale = 1.0;
            const double* LIVEWDF_RESTRICT dvData = ctx.dv.data();
            const double* LIVEWDF_RESTRICT sUp = ctx.stepUp.data();
            const double* LIVEWDF_RESTRICT sDown = ctx.stepDown.data();
            for (int r = 0; r < ctx.K; ++r) {
                const double d = dvData[r];
                const double lim = (d >= 0.0) ? sUp[r] : sDown[r];
                const double mag = std::abs(d);
                if (mag * scale > lim)
                    scale = lim / mag;
            }
            if (scale < 1.0) {
                ++ctx.limitHits;
                for (int r = 0; r < ctx.K; ++r)
                    ctx.dv[static_cast<size_t>(r)] *= scale;
            }

            double stepSize = 1.0;
            tPhase = tick();
            bool stepHasJ = false;
            double ftNorm = 0.0;
            // Near a hard op-amp rail the fourth trial can still worsen the
            // full residual. A few more backtracks avoid a discarded sample.
            const int maxTrials = ctx.sharedColumns && !ctx.opAmps.empty() ? 10 : 4;
            for (int h = 0; h < maxTrials; ++h) {
                ++ctx.lineTrials;
                for (int r = 0; r < ctx.K; ++r)
                    ctx.vt[static_cast<size_t>(r)] = ctx.vv[static_cast<size_t>(r)] + stepSize * ctx.dv[static_cast<size_t>(r)];

                ctx.limitHits += limitDiodePorts(ctx, ctx.vt.data(), ctx.vv.data());
                ctx.limitHits += limitBjtJunctions(ctx, ctx.vt.data(), ctx.vv.data());
                if (h == 0) {
                    currentsInto(ctx, ctx.vt, ctx.iv, ctx.Jt);
                    stepHasJ = true;
                } else {
                    currentsOnlyInto(ctx, ctx.vt, ctx.iv);
                    stepHasJ = false;
                }
                residualInto(ctx, ctx.vt, ctx.vOc, ctx.Z, ctx.iv, ctx.ft);
                ftNorm = maxAbs(ctx.ft, ctx.K);
                if (ftNorm <= base || h + 1 == maxTrials)
                    break;
                stepSize = clampAwareStep(ctx, stepSize);
            }
            if (ctx.prof)
                tock(ctx.prof->linesearch);

            double maxDelta = 0.0, maxV = 0.0;
            for (int r = 0; r < ctx.K; ++r) {
                const size_t rs = static_cast<size_t>(r);
                maxDelta = std::max(maxDelta, std::abs(ctx.vt[rs] - ctx.vv[rs]));
                ctx.vv[rs] = ctx.vt[rs];
                maxV = std::max(maxV, std::abs(ctx.vv[rs]));
            }

            ctx.ii.swap(ctx.iv);
            if (stepHasJ)
                ctx.J.swap(ctx.Jt);
            ctx.F.swap(ctx.ft);
            curValid = stepHasJ;
            base = ftNorm;
            if (base < ctx.bestFn) {
                ctx.bestFn = base;
                ctx.bestV = ctx.vv;
                ctx.bestI = ctx.ii;
            }

            if (base < tol) {
                reached = true;
                break;
            }
            if (it > 0 && maxDelta < 1e-13 * std::max(1.0, maxV))
                break;
        }

        if (!curValid) {
            tPhase = tick();
            currentsInto(ctx, ctx.vv, ctx.ii, ctx.J);
            residualInto(ctx, ctx.vv, ctx.vOc, ctx.Z, ctx.ii, ctx.F);
            if (ctx.prof)
                tock(ctx.prof->device);
            curValid = true;
            base = maxAbs(ctx.F, ctx.K);
        }
        if (base < ctx.bestFn) {
            ctx.bestFn = base;
            ctx.bestV = ctx.vv;
            ctx.bestI = ctx.ii;
        }

        bool kept_best = false;
        const double finalFn = base;
        if (!reached && ctx.keepBestOn && ctx.bestFn < finalFn) {
            ctx.vv = ctx.bestV;
            ctx.ii = ctx.bestI;
            kept_best = true;
        }
        ctx.keptBest = kept_best;
        ctx.lastResidual = kept_best ? ctx.bestFn : finalFn;
        ctx.lastConverged = reached || ((kept_best ? ctx.bestFn : finalFn) < tol);
        ctx.jointLastIters = ctx.newtonIters - jointIt0;
        ctx.jointLastTrials = ctx.lineTrials - jointTr0;
    }

    /// The callers pin from the previous sample's currents before solving, so
    /// re-read the pin from the solved ones -- the in-sample test the closed
    /// form makes -- and solve again while a triode changed domain, at most
    /// `view.count` more times.
    static inline void resolveTriodeDomains(NewtonContext& ctx, int maxIter, double tol,
                                            const BlockView& view) noexcept {
        for (int pass = 0; pass < view.count &&
                           RootClosedForms::pinTriodeDomains(
                               view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                               const_cast<unsigned char*>(ctx.triodeDomainPin.data()),
                               ctx.comp, ctx.rootDevices.data(), ctx.K);
             ++pass)
            solveRootsNewtonPass(ctx, maxIter, tol, false, true);
    }

    /// Multiport Newton for the device currents.
    static inline void solveRootsNewton(NewtonContext& ctx, int maxIter, double tol,
                                        const BlockView& view, std::vector<double>& vvPrev,
                                        std::vector<double>& vvPrev2) noexcept {
        RootClosedForms::pinTriodeDomains(
            view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
            const_cast<unsigned char*>(ctx.triodeDomainPin.data()),
            ctx.comp, ctx.rootDevices.data(), ctx.K);
        solveRootsNewtonPass(ctx, maxIter, tol, true, false);
        // The device laws and pinned domains stay fixed between these two passes.
        if (!ctx.lastConverged && (!ctx.skipIdenticalRetry || predictionChangesSeed(ctx))) {
            solveRootsNewtonPass(ctx, maxIter, tol, false, false);
        }
        resolveTriodeDomains(ctx, maxIter, tol, view);
        vvPrev2 = vvPrev;
        vvPrev = ctx.vv;
    }

    /// Multiport Newton seeded from the block solve's result.
    static inline void solveRootsNewtonSeeded(NewtonContext& ctx, int maxIter, double tol,
                                              const BlockView& view, std::vector<double>& vvPrev,
                                              std::vector<double>& vvPrev2) noexcept {
        RootClosedForms::pinTriodeDomains(
            view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
            const_cast<unsigned char*>(ctx.triodeDomainPin.data()),
            ctx.comp, ctx.rootDevices.data(), ctx.K);
        solveRootsNewtonPass(ctx, maxIter, tol, false, true);
        if (!ctx.lastConverged)
            solveRootsNewtonPass(ctx, maxIter, tol, false, false);
        resolveTriodeDomains(ctx, maxIter, tol, view);
        vvPrev2 = vvPrev;
        vvPrev = ctx.vv;
    }

};

} // namespace wdf::solver
