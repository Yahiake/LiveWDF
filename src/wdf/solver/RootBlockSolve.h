#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <limits>
#include <cstddef>

#include "wdf/DenseTopology.h"
#include "wdf/DenseSolve.h"
#include "wdf/solver/RootResidual.h"
#include "wdf/solver/PushPullTries.h"
#include "wdf/solver/RootJacobian.h"
#include "wdf/solver/RootClosedForms.h"
#include "wdf/solver/RootNewton.h"
#include "wdf/solver/PushPullPairSolve.h"
#include "wdf/solver/QuadricKnotStep.h"

#if defined(_MSC_VER)
#define LIVEWDF_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define LIVEWDF_RESTRICT __restrict__
#else
#define LIVEWDF_RESTRICT
#endif

namespace wdf::solver {

struct BlockSolveContext {
    // Scratch Block Buffers
    std::vector<double>& blockBase;
    std::vector<double>& blockI;
    std::vector<double>& blockJ;
    std::vector<double>& blockF;
    std::vector<double>& blockV;
    std::vector<double>& blockVOld;
    std::vector<double>& blockDv;
    std::vector<double>& blockM;
    std::vector<double>& blockRhs;

    // Joint Engine Buffers
    std::vector<double>& vv;
    std::vector<double>& ii;
    std::vector<double>& iv;
    std::vector<double>& J;
    std::vector<double>& F;
    std::vector<double>& vOc;
    const std::vector<double>& Z;
    const std::vector<double>& Zred;
    std::vector<double>& vvPrev;
    std::vector<double>& vvPrev2;
    const std::vector<double>& stepUp;
    const std::vector<double>& stepDown;
    const std::vector<int>& redCol;

    int K = 0;
    int nRed = 0;

    // Devices & Models
    const std::vector<RootDevice>& rootDevices;
    const Compiled& comp;
    const std::vector<BjtModel>& bjts;
    const std::vector<double>& ctrlResValues;
    const std::vector<Pentode>& pentodes;
    const std::vector<OpAmpModel>& opAmps;
    const std::vector<double>& opAmpVp;
    const std::vector<double>& opAmpAlpha;
    const std::vector<double>& opAmpMaxDv;
    std::vector<unsigned char>& triodeDomainPin;
    const std::vector<DiodeModel>& grpDiode;
    const std::vector<JunctionLimiter>& junctionLimiters;
    const std::vector<GridJunctionLimiter>& gridJunctionLimiters;

    bool staticMode = false;
    PhaseProfile* prof = nullptr;
    bool profileOn = false;

    // Diagnostics & state counters
    long long& newtonIters;
    long long& deviceEvals;
    long long& limitHits;
    long long& gridLimitHits;
    long long& knotStepChoices;
    long long& knotTriggeredSolves;
    long long& knotConvergedSolves;
    long long& knotConvergedIterations;
    bool& lastBlockFallback;
    double& lastBlockResidual;
    long long& blockFallbacks;
    long long& resweepSweeps;
    long long& resweepClearedFirst;
    long long& resweepClearedSecond;
    long long& runtimeBlockFallbacks;
    bool& runtimeBlockUsed;
    bool& lastConverged;
    double& lastResidual;
    bool& keptBest;
    bool& jfetSolveFailed;
    bool& mosfetSolveFailed;
    long long& jfetFallbacks;
    long long& mosfetFallbacks;
    std::vector<double>& gridLag;
    bool mixedEnabled;
    long long& mixedAttempts;
    long long& mixedAccepted;
    double modeTol;
    bool previewMode;
    bool keepBlockAnswer = false;
    bool reductions = true;
};

class RootBlockSolve {
public:
    /// Use the current sample's exact scalar law before cached affine reduction.
    static bool hasDirectSingleton(const NewtonBlock& block, const std::vector<RootDevice>& devices,
                                   const Compiled& comp, const std::vector<OpAmpModel>& opAmps,
                                   const std::vector<double>& z, int stride) noexcept {
        if (block.devices.size() != 1)
            return false;
        const auto& device = devices[static_cast<size_t>(block.devices.front())];
        if (device.kind == DeviceKind::ControlledRes)
            return !comp.otaCurrentLaw[device.compIndex];
        if (device.kind != DeviceKind::OpAmp)
            return false;

        // Op-amp ports are out, pos, neg. Negative or absent feedback
        // makes the hard-rail output equation monotone.
        const auto& model = opAmps[device.compIndex];
        const size_t out = static_cast<size_t>(device.portBase);
        return !model.softRails && !model.hasClassABOutput() &&
               z[(out + 1) * stride + out] <= z[(out + 2) * stride + out];
    }

    /// F = v - base + Z*i for this block's folded linear network.
    template<bool Reduced = false>
    static inline double blockResidual(const NewtonBlock& block, BlockSolveContext& ctx,
                                       int n) noexcept {
        const auto& devices = Reduced ? block.reducedDevices : block.devices;
        const auto& ports = Reduced ? block.reducedPorts : block.ports;
        const auto& networkZ = Reduced ? block.reducedZ : block.blockZ;
        alignas(32) double currents[32];
        const int packed = std::min(n, 32);
        for (int j = 0; j < packed; ++j)
            currents[j] = ctx.blockI[static_cast<size_t>(ports[static_cast<size_t>(j)])];
        const double* LIVEWDF_RESTRICT bz = networkZ.data();
        int local = 0;
        double maximum = 0.0;
        for (int di : devices) {
            const auto& dev = ctx.rootDevices[di];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++local) {
                double f = ctx.blockV[static_cast<size_t>(r)] -
                           ctx.blockBase[static_cast<size_t>(r)];
                if (!networkZ.empty()) {
                    const double* LIVEWDF_RESTRICT z = bz + static_cast<size_t>(local) * n;
                    if (n == 1) {
                        f += z[0] * currents[0];
                    } else if (n == 2) {
                        f += z[0] * currents[0] + z[1] * currents[1];
                    } else {
                        for (int j = 0; j < packed; ++j)
                            f += z[j] * currents[j];
                        for (int j = packed; j < n; ++j)
                            f += z[j] * ctx.blockI[static_cast<size_t>(ports[static_cast<size_t>(j)])];
                    }
                } else {
                    const double* LIVEWDF_RESTRICT z = ctx.Z.data() + static_cast<size_t>(r) * ctx.K;
                    for (int c : ports)
                        f += z[c] * ctx.blockI[static_cast<size_t>(c)];
                }
                ctx.blockF[static_cast<size_t>(r)] = f;
                maximum = std::max(maximum, std::abs(f));
            }
        }
        return maximum;
    }

    /// Solve one block by Newton on its own live columns.
    template<bool Reduced = false>
    static inline bool solveBlockNewton(
        const BlockView& view,
        int b,
        int s,
        int maxIter,
        double tol,
        bool relaxed,
        BlockSolveContext& ctx,
        const NewtonBlock* reducedPlan = nullptr,
        bool* acceptedDeviceCurrents = nullptr) noexcept
    {
        const NewtonBlock& block = Reduced && reducedPlan ? *reducedPlan : (*view.blocks)[static_cast<size_t>(b)];
        const auto& devices = Reduced ? block.reducedDevices : block.devices;
        const auto& ports = Reduced ? block.reducedPorts : block.ports;
        const auto& networkZ = Reduced ? block.reducedZ : block.blockZ;
        const int n = static_cast<int>(ports.size());
        if (n <= 0 || n > ctx.nRed)
            return false;

        int pentodeCount = 0;
        for (int di : devices)
            pentodeCount += ctx.rootDevices[di].kind == DeviceKind::Pentode;
        const bool pushPullPreview = ctx.previewMode && pentodeCount == 2;

        using clk = std::chrono::steady_clock;
        const auto tick = [&] { return ctx.profileOn ? clk::now() : clk::time_point{}; };
        clk::time_point tPhase = tick();
        const auto tock = [&](double& acc) {
            if (ctx.profileOn) {
                const auto now = clk::now();
                acc += std::chrono::duration<double>(now - tPhase).count();
                tPhase = now;
            }
        };

        if constexpr (!Reduced) {
            int localDevPort = 0;
            for (int di : devices) {
                const RootDevice& dev = ctx.rootDevices[di];
                for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++localDevPort) {
                    ctx.blockBase[static_cast<size_t>(r)] =
                        relaxed ? RootClosedForms::foldPortAll(view, block, localDevPort, r, b, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(), ctx.K)
                                : RootClosedForms::foldPort(view, block, localDevPort, r, s, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(), ctx.K);
                }
            }
            static const bool slewPushPull = livewdfPpTry("slew");
            if (slewPushPull && pushPullPreview && !relaxed) {
                constexpr double maxGridDriveStep = 40.0;
                int localRow = 0;
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++localRow) {
                        if (dev.kind != DeviceKind::Pentode || r != dev.portBase + 1)
                            continue;
                        double previousDrive = ctx.vvPrev[static_cast<size_t>(r)];
                        const double* z = networkZ.data() + static_cast<size_t>(localRow) * n;
                        for (int j = 0; j < n; ++j)
                            previousDrive += z[j] * ctx.ii[static_cast<size_t>(ports[static_cast<size_t>(j)])];
                        const double step = ctx.blockBase[static_cast<size_t>(r)] - previousDrive;
                        ctx.blockBase[static_cast<size_t>(r)] = previousDrive +
                            std::clamp(step, -maxGridDriveStep, maxGridDriveStep);
                    }
                }
            }
        }

        for (int di : devices) {
            const RootDevice& dev = ctx.rootDevices[di];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                const double v = ctx.vv[static_cast<size_t>(r)];
                ctx.blockV[static_cast<size_t>(r)] = std::isfinite(v) ? v : 0.0;
            }
        }

        if (ctx.prof)
            tock(ctx.prof->predict);

        const double effTol = std::max(tol, Reduced ? 1e-12 : 1e-9);
        // Under the loose Preview tolerance only a Newton step lands the block, like the joint solve.
        const double landTol = Reduced ? effTol : std::max(std::min(tol, ctx.modeTol), 1e-9);
        bool landing = false;
        // An op-amp carries a one-sample internal state (`opAmpVp`), so the
        // currents committed for the previous sample are not necessarily the
        // currents the device law produces for this sample.  The initial
        // residual shortcut is only valid for memoryless blocks; with an
        // op-amp, evaluate the law first or the block will declare the last
        // sample's state converged and never advance a slewing or railed
        // stage.
        bool hasOpAmp = Reduced;
        for (int di : devices) {
            if (ctx.rootDevices[di].kind == DeviceKind::OpAmp) {
                hasOpAmp = true;
            }
        }
        if (hasOpAmp && n >= 4 && !relaxed) {
            // In coupled stages the preceding two samples give a useful
            // first iterate. Keep the guess local to the last accepted
            // voltage. All subsequent step limits and residual gates apply.
            constexpr double maxPrediction = 0.05;
            for (int di : devices) {
                const auto& dev = ctx.rootDevices[di];
                for (int i = dev.portBase; i < dev.portBase + dev.numPorts; ++i) {
                    const size_t r = static_cast<size_t>(i);
                    const double delta = ctx.vvPrev[r] - ctx.vvPrev2[r];
                    if (std::isfinite(delta))
                        ctx.blockV[r] += std::clamp(delta, -maxPrediction, maxPrediction);
                }
            }
        }
        if (!hasOpAmp) {
            for (int c : ports) {
                const double prev = ctx.ii[static_cast<size_t>(c)];
                ctx.blockI[static_cast<size_t>(c)] = std::isfinite(prev) ? prev : 0.0;
            }
            if (blockResidual<Reduced>(block, ctx, n) < landTol) {
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                        ctx.ii[static_cast<size_t>(r)] = ctx.blockI[static_cast<size_t>(r)];
                        ctx.vv[static_cast<size_t>(r)] = ctx.blockV[static_cast<size_t>(r)];
                    }
                }
                return true;
            }
        }

        bool converged = false;
        bool knotTriggered = false;
        bool curValid = false;
        int fullDescent = 0;
        double maxF = 0.0;
        double prevF = 0.0;
        bool luValid = false;
        int luReuseCount = 0;
        alignas(16) int piv[64];
        const double* LIVEWDF_RESTRICT bz = networkZ.data();
        static const bool cap48 = livewdfPpTry("cap48");
        const int localMax = Reduced ? std::min(maxIter, 10) :
            pushPullPreview && cap48 ? 48 : std::min(maxIter, pushPullPreview ? 32 : 24);
        alignas(32) double bestV[64], bestI[64];
        double bestF = std::numeric_limits<double>::infinity();
        const bool knotsEnabled = quadricKnotStepEnabled();
        const long long solveStartIters = knotsEnabled ? ctx.newtonIters : 0;
        const auto keepBest = [&] {
            if (!ctx.keepBlockAnswer || !(maxF < bestF) || n > 64)
                return;
            bestF = maxF;
            for (int j = 0; j < n; ++j) {
                bestV[j] = ctx.blockV[static_cast<size_t>(ports[static_cast<size_t>(j)])];
                bestI[j] = ctx.blockI[static_cast<size_t>(ports[static_cast<size_t>(j)])];
            }
        };
        for (int it = 0; it < localMax; ++it) {
            if constexpr (!Reduced) {
                if (it >= (cap48 && pushPullPreview ? 40 : 24) && fullDescent < 2)
                    break;
            }
            ++ctx.newtonIters;
            if (!curValid) {
                for (int di : devices) {
                    RootResidual::currentsForDevice(
                        ctx.rootDevices[di], ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(),
                        ctx.bjts.data(), ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(),
                        ctx.opAmpMaxDv.data(), ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(),
                        ctx.K, ctx.blockV.data(), ctx.blockI.data(), ctx.blockJ.data(), ctx.deviceEvals);
                }
                if (ctx.prof)
                    tock(ctx.prof->device);

                maxF = blockResidual<Reduced>(block, ctx, n);
                keepBest();
                if (ctx.prof)
                    tock(ctx.prof->device);
                if (maxF < landTol) {
                    converged = true;
                    break;
                }
                landing = maxF < effTol;
            }
            curValid = false;

            const double kappa = (prevF > 1e-15) ? (maxF / prevF) : 1.0;
            const bool canReuse = (n >= 3) && (n <= 64) && luValid && (landing || ((luReuseCount < 2) && (kappa < 0.4) && (maxF < 0.05)));

            bool solveOk = false;
            if (canReuse) {
                std::fill_n(ctx.blockRhs.data(), static_cast<size_t>(n), 0.0);
                int row = 0;
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    const int o = dev.portBase;
                    const int nAct = activePortCount(dev);
                    for (int rr = 0; rr < nAct; ++rr) {
                        const int rp = activePort(dev, rr);
                        const double* LIVEWDF_RESTRICT jRow = ctx.blockJ.data() + static_cast<size_t>(rp) * ctx.K;
                        double rhs = 0.0;
                        for (int q = o; q < o + dev.numPorts; ++q) {
                            const double jq = jRow[q];
                            if (jq != 0.0)
                                rhs -= jq * ctx.blockF[static_cast<size_t>(q)];
                        }
                        ctx.blockRhs[static_cast<size_t>(row)] = rhs;
                        ++row;
                    }
                }
                if (ctx.prof)
                    tock(ctx.prof->jacobian);

                detail::solveLUData(ctx.blockM.data(), piv, ctx.blockRhs.data(), n);
                solveOk = true;
                if (ctx.prof)
                    tock(ctx.prof->lu);
                ++luReuseCount;
            } else {
                if (it > 0 && n >= 3) {
                    for (int di : devices) {
                        RootResidual::currentsForDevice(
                            ctx.rootDevices[di], ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(),
                            ctx.bjts.data(), ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(),
                            ctx.opAmpMaxDv.data(), ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(),
                            ctx.K, ctx.blockV.data(), ctx.blockI.data(), ctx.blockJ.data(), ctx.deviceEvals);
                    }
                    if (ctx.prof)
                        tock(ctx.prof->device);
                }

                std::fill_n(ctx.blockM.data(), static_cast<size_t>(n) * n, 0.0);
                std::fill_n(ctx.blockRhs.data(), static_cast<size_t>(n), 0.0);

                int row = 0;
                int devPortBase = 0;
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    const int o = dev.portBase;
                    const int nAct = activePortCount(dev);
                    for (int rr = 0; rr < nAct; ++rr) {
                        const int rp = activePort(dev, rr);
                        const double* LIVEWDF_RESTRICT jRow = ctx.blockJ.data() + static_cast<size_t>(rp) * ctx.K;
                        double rhs = 0.0;
                        double* LIVEWDF_RESTRICT mRow = ctx.blockM.data() + static_cast<size_t>(row) * n;
                        for (int q = o; q < o + dev.numPorts; ++q) {
                            const double jq = jRow[q];
                            if (jq == 0.0)
                                continue;
                            const int localQ = devPortBase + (q - o);
                            if (!networkZ.empty()) {
                                const double* LIVEWDF_RESTRICT zq = bz + static_cast<size_t>(localQ) * n;
                                if (n == 1) {
                                    mRow[0] += jq * zq[0];
                                } else if (n == 2) {
                                    mRow[0] += jq * zq[0];
                                    mRow[1] += jq * zq[1];
                                } else {
                                    for (int j = 0; j < n; ++j)
                                        mRow[j] += jq * zq[j];
                                }
                            } else {
                                const double* zq = ctx.Z.data() + static_cast<size_t>(q) * ctx.K;
                                for (int j = 0; j < n; ++j)
                                    mRow[j] += jq * zq[ports[static_cast<size_t>(j)]];
                            }
                            rhs -= jq * ctx.blockF[static_cast<size_t>(q)];
                        }
                        mRow[row] += 1.0;
                        ctx.blockRhs[static_cast<size_t>(row)] = rhs;
                        ++row;
                    }
                    devPortBase += dev.numPorts;
                }
                if (row != n)
                    return false;
                if (ctx.prof)
                    tock(ctx.prof->jacobian);

                if (n < 3 || n > 64) {
                    solveOk = detail::solveData(ctx.blockM.data(), ctx.blockRhs.data(), n);
                } else {
                    // The assembled matrix is scratch. Retain its factors
                    // here for the subsequent modified-Newton steps.
                    solveOk = detail::factorLUData(ctx.blockM.data(), piv, n);
                    if (solveOk) {
                        luValid = true;
                        luReuseCount = 0;
                        detail::solveLUData(ctx.blockM.data(), piv, ctx.blockRhs.data(), n);
                    }
                }
                if (ctx.prof)
                    tock(ctx.prof->lu);
            }
            if (!solveOk)
                return false;

            int localR = 0;
            const double* LIVEWDF_RESTRICT rhsData = ctx.blockRhs.data();
            for (int di : devices) {
                const RootDevice& dev = ctx.rootDevices[di];
                for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++localR) {
                    double d = -ctx.blockF[static_cast<size_t>(r)];
                    if (!networkZ.empty()) {
                        const double* LIVEWDF_RESTRICT zr = bz + static_cast<size_t>(localR) * n;
                        if (n == 1) {
                            d -= zr[0] * rhsData[0];
                        } else if (n == 2) {
                            d -= (zr[0] * rhsData[0] + zr[1] * rhsData[1]);
                        } else {
                            for (int j = 0; j < n; ++j)
                                d -= zr[j] * rhsData[j];
                        }
                    } else {
                        const double* zr = ctx.Z.data() + static_cast<size_t>(r) * ctx.K;
                        for (int j = 0; j < n; ++j)
                            d -= zr[ports[static_cast<size_t>(j)]] * rhsData[j];
                    }
                    ctx.blockDv[static_cast<size_t>(r)] = d;
                }
            }

            if constexpr (!Reduced) {
                static const bool predict = livewdfPpTry("predict");
                int pair[2]{-1, -1}, pairCount = 0, rising = -1;
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    if (dev.kind != DeviceKind::Pentode)
                        continue;
                    if (pairCount < 2)
                        pair[pairCount++] = di;
                    if (ctx.blockDv[static_cast<size_t>(dev.portBase + 1)] > 80.0)
                        rising = di;
                }
                if (predict && it == 0 && pushPullPreview && pairCount == 2 && rising >= 0) {
                    for (int di : devices) {
                        const RootDevice& dev = ctx.rootDevices[di];
                        for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r)
                            ctx.blockV[static_cast<size_t>(r)] += ctx.blockDv[static_cast<size_t>(r)];
                    }
                    const RootDevice& a = ctx.rootDevices[pair[0]];
                    const RootDevice& b = ctx.rootDevices[pair[1]];
                    ctx.blockV[static_cast<size_t>(a.portBase)] = ctx.vvPrev[static_cast<size_t>(b.portBase)];
                    ctx.blockV[static_cast<size_t>(a.portBase + 2)] = ctx.vvPrev[static_cast<size_t>(b.portBase + 2)];
                    ctx.blockV[static_cast<size_t>(b.portBase)] = ctx.vvPrev[static_cast<size_t>(a.portBase)];
                    ctx.blockV[static_cast<size_t>(b.portBase + 2)] = ctx.vvPrev[static_cast<size_t>(a.portBase + 2)];
                    ctx.blockV[static_cast<size_t>(ctx.rootDevices[rising].portBase + 1)] = 0.25;
                    curValid = false;
                    luValid = false;
                    fullDescent = 0;
                    continue;
                }
            }

            for (int di : devices) {
                const RootDevice& dev = ctx.rootDevices[di];
                if (dev.kind == DeviceKind::Bjt) {
                    const int o = dev.portBase;
                    const auto& m = ctx.bjts[dev.compIndex];
                    double nextCe = 0.0, nextBe = 0.0;
                    m.applyMNRStep(ctx.blockV[static_cast<size_t>(o)],
                                   ctx.blockV[static_cast<size_t>(o + 1)],
                                   ctx.blockDv[static_cast<size_t>(o)],
                                   ctx.blockDv[static_cast<size_t>(o + 1)],
                                   nextCe, nextBe);
                    ctx.blockDv[static_cast<size_t>(o)] = nextCe - ctx.blockV[static_cast<size_t>(o)];
                    ctx.blockDv[static_cast<size_t>(o + 1)] = nextBe - ctx.blockV[static_cast<size_t>(o + 1)];
                }
            }

            const QuadricKnotSteps knotSteps = knotsEnabled && ctx.previewMode
                ? quadricKnotSteps(devices, ctx.rootDevices, ctx.comp, ctx.pentodes,
                                   ctx.triodeDomainPin, ctx.blockV.data(), ctx.blockDv.data())
                : QuadricKnotSteps{};

            double scale = 1.0;
            bool onlyTubePlatesLimited = pushPullPreview;
            int limitedPorts = 0;
            for (int di : devices) {
                const RootDevice& dev = ctx.rootDevices[di];
                for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                    const double d = ctx.blockDv[static_cast<size_t>(r)];
                    const double lim = (d >= 0.0) ? ctx.stepUp[static_cast<size_t>(r)]
                                                  : ctx.stepDown[static_cast<size_t>(r)];
                    const double mag = std::abs(d);
                    if (mag > lim) {
                        ++limitedPorts;
                        onlyTubePlatesLimited &= r == dev.portBase &&
                            (dev.kind == DeviceKind::Triode || dev.kind == DeviceKind::Pentode);
                    }
                    if (mag > 0.0 && mag * scale > lim)
                        scale = lim / mag;
                }
            }
            if constexpr (!Reduced) {
                static const bool sumdiff = livewdfPpTry("sumdiff");
                if (sumdiff && it == 0 && limitedPorts > 0 && pushPullPreview &&
                    solvePushPullDifference(block, ctx, networkZ, n)) {
                    ++ctx.limitHits;
                    luValid = false;
                    fullDescent = 0;
                    continue;
                }
            }
            if (!knotSteps.active && limitedPorts > 0 && onlyTubePlatesLimited) {
                ++ctx.limitHits;
                // Bound each plate's excursion through cutoff/reverse regions
                // without throttling the grids that move a push-pull stage across.
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    if (dev.kind != DeviceKind::Triode && dev.kind != DeviceKind::Pentode)
                        continue;
                    const int r = dev.portBase;
                    const double d = ctx.blockDv[static_cast<size_t>(r)];
                    const double lim = d >= 0.0 ? ctx.stepUp[static_cast<size_t>(r)]
                                                : ctx.stepDown[static_cast<size_t>(r)];
                    ctx.blockDv[static_cast<size_t>(r)] = std::clamp(d, -lim, lim);
                }
                scale = 1.0;
            } else if (!knotSteps.active && scale < 1.0) {
                ++ctx.limitHits;
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r)
                        ctx.blockDv[static_cast<size_t>(r)] *= scale;
                }
            }
            if (knotSteps.active)
                scale = 1.0;
            if (landing) {
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r)
                        for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q)
                            ctx.blockI[static_cast<size_t>(r)] += ctx.blockJ[static_cast<size_t>(r) * ctx.K + q] * ctx.blockDv[static_cast<size_t>(q)];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r)
                        ctx.blockV[static_cast<size_t>(r)] += ctx.blockDv[static_cast<size_t>(r)];
                }
                converged = true;
                break;
            }

            if (n < 3) {
                fullDescent = 0;
                double maxStep = 0.0, maxV = 0.0;
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                        const double old = ctx.blockV[static_cast<size_t>(r)];
                        const double d = std::abs(ctx.blockDv[static_cast<size_t>(r)]);
                        const double v = std::abs(old);
                        maxStep = std::max(maxStep, d);
                        maxV = std::max(maxV, v);
                        ctx.blockVOld[static_cast<size_t>(r)] = old;
                        ctx.blockV[static_cast<size_t>(r)] = old + ctx.blockDv[static_cast<size_t>(r)];
                    }
                }
                for (const auto& gl : ctx.gridJunctionLimiters) {
                    bool inBlock = false;
                    for (int di : devices) {
                        const auto& dev = ctx.rootDevices[di];
                        if (gl.port >= dev.portBase && gl.port < dev.portBase + dev.numPorts) {
                            inBlock = true;
                            break;
                        }
                    }
                    if (inBlock) {
                        bool limited = false;
                        ctx.blockV[gl.port] = gl.limitGrid(ctx.blockVOld[gl.port], ctx.blockV[gl.port], limited);
                        if (limited) {
                            ++ctx.limitHits;
                            ++ctx.gridLimitHits;
                        }
                    }
                }
                for (const auto& jl : ctx.junctionLimiters) {
                    bool inBlock = false;
                    for (int di : devices) {
                        const auto& dev = ctx.rootDevices[di];
                        if (jl.port >= dev.portBase && jl.port < dev.portBase + dev.numPorts) {
                            inBlock = true;
                            break;
                        }
                    }
                    if (inBlock) {
                        bool limited = false;
                        if (jl.negate) {
                            const double oldMirrored = -ctx.blockVOld[jl.port];
                            const double newMirrored = -ctx.blockV[jl.port];
                            const double clamped = jl.model->limit(oldMirrored, newMirrored, limited);
                            ctx.blockV[jl.port] = -clamped;
                        } else {
                            ctx.blockV[jl.port] = jl.model->limit(ctx.blockVOld[jl.port], ctx.blockV[jl.port], limited);
                        }
                        if (limited)
                            ++ctx.limitHits;
                    }
                }
                if (it > 0 && maxStep < 1e-11 * std::max(1.0, maxV)) {
                    // A collapsed step with a residual above the gate is not
                    // convergence.  Let the caller try the exact full block /
                    // joint solve instead of accepting a point the verifier
                    // will reject.
                    break;
                }
                curValid = false;
            } else {
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                        ctx.blockVOld[static_cast<size_t>(r)] = ctx.blockV[static_cast<size_t>(r)];
                    }
                }

                const double baseF = maxF;
                double stepSize = 1.0;
                double ftNorm = 0.0;
                const int maxLineTrials = knotSteps.active ? knotSteps.knots + 1 : 4;
                double bestTrialF = std::numeric_limits<double>::infinity();
                double bestTrialStep = 1.0;
                int bestTrialIndex = -1;
                alignas(32) double trialV[64], trialI[64];
                for (int h = 0; h < maxLineTrials; ++h) {
                    if (knotSteps.active)
                        stepSize = knotSteps.values[static_cast<size_t>(h)];
                    for (int di : devices) {
                        const RootDevice& dev = ctx.rootDevices[di];
                        for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                            ctx.blockV[static_cast<size_t>(r)] =
                                ctx.blockVOld[static_cast<size_t>(r)] + stepSize * ctx.blockDv[static_cast<size_t>(r)];
                        }
                    }

                    // Limit junctions belonging to this block
                    for (const auto& gl : ctx.gridJunctionLimiters) {
                        bool inBlock = false;
                        for (int di : devices) {
                            const auto& dev = ctx.rootDevices[di];
                            if (gl.port >= dev.portBase && gl.port < dev.portBase + dev.numPorts) {
                                inBlock = true;
                                break;
                            }
                        }
                        if (inBlock) {
                            bool limited = false;
                            ctx.blockV[gl.port] = gl.limitGrid(ctx.blockVOld[gl.port], ctx.blockV[gl.port], limited);
                            if (limited) {
                                ++ctx.limitHits;
                                ++ctx.gridLimitHits;
                            }
                        }
                    }
                    for (const auto& jl : ctx.junctionLimiters) {
                        bool inBlock = false;
                        for (int di : devices) {
                            const auto& dev = ctx.rootDevices[di];
                            if (jl.port >= dev.portBase && jl.port < dev.portBase + dev.numPorts) {
                                inBlock = true;
                                break;
                            }
                        }
                        if (inBlock) {
                            bool limited = false;
                            if (jl.negate) {
                                const double oldMirrored = -ctx.blockVOld[jl.port];
                                const double newMirrored = -ctx.blockV[jl.port];
                                const double clamped = jl.model->limit(oldMirrored, newMirrored, limited);
                                ctx.blockV[jl.port] = -clamped;
                            } else {
                                ctx.blockV[jl.port] = jl.model->limit(ctx.blockVOld[jl.port], ctx.blockV[jl.port], limited);
                            }
                            if (limited)
                                ++ctx.limitHits;
                        }
                    }

                    for (int di : devices) {
                        RootResidual::currentsForDevice(
                            ctx.rootDevices[di], ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(),
                            ctx.bjts.data(), ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(),
                            ctx.opAmpMaxDv.data(), ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(),
                            ctx.K, ctx.blockV.data(), ctx.blockI.data(), nullptr, ctx.deviceEvals);
                    }

                    ftNorm = blockResidual<Reduced>(block, ctx, n);

                    if (knotSteps.active) {
                        if (ftNorm < bestTrialF) {
                            bestTrialF = ftNorm;
                            bestTrialStep = stepSize;
                            bestTrialIndex = h;
                            for (int j = 0; j < n; ++j) {
                                const size_t p = static_cast<size_t>(ports[static_cast<size_t>(j)]);
                                trialV[j] = ctx.blockV[p];
                                trialI[j] = ctx.blockI[p];
                            }
                        }
                        if (h == maxLineTrials - 1) {
                            ftNorm = bestTrialF;
                            stepSize = bestTrialStep;
                            for (int j = 0; j < n; ++j) {
                                const size_t p = static_cast<size_t>(ports[static_cast<size_t>(j)]);
                                ctx.blockV[p] = trialV[j];
                                ctx.blockI[p] = trialI[j];
                            }
                            if (bestTrialIndex >= 0 && bestTrialIndex < knotSteps.knots) {
                                ++ctx.knotStepChoices;
                                if (!knotTriggered) {
                                    knotTriggered = true;
                                    ++ctx.knotTriggeredSolves;
                                }
                            }
                        }
                    } else {
                        if (ftNorm <= baseF || h == maxLineTrials - 1)
                            break;
                        stepSize *= 0.5;
                    }
                }
                if (ctx.prof)
                    tock(ctx.prof->linesearch);

                fullDescent = stepSize == 1.0 && scale == 1.0 && ftNorm < baseF
                    ? fullDescent + 1 : 0;

                double maxStep = 0.0, maxV = 0.0;
                for (int di : devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                        const double d = std::abs(ctx.blockV[static_cast<size_t>(r)] - ctx.blockVOld[static_cast<size_t>(r)]);
                        const double v = std::abs(ctx.blockV[static_cast<size_t>(r)]);
                        maxStep = std::max(maxStep, d);
                        maxV = std::max(maxV, v);
                    }
                }

                if (ftNorm > baseF) {
                    luValid = false;
                }
                prevF = baseF;
                maxF = ftNorm;
                keepBest();
                if (maxF < landTol) {
                    converged = true;
                    break;
                }
                landing = maxF < effTol;
                if (it > 0 && maxStep < 1e-11 * std::max(1.0, maxV)) {
                    // Same rule as the small-block path: a stalled step above
                    // the gate is a failed solve, not a converged one.
                    break;
                }

                curValid = true;
            }
        }
        if constexpr (Reduced) {
            if ((converged || landing) && knotTriggered) {
                ++ctx.knotConvergedSolves;
                ctx.knotConvergedIterations += ctx.newtonIters - solveStartIters;
            }
            return converged || landing; // Caller reconstructs and certifies the original block.
        } else if (!converged && !landing) {
            // A kept block answer takes a stalled block's best iterate; the full-residual guard still sends a gross miss to the joint solve.
            if (!ctx.keepBlockAnswer || !std::isfinite(bestF))
                return false;
            for (int j = 0; j < n; ++j) {
                ctx.blockV[static_cast<size_t>(ports[static_cast<size_t>(j)])] = bestV[j];
                ctx.blockI[static_cast<size_t>(ports[static_cast<size_t>(j)])] = bestI[j];
            }
            ctx.keptBest = true;
        }


        for (int di : devices) {
            const RootDevice& dev = ctx.rootDevices[di];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                ctx.ii[static_cast<size_t>(r)] = ctx.blockI[static_cast<size_t>(r)];
                ctx.vv[static_cast<size_t>(r)] = ctx.blockV[static_cast<size_t>(r)];
            }
        }

        if (acceptedDeviceCurrents)
            *acceptedDeviceCurrents = true;
        if ((converged || landing) && knotTriggered) {
            ++ctx.knotConvergedSolves;
            ctx.knotConvergedIterations += ctx.newtonIters - solveStartIters;
        }
        return true;
    }

    /// Whether every affine device in the block is still on the piece the
    /// elimination plan was built for.  Op-amps are only reduced in their
    /// linear region; a rail, a slew limit, or soft clipping sends the block
    /// back to the full block Newton, which has the exact device law.
    static inline bool canUseLinearReduction(
        const NewtonBlock& block,
        const BlockSolveContext& ctx) noexcept
    {
        for (int d = 0; d < static_cast<int>(block.linearDevices.size()); ++d) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(block.linearDevices[static_cast<size_t>(d)])];
            if (dev.kind != DeviceKind::OpAmp)
                continue;
            if (dev.compIndex >= ctx.opAmps.size() ||
                dev.compIndex >= ctx.opAmpVp.size() ||
                dev.compIndex >= ctx.opAmpAlpha.size() ||
                dev.compIndex >= ctx.opAmpMaxDv.size())
                return false;
            const OpAmpModel& oa = ctx.opAmps[dev.compIndex];
            double vDiff = ctx.vv[static_cast<size_t>(dev.portBase + 1)] -
                           ctx.vv[static_cast<size_t>(dev.portBase + 2)];
            if (!std::isfinite(vDiff))
                vDiff = 0.0;
            double derivative = 0.0;
            const double vSource = OpAmpEngineLaw::sourceVoltage(
                oa, vDiff, ctx.opAmpVp[dev.compIndex], ctx.opAmpAlpha[dev.compIndex],
                ctx.opAmpMaxDv[dev.compIndex], ctx.staticMode, &derivative);
            if (!std::isfinite(vSource) || !std::isfinite(derivative))
                return false;
            const double g = block.linearG[static_cast<size_t>(d)];
            const double scale = std::max(1.0, std::abs(g));
            if (std::abs(derivative - g) > 1e-9 * scale)
                return false;
        }
        return true;
    }

    /// Form `K0` for this solve and the reduced open-circuit voltages.
    ///
    /// `A K0 = b` with `b_d = h_d + g_d . vOc_d`; the reduced row voltage is
    /// `vOc'_q = vOc_q - Z_qL K0`.  The network part (`A`, `K`, `Z'`) was
    /// prepared with the block, so this is one small solve per sample.
    static inline bool prepareReducedBases(
        const BlockView& view,
        int b,
        int s,
        bool relaxed,
        BlockSolveContext& ctx,
        double* LIVEWDF_RESTRICT k0,
        const NewtonBlock* reducedPlan = nullptr) noexcept
    {
        const NewtonBlock& block = reducedPlan ? *reducedPlan : (*view.blocks)[static_cast<size_t>(b)];
        const int mL = static_cast<int>(block.linearDevices.size());
        if (mL <= 0 || mL > 32)
            return false;

        const auto fold = [&](int r) {
            return relaxed ? RootClosedForms::foldPortAll(
                                 view, r, b, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(), ctx.K)
                           : RootClosedForms::foldPort(
                                 view, r, s, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(), ctx.K);
        };

        alignas(32) double rhs[32];

        for (int d = 0; d < mL; ++d) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(block.linearDevices[static_cast<size_t>(d)])];
            const int o = dev.portBase;
            if (dev.kind == DeviceKind::OpAmp) {
                if (dev.compIndex >= ctx.opAmps.size() ||
                    dev.compIndex >= ctx.opAmpVp.size() ||
                    dev.compIndex >= ctx.opAmpAlpha.size() ||
                    dev.compIndex >= ctx.opAmpMaxDv.size())
                    return false;
                const OpAmpModel& oa = ctx.opAmps[dev.compIndex];
                const double g = block.linearG[static_cast<size_t>(d)];
                const double ro = oa.outputImpedance;
                const double vo = fold(o);
                const double vp = fold(o + 1);
                const double vn = fold(o + 2);
                double vDiff = ctx.vv[static_cast<size_t>(o + 1)] -
                               ctx.vv[static_cast<size_t>(o + 2)];
                if (!std::isfinite(vDiff))
                    vDiff = 0.0;
                double derivative = 0.0;
                const double vSource = OpAmpEngineLaw::sourceVoltage(
                    oa, vDiff, ctx.opAmpVp[dev.compIndex], ctx.opAmpAlpha[dev.compIndex],
                    ctx.opAmpMaxDv[dev.compIndex], ctx.staticMode, &derivative);
                if (!std::isfinite(vSource) || !std::isfinite(derivative))
                    return false;
                // The reduction is only valid while the op-amp stays on the
                // same affine segment whose slope was baked into linearG.
                const double gScale = std::max(1.0, std::abs(g));
                if (std::abs(derivative - g) > 1e-9 * gScale)
                    return false;
                // i_o = (v_o - vSource)/Ro, with the source linearised as
                // vSource0 + g*(vDiff - vDiff0).  The affine constant is
                // -(vSource0 - g*vDiff0)/Ro.
                if (reducedPlan) {
                    const double intercept = g == 0.0 ? vSource :
                        ctx.opAmpVp[dev.compIndex] / (1.0 + std::clamp(ctx.opAmpAlpha[dev.compIndex], 0.0, 1.0));
                    rhs[d] = (std::fma(-g, vp - vn, oa.outputVoltageWeight() * vo) - intercept) / ro;
                } else {
                    const double h = -(vSource - g * vDiff) / ro;
                    rhs[d] = h + (oa.outputVoltageWeight() / ro) * vo - (g / ro) * vp + (g / ro) * vn;
                }
            } else if (dev.kind == DeviceKind::ControlledRes) {
                const double g = block.linearG[static_cast<size_t>(d)];
                rhs[d] = g * fold(o);
            } else {
                return false;
            }
        }

        if (reducedPlan) {
            detail::solveLUData(block.linearLU.data(), block.linearPivots.data(), rhs, mL);
        } else {
            alignas(32) double a[32 * 32];
            std::copy(block.linearA.begin(), block.linearA.end(), a);
            if (!detail::solveData(a, rhs, mL))
                return false;
        }
        for (int d = 0; d < mL; ++d)
            k0[d] = rhs[d];

        for (int r : block.reducedRowPorts) {
            double v = fold(r);
            const double* zr = ctx.Z.data() + static_cast<size_t>(r) * ctx.K;
            for (int d = 0; d < mL; ++d) {
                const double z = zr[block.linearPorts[static_cast<size_t>(d)]];
                v = reducedPlan ? std::fma(-z, k0[d], v) : v - z * k0[d];
            }
            ctx.blockBase[static_cast<size_t>(r)] = v;
        }
        return true;
    }

    /// Reconstruct the eliminated currents from `i_L = K0 + K i_N`, write the
    /// block's currents and voltages, then check that nothing went non-finite.
    /// The old values are kept in the block scratch so a failure leaves the
    /// engine exactly as it was for the full block Newton fallback.
    static inline bool reconstructReducedBlock(
        const BlockView& view,
        int b,
        int s,
        bool relaxed,
        double tol,
        BlockSolveContext& ctx,
        const double* LIVEWDF_RESTRICT k0,
        bool verifyMixed = false,
        const NewtonBlock* reducedPlan = nullptr) noexcept
    {
        const NewtonBlock& block = reducedPlan ? *reducedPlan : (*view.blocks)[static_cast<size_t>(b)];
        const int nN = static_cast<int>(block.reducedPorts.size());

        for (int di : block.devices) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(di)];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                ctx.blockVOld[static_cast<size_t>(r)] = ctx.vv[static_cast<size_t>(r)];
                ctx.blockDv[static_cast<size_t>(r)] = ctx.ii[static_cast<size_t>(r)];
            }
        }

        for (int d = 0; d < static_cast<int>(block.linearDevices.size()); ++d) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(block.linearDevices[static_cast<size_t>(d)])];
            for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q)
                ctx.ii[static_cast<size_t>(q)] = 0.0;
            double i = k0[d];
            for (int j = 0; j < nN; ++j) {
                const double k = block.linearK[static_cast<size_t>(d) * nN + j];
                const double current = ctx.blockI[static_cast<size_t>(block.reducedPorts[static_cast<size_t>(j)])];
                i = verifyMixed ? std::fma(k, current, i) : i + k * current;
            }
            ctx.ii[static_cast<size_t>(block.linearPorts[static_cast<size_t>(d)])] = i;
        }

        for (int di : block.reducedDevices) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(di)];
            for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q)
                ctx.ii[static_cast<size_t>(q)] = ctx.blockI[static_cast<size_t>(q)];
        }

        RootClosedForms::settleBlockVoltages(
            view, b, s, relaxed, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
            ctx.vv.data(), ctx.rootDevices.data(), ctx.K);

        // The reduced solve enforced the *linearised* affine law.  Check it
        // against the actual law at the voltages we just settled: the op-amp
        // may have crossed into slew limiting or a rail during this sample,
        // even though it was linear at the warm start.  A mismatch here is
        // invisible to the network residual below (the current already
        // satisfies the network), so it has to be checked explicitly.
        bool lawValid = true;
        const double lawTol = std::max(tol, 1e-9);
        for (int d = 0; d < static_cast<int>(block.linearDevices.size()) && lawValid; ++d) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(block.linearDevices[static_cast<size_t>(d)])];
            const int o = dev.portBase;
            if (dev.kind == DeviceKind::OpAmp) {
                const OpAmpModel& oa = ctx.opAmps[dev.compIndex];
                const double vDiff = ctx.vv[static_cast<size_t>(o + 1)] -
                                     ctx.vv[static_cast<size_t>(o + 2)];
                double derivative = 0.0;
                const double vSource = OpAmpEngineLaw::sourceVoltage(
                    oa, vDiff, ctx.opAmpVp[dev.compIndex], ctx.opAmpAlpha[dev.compIndex],
                    ctx.opAmpMaxDv[dev.compIndex], ctx.staticMode, &derivative);
                const double vSourceUsed = oa.sourceVoltageFromSolverCurrent(
                    ctx.vv[static_cast<size_t>(o)], ctx.ii[static_cast<size_t>(o)]);
                const double scale = std::max(1.0, std::abs(vSource));
                if (!std::isfinite(vSource) || !std::isfinite(vSourceUsed) ||
                    std::abs(vSource - vSourceUsed) > lawTol * scale)
                    lawValid = false;
                if (verifyMixed) {
                    const double g = block.linearG[static_cast<size_t>(d)];
                    const double slopeScale = std::max(1.0, std::abs(g));
                    if (!std::isfinite(derivative) || std::abs(derivative - g) > 1e-9 * slopeScale)
                        lawValid = false;
                    // Recover a representable output voltage from the
                    // affine current where rounding in the full Z dot product
                    // perturbs the current. This changes only the candidate
                    // and still has to pass the original full-voltage gate.
                    if (lawValid && !oa.outputResistanceEmbedded) {
                        const double recovered = std::fma(oa.outputImpedance,
                            ctx.ii[static_cast<size_t>(o)], vSource);
                        const double delta = std::abs(recovered - ctx.vv[static_cast<size_t>(o)]);
                        if (delta <= 0.125 * lawTol)
                            ctx.vv[static_cast<size_t>(o)] = recovered;
                    }
                    double gOut = 0.0;
                    ctx.blockI[static_cast<size_t>(o)] = oa.outputCurrent(
                        oa.outputVoltageWeight() * ctx.vv[static_cast<size_t>(o)] - vSource, gOut);
                    ctx.blockI[static_cast<size_t>(o + 1)] = 0.0;
                    ctx.blockI[static_cast<size_t>(o + 2)] = 0.0;
                    ++ctx.deviceEvals;
                }
            } else if (dev.kind == DeviceKind::ControlledRes) {
                const double r = ctx.ctrlResValues[dev.compIndex];
                const double vUsed = ctx.ii[static_cast<size_t>(o)] * r;
                const double scale = std::max(1.0, std::abs(ctx.vv[static_cast<size_t>(o)]));
                if (!std::isfinite(vUsed) ||
                    std::abs(ctx.vv[static_cast<size_t>(o)] - vUsed) > lawTol * scale)
                    lawValid = false;
            }
        }
        if (!lawValid) {
            for (int dj : block.devices) {
                const RootDevice& devj = ctx.rootDevices[static_cast<size_t>(dj)];
                for (int q = devj.portBase; q < devj.portBase + devj.numPorts; ++q) {
                    ctx.ii[static_cast<size_t>(q)] = ctx.blockDv[static_cast<size_t>(q)];
                    ctx.vv[static_cast<size_t>(q)] = ctx.blockVOld[static_cast<size_t>(q)];
                }
            }
            return false;
        }

        for (int di : block.devices) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(di)];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                if (!std::isfinite(ctx.ii[static_cast<size_t>(r)]) ||
                    !std::isfinite(ctx.vv[static_cast<size_t>(r)])) {
                    for (int dj : block.devices) {
                        const RootDevice& devj = ctx.rootDevices[static_cast<size_t>(dj)];
                        for (int q = devj.portBase; q < devj.portBase + devj.numPorts; ++q) {
                            ctx.ii[static_cast<size_t>(q)] = ctx.blockDv[static_cast<size_t>(q)];
                            ctx.vv[static_cast<size_t>(q)] = ctx.blockVOld[static_cast<size_t>(q)];
                        }
                    }
                    return false;
                }
            }
        }
        return true;
    }

    /// Try one precompiled slope pattern. Both the region and the full
    /// original-law residual must pass before the candidate can commit.
    static inline bool solveMixedReducedBlock(
        const BlockView& view, int b, int s, int maxIter, double tol, bool relaxed,
        BlockSolveContext& ctx) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        if (ctx.staticMode || relaxed || block.alternateLinearSlopes.empty())
            return false;

        unsigned slopeMask = 0;
        for (size_t d = 0; d < block.linearDevices.size(); ++d) {
            const auto& dev = ctx.rootDevices[static_cast<size_t>(block.linearDevices[d])];
            const auto ci = dev.compIndex;
            const int o = dev.portBase;
            double derivative = 0.0;
            OpAmpEngineLaw::sourceVoltage(
                ctx.opAmps[ci], ctx.vv[o + 1] - ctx.vv[o + 2],
                ctx.opAmpVp[ci], ctx.opAmpAlpha[ci], ctx.opAmpMaxDv[ci], false, &derivative);
            if (derivative != 0.0)
                slopeMask |= 1u << d;
        }
        const auto plan = std::find_if(block.alternateLinearSlopes.begin(), block.alternateLinearSlopes.end(),
            [&](const NewtonBlock& variant) { return variant.linearSlopeMask == slopeMask; });
        if (plan == block.alternateLinearSlopes.end())
            return false;

        ++ctx.mixedAttempts;
        alignas(32) double k0[32];
        if (!prepareReducedBases(view, b, s, relaxed, ctx, k0, &*plan) ||
            !solveBlockNewton<true>(view, b, s, maxIter, tol, relaxed, ctx, &*plan) ||
            !reconstructReducedBlock(view, b, s, relaxed, tol, ctx, k0, true, &*plan))
            return false;

        for (int r : plan->reducedRowPorts)
            ctx.vv[static_cast<size_t>(r)] = ctx.blockV[static_cast<size_t>(r)];
        for (int di : block.devices) {
            const auto& dev = ctx.rootDevices[static_cast<size_t>(di)];
            for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q) {
                ctx.blockBase[static_cast<size_t>(q)] = RootClosedForms::foldPort(
                    view, q, s, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(), ctx.K);
                ctx.blockV[static_cast<size_t>(q)] = ctx.vv[static_cast<size_t>(q)];
            }
        }
        const double certifiedResidual = blockResidual(block, ctx, static_cast<int>(block.ports.size()));
        const auto restore = [&] {
            for (int di : block.devices) {
                const auto& dev = ctx.rootDevices[static_cast<size_t>(di)];
                for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q) {
                    ctx.ii[static_cast<size_t>(q)] = ctx.blockDv[static_cast<size_t>(q)];
                    ctx.vv[static_cast<size_t>(q)] = ctx.blockVOld[static_cast<size_t>(q)];
                }
            }
            return false;
        };
        if (!(certifiedResidual <= std::max(tol, 1e-9)))
            return restore();
        for (int di : block.devices) {
            const auto& dev = ctx.rootDevices[static_cast<size_t>(di)];
            for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q)
                if (!std::isfinite(ctx.blockF[static_cast<size_t>(q)]))
                    return restore();
        }
        for (int di : block.devices) {
            const auto& dev = ctx.rootDevices[static_cast<size_t>(di)];
            for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q)
                ctx.ii[static_cast<size_t>(q)] = ctx.blockI[static_cast<size_t>(q)];
        }
        // With one block the assembled original-law residual above is the root
        // acceptance residual; with several the pass checks the whole root.
        if (view.count == 1)
            ctx.lastBlockResidual = certifiedResidual;
        ++ctx.mixedAccepted;
        return true;
    }

    /// Solve one block after its affine devices have been eliminated.
    ///
    /// N = 0: the block is linear; `K0` is the answer.
    /// N = 1 diode: one Wright omega call with `Z'` absorbed into `Rs`.
    /// N = 2 anti-parallel diodes: the scalar pair solve on `Z'` (equal or
    /// unequal branch models).
    /// Otherwise the caller solves the original block by Newton.
    static inline bool solveReducedBlock(
        const BlockView& view,
        int b,
        int s,
        double tol,
        bool relaxed,
        BlockSolveContext& ctx) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const int nN = static_cast<int>(block.reducedPorts.size());
        // Only these reduced systems have a solver. A mixed block with
        // several coupled nonlinear devices goes directly to block Newton.
        if (!block.linearReduction)
            return false;
        if (nN != 0 && !block.reducedAntiParallel && !(block.reducedScalarDiode && nN == 1))
            return false;

        alignas(32) double k0[32];
        if (!prepareReducedBases(view, b, s, relaxed, ctx, k0))
            return false;

        if (nN == 0) {
            // Pure affine block.  `K0` already includes the op-amp state and
            // the folded open-circuit voltages.
        } else if (block.reducedAntiParallel) {
            const int p0 = block.reducedPorts[0];
            const int p1 = block.reducedPorts[1];
            const int r0 = block.reducedRowOfPort[static_cast<size_t>(p0)];
            const int r1 = block.reducedRowOfPort[static_cast<size_t>(p1)];
            if (r0 < 0 || r1 < 0)
                return false;
            const double Rp = block.reducedZ[static_cast<size_t>(r0) * nN + 0];
            const double voDiff = 0.5 * (ctx.blockBase[static_cast<size_t>(p0)] -
                                         ctx.blockBase[static_cast<size_t>(p1)]);
            double vOut = 0.0, i1 = 0.0, i2 = 0.0;
            block.reducedPair.solvePort(voDiff, Rp, vOut, i1, i2);
            if (!std::isfinite(i1) || !std::isfinite(i2))
                return false;
            for (int di : block.reducedDevices) {
                const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(di)];
                for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q)
                    ctx.blockI[static_cast<size_t>(q)] = 0.0;
            }
            ctx.blockI[static_cast<size_t>(p0)] = i1;
            ctx.blockI[static_cast<size_t>(p1)] = i2;
        } else {
            const int p = block.reducedScalarPort;
            for (int di : block.reducedDevices) {
                const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(di)];
                for (int q = dev.portBase; q < dev.portBase + dev.numPorts; ++q)
                    ctx.blockI[static_cast<size_t>(q)] = 0.0;
            }
            double i = 0.0, di = 0.0;
            block.reducedScalarModel.eval(ctx.blockBase[static_cast<size_t>(p)], i, di);
            if (!std::isfinite(i))
                return false;
            ctx.blockI[static_cast<size_t>(p)] = i;
        }

        return reconstructReducedBlock(view, b, s, relaxed, tol, ctx, k0);
    }

    /// Solve a singleton root port with the device's exact closed form. Used by
    /// both the forward block pass and the Gauss-Seidel relaxation sweeps.
    static inline bool solveScalarBlock(
        const BlockView& view,
        int b,
        int s,
        bool relaxed,
        BlockSolveContext& ctx) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        if (!isScalarBlock(block, ctx.rootDevices.data()))
            return false;
        const int g = block.devices[0];
        switch (ctx.rootDevices[static_cast<size_t>(g)].kind) {
        case DeviceKind::Triode:
            RootClosedForms::solveTriodeGroup(
                g, s, relaxed, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.triodeDomainPin.data(), ctx.comp, ctx.rootDevices.data(), ctx.K);
            break;
        case DeviceKind::Diode:
            RootClosedForms::solveDiodeGroup(
                g, s, relaxed, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.grpDiode.data(), ctx.rootDevices.data(), ctx.K);
            break;
        case DeviceKind::ControlledRes:
            RootClosedForms::solveCtrlResGroup(
                g, s, relaxed, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.ctrlResValues.data(), ctx.rootDevices.data(), ctx.K);
            break;
        case DeviceKind::Jfet:
            RootClosedForms::solveJfetGroup(
                g, s, relaxed, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.comp, ctx.rootDevices.data(), ctx.K, ctx.jfetFallbacks,
                ctx.jfetSolveFailed);
            break;
        case DeviceKind::Mosfet:
            RootClosedForms::solveMosfetGroup(
                g, s, relaxed, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.comp, ctx.rootDevices.data(), ctx.K, ctx.mosfetFallbacks,
                ctx.mosfetSolveFailed);
            break;
        case DeviceKind::OpAmp:
            RootClosedForms::solveOpAmpGroup(
                g, s, relaxed, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(),
                ctx.opAmpMaxDv.data(), ctx.staticMode, ctx.rootDevices.data(), ctx.K);
            break;
        case DeviceKind::Pentode:
        case DeviceKind::Bjt:
            break;
        }
        if (ctx.jfetSolveFailed || ctx.mosfetSolveFailed)
            return false;
        RootClosedForms::settleBlockVoltages(
            view, b, s, relaxed, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
            ctx.vv.data(), ctx.rootDevices.data(), ctx.K);
        return true;
    }

    /// Solve block `b` at position `s` by its kind; false when its solve failed.
    static inline bool solveBlock(
        const BlockView& view, int b, int s, int maxIter, double tol, bool relaxed,
        BlockSolveContext& ctx, bool* acceptedDeviceCurrents = nullptr) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        // Later stages amplify an error left in a block without a pentode, so only power tubes get the loose Preview tolerance.
        if (std::ranges::none_of(block.devices, [&](int d) { return ctx.rootDevices[static_cast<size_t>(d)].kind == DeviceKind::Pentode; }))
            tol = std::min(tol, ctx.modeTol);
        if (hasDirectSingleton(block, ctx.rootDevices, ctx.comp, ctx.opAmps, ctx.Z, ctx.K))
            return solveScalarBlock(view, b, s, relaxed, ctx);
        if (ctx.mixedEnabled && !block.alternateLinearSlopes.empty() &&
            solveMixedReducedBlock(view, b, s, maxIter, tol, relaxed, ctx))
            return true;
        if (block.linearReduction && ctx.reductions)
            return solveReducedBlock(view, b, s, tol, relaxed, ctx) ||
                   solveBlockNewton(view, b, s, maxIter, tol, relaxed, ctx, nullptr, acceptedDeviceCurrents);
        if (block.isAntiParallelDiodePair) {
            RootClosedForms::solveAntiParallelDiodeGroup(
                view, b, s, relaxed, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.vv.data(), ctx.rootDevices.data(), ctx.K);
            return true;
        }
        if (isScalarBlock(block, ctx.rootDevices.data()))
            return solveScalarBlock(view, b, s, relaxed, ctx);
        return solveBlockNewton(view, b, s, maxIter, tol, relaxed, ctx, nullptr, acceptedDeviceCurrents);
    }

    /// Solve the root in block order, using the scalar closed form for a
    /// singleton and the block Newton otherwise.
    static inline void solveRootBlocks(
        BlockSolveContext& ctx,
        NewtonContext& newtonCtx,
        const BlockView& view,
        int maxIter,
        double tol,
        int relaxSweeps = 0) noexcept
    {
        if (!ctx.gridLag.empty())
            predictLaterGrids(ctx, view.frozen);
        laggedIntoOpenCircuit(ctx, view, -1.0);
        RootClosedForms::pinTriodeDomains(
            view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
            ctx.triodeDomainPin.data(), ctx.comp, ctx.rootDevices.data(), ctx.K);
        ctx.lastBlockFallback = false;
        ctx.lastBlockResidual = std::numeric_limits<double>::infinity();
        ctx.jfetSolveFailed = false;
        ctx.mosfetSolveFailed = false;
        ctx.newtonIters = 0;
        ctx.deviceEvals = 0;
        ctx.keptBest = false;

        bool failed = false;
        bool acceptedDeviceCurrents = false;
        bool* saveDeviceCurrentStatus = view.count == 1 && ctx.gridLag.empty()
            ? &acceptedDeviceCurrents : nullptr;
        for (int s = 0; s < view.count && !failed; ++s)
            failed = !solveBlock(view, (*view.order)[static_cast<size_t>(s)], s, maxIter, tol, false, ctx,
                                 saveDeviceCurrentStatus);

        // A predicted grid current can warm-start later blocks and be resolved
        // by resolveMissedGrids. Regardless of prediction, the full system residual
        // must ALWAYS be evaluated to verify convergence.
        if (!failed && relaxSweeps == 0 && !ctx.gridLag.empty())
            resolveMissedGrids(ctx, view, maxIter, tol, failed);

        if (!failed && !std::isfinite(ctx.lastBlockResidual)) {
            // A later block re-solve can move the voltage of an earlier block.
            // Re-evaluate every root law at the final voltages before the
            // residual check: `ii` must describe the returned `vv`, not a
            // pre-resolve iterate that happens to leave a small stale residual.
            // A single Newton block already evaluated all device currents at
            // its accepted voltage. Still check the full residual.
            // Frozen ports keep the currents they were given; F is scratch until the residual.
            for (int r = 0; view.frozen && r < ctx.K; ++r)
                ctx.F[static_cast<size_t>(r)] = ctx.ii[static_cast<size_t>(r)];
            if (!acceptedDeviceCurrents)
                RootNewton::currentsOnlyInto(newtonCtx, ctx.vv, ctx.ii);
            for (int r = 0; view.frozen && r < ctx.K; ++r)
                if (view.frozen[r])
                    ctx.ii[static_cast<size_t>(r)] = ctx.F[static_cast<size_t>(r)];
            RootNewton::residualInto(newtonCtx, ctx.vv, ctx.vOc, ctx.Z, ctx.ii, ctx.F);
            for (int s = 0; view.held && s < view.count; ++s)
                for (const auto& [r, d] : (*view.blocks)[static_cast<size_t>((*view.order)[static_cast<size_t>(s)])].lagged)
                    ctx.F[static_cast<size_t>(r)] -= d.z * ctx.ii[static_cast<size_t>(d.col)];
            for (int r = 0; view.frozen && r < ctx.K; ++r)
                ctx.F[static_cast<size_t>(r)] *= !view.frozen[r];
            ctx.lastBlockResidual = RootNewton::maxAbs(ctx.F, ctx.K);
        }

        const double effTol = std::max(tol, 1e-9);

        static const bool resweep = livewdfPpTry("resweep");
        if (resweep && ctx.previewMode && !failed && ctx.lastBlockResidual > effTol) {
            for (int sweep = 0; sweep < 2 && ctx.lastBlockResidual > effTol && !failed; ++sweep) {
                bool tried = false;
                for (int s = 0; s < view.count && !failed; ++s) {
                    const int b = (*view.order)[static_cast<size_t>(s)];
                    const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
                    const bool missed = std::ranges::any_of(block.devices, [&](int di) {
                        const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(di)];
                        for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r)
                            if (std::abs(ctx.F[static_cast<size_t>(r)]) > effTol)
                                return true;
                        return false;
                    });
                    if (!missed)
                        continue;
                    tried = true;
                    // vOc already carries held lagged currents; exclude their live allFold terms.
                    for (const auto& [r, d] : block.lagged)
                        ctx.vOc[static_cast<size_t>(r)] += d.z * ctx.ii[static_cast<size_t>(d.col)];
                    failed = !solveBlock(view, b, s, maxIter, tol, true, ctx);
                    for (const auto& [r, d] : block.lagged)
                        ctx.vOc[static_cast<size_t>(r)] -= d.z * ctx.ii[static_cast<size_t>(d.col)];
                }
                if (!tried)
                    break;
                ++ctx.resweepSweeps;
                if (failed)
                    break;
                for (int r = 0; view.frozen && r < ctx.K; ++r)
                    ctx.F[static_cast<size_t>(r)] = ctx.ii[static_cast<size_t>(r)];
                RootNewton::currentsOnlyInto(newtonCtx, ctx.vv, ctx.ii);
                for (int r = 0; view.frozen && r < ctx.K; ++r)
                    if (view.frozen[r])
                        ctx.ii[static_cast<size_t>(r)] = ctx.F[static_cast<size_t>(r)];
                RootNewton::residualInto(newtonCtx, ctx.vv, ctx.vOc, ctx.Z, ctx.ii, ctx.F);
                for (int s = 0; view.held && s < view.count; ++s)
                    for (const auto& [r, d] : (*view.blocks)[static_cast<size_t>((*view.order)[static_cast<size_t>(s)])].lagged)
                        ctx.F[static_cast<size_t>(r)] -= d.z * ctx.ii[static_cast<size_t>(d.col)];
                for (int r = 0; view.frozen && r < ctx.K; ++r)
                    ctx.F[static_cast<size_t>(r)] *= !view.frozen[r];
                ctx.lastBlockResidual = RootNewton::maxAbs(ctx.F, ctx.K);
                if (ctx.lastBlockResidual <= effTol)
                    ++(sweep ? ctx.resweepClearedSecond : ctx.resweepClearedFirst);
            }
        }

        // Generalized fine blocks are a Gauss-Seidel splitting of a single
        // strongly-coupled SCC. The first pass is the usual forward sweep with
        // predecessor currents; each extra sweep refolds every block against
        // the latest currents, so a per-device split converges to the same
        // fixed point as the joint Newton when the coupling is linear.
        for (int sweep = 0; sweep < relaxSweeps && !failed; ++sweep) {
            if (ctx.lastBlockResidual <= effTol)
                break;
            for (int s = 0; s < view.count && !failed; ++s)
                failed = !solveBlock(view, (*view.order)[static_cast<size_t>(s)], s, maxIter, tol, true, ctx);
            if (failed)
                break;

            const int nR = ctx.nRed;
            const int* LIVEWDF_RESTRICT rCols = ctx.redCol.data();
            const double* LIVEWDF_RESTRICT iiData = ctx.ii.data();
            if (!ctx.Zred.empty()) {
                alignas(32) double iRed[64];
                const int nCap = std::min(nR, 64);
                for (int j = 0; j < nCap; ++j)
                    iRed[j] = iiData[rCols[j]];
                const double* LIVEWDF_RESTRICT zRedData = ctx.Zred.data();
                for (int r = 0; r < ctx.K; ++r) {
                    double v = ctx.vOc[static_cast<size_t>(r)];
                    const double* LIVEWDF_RESTRICT zr = zRedData + static_cast<size_t>(r) * nR;
                    for (int j = 0; j < nCap; ++j)
                        v -= zr[j] * iRed[j];
                    for (int j = nCap; j < nR; ++j)
                        v -= zr[j] * iiData[rCols[j]];
                    ctx.vv[static_cast<size_t>(r)] = v;
                }
            } else {
                for (int r = 0; r < ctx.K; ++r) {
                    double v = ctx.vOc[static_cast<size_t>(r)];
                    const double* LIVEWDF_RESTRICT zr = ctx.Z.data() + static_cast<size_t>(r) * ctx.K;
                    for (int j = 0; j < nR; ++j)
                        v -= zr[rCols[j]] * iiData[rCols[j]];
                    ctx.vv[static_cast<size_t>(r)] = v;
                }
            }
            // Re-evaluate into the live current vector: the accepted sample
            // reconstructs node voltages and updates reactive state from
            // `ii`, not from the scratch `iv` buffer.
            RootNewton::currentsOnlyInto(newtonCtx, ctx.vv, ctx.ii);
            RootNewton::residualInto(newtonCtx, ctx.vv, ctx.vOc, ctx.Z, ctx.ii, ctx.F);
            ctx.lastBlockResidual = RootNewton::maxAbs(ctx.F, ctx.K);
        }


        // The pin above is the previous sample's, so a block Newton may have
        // solved a triode in the wrong domain; the joint Newton re-decides it.
        laggedIntoOpenCircuit(ctx, view, 1.0);
        const bool pinChanged = RootClosedForms::pinTriodeDomains(
            view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
            ctx.triodeDomainPin.data(), ctx.comp, ctx.rootDevices.data(), ctx.K);
        if (!failed && !pinChanged && (ctx.lastBlockResidual <= effTol || (ctx.keepBlockAnswer && withinSwing(ctx)))) {
            ctx.lastConverged = true;
            ctx.lastResidual = ctx.lastBlockResidual;
            ctx.vvPrev2 = ctx.vvPrev;
            ctx.vvPrev = ctx.vv;
            return;
        }

        ctx.lastBlockFallback = true;
        ++ctx.blockFallbacks;
        ctx.lastConverged = false;
        if (failed)
            RootNewton::solveRootsNewton(newtonCtx, maxIter, tol, view, ctx.vvPrev, ctx.vvPrev2);
        else
            RootNewton::solveRootsNewtonSeeded(newtonCtx, maxIter, tol, view, ctx.vvPrev, ctx.vvPrev2);
    }

    /// A circuit that opts in keeps a block answer whose full residual misses by a fraction of each
    /// port's swing: that error is a lagged stage coupling, and a joint redo of
    /// only some samples switches between two answers, which is heard as noise.
    static inline bool withinSwing(const BlockSolveContext& ctx) noexcept
    {
        for (int r = 0; r < ctx.K; ++r) {
            const size_t i = static_cast<size_t>(r);
            if (!(std::abs(ctx.F[i]) <= 0.25 * (std::abs(ctx.vv[i]) + std::abs(ctx.vOc[i]))))
                return false;
        }
        return true;
    }

    /// Move the lagged couplings of `view` into (sign -1) or back out of (+1)
    /// the open-circuit voltages, at the previous sample's currents.
    static inline void laggedIntoOpenCircuit(BlockSolveContext& ctx, const BlockView& view, double sign) noexcept
    {
        for (int s = 0; view.held && s < view.count; ++s)
            for (const auto& [r, d] : (*view.blocks)[static_cast<size_t>((*view.order)[static_cast<size_t>(s)])].lagged) {
                if (sign < 0.0)
                    view.held[d.col] = ctx.ii[static_cast<size_t>(d.col)];
                ctx.vOc[static_cast<size_t>(r)] += sign * d.z * view.held[d.col];
            }
    }

    /// Runtime active-set variant of `solveRootBlocks`.
    static inline void solveRootBlocksRuntime(
        BlockSolveContext& ctx,
        NewtonContext& newtonCtx,
        const BlockView& view,
        int maxIter,
        double tol) noexcept
    {
        RootClosedForms::pinTriodeDomains(
            view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
            ctx.triodeDomainPin.data(), ctx.comp, ctx.rootDevices.data(), ctx.K);
        ctx.lastBlockFallback = false;
        ctx.lastBlockResidual = std::numeric_limits<double>::infinity();
        ctx.jfetSolveFailed = false;
        ctx.mosfetSolveFailed = false;
        ctx.newtonIters = 0;
        ctx.deviceEvals = 0;
        ctx.keptBest = false;
        ctx.runtimeBlockUsed = true;

        bool failed = false;
        for (int s = 0; s < view.count; ++s) {
            const int b = (*view.order)[static_cast<size_t>(s)];
            const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
            if (block.linearReduction) {
                if (!solveReducedBlock(view, b, s, tol, false, ctx) &&
                    !solveBlockNewton(view, b, s, maxIter, tol, false, ctx)) {
                    failed = true;
                    break;
                }
            } else if (block.isAntiParallelDiodePair) {
                RootClosedForms::solveAntiParallelDiodeGroup(
                    view, b, s, false, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                    ctx.vv.data(), ctx.rootDevices.data(), ctx.K);
            } else if (!solveBlockNewton(view, b, s, maxIter, tol, false, ctx)) {
                failed = true;
                break;
            }
        }

        if (!failed) {
            // The forward sweep may leave earlier blocks solved against a
            // predecessor current that a later block has since changed.
            // Re-evaluate the complete root-current vector at the final
            // voltages before certifying the residual.
            RootNewton::currentsOnlyInto(newtonCtx, ctx.vv, ctx.ii);
            RootNewton::residualInto(newtonCtx, ctx.vv, ctx.vOc, ctx.Z, ctx.ii, ctx.F);
            ctx.lastBlockResidual = RootNewton::maxAbs(ctx.F, ctx.K);
        }

        // Pass 2: Gauss-Seidel relaxation sweep if inter-stage coupling left residual
        const double effTol = std::max(tol, 1e-9);
        if (!failed && ctx.lastBlockResidual > effTol) {
            for (int s = 0; s < view.count; ++s) {
                const int b = (*view.order)[static_cast<size_t>(s)];
                const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
                if (block.linearReduction) {
                    if (!solveReducedBlock(view, b, s, tol, true, ctx) &&
                        !solveBlockNewton(view, b, s, maxIter, tol, true, ctx)) {
                        failed = true;
                        break;
                    }
                } else if (block.isAntiParallelDiodePair) {
                    RootClosedForms::solveAntiParallelDiodeGroup(
                        view, b, s, true, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                        ctx.vv.data(), ctx.rootDevices.data(), ctx.K);
                } else if (!solveBlockNewton(view, b, s, maxIter, tol, true, ctx)) {
                    failed = true;
                    break;
                }
            }
            if (!failed) {
                const int nR = ctx.nRed;
                const int* LIVEWDF_RESTRICT rCols = ctx.redCol.data();
                const double* LIVEWDF_RESTRICT iiData = ctx.ii.data();
                if (!ctx.Zred.empty()) {
                    alignas(32) double iRed[64];
                    const int nCap = std::min(nR, 64);
                    for (int j = 0; j < nCap; ++j)
                        iRed[j] = iiData[rCols[j]];
                    const double* LIVEWDF_RESTRICT zRedData = ctx.Zred.data();
                    for (int r = 0; r < ctx.K; ++r) {
                        double v = ctx.vOc[static_cast<size_t>(r)];
                        const double* LIVEWDF_RESTRICT zr = zRedData + static_cast<size_t>(r) * nR;
                        for (int j = 0; j < nCap; ++j)
                            v -= zr[j] * iRed[j];
                        for (int j = nCap; j < nR; ++j)
                            v -= zr[j] * iiData[rCols[j]];
                        ctx.vv[static_cast<size_t>(r)] = v;
                    }
                } else {
                    for (int r = 0; r < ctx.K; ++r) {
                        double v = ctx.vOc[static_cast<size_t>(r)];
                        const double* zr = ctx.Z.data() + static_cast<size_t>(r) * ctx.K;
                        for (int j = 0; j < nR; ++j)
                            v -= zr[rCols[j]] * iiData[rCols[j]];
                        ctx.vv[static_cast<size_t>(r)] = v;
                    }
                }
                // Keep the live current vector consistent with the final
                // voltages; `iv` is scratch and is not what `step()` commits.
                RootNewton::currentsOnlyInto(newtonCtx, ctx.vv, ctx.ii);
                RootNewton::residualInto(newtonCtx, ctx.vv, ctx.vOc, ctx.Z, ctx.ii, ctx.F);
                ctx.lastBlockResidual = RootNewton::maxAbs(ctx.F, ctx.K);
            }
        }

        // The pin above is the previous sample's, so a block Newton may have
        // solved a triode in the wrong domain; the joint Newton re-decides it.
        if (!failed && ctx.lastBlockResidual <= effTol &&
            !RootClosedForms::pinTriodeDomains(
                view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.triodeDomainPin.data(), ctx.comp, ctx.rootDevices.data(), ctx.K)) {
            ctx.lastConverged = true;
            ctx.lastResidual = ctx.lastBlockResidual;
            ctx.vvPrev2 = ctx.vvPrev;
            ctx.vvPrev = ctx.vv;
            return;
        }

        ctx.lastBlockFallback = true;
        ++ctx.blockFallbacks;
        ++ctx.runtimeBlockFallbacks;
        ctx.lastConverged = false;
        if (failed)
            RootNewton::solveRootsNewton(newtonCtx, maxIter, tol, view, ctx.vvPrev, ctx.vvPrev2);
        else
            RootNewton::solveRootsNewtonSeeded(newtonCtx, maxIter, tol, view, ctx.vvPrev, ctx.vvPrev2);
    }

    /// A later block's grid current is folded in before that block is solved,
    /// so predict it from the last two samples.
    static inline void predictLaterGrids(BlockSolveContext& ctx, const unsigned char* frozen = nullptr) noexcept
    {
        const int K = ctx.K;
        double* ii = ctx.ii.data();
        double* lag = ctx.gridLag.data();
        for (const RootDevice& dev : ctx.rootDevices) {
            if (dev.kind != DeviceKind::Triode)
                continue;
            const int c = dev.portBase + 2;
            const double now = ii[c];
            ii[c] = lag[K + c] = frozen && frozen[c] ? now : std::max(0.0, 2.0 * now - lag[c]);
            lag[c] = now;
        }
    }

    /// Solve once more each block whose grid prediction missed or whose
    /// inputs moved. Returns whether a grid conducted.
    static inline bool resolveMissedGrids(
        BlockSolveContext& ctx, const BlockView& view, int maxIter, double tol, bool& failed) noexcept
    {
        const int K = ctx.K;
        double* ii = ctx.ii.data();
        double* lag = ctx.gridLag.data();
        bool conducts = false;
        for (const RootDevice& dev : ctx.rootDevices)
            conducts |= dev.kind == DeviceKind::Triode && ii[dev.portBase + 2] != lag[K + dev.portBase + 2];
        if (!conducts)
            return false;
        std::copy(ii, ii + K, lag + 2 * K);
        bool moved = false;
        for (int s = 0; s < view.count; ++s) {
            const NewtonBlock& block = (*view.blocks)[static_cast<size_t>((*view.order)[static_cast<size_t>(s)])];
            bool redo = false;
            for (int c : block.laterGrid)
                redo |= ii[c] != lag[K + c];
            for (size_t r = 0; moved && !redo && r < block.predFold.size(); ++r)
                for (const auto& d : block.predFold[r])
                    redo |= ii[d.col] != lag[2 * K + d.col];
            if (redo) {
                failed |= !solveBlock(view, (*view.order)[static_cast<size_t>(s)], s, maxIter, tol, false, ctx);
                moved = true;
            }
        }
        return true;
    }

    /// Closed form pass for all-scalar partitioned stages.
    static inline void solveRootGroups(
        BlockSolveContext& ctx,
        const BlockView& view,
        int maxIter,
        double tol) noexcept
    {
        (void)maxIter;
        (void)tol;
        ctx.jfetSolveFailed = false;
        ctx.mosfetSolveFailed = false;


        for (int s = 0; s < view.count; ++s) {
            const int b = (*view.order)[static_cast<size_t>(s)];
            const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
            if (block.isAntiParallelDiodePair) {
                RootClosedForms::solveAntiParallelDiodeGroup(
                    view, b, s, false, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                    ctx.vv.data(), ctx.rootDevices.data(), ctx.K);
                continue;
            }
            const int g = block.devices[0];
            switch (ctx.rootDevices[g].kind) {
            case DeviceKind::Triode:
                RootClosedForms::solveTriodeGroup(
                    g, s, false, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                    ctx.triodeDomainPin.data(), ctx.comp, ctx.rootDevices.data(), ctx.K);
                break;
            case DeviceKind::Diode:
                RootClosedForms::solveDiodeGroup(
                    g, s, false, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                    ctx.grpDiode.data(), ctx.rootDevices.data(), ctx.K);
                break;
            case DeviceKind::ControlledRes:
                RootClosedForms::solveCtrlResGroup(
                    g, s, false, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                    ctx.ctrlResValues.data(), ctx.rootDevices.data(), ctx.K);
                break;
            case DeviceKind::Jfet:
                RootClosedForms::solveJfetGroup(
                    g, s, false, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                    ctx.comp, ctx.rootDevices.data(), ctx.K, ctx.jfetFallbacks, ctx.jfetSolveFailed);
                break;
            case DeviceKind::Mosfet:
                RootClosedForms::solveMosfetGroup(
                    g, s, false, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                    ctx.comp, ctx.rootDevices.data(), ctx.K, ctx.mosfetFallbacks, ctx.mosfetSolveFailed);
                break;
            case DeviceKind::OpAmp:
                RootClosedForms::solveOpAmpGroup(
                    g, s, false, b, view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                    ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(), ctx.opAmpMaxDv.data(),
                    ctx.staticMode, ctx.rootDevices.data(), ctx.K);
                break;
            case DeviceKind::Pentode:
            case DeviceKind::Bjt:
                break;
            }
            RootClosedForms::settleBlockVoltages(
                view, b, s, false, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.vv.data(), ctx.rootDevices.data(), ctx.K);
        }
        ctx.newtonIters = 0;
        ctx.deviceEvals = view.count;
        ctx.keptBest = false;
        const bool failed = ctx.jfetSolveFailed || ctx.mosfetSolveFailed;
        ctx.lastConverged = !failed;
        ctx.lastResidual = failed ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
        ctx.vvPrev2 = ctx.vvPrev;
        ctx.vvPrev = ctx.vv;
    }
};

} // namespace wdf::solver
