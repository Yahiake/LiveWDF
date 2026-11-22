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
#include "wdf/solver/RootJacobian.h"
#include "wdf/solver/RootClosedForms.h"
#include "wdf/solver/RootNewton.h"

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
    const std::vector<double>& vOc;
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
    bool& lastBlockFallback;
    double& lastBlockResidual;
    long long& blockFallbacks;
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
};

class RootBlockSolve {
public:
    /// Solve one block by Newton on its own live columns.
    static inline bool solveBlockNewton(
        const BlockView& view,
        int b,
        int s,
        int maxIter,
        double tol,
        bool relaxed,
        BlockSolveContext& ctx) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const int n = static_cast<int>(block.ports.size());
        if (n <= 0 || n > ctx.nRed)
            return false;

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

        int localDevPort = 0;
        for (int di : block.devices) {
            const RootDevice& dev = ctx.rootDevices[di];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++localDevPort) {
                ctx.blockBase[static_cast<size_t>(r)] =
                    relaxed ? RootClosedForms::foldPortAll(view, block, localDevPort, r, b, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(), ctx.K)
                            : RootClosedForms::foldPort(view, block, localDevPort, r, s, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(), ctx.K);
            }
        }

        for (int c : block.ports) {
            const double prev = ctx.ii[static_cast<size_t>(c)];
            ctx.blockI[static_cast<size_t>(c)] = std::isfinite(prev) ? prev : 0.0;
        }
        for (int di : block.devices) {
            const RootDevice& dev = ctx.rootDevices[di];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                const double v = ctx.vv[static_cast<size_t>(r)];
                ctx.blockV[static_cast<size_t>(r)] = std::isfinite(v) ? v : 0.0;
            }
        }

        if (ctx.prof)
            tock(ctx.prof->predict);

        const double effTol = std::max(tol, 1e-9);
        // An op-amp carries a one-sample internal state (`opAmpVp`), so the
        // currents committed for the previous sample are not necessarily the
        // currents the device law produces for this sample.  The initial
        // residual shortcut is only valid for memoryless blocks; with an
        // op-amp, evaluate the law first or the block will declare the last
        // sample's state converged and never advance a slewing or railed
        // stage.
        bool hasOpAmp = false;
        for (int di : block.devices) {
            if (ctx.rootDevices[di].kind == DeviceKind::OpAmp) {
                hasOpAmp = true;
                break;
            }
        }
        int localR0 = 0;
        const double* LIVEWDF_RESTRICT bz0 = block.blockZ.data();
        double initialMaxF = 0.0;
        alignas(32) double iPorts[32];
        const int nPorts = std::min(n, 32);
        for (int j = 0; j < nPorts; ++j)
            iPorts[j] = ctx.blockI[static_cast<size_t>(block.ports[static_cast<size_t>(j)])];

        for (int di : block.devices) {
            const RootDevice& dev = ctx.rootDevices[di];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++localR0) {
                double f = ctx.blockV[static_cast<size_t>(r)] -
                           ctx.blockBase[static_cast<size_t>(r)];
                if (!block.blockZ.empty()) {
                    const double* LIVEWDF_RESTRICT zr = bz0 + static_cast<size_t>(localR0) * n;
                    if (n == 1) {
                        f += zr[0] * iPorts[0];
                    } else if (n == 2) {
                        f += (zr[0] * iPorts[0] + zr[1] * iPorts[1]);
                    } else {
                        for (int j = 0; j < nPorts; ++j)
                            f += zr[j] * iPorts[j];
                        for (int j = nPorts; j < n; ++j)
                            f += zr[j] * ctx.blockI[static_cast<size_t>(block.ports[static_cast<size_t>(j)])];
                    }
                } else {
                    const double* zr = ctx.Z.data() + static_cast<size_t>(r) * ctx.K;
                    for (int c : block.ports)
                        f += zr[c] * ctx.blockI[static_cast<size_t>(c)];
                }
                ctx.blockF[static_cast<size_t>(r)] = f;
                initialMaxF = std::max(initialMaxF, std::abs(f));
            }
        }
        if (!hasOpAmp && initialMaxF < effTol) {
            for (int di : block.devices) {
                const RootDevice& dev = ctx.rootDevices[di];
                for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                    ctx.ii[static_cast<size_t>(r)] = ctx.blockI[static_cast<size_t>(r)];
                    ctx.vv[static_cast<size_t>(r)] = ctx.blockV[static_cast<size_t>(r)];
                }
            }
            return true;
        }

        bool converged = false;
        bool curValid = false;
        double maxF = 0.0;
        double prevF = 0.0;
        bool luValid = false;
        int luReuseCount = 0;
        alignas(32) double luFactors[64 * 64];
        alignas(16) int piv[64];
        const double* LIVEWDF_RESTRICT bz = block.blockZ.data();
        const int localMax = std::min(maxIter, 24);
        for (int it = 0; it < localMax; ++it) {
            ++ctx.newtonIters;
            if (!curValid) {
                for (int di : block.devices) {
                    RootResidual::currentsForDevice(
                        ctx.rootDevices[di], ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(),
                        ctx.bjts.data(), ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(),
                        ctx.opAmpMaxDv.data(), ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(),
                        ctx.K, ctx.blockV.data(), ctx.blockI.data(), ctx.blockJ.data(), ctx.deviceEvals);
                }
                if (ctx.prof)
                    tock(ctx.prof->device);

                for (int j = 0; j < nPorts; ++j)
                    iPorts[j] = ctx.blockI[static_cast<size_t>(block.ports[static_cast<size_t>(j)])];

                int localR = 0;
                for (int di : block.devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++localR) {
                        double f = ctx.blockV[static_cast<size_t>(r)] -
                                   ctx.blockBase[static_cast<size_t>(r)];
                        if (!block.blockZ.empty()) {
                            const double* LIVEWDF_RESTRICT zr = bz + static_cast<size_t>(localR) * n;
                            if (n == 1) {
                                f += zr[0] * iPorts[0];
                            } else if (n == 2) {
                                f += (zr[0] * iPorts[0] + zr[1] * iPorts[1]);
                            } else {
                                for (int j = 0; j < nPorts; ++j)
                                    f += zr[j] * iPorts[j];
                                for (int j = nPorts; j < n; ++j)
                                    f += zr[j] * ctx.blockI[static_cast<size_t>(block.ports[static_cast<size_t>(j)])];
                            }
                        } else {
                            const double* zr = ctx.Z.data() + static_cast<size_t>(r) * ctx.K;
                            for (int c : block.ports)
                                f += zr[c] * ctx.blockI[static_cast<size_t>(c)];
                        }
                        ctx.blockF[static_cast<size_t>(r)] = f;
                    }
                }

                maxF = 0.0;
                for (int di : block.devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r)
                        maxF = std::max(maxF, std::abs(ctx.blockF[static_cast<size_t>(r)]));
                }
                if (ctx.prof)
                    tock(ctx.prof->device);
                if (maxF < effTol) {
                    converged = true;
                    break;
                }
            }
            curValid = false;

            const double kappa = (prevF > 1e-15) ? (maxF / prevF) : 1.0;
            const bool canReuse = (n >= 3) && (n <= 64) && luValid && (luReuseCount < 2) && (kappa < 0.4) && (maxF < 0.05);

            bool solveOk = false;
            if (canReuse) {
                std::fill_n(ctx.blockRhs.data(), static_cast<size_t>(n), 0.0);
                int row = 0;
                for (int di : block.devices) {
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

                detail::solveLUData(luFactors, piv, ctx.blockRhs.data(), n);
                solveOk = true;
                if (ctx.prof)
                    tock(ctx.prof->lu);
                ++luReuseCount;
            } else {
                if (it > 0 && n >= 3) {
                    for (int di : block.devices) {
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
                for (int di : block.devices) {
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
                            if (!block.blockZ.empty()) {
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
                                    mRow[j] += jq * zq[block.ports[static_cast<size_t>(j)]];
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

                if (n < 3) {
                    solveOk = detail::solveData(ctx.blockM.data(), ctx.blockRhs.data(), n);
                } else if (n <= 64) {
                    std::memcpy(luFactors, ctx.blockM.data(), static_cast<size_t>(n) * n * sizeof(double));
                    solveOk = detail::factorLUData(luFactors, piv, n);
                    if (solveOk) {
                        luValid = true;
                        luReuseCount = 0;
                        detail::solveLUData(luFactors, piv, ctx.blockRhs.data(), n);
                    }
                } else {
                    solveOk = detail::factorLUData(ctx.blockM.data(), piv, n);
                    if (solveOk)
                        detail::solveLUData(ctx.blockM.data(), piv, ctx.blockRhs.data(), n);
                }
                if (ctx.prof)
                    tock(ctx.prof->lu);
            }
            if (!solveOk)
                return false;

            int localR = 0;
            const double* LIVEWDF_RESTRICT rhsData = ctx.blockRhs.data();
            for (int di : block.devices) {
                const RootDevice& dev = ctx.rootDevices[di];
                for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++localR) {
                    double d = -ctx.blockF[static_cast<size_t>(r)];
                    if (!block.blockZ.empty()) {
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
                            d -= zr[block.ports[static_cast<size_t>(j)]] * rhsData[j];
                    }
                    ctx.blockDv[static_cast<size_t>(r)] = d;
                }
            }

            for (int di : block.devices) {
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

            double scale = 1.0;
            for (int di : block.devices) {
                const RootDevice& dev = ctx.rootDevices[di];
                for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                    const double d = ctx.blockDv[static_cast<size_t>(r)];
                    const double lim = (d >= 0.0) ? ctx.stepUp[static_cast<size_t>(r)]
                                                  : ctx.stepDown[static_cast<size_t>(r)];
                    const double mag = std::abs(d);
                    if (mag > 0.0 && mag * scale > lim)
                        scale = lim / mag;
                }
            }
            if (scale < 1.0) {
                ++ctx.limitHits;
                for (int di : block.devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r)
                        ctx.blockDv[static_cast<size_t>(r)] *= scale;
                }
            }

            if (n < 3) {
                double maxStep = 0.0, maxV = 0.0;
                for (int di : block.devices) {
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
                    for (int di : block.devices) {
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
                    for (int di : block.devices) {
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
                for (int di : block.devices) {
                    const RootDevice& dev = ctx.rootDevices[di];
                    for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                        ctx.blockVOld[static_cast<size_t>(r)] = ctx.blockV[static_cast<size_t>(r)];
                    }
                }

                const double baseF = maxF;
                double stepSize = 1.0;
                double ftNorm = 0.0;
                const int maxLineTrials = 4;
                for (int h = 0; h < maxLineTrials; ++h) {
                    for (int di : block.devices) {
                        const RootDevice& dev = ctx.rootDevices[di];
                        for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                            ctx.blockV[static_cast<size_t>(r)] =
                                ctx.blockVOld[static_cast<size_t>(r)] + stepSize * ctx.blockDv[static_cast<size_t>(r)];
                        }
                    }

                    // Limit junctions belonging to this block
                    for (const auto& gl : ctx.gridJunctionLimiters) {
                        bool inBlock = false;
                        for (int di : block.devices) {
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
                        for (int di : block.devices) {
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

                    for (int di : block.devices) {
                        RootResidual::currentsForDevice(
                            ctx.rootDevices[di], ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(),
                            ctx.bjts.data(), ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(),
                            ctx.opAmpMaxDv.data(), ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(),
                            ctx.K, ctx.blockV.data(), ctx.blockI.data(), nullptr, ctx.deviceEvals);
                    }

                    for (int j = 0; j < nPorts; ++j)
                        iPorts[j] = ctx.blockI[static_cast<size_t>(block.ports[static_cast<size_t>(j)])];

                    int localR2 = 0;
                    for (int di : block.devices) {
                        const RootDevice& dev = ctx.rootDevices[di];
                        for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++localR2) {
                            double f = ctx.blockV[static_cast<size_t>(r)] -
                                       ctx.blockBase[static_cast<size_t>(r)];
                            if (!block.blockZ.empty()) {
                                const double* LIVEWDF_RESTRICT zr = bz + static_cast<size_t>(localR2) * n;
                                if (n == 1) {
                                    f += zr[0] * iPorts[0];
                                } else if (n == 2) {
                                    f += (zr[0] * iPorts[0] + zr[1] * iPorts[1]);
                                } else {
                                    for (int j = 0; j < nPorts; ++j)
                                        f += zr[j] * iPorts[j];
                                    for (int j = nPorts; j < n; ++j)
                                        f += zr[j] * ctx.blockI[static_cast<size_t>(block.ports[static_cast<size_t>(j)])];
                                }
                            } else {
                                const double* zr = ctx.Z.data() + static_cast<size_t>(r) * ctx.K;
                                for (int c : block.ports)
                                    f += zr[c] * ctx.blockI[static_cast<size_t>(c)];
                            }
                            ctx.blockF[static_cast<size_t>(r)] = f;
                        }
                    }

                    ftNorm = 0.0;
                    for (int di : block.devices) {
                        const RootDevice& dev = ctx.rootDevices[di];
                        for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r)
                            ftNorm = std::max(ftNorm, std::abs(ctx.blockF[static_cast<size_t>(r)]));
                    }

                    if (ftNorm <= baseF || h == maxLineTrials - 1)
                        break;
                    stepSize *= 0.5;
                }
                if (ctx.prof)
                    tock(ctx.prof->linesearch);

                double maxStep = 0.0, maxV = 0.0;
                for (int di : block.devices) {
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
                if (maxF < effTol) {
                    converged = true;
                    break;
                }
                if (it > 0 && maxStep < 1e-11 * std::max(1.0, maxV)) {
                    // Same rule as the small-block path: a stalled step above
                    // the gate is a failed solve, not a converged one.
                    break;
                }

                curValid = true;
            }
        }
        if (!converged)
            return false;


        for (int di : block.devices) {
            const RootDevice& dev = ctx.rootDevices[di];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r) {
                ctx.ii[static_cast<size_t>(r)] = ctx.blockI[static_cast<size_t>(r)];
                ctx.vv[static_cast<size_t>(r)] = ctx.blockV[static_cast<size_t>(r)];
            }
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
        double* LIVEWDF_RESTRICT k0) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const int mL = static_cast<int>(block.linearDevices.size());
        if (mL <= 0 || mL > 32)
            return false;

        const auto fold = [&](int r) {
            return relaxed ? RootClosedForms::foldPortAll(
                                 view, r, b, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(), ctx.K)
                           : RootClosedForms::foldPort(
                                 view, r, s, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(), ctx.K);
        };

        alignas(32) double a[32 * 32];
        std::copy(block.linearA.begin(), block.linearA.end(), a);
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
                const double h = -(vSource - g * vDiff) / ro;
                rhs[d] = h + (1.0 / ro) * vo - (g / ro) * vp + (g / ro) * vn;
            } else if (dev.kind == DeviceKind::ControlledRes) {
                const double g = block.linearG[static_cast<size_t>(d)];
                rhs[d] = g * fold(o);
            } else {
                return false;
            }
        }

        if (!detail::solveData(a, rhs, mL))
            return false;
        for (int d = 0; d < mL; ++d)
            k0[d] = rhs[d];

        for (int r : block.reducedRowPorts) {
            double v = fold(r);
            const double* zr = ctx.Z.data() + static_cast<size_t>(r) * ctx.K;
            for (int d = 0; d < mL; ++d)
                v -= zr[block.linearPorts[static_cast<size_t>(d)]] * k0[d];
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
        const double* LIVEWDF_RESTRICT k0) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
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
            for (int j = 0; j < nN; ++j)
                i += block.linearK[static_cast<size_t>(d) * nN + j] *
                     ctx.blockI[static_cast<size_t>(block.reducedPorts[static_cast<size_t>(j)])];
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
                const double vSourceUsed =
                    ctx.vv[static_cast<size_t>(o)] -
                    ctx.ii[static_cast<size_t>(o)] * oa.outputImpedance;
                const double scale = std::max(1.0, std::abs(vSource));
                if (!std::isfinite(vSource) || !std::isfinite(vSourceUsed) ||
                    std::abs(vSource - vSourceUsed) > lawTol * scale)
                    lawValid = false;
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

    /// Solve one block after its affine devices have been eliminated.
    ///
    /// N = 0: the block is linear; `K0` is the answer.
    /// N = 1 diode: one Wright omega call with `Z'` absorbed into `Rs`.
    /// N = 2 anti-parallel diodes: the scalar pair solve on `Z'` (equal or
    /// unequal branch models).
    /// otherwise: a Newton loop on the reduced nonlinear ports.
    static inline bool solveReducedBlock(
        const BlockView& view,
        int b,
        int s,
        double tol,
        bool relaxed,
        BlockSolveContext& ctx) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        if (!block.linearReduction)
            return false;

        alignas(32) double k0[32];
        if (!prepareReducedBases(view, b, s, relaxed, ctx, k0))
            return false;

        const int nN = static_cast<int>(block.reducedPorts.size());
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
        } else if (block.reducedScalarDiode && nN == 1) {
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
        } else {
            // A mixed block with more than a scalar diode or an anti-parallel
            // pair has no closed form on the Schur complement.  The reduced
            // Newton iterates the nonlinear ports but holds the affine devices
            // at a single linearisation, so its eliminated rows can miss the
            // gate even when the reduced residual looks converged.  The full
            // block Newton owns every device law and every row; use it.
            return false;
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
        BlockSolveContext& ctx) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        if (block.linearReduction)
            return solveReducedBlock(view, b, s, tol, relaxed, ctx) ||
                   solveBlockNewton(view, b, s, maxIter, tol, relaxed, ctx);
        if (block.isAntiParallelDiodePair) {
            RootClosedForms::solveAntiParallelDiodeGroup(
                view, b, s, relaxed, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
                ctx.vv.data(), ctx.rootDevices.data(), ctx.K);
            return true;
        }
        if (isScalarBlock(block, ctx.rootDevices.data()))
            return solveScalarBlock(view, b, s, relaxed, ctx);
        return solveBlockNewton(view, b, s, maxIter, tol, relaxed, ctx);
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
            predictLaterGrids(ctx);
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
        for (int s = 0; s < view.count && !failed; ++s)
            failed = !solveBlock(view, (*view.order)[static_cast<size_t>(s)], s, maxIter, tol, false, ctx);

        // A predicted grid current can warm-start later blocks and be resolved
        // by resolveMissedGrids. Regardless of prediction, the full system residual
        // must ALWAYS be evaluated to verify convergence.
        if (!failed && relaxSweeps == 0 && !ctx.gridLag.empty())
            resolveMissedGrids(ctx, view, maxIter, tol, failed);

        if (!failed) {
            // A later block re-solve can move the voltage of an earlier block.
            // Re-evaluate every root law at the final voltages before the
            // residual check: `ii` must describe the returned `vv`, not a
            // pre-resolve iterate that happens to leave a small stale residual.
            RootNewton::currentsOnlyInto(newtonCtx, ctx.vv, ctx.ii);
            RootNewton::residualInto(newtonCtx, ctx.vv, ctx.vOc, ctx.Z, ctx.ii, ctx.F);
            ctx.lastBlockResidual = RootNewton::maxAbs(ctx.F, ctx.K);
        }

        const double effTol = std::max(tol, 1e-9);

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
        const bool pinChanged = RootClosedForms::pinTriodeDomains(
            view, ctx.Z.data(), ctx.vOc.data(), ctx.ii.data(),
            ctx.triodeDomainPin.data(), ctx.comp, ctx.rootDevices.data(), ctx.K);
        if (!failed && ctx.lastBlockResidual <= effTol && !pinChanged) {
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
    static inline void predictLaterGrids(BlockSolveContext& ctx) noexcept
    {
        const int K = ctx.K;
        double* ii = ctx.ii.data();
        double* lag = ctx.gridLag.data();
        for (const RootDevice& dev : ctx.rootDevices) {
            if (dev.kind != DeviceKind::Triode)
                continue;
            const int c = dev.portBase + 2;
            const double now = ii[c];
            ii[c] = lag[K + c] = std::max(0.0, 2.0 * now - lag[c]);
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
