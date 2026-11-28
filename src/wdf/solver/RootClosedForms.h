#pragma once

#include <cmath>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

#include "wdf/DenseTopology.h"
#include "spice/Compiled.h"
#include "devices/Diode.h"
#include "devices/Triode.h"
#include "devices/Jfet.h"
#include "devices/Mosfet.h"
#include "devices/OpAmp.h"

#if defined(_MSC_VER)
#define LIVEWDF_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define LIVEWDF_RESTRICT __restrict__
#else
#define LIVEWDF_RESTRICT
#endif

namespace wdf::solver {

class RootClosedForms {
public:
    /// Fast folding using pre-gathered non-zero predecessor couplings.
    /// Fast fold using pre-gathered non-zero predecessor dependencies if available.
    static inline double foldPort(
        const BlockView& view,
        const NewtonBlock& block,
        int localR,
        int globalR,
        int s,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        const double* LIVEWDF_RESTRICT iiData,
        int K) noexcept
    {
        if (static_cast<size_t>(localR) < block.predFold.size()) {
            double vo = vOcData[globalR];
            for (const auto& dep : block.predFold[static_cast<size_t>(localR)])
                vo -= dep.z * iiData[dep.col];
            return vo;
        }
        return foldPort(view, globalR, s, zData, vOcData, iiData, K);
    }

    /// Port `r`'s open-circuit voltage with the `s` blocks already solved folded in.
    static inline double foldPort(
        const BlockView& view,
        int r,
        int s,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        const double* LIVEWDF_RESTRICT iiData,
        int K) noexcept
    {
        const double* LIVEWDF_RESTRICT zr = zData + static_cast<size_t>(r) * K;
        double vo = vOcData[r];
        for (int p = 0; p < s; ++p) {
            const int bp = (*view.order)[static_cast<size_t>(p)];
            const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(bp)];
            for (int c : block.ports) {
                const double z = zr[c];
                if (z != 0.0)
                    vo -= z * iiData[c];
            }
        }
        return vo;
    }

    /// Fast folding using pre-gathered couplings from all other blocks.
    static inline double foldPortAll(
        const BlockView& view,
        const NewtonBlock& block,
        int localR,
        int globalR,
        int currentBlock,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        const double* LIVEWDF_RESTRICT iiData,
        int K) noexcept
    {
        if (static_cast<size_t>(localR) < block.allFold.size()) {
            double vo = vOcData[globalR];
            for (const auto& dep : block.allFold[static_cast<size_t>(localR)])
                vo -= dep.z * iiData[dep.col];
            return vo;
        }
        return foldPortAll(view, globalR, currentBlock, zData, vOcData, iiData, K);
    }

    /// Port `r`'s open-circuit voltage with all blocks except `currentBlock` folded in.
    static inline double foldPortAll(
        const BlockView& view,
        int r,
        int currentBlock,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        const double* LIVEWDF_RESTRICT iiData,
        int K) noexcept
    {
        const double* LIVEWDF_RESTRICT zr = zData + static_cast<size_t>(r) * K;
        double vo = vOcData[r];
        for (int p = 0; p < view.count; ++p) {
            const int bp = (*view.order)[static_cast<size_t>(p)];
            if (bp == currentBlock)
                continue;
            const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(bp)];
            for (int c : block.ports) {
                const double z = zr[c];
                if (z != 0.0)
                    vo -= z * iiData[c];
            }
        }
        return vo;
    }

    /// Decide every triode's domain state from the currents in `iiData`, and
    /// report whether any decision changed.
    static inline bool pinTriodeDomains(
        const BlockView& view,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        const double* LIVEWDF_RESTRICT iiData,
        unsigned char* LIVEWDF_RESTRICT triodeDomainPin,
        const Compiled& comp,
        const RootDevice* LIVEWDF_RESTRICT rootDevices,
        int K, const double* loaded = nullptr) noexcept
    {
        if (comp.tri.empty()) return false;
        bool changed = false;
        for (int p = 0; p < view.count; ++p) {
            const int b = (*view.order)[static_cast<size_t>(p)];
            const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
            for (int g : block.devices) {
                const RootDevice& dev = rootDevices[g];
                if (dev.kind != DeviceKind::Triode)
                    continue;
                const int o = dev.portBase;
                const TriodeModel& m = comp.tri[dev.compIndex].model;
                double voPk = loaded ? loaded[2 * dev.compIndex] : foldPort(view, o, view.count, zData, vOcData, iiData, K);
                double voGik = loaded ? loaded[2 * dev.compIndex + 1] : foldPort(view, o + 2, view.count, zData, vOcData, iiData, K);
                voPk += zData[static_cast<size_t>(o) * K + o] * iiData[o];
                voGik += zData[static_cast<size_t>(o + 2) * K + o] * iiData[o];
                if (dev.numActiveRows == 2) {
                    voPk += zData[static_cast<size_t>(o) * K + o + 2] * iiData[o + 2];
                    voGik += zData[static_cast<size_t>(o + 2) * K + o + 2] * iiData[o + 2];
                }
                double ip = 0.0;
                double ig = 0.0;
                const unsigned char pin = solveTriode(m, voPk, voGik, zData, o, K, ip, ig);
                changed |= triodeDomainPin[o] != pin;
                triodeDomainPin[o] = pin;
            }
        }
        return changed;
    }

    /// The plate law for `vpk = pv - pz*ip`, `vgk = gv - gz*ip`.
    static inline unsigned char solvePlate(
        const TriodeModel& m, double pv, double pz, double gv, double gz, double& ip) noexcept
    {
        if (m.pieceQuadric) {
            constexpr double a = 9.97167208, b = 3.4528332, cc = 1.40669992;
            constexpr double d = -3.12592106, e = 2.40056664, f = 1.71907587;
            constexpr double h = -7.98593702, jointV = 100.43918363;
            const double x0 = pv / 250.0, y0 = gv / 2.0;
            const double xz = pz / 250.0, yz = gz / 2.0;
            double best = std::numeric_limits<double>::infinity();
            const auto solveRegion = [&](bool upper) noexcept {
                const double joint = jointV / 250.0;
                const double q0 = a * x0 * x0 + b * x0 * y0 + cc * y0 * y0 +
                    d * x0 + e * y0 + f + (upper ? h * (x0 - joint) * (x0 - joint) : 0.0);
                const double q1 = -2.0 * a * x0 * xz - b * (x0 * yz + y0 * xz) -
                    2.0 * cc * y0 * yz - d * xz - e * yz -
                    (upper ? 2.0 * h * (x0 - joint) * xz : 0.0);
                const double q2 = a * xz * xz + b * xz * yz + cc * yz * yz +
                    (upper ? h * xz * xz : 0.0);
                const double c2 = 0.001 * m.pieceScale * q2;
                const double c1 = 0.001 * m.pieceScale * q1 - 1.0;
                const double c0 = 0.001 * m.pieceScale * q0;
                if (std::abs(c2) < 1e-30) {
                    const double candidate = -c0 / c1;
                    const double vpk = pv - pz * candidate;
                    if (candidate >= 0.0 && ((upper && vpk > jointV) || (!upper && vpk <= jointV))) {
                        best = std::min(best, candidate);
                    }
                    return;
                }
                const double discriminant = c1 * c1 - 4.0 * c2 * c0;
                if (discriminant < 0.0)
                    return;
                const double root = std::sqrt(discriminant);
                const double roots[2] = {(-c1 - root) / (2.0 * c2), (-c1 + root) / (2.0 * c2)};
                for (double candidate : roots) {
                    const double vpk = pv - pz * candidate;
                    if (candidate >= 0.0 && ((upper && vpk > jointV) || (!upper && vpk <= jointV))) {
                        best = std::min(best, candidate);
                    }
                }
            };
            solveRegion(false);
            solveRegion(true);
            ip = std::isfinite(best) ? best : 0.0;
            if (pv - pz * ip >= 0.0)
                return 0;
            if (pz > 0.0 && pv > 0.0) {
                ip = pv / ((1.0 + TriodeModel::kShort) * pz);
                return 1;
            }
            ip = 0.0;
            return 2;
        }
        const double Q = pv + m.cg * gv + m.c0;
        const double D = 1.0 + 4.0 * m.p.kP2 * (pz + m.cg * gz) * Q;
        ip = 0.0;
        if (D < 0.0)
            return 0;
        const double q = 2.0 * Q / (1.0 + std::sqrt(D));
        if (q <= 0.0)
            return 0;
        ip = m.p.kP2 * q * q;
        if (pv - pz * ip >= 0.0)
            return 0;
        if (pz > 0.0 && pv > 0.0) {
            ip = pv / ((1.0 + TriodeModel::kShort) * pz);
            return 1;
        }
        ip = 0.0;
        return 2;
    }

    /// The paper's post-solve test, once per grid state: solve the quadric,
    /// read `Vpk` back, and when it is negative project onto the short if the
    /// open-circuit plate voltage is positive and onto cold otherwise. A
    /// conducting grid folds its line into the plate's source and impedance,
    /// which leaves the same quadratic in `q`. The lines' slopes rise, so it
    /// solves again on each next line until the solved grid is below it.
    /// Returns the domain state.
    static inline unsigned char solveTriode(
        const TriodeModel& m,
        double voPk,
        double voGik,
        const double* LIVEWDF_RESTRICT zData,
        int o,
        int K,
        double& ip,
        double& ig) noexcept
    {
        const double* zp = zData + static_cast<size_t>(o) * K;
        const double* zg = zData + static_cast<size_t>(o + 2) * K;
        unsigned char pin = solvePlate(m, voPk, zp[o], voGik, zg[o], ip);
        ig = 0.0;
        for (const TriodeModel::GridLine& l : m.gridLines) {
            if (!m.gridConducts || voGik - zg[o] * ip - zg[o + 2] * ig - l.on <= l.r * ig)
                break;
            const double a = 1.0 / (l.r + zg[o + 2]);
            const double e = a * (voGik - l.on);
            pin = solvePlate(m, voPk - zp[o + 2] * e, zp[o] - zp[o + 2] * a * zg[o],
                             l.on + l.r * e, l.r * a * zg[o], ip);
            ig = e - a * zg[o] * ip;
        }
        return pin;
    }

    /// The block's final port voltages after a scalar closed form.
    static inline void settleBlockVoltages(
        const BlockView& view,
        int b,
        int s,
        bool relaxed,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        const double* LIVEWDF_RESTRICT iiData,
        double* LIVEWDF_RESTRICT vvData,
        const RootDevice* LIVEWDF_RESTRICT rootDevices,
        int K) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        int localR = 0;
        const int n = static_cast<int>(block.ports.size());
        const double* LIVEWDF_RESTRICT bz = block.blockZ.data();
        for (int di : block.devices) {
            const RootDevice& dev = rootDevices[di];
            for (int r = dev.portBase; r < dev.portBase + dev.numPorts; ++r, ++localR) {
                double v = relaxed ? foldPortAll(view, block, localR, r, b, zData, vOcData, iiData, K)
                                   : foldPort(view, block, localR, r, s, zData, vOcData, iiData, K);
                if (!block.blockZ.empty()) {
                    const double* LIVEWDF_RESTRICT zr = bz + static_cast<size_t>(localR) * n;
                    if (n == 1) {
                        v -= zr[0] * iiData[block.ports[0]];
                    } else if (n == 2) {
                        v -= (zr[0] * iiData[block.ports[0]] + zr[1] * iiData[block.ports[1]]);
                    } else {
                        for (int j = 0; j < n; ++j)
                            v -= zr[j] * iiData[block.ports[static_cast<size_t>(j)]];
                    }
                } else {
                    const double* LIVEWDF_RESTRICT zr = zData + static_cast<size_t>(r) * K;
                    if (n == 1) {
                        const int c = block.ports[0];
                        v -= zr[c] * iiData[c];
                    } else {
                        for (int c : block.ports)
                            v -= zr[c] * iiData[c];
                    }
                }
                vvData[r] = v;
            }
        }
    }

    /// One quadric triode with its grid line in exact closed form.
    static inline void solveTriodeGroup(
        int g,
        int s,
        bool relaxed,
        int b,
        const BlockView& view,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        double* LIVEWDF_RESTRICT iiData,
        unsigned char* LIVEWDF_RESTRICT triodeDomainPin,
        const Compiled& comp,
        const RootDevice* LIVEWDF_RESTRICT rootDevices,
        int K) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const RootDevice& dev = rootDevices[g];
        const int o = dev.portBase;
        const TriodeModel& m = comp.tri[dev.compIndex].model;

        const double voPk = relaxed ? foldPortAll(view, block, 0, o, b, zData, vOcData, iiData, K)
                                    : foldPort(view, block, 0, o, s, zData, vOcData, iiData, K);
        const double voGik = relaxed ? foldPortAll(view, block, 2, o + 2, b, zData, vOcData, iiData, K)
                                     : foldPort(view, block, 2, o + 2, s, zData, vOcData, iiData, K);
        double ip = 0.0;
        double ig = 0.0;
        triodeDomainPin[o] = m.gridConducts
            ? solveTriode(m, voPk, voGik, zData, o, K, ip, ig)
            : solvePlate(m, voPk, zData[static_cast<size_t>(o) * K + o], voGik,
                         zData[static_cast<size_t>(o + 2) * K + o], ip);
        iiData[o] = ip;
        iiData[o + 1] = 0.0;
        iiData[o + 2] = ig;
    }

    /// One diode in exact scalar closed form via Wright Omega.
    static inline void solveDiodeGroup(
        int g,
        int s,
        bool relaxed,
        int b,
        const BlockView& view,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        double* LIVEWDF_RESTRICT iiData,
        const DiodeModel* LIVEWDF_RESTRICT grpDiode,
        const RootDevice* LIVEWDF_RESTRICT rootDevices,
        int K) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const RootDevice& dev = rootDevices[g];
        const int e = dev.portBase;
        double i = 0.0;
        double di = 0.0;
        const double vo = relaxed ? foldPortAll(view, block, 0, e, b, zData, vOcData, iiData, K)
                                  : foldPort(view, block, 0, e, s, zData, vOcData, iiData, K);
        grpDiode[dev.compIndex].eval(vo, i, di);
        iiData[e] = i;
    }

    /// Anti-parallel diode pair: both branch laws are exact, and the reversed
    /// ports collapse their two port equations into one scalar solve. Werner
    /// eqn (39) supplies that solve's closed-form start; the port equation's
    /// inversion is finished on the exact laws (see AntiParallelDiodeModel).
    static inline void solveAntiParallelDiodeGroup(
        const BlockView& view,
        int b,
        int s,
        bool relaxed,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        double* LIVEWDF_RESTRICT iiData,
        double* LIVEWDF_RESTRICT vvData,
        const RootDevice* LIVEWDF_RESTRICT rootDevices,
        int K) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const int p0 = block.ports[0];
        const int p1 = block.ports[1];
        const double vo0 = relaxed ? foldPortAll(view, block, 0, p0, b, zData, vOcData, iiData, K)
                                   : foldPort(view, block, 0, p0, s, zData, vOcData, iiData, K);
        const double vo1 = relaxed ? foldPortAll(view, block, 1, p1, b, zData, vOcData, iiData, K)
                                   : foldPort(view, block, 1, p1, s, zData, vOcData, iiData, K);
        const double Rp = zData[static_cast<size_t>(p0) * K + p0];
        // Reversed ports: v1 = -v0 and Z10 = -Z00 in a reciprocal network, so
        // the pair needs only its driving-point impedance and the common-mode
        // open-circuit voltage.
        const double voDiff = 0.5 * (vo0 - vo1);
        double vOut = 0.0, i1 = 0.0, i2 = 0.0;
        block.diodePair.solvePort(voDiff, Rp, vOut, i1, i2);
        iiData[p0] = i1;
        iiData[p1] = i2;
        settleBlockVoltages(view, b, s, relaxed, zData, vOcData, iiData, vvData, rootDevices, K);
    }

    /// One controlled resistor in exact closed form.
    static inline void solveCtrlResGroup(
        int g,
        int s,
        bool relaxed,
        int b,
        const BlockView& view,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        double* LIVEWDF_RESTRICT iiData,
        const double* LIVEWDF_RESTRICT ctrlResValues,
        const RootDevice* LIVEWDF_RESTRICT rootDevices,
        int K) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const RootDevice& dev = rootDevices[g];
        const int e = dev.portBase;
        const double vo = relaxed ? foldPortAll(view, block, 0, e, b, zData, vOcData, iiData, K)
                                  : foldPort(view, block, 0, e, s, zData, vOcData, iiData, K);
        const double R = ctrlResValues[dev.compIndex];
        const double Zee = zData[static_cast<size_t>(e) * K + e];
        const double denom = R + Zee;
        const double i = (std::abs(denom) > 1e-12) ? (vo / denom) : 0.0;
        iiData[e] = i;
    }

    /// One general JFET in scalar closed form / bracketed reference.
    static inline void solveJfetGroup(
        int g,
        int s,
        bool relaxed,
        int b,
        const BlockView& view,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        double* LIVEWDF_RESTRICT iiData,
        const Compiled& comp,
        const RootDevice* LIVEWDF_RESTRICT rootDevices,
        int K,
        long long& jfetFallbacks,
        bool& jfetSolveFailed) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const RootDevice& dev = rootDevices[g];
        const int o = dev.portBase;
        const JfetModel& m = comp.jfet[dev.compIndex].model;
        const double voDs = relaxed ? foldPortAll(view, block, 0, o, b, zData, vOcData, iiData, K)
                                    : foldPort(view, block, 0, o, s, zData, vOcData, iiData, K);
        const double voGs = relaxed ? foldPortAll(view, block, 1, o + 1, b, zData, vOcData, iiData, K)
                                    : foldPort(view, block, 1, o + 1, s, zData, vOcData, iiData, K);
        const double zdd = zData[static_cast<size_t>(o) * K + o];
        const double zgd = zData[static_cast<size_t>(o + 1) * K + o];
        const double seed = std::isfinite(iiData[o]) ? iiData[o] : 0.0;

        JfetGroupSolution sol = m.solveGroup(voDs, voGs, zdd, zgd, seed);
        if (!sol.ok) {
            ++jfetFallbacks;
            sol = m.solveReferenceSeeded(voDs, voGs, zdd, zgd, seed);
            sol.usedReference = true;
        }
        if (!sol.ok) {
            iiData[o] = 0.0;
            iiData[o + 1] = 0.0;
            jfetSolveFailed = true;
            return;
        }
        iiData[o] = sol.id;
        iiData[o + 1] = 0.0;
    }

    /// One general n-channel MOSFET in scalar closed form / bracketed reference.
    static inline void solveMosfetGroup(
        int g,
        int s,
        bool relaxed,
        int b,
        const BlockView& view,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        double* LIVEWDF_RESTRICT iiData,
        const Compiled& comp,
        const RootDevice* LIVEWDF_RESTRICT rootDevices,
        int K,
        long long& mosfetFallbacks,
        bool& mosfetSolveFailed) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const RootDevice& dev = rootDevices[g];
        const int o = dev.portBase;
        const MosfetModel& m = comp.mosfet[dev.compIndex].model;
        const double voDs = relaxed ? foldPortAll(view, block, 0, o, b, zData, vOcData, iiData, K)
                                    : foldPort(view, block, 0, o, s, zData, vOcData, iiData, K);
        const double voGs = relaxed ? foldPortAll(view, block, 1, o + 1, b, zData, vOcData, iiData, K)
                                    : foldPort(view, block, 1, o + 1, s, zData, vOcData, iiData, K);
        const double zdd = zData[static_cast<size_t>(o) * K + o];
        const double zgd = zData[static_cast<size_t>(o + 1) * K + o];
        const double seed = std::isfinite(iiData[o]) ? iiData[o] : 0.0;

        MosfetGroupSolution sol = m.solveGroup(voDs, voGs, zdd, zgd, seed);
        if (!sol.ok) {
            ++mosfetFallbacks;
            sol = m.solveReferenceSeeded(voDs, voGs, zdd, zgd, seed);
            sol.usedReference = true;
        }
        if (!sol.ok) {
            iiData[o] = 0.0;
            iiData[o + 1] = 0.0;
            mosfetSolveFailed = true;
            return;
        }
        iiData[o] = sol.id;
        iiData[o + 1] = 0.0;
    }

    struct ClassABSolution {
        double loadCurrent = 0.0;
        double modelCurrent = 0.0;
        double residual = 0.0;
        bool ok = false;
    };

    /// The passive load and negative feedback make the output error fall
    /// with current. The output current rises with error, so the intersection
    /// is unique. Solve in volts to avoid Newton steps across the nearly
    /// flat current-limit regions, then check the original network residual.
    static ClassABSolution solveClassABOutput(
        const OpAmpModel& model, double voOut, double voPos, double voNeg,
        double zoo, double zpo, double zno, double vp, double alpha, double maxDv,
        bool staticMode, int maxIter, double tolerance, long long& evaluations) noexcept
    {
        if (!(zoo >= 0.0 && zpo <= zno))
            return {};
        const auto errorAt = [&](double current, double& derivative) noexcept {
            const double vDiff = (voPos - zpo * current) - (voNeg - zno * current);
            const double source = OpAmpEngineLaw::sourceVoltage(
                model, vDiff, vp, alpha, maxDv, staticMode, &derivative);
            return (voOut - zoo * current) - source;
        };
        const double limit = model.outputCurrentLimit;
        const double impedance = model.outputImpedance + zoo;
        const double zDiff = zpo - zno;
        const double zMax = std::max({std::abs(zoo), std::abs(zpo), std::abs(zno)});
        double gain = 0.0;
        // Use the unclamped pole for the load-line seed. A clamp active
        // at zero current can be inactive at the solution.
        const double a = std::clamp(alpha, 0.0, 1.0);
        const double rawGain = staticMode ? model.openLoopGain : a * model.openLoopGain / (1.0 + a);
        const double bias = staticMode ? 0.0 : vp / (1.0 + a);
        double initialCurrent = (voOut - bias - rawGain * (voPos - voNeg)) /
                                (impedance - rawGain * zDiff);
        double seedGain = 0.0;
        double error = errorAt(initialCurrent, seedGain);
        if (seedGain == 0.0)
            initialCurrent += (error - model.outputImpedance * initialCurrent) / impedance;
        double lo = errorAt(limit, gain);
        double hi = errorAt(-limit, gain);
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi)
            return {};
        initialCurrent = std::clamp(initialCurrent, -limit, limit);
        double conductanceSeed = 0.0;
        error = model.classABVoltageError(initialCurrent, conductanceSeed);
        const double loadedErrorSeed = errorAt(initialCurrent, seedGain);
        error -= (error - loadedErrorSeed) /
                 (1.0 + conductanceSeed * (zoo - seedGain * zDiff));
        error = std::clamp(error, lo, hi);
        const double bound = zMax *
            (limit + model.outputJunctionIs + OpAmpModel::kSteeringDiodeIs) /
            (2.0 * kThermalVoltage);
        for (int iteration = 0; iteration < std::min(maxIter, 20); ++iteration) {
            double conductance = 0.0;
            const double current = model.outputCurrent(error, conductance);
            const double loadedError = errorAt(current, gain);
            const double residual = error - loadedError;
            ++evaluations;
            // Each logistic steering fraction has derivative at most 1/4.
            // The sum bounds the original port residual even across clamps.
            const double maxNetworkResidual = bound * std::abs(residual);
            if (maxNetworkResidual <= tolerance)
                return {current, current, maxNetworkResidual, true};
            if (std::abs(residual) <= 1e-8) {
                double unused = 0.0;
                const double modelCurrent = model.outputCurrent(loadedError, unused);
                const double networkResidual = zMax * std::abs(modelCurrent - current);
                ++evaluations;
                if (std::isfinite(modelCurrent) && networkResidual <= tolerance)
                    return {current, modelCurrent, networkResidual, true};
            }
            if (residual > 0.0)
                hi = error;
            else
                lo = error;
            const double slope = 1.0 + conductance * (zoo - gain * zDiff);
            const double next = error - residual / slope;
            error = next > lo && next < hi ? next : 0.5 * (lo + hi);
        }
        return {};
    }

    /// One operational amplifier stage in Harold Black negative feedback closed form.
    static inline void solveOpAmpGroup(
        int g,
        int s,
        bool relaxed,
        int b,
        const BlockView& view,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT vOcData,
        double* LIVEWDF_RESTRICT iiData,
        const OpAmpModel* LIVEWDF_RESTRICT opAmps,
        const double* LIVEWDF_RESTRICT opAmpVp,
        const double* LIVEWDF_RESTRICT opAmpAlpha,
        const double* LIVEWDF_RESTRICT opAmpMaxDv,
        bool staticMode,
        const RootDevice* LIVEWDF_RESTRICT rootDevices,
        int K) noexcept
    {
        const NewtonBlock& block = (*view.blocks)[static_cast<size_t>(b)];
        const RootDevice& dev = rootDevices[g];
        const int o = dev.portBase;
        const size_t ci = dev.compIndex;
        const OpAmpModel& oa = opAmps[ci];

        const double voOut = relaxed ? foldPortAll(view, block, 0, o, b, zData, vOcData, iiData, K)
                                     : foldPort(view, block, 0, o, s, zData, vOcData, iiData, K);
        const double voPos = relaxed ? foldPortAll(view, block, 1, o + 1, b, zData, vOcData, iiData, K)
                                     : foldPort(view, block, 1, o + 1, s, zData, vOcData, iiData, K);
        const double voNeg = relaxed ? foldPortAll(view, block, 2, o + 2, b, zData, vOcData, iiData, K)
                                     : foldPort(view, block, 2, o + 2, s, zData, vOcData, iiData, K);

        const double Zoo = zData[static_cast<size_t>(o) * K + o];
        const double Zpo = zData[static_cast<size_t>(o + 1) * K + o];
        const double Zno = zData[static_cast<size_t>(o + 2) * K + o];

        const double dZ = Zpo - Zno;
        const double dVo = voPos - voNeg;

        double vBias = 0.0;
        double G = oa.openLoopGain;
        if (!staticMode) {
            const double a = std::clamp(opAmpAlpha[ci], 0.0, 1.0);
            const double denom = 1.0 + a;
            vBias = opAmpVp[ci] / denom;
            G = a * oa.openLoopGain / denom;
        }

        const double Ro = oa.outputImpedance;
        const double outWeight = oa.outputVoltageWeight();
        const double vOutTerm = outWeight * voOut;
        const double zOutTerm = outWeight * Zoo;
        double i0 = 0.0;
        if (G > 1e4 && std::abs(dZ) > 1e-12) {
            const double invG = 1.0 / G;
            const double num = -dVo + (vOutTerm - vBias) * invG;
            const double den = -dZ + (Ro + zOutTerm) * invG;
            if (std::abs(den) > 1e-15) {
                i0 = num / den;
            }
        } else {
            const double linDenom = Ro + zOutTerm - G * dZ;
            if (std::abs(linDenom) > 1e-15) {
                i0 = (vOutTerm - vBias - G * dVo) / linDenom;
            }
        }

        double vDiff = dVo - dZ * i0;
        double vSource = vBias + G * vDiff;

        if (oa.hasClassABOutput()) {
            // The output current law is bounded by the source and sink
            // drive currents. Its intersection with the external load line
            // is solved in that bracket; start at the small-signal solution.
            double lo = -oa.outputCurrentLimit;
            double hi = oa.outputCurrentLimit;
            i0 = std::clamp(i0, lo, hi);
            for (int iteration = 0; iteration < 28; ++iteration) {
                double dSource = 0.0;
                vDiff = dVo - dZ * i0;
                vSource = OpAmpEngineLaw::sourceVoltage(
                    oa, vDiff, opAmpVp[ci], opAmpAlpha[ci], opAmpMaxDv[ci], staticMode, &dSource);
                double conductance = 0.0;
                const double current = oa.outputCurrent(voOut - Zoo * i0 - vSource, conductance);
                const double residual = i0 - current;
                if (std::abs(residual) < 1e-12)
                    break;
                if (residual > 0.0)
                    hi = i0;
                else
                    lo = i0;
                const double slope = 1.0 + conductance * (Zoo - dSource * dZ);
                const double next = slope > 0.0 ? i0 - residual / slope : i0;
                i0 = next > lo && next < hi ? next : 0.5 * (lo + hi);
            }
            iiData[o] = i0;
            iiData[o + 1] = 0.0;
            iiData[o + 2] = 0.0;
            return;
        }

        if (!staticMode && std::isfinite(opAmpMaxDv[ci])) {
            const double vPrev = opAmpVp[ci];
            const double maxDv = opAmpMaxDv[ci];
            if (vSource > vPrev + maxDv) {
                vSource = vPrev + maxDv;
                const double denom = Ro + zOutTerm;
                i0 = (std::abs(denom) > 1e-15) ? (vOutTerm - vSource) / denom : 0.0;
            } else if (vSource < vPrev - maxDv) {
                vSource = vPrev - maxDv;
                const double denom = Ro + zOutTerm;
                i0 = (std::abs(denom) > 1e-15) ? (vOutTerm - vSource) / denom : 0.0;
            }
        }

        if (oa.softRails) {
            vSource = oa.applyRails(vSource);
            const double denom = Ro + zOutTerm;
            i0 = (std::abs(denom) > 1e-15) ? (vOutTerm - vSource) / denom : 0.0;
        } else {
            const double hi = oa.railHigh();
            const double lo = oa.railLow();
            if (vSource > hi) {
                vSource = hi;
                const double denom = Ro + zOutTerm;
                i0 = (std::abs(denom) > 1e-15) ? (vOutTerm - hi) / denom : 0.0;
            } else if (vSource < lo) {
                vSource = lo;
                const double denom = Ro + zOutTerm;
                i0 = (std::abs(denom) > 1e-15) ? (vOutTerm - lo) / denom : 0.0;
            }
        }

        iiData[o] = i0;
        iiData[o + 1] = 0.0;
        iiData[o + 2] = 0.0;
    }
};

} // namespace wdf::solver
