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
#include "wdf/solver/PentodePlatePair.h"
#include "wdf/solver/RootJacobian.h"
#include "wdf/solver/SharedNewtonColumns.h"
#include "wdf/solver/TriodeDomainTransfer.h"
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
    const TriodeDomainTransfer* triodeTransfer;
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
    bool pentodePreview = false;
    double modeTol = 0.0;
    const unsigned char* frozen = nullptr;  ///< Replayed ports: their residual is not the solve's
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
                v.data(), vOc.data(), i.data(), ctx.redCol.data(), ctx.K, F.data())) {
        } else if (!ctx.Zred.empty()) {
            RootResidual::residualIntoFast(
                v.data(), vOc.data(), ctx.Zred.data(), i.data(), F.data(),
                ctx.redCol.data(), ctx.nRed, ctx.K);
        } else {
            RootResidual::residualInto(
                v.data(), vOc.data(), Z.data(), i.data(), F.data(),
                ctx.redCol.data(), ctx.nRed, ctx.K);
        }
        for (int r = 0; ctx.frozen && r < ctx.K; ++r)
            F[static_cast<size_t>(r)] *= !ctx.frozen[r];
    }

    static double roundingFloor(const NewtonContext& ctx) noexcept {
        const int K = ctx.K;
        double floor = 0.0;
        for (int r = 0; r < K; ++r) {
            double row = std::abs(ctx.vv[r]) + std::abs(ctx.vOc[r]);
            for (int j = 0; j < K; ++j) {
                if (ctx.Z[r * K + j] == 0.0)
                    continue;
                double scale = std::abs(ctx.ii[j]);
                for (int k = 0; k < K; ++k)
                    scale += std::abs(ctx.J[j * K + k] * ctx.vv[k]);
                row += std::abs(ctx.Z[r * K + j]) * scale;
            }
            floor = std::max(floor, row);
        }
        return 64.0 * std::numeric_limits<double>::epsilon() * floor;
    }

    static const RootDevice* coupledPentodes(const NewtonContext& ctx) noexcept {
        if (!ctx.pentodePreview || ctx.staticMode || ctx.pentodes.size() != 2 || ctx.rootDevices.size() < 2)
            return nullptr;
        const auto& devices = ctx.rootDevices;
        const auto left = std::find_if(devices.begin(), devices.end(), [](const RootDevice& d) {
            return d.kind == DeviceKind::Pentode;
        });
        if (left == devices.end() || left + 1 == devices.end() || left[1].kind != DeviceKind::Pentode)
            return nullptr;
        const int l = left[0].portBase, r = left[1].portBase;
        const double* zl = ctx.Z.data() + l * ctx.K;
        const double* zr = ctx.Z.data() + r * ctx.K;
        // Grid current is held fixed. Screen current changes with plate voltage,
        // so an isolated plate-pair map requires no screen-to-plate transfer.
        if (zl[l + 2] != 0. || zl[r + 2] != 0. || zr[l + 2] != 0. || zr[r + 2] != 0. ||
            zl[r] == 0. || !(std::abs(zl[r] - zr[l]) <= 1e-12 * std::abs(zl[r])))
            return nullptr;
        return &*left;
    }

    static double projectPentodePlates(NewtonContext& ctx, const RootDevice* pair, std::vector<double>& v,
                                     std::vector<double>& i, std::vector<double>* jacobian,
                                     std::vector<double>& residual, double initialNorm) noexcept {
        const auto& left = pair[0];
        const auto& right = pair[1];
        const int l = left.portBase, r = right.portBase;
        // Near the solution a further ordinary Newton step costs less.
        if (std::max(std::abs(residual[l]), std::abs(residual[r])) < .05)
            return initialNorm;
        const double* zl = ctx.Z.data() + l * ctx.K;
        const double* zr = ctx.Z.data() + r * ctx.K;
        const auto& model1 = ctx.pentodes[left.compIndex];
        const auto& model2 = ctx.pentodes[right.compIndex];
        DerkDPlate law1, law2;
        if (!law1.at(model1, v.data() + l, i[l + 2]) || !law2.at(model2, v.data() + r, i[r + 2]))
            return initialNorm;
        const double drive1 = v[l] - residual[l] + zl[l] * i[l] + zl[r] * i[r];
        const double drive2 = v[r] - residual[r] + zr[l] * i[l] + zr[r] * i[r];
        double plate1, plate2;
        if (!solvePentodePlatePair(law1, law2, drive1, drive2, zl[l], zr[r], zl[r], plate1, plate2))
            return initialNorm;

        // A plate crossing can abruptly change screen current. Accept the
        // projection only if every row stays within the original residual norm.
        const auto screenDelta = [&](int port, double plate, const DerkDPlate& law, const ReefmanPentode& m) {
            return (plate >= 0. ? law.screenBase * m.split().screenShare(plate) : 0.) - i[port + 2];
        };
        const double dIp1 = law1.current(plate1) - i[l], dIp2 = law2.current(plate2) - i[r];
        const double dIs1 = screenDelta(l, plate1, law1, model1.reefman);
        const double dIs2 = screenDelta(r, plate2, law2, model2.reefman);
        for (int j = 0; j < ctx.K; ++j) {
            const double* row = ctx.Z.data() + j * ctx.K;
            const double dv = j == l ? plate1 - v[l] : (j == r ? plate2 - v[r] : 0.);
            const double projected = residual[j] + dv + row[l] * dIp1 + row[r] * dIp2 +
                                     row[l + 2] * dIs1 + row[r + 2] * dIs2;
            if (!(std::abs(projected) <= initialNorm))
                return initialNorm;
        }
        v[l] = plate1;
        v[r] = plate2;
        for (const auto* device : {&left, &right}) {
            RootResidual::currentsForDevice(*device, ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(),
                ctx.bjts.data(), ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(), ctx.opAmpMaxDv.data(),
                ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(), ctx.K, v.data(), i.data(),
                jacobian ? jacobian->data() : nullptr, ctx.deviceEvals);
        }
        residualInto(ctx, v, ctx.vOc, ctx.Z, i, residual);
        return maxAbs(residual, ctx.K);
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
    static inline bool solveRootsNewtonPass(NewtonContext& ctx, int maxIter, double tol,
                                           bool usePrediction, bool useCurrentSeed = false,
                                           bool allowExtraBacktracking = true) noexcept {
        bool usedExtraBacktracking = false;
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

        const RootDevice* platePair = coupledPentodes(ctx);
        bool curValid = false;
        bool reached = false;
        const bool poweredPreview = ctx.pentodePreview && !ctx.staticMode;
        ctx.bestFn = std::numeric_limits<double>::infinity();
        double base = 0.0;
        for (int it = 0; it < maxIter; ++it) {
            ++ctx.newtonIters;
            if (!curValid) {
                tPhase = tick();
                currentsInto(ctx, ctx.vv, ctx.ii, ctx.J);
                residualInto(ctx, ctx.vv, ctx.vOc, ctx.Z, ctx.ii, ctx.F);
                base = maxAbs(ctx.F, ctx.K);
                if (ctx.prof)
                    tock(ctx.prof->device);
                curValid = true;
            }
            if (base < ctx.bestFn) {
                ctx.bestFn = base;
                ctx.bestV = ctx.vv;
                ctx.bestI = ctx.ii;
            }
            // An iterate inside the loose Preview tolerance still carries an error that jumps from sample to
            // sample. Land it with the Newton step its own residual and Jacobian define instead.
            const bool landing = poweredPreview && base < tol && base >= ctx.modeTol;
            if (base < tol && !landing) {
                reached = true;
                break;
            }

            tPhase = tick();
            const bool compact = ctx.sharedColumns && ctx.sharedColumns->available();
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
            if (compact)
                ctx.sharedColumns->assemble(ctx.J.data(), ctx.F.data(), ctx.K,
                                            ctx.redCol.data(), ctx.rootDevices);
            else
                assembleFull();
            if (ctx.prof)
                tock(ctx.prof->jacobian);

            tPhase = tick();
            bool solved = compact && ctx.sharedColumns->finish(ctx.F.data(), ctx.K, ctx.dv.data());
            if (!solved) {
                if (compact)
                    assembleFull();
                if (poweredPreview && ctx.nRed >= 8 && !ctx.Zred.empty()) {
                    solved = detail::solveDroppingZeroCorrections(ctx.Mr.data(), ctx.rN.data(), ctx.nRed);
                    if (solved)
                        RootJacobian::backsubFast(ctx.rN.data(), ctx.Zred.data(), ctx.F.data(),
                                                   ctx.nRed, ctx.K, ctx.dv.data());
                } else {
                    solved = !ctx.Zred.empty()
                        ? RootJacobian::reducedFinishFast(ctx.Mr.data(), ctx.rN.data(), ctx.nRed,
                              ctx.Zred.data(), ctx.F.data(), ctx.K, ctx.dv.data())
                        : RootJacobian::reducedFinish(ctx.Mr.data(), ctx.rN.data(), ctx.nRed,
                              ctx.Z.data(), ctx.F.data(), ctx.redCol.data(), ctx.K, ctx.dv.data());
                }
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
            if (landing) {
                for (const auto& dev : ctx.rootDevices)
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r)
                        for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q)
                            ctx.ii[static_cast<size_t>(r)] += ctx.J[static_cast<size_t>(r) * ctx.K + q] * ctx.dv[static_cast<size_t>(q)];
                for (int r = 0; r < ctx.K; ++r)
                    ctx.vv[static_cast<size_t>(r)] += ctx.dv[static_cast<size_t>(r)];
                reached = true;
                break;
            }

            double stepSize = 1.0;
            tPhase = tick();
            bool stepHasJ = false;
            double ftNorm = 0.0;
            // Near a hard op-amp rail the fourth trial can still worsen the
            // full residual. A few more backtracks avoid a discarded sample.
            const int maxTrials = allowExtraBacktracking && compact && !ctx.opAmps.empty() ? 10 : 4;
            int h = 0;
            for (; h < maxTrials; ++h) {
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
                if (platePair && (ftNorm >= base || h > 0))
                    ftNorm = projectPentodePlates(ctx, platePair, ctx.vt, ctx.iv, stepHasJ ? &ctx.Jt : nullptr, ctx.ft, ftNorm);
                if (ftNorm <= base || h + 1 == maxTrials)
                    break;
                stepSize = clampAwareStep(ctx, stepSize);
            }
            usedExtraBacktracking |= h >= 4;
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

            if (base < tol && !(poweredPreview && base >= ctx.modeTol)) {
                reached = true;
                break;
            }
            if (it > 0 && maxDelta < 1e-13 * std::max(1.0, maxV)) {
                // A stalled step leaves only rounding, which a high-gain law such as an
                // op-amp's open loop amplifies past tol; whether that rounding lands under
                // tol must not decide a reject, it differs between compilers.
                reached = curValid && base < roundingFloor(ctx);
                break;
            }
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
        return usedExtraBacktracking;
    }

    static bool pinDomains(NewtonContext& ctx, const BlockView& view) noexcept {
        const bool folded = ctx.triodeTransfer && ctx.triodeTransfer->fold(
            view, ctx.vOc.data(), ctx.ii.data(), ctx.dv.data());
        return RootClosedForms::pinTriodeDomains(
            view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
            const_cast<unsigned char*>(ctx.triodeDomainPin.data()), ctx.comp,
            ctx.rootDevices.data(), ctx.K, folded ? ctx.dv.data() : nullptr);
    }

    /// The callers pin from the previous sample's currents before solving, so
    /// re-read the pin from the solved ones -- the in-sample test the closed
    /// form makes -- and solve again while a triode changed domain, at most
    /// `view.count` more times.
    static inline void resolveTriodeDomains(NewtonContext& ctx, int maxIter, double tol,
                                            const BlockView& view) noexcept {
        for (int pass = 0; pass < view.count &&
                           pinDomains(ctx, view);
             ++pass)
            solveRootsNewtonPass(ctx, maxIter, tol, false, true);
    }

    /// Multiport Newton for the device currents.
    static inline void solveRootsNewton(NewtonContext& ctx, int maxIter, double tol,
                                        const BlockView& view, std::vector<double>& vvPrev,
                                        std::vector<double>& vvPrev2) noexcept {
        pinDomains(ctx, view);
        const bool usedExtra = solveRootsNewtonPass(ctx, maxIter, tol, true);
        // A failed deeply damped search can stall near a switching boundary.
        // With the usual four trials, the retry takes a different path even from the same voltage.
        if (!ctx.lastConverged && (usedExtra || !ctx.skipIdenticalRetry || predictionChangesSeed(ctx)))
            solveRootsNewtonPass(ctx, maxIter, tol, false, false, !usedExtra);
        resolveTriodeDomains(ctx, maxIter, tol, view);
        vvPrev2 = vvPrev;
        vvPrev = ctx.vv;
    }

    /// Multiport Newton seeded from the block solve's result.
    static inline void solveRootsNewtonSeeded(NewtonContext& ctx, int maxIter, double tol,
                                              const BlockView& view, std::vector<double>& vvPrev,
                                              std::vector<double>& vvPrev2) noexcept {
        pinDomains(ctx, view);
        const bool usedExtra = solveRootsNewtonPass(ctx, maxIter, tol, false, true);
        if (!ctx.lastConverged)
            solveRootsNewtonPass(ctx, maxIter, tol, false, false, !usedExtra);
        resolveTriodeDomains(ctx, maxIter, tol, view);
        vvPrev2 = vvPrev;
        vvPrev = ctx.vv;
    }

};

} // namespace wdf::solver
