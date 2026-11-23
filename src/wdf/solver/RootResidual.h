#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstddef>

#include "wdf/DenseTopology.h"
#include "spice/Compiled.h"
#include "devices/Diode.h"
#include "devices/Triode.h"
#include "devices/Pentode.h"
#include "devices/Bjt.h"
#include "devices/OpAmp.h"

#if defined(_MSC_VER)
#define LIVEWDF_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define LIVEWDF_RESTRICT __restrict__
#else
#define LIVEWDF_RESTRICT
#endif

namespace wdf::solver {

class RootResidual {
public:
    /// One device's currents and Jacobian into the full K-wide arrays.
    static inline void currentsForDevice(
        const RootDevice& dev,
        const Compiled& comp,
        const double* ctrlResValues,
        const Pentode* pentodes,
        const BjtModel* bjts,
        const OpAmpModel* opAmps,
        const double* opAmpVp,
        const double* opAmpAlpha,
        const double* opAmpMaxDv,
        bool staticMode,
        const unsigned char* triodeDomainPin,
        const double* Z,
        int K,
        const double* vData,
        double* iData,
        double* jData,
        long long& deviceEvals) noexcept
    {
        const int np = dev.numPorts;
        const int o = dev.portBase;
        if (jData) {
            for (int rr = 0; rr < np; ++rr) {
                for (int cc = 0; cc < np; ++cc) {
                    jData[static_cast<size_t>(o + rr) * K + o + cc] = 0.0;
                }
            }
        }

        switch (dev.kind) {
            case DeviceKind::Triode: {
                const double vpk = vData[o];
                const double vgint = vData[o + 2];
                const unsigned char pin = triodeDomainPin[static_cast<size_t>(o)];
                TriodeModel::Currents cur;
                if (pin == 1) {
                    const double gShort = 1.0 / (TriodeModel::kShort * Z[static_cast<size_t>(o) * K + o]);
                    cur.ip = vpk * gShort;
                    cur.dvpk = gShort;
                } else if (pin != 2) {
                    cur = comp.tri[dev.compIndex].model.evaluateQuadric(vgint, vpk);
                }
                ++deviceEvals;
                double dig = 0.0;
                const double ig = comp.tri[dev.compIndex].model.gridCurrent(vgint, dig);
                iData[o] = cur.ip;
                iData[o + 1] = 0.0;
                iData[o + 2] = ig;
                if (jData) {
                    jData[o * K + o] = cur.dvpk;
                    jData[o * K + o + 2] = cur.dvgk;
                    jData[(o + 2) * K + o + 2] = dig;
                }
                break;
            }
            case DeviceKind::Diode: {
                const int e = dev.portBase;
                double id = 0.0, dd = 0.0;
                comp.diode[dev.compIndex].second.eval(vData[e], id, dd);
                ++deviceEvals;
                iData[e] = id;
                if (jData)
                    jData[e * K + e] = dd;
                break;
            }
            case DeviceKind::ControlledRes: {
                const int e = dev.portBase;
                const double R = ctrlResValues[dev.compIndex];
                const double G = (R > 1e-12) ? (1.0 / R) : 1e12;
                ++deviceEvals;
                if (comp.otaCurrentLaw[dev.compIndex]) {
                    const double t = std::tanh(vData[e] / OtaControl::pairVolts);
                    iData[e] = G * OtaControl::pairVolts * t;
                    if (jData)
                        jData[e * K + e] = G * (1.0 - t * t);
                } else if (const double crossover = comp.dbx200Crossover[dev.compIndex];
                           crossover != 0.0) {
                    const double v = vData[e];
                    const double split = std::copysign(std::hypot(v, crossover), crossover);
                    iData[e] = 0.5 * G * (v + split);
                    if (jData)
                        jData[e * K + e] = 0.5 * G * (1.0 + v / split);
                } else {
                    iData[e] = vData[e] * G;
                    if (jData)
                        jData[e * K + e] = G;
                }
                break;
            }
            case DeviceKind::Pentode: {
                const int base = dev.portBase;
                const double vpk = vData[base];
                const double vgk = vData[base + 1];
                const double vsk = vData[base + 2];
                const PentodeCurrents cur = pentodes[dev.compIndex].eval(vgk, vsk, vpk);
                ++deviceEvals;
                iData[base] = cur.ip;
                iData[base + 1] = cur.ig;
                iData[base + 2] = cur.is;
                if (jData) {
                    jData[base * K + base] = cur.dip_dvpk;
                    jData[base * K + base + 1] = cur.dip_dvgk;
                    jData[base * K + base + 2] = cur.dip_dvsk;
                    jData[(base + 1) * K + base] = 0.0;
                    jData[(base + 1) * K + base + 1] = cur.dig_dvgk;
                    jData[(base + 1) * K + base + 2] = 0.0;
                    jData[(base + 2) * K + base] = cur.dis_dvpk;
                    jData[(base + 2) * K + base + 1] = cur.dis_dvgk;
                    jData[(base + 2) * K + base + 2] = cur.dis_dvsk;
                }
                break;
            }
            case DeviceKind::Jfet: {
                const auto cur = comp.jfet[dev.compIndex].model.evaluate(vData[o + 1], vData[o]);
                ++deviceEvals;
                iData[o] = cur.valid ? cur.id : 0.0;
                iData[o + 1] = 0.0;
                if (jData) {
                    jData[o * K + o] = cur.valid ? cur.did_ds : 0.0;
                    jData[o * K + o + 1] = cur.valid ? cur.did_gs : 0.0;
                    jData[(o + 1) * K + o] = 0.0;
                    jData[(o + 1) * K + o + 1] = 0.0;
                }
                break;
            }
            case DeviceKind::Mosfet: {
                const auto cur = comp.mosfet[dev.compIndex].model.evaluate(vData[o + 1], vData[o]);
                ++deviceEvals;
                iData[o] = cur.valid ? cur.id : 0.0;
                iData[o + 1] = 0.0;
                if (jData) {
                    jData[o * K + o] = cur.valid ? cur.did_ds : 0.0;
                    jData[o * K + o + 1] = cur.valid ? cur.did_gs : 0.0;
                    jData[(o + 1) * K + o] = 0.0;
                    jData[(o + 1) * K + o + 1] = 0.0;
                }
                break;
            }
            case DeviceKind::Bjt: {
                const double vce = vData[o];
                const double vbe = vData[o + 1];
                const auto cur = comp.bjt[dev.compIndex].validModel
                    ? bjts[dev.compIndex].evaluateValidatedTerminals(vce, vbe)
                    : BjtModel::BjtCurrents{};
                ++deviceEvals;
                iData[o] = cur.valid ? cur.iC : 0.0;
                iData[o + 1] = cur.valid ? cur.iB : 0.0;
                iData[o + 2] = cur.valid ? cur.iE : 0.0;
                if (jData) {
                    jData[o * K + o] = cur.valid ? cur.diC_dVce : 0.0;
                    jData[o * K + o + 1] = cur.valid ? cur.diC_dVbe : 0.0;
                    jData[o * K + o + 2] = 0.0;
                    jData[(o + 1) * K + o] = cur.valid ? cur.diB_dVce : 0.0;
                    jData[(o + 1) * K + o + 1] = cur.valid ? cur.diB_dVbe : 0.0;
                    jData[(o + 1) * K + o + 2] = 0.0;
                    jData[(o + 2) * K + o] = cur.valid ? -cur.diC_dVce - cur.diB_dVce : 0.0;
                    jData[(o + 2) * K + o + 1] = cur.valid ? -cur.diC_dVbe - cur.diB_dVbe : 0.0;
                    jData[(o + 2) * K + o + 2] = 0.0;
                }
                break;
            }
            case DeviceKind::OpAmp: {
                const size_t ci = dev.compIndex;
                const OpAmpModel& oa = opAmps[ci];
                const double vDiff = vData[o + 1] - vData[o + 2];
                double dSourceDvDiff = 0.0;
                const double vSource = OpAmpEngineLaw::sourceVoltage(
                    oa, vDiff, opAmpVp[ci], opAmpAlpha[ci], opAmpMaxDv[ci],
                    staticMode, &dSourceDvDiff);
                double gOut = 0.0;
                ++deviceEvals;
                const double outputVoltage = oa.outputVoltageWeight() * vData[o];
                iData[o] = oa.outputCurrent(outputVoltage - vSource, gOut);
                iData[o + 1] = 0.0;
                iData[o + 2] = 0.0;
                if (jData) {
                    jData[o * K + o] = oa.outputVoltageWeight() * gOut;
                    jData[o * K + o + 1] = -dSourceDvDiff * gOut;
                    jData[o * K + o + 2] = dSourceDvDiff * gOut;
                    jData[(o + 1) * K + o] = 0.0;
                    jData[(o + 1) * K + o + 1] = 0.0;
                    jData[(o + 1) * K + o + 2] = 0.0;
                    jData[(o + 2) * K + o] = 0.0;
                    jData[(o + 2) * K + o + 1] = 0.0;
                    jData[(o + 2) * K + o + 2] = 0.0;
                }
                break;
            }
        }
    }

    /// Residual: F = v - vOc + Z*i
    static inline void residualInto(
        const double* LIVEWDF_RESTRICT vData,
        const double* LIVEWDF_RESTRICT vocData,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT iData,
        double* LIVEWDF_RESTRICT fData,
        const int* LIVEWDF_RESTRICT cols,
        int nRed,
        int K) noexcept
    {
        for (int r = 0; r < K; ++r) {
            double s = vData[r] - vocData[r];
            const double* LIVEWDF_RESTRICT zr = zData + r * K;
            for (int j = 0; j < nRed; ++j) {
                const int c = cols[j];
                s += zr[c] * iData[c];
            }
            fData[r] = s;
        }
    }

    /// Fixed-size unrolled residual computation using contiguous submatrix Zred of shape (K x N).
    /// Keeps gathered iRed in vector registers across all K rows.
    template <int N>
    static inline void residualIntoFixed(
        const double* LIVEWDF_RESTRICT vData,
        const double* LIVEWDF_RESTRICT vocData,
        const double* LIVEWDF_RESTRICT zRed,
        const double* LIVEWDF_RESTRICT iData,
        double* LIVEWDF_RESTRICT fData,
        const int* LIVEWDF_RESTRICT cols,
        int K) noexcept
    {
        if constexpr (N == 1) {
            const double i0 = iData[cols[0]];
            for (int r = 0; r < K; ++r) {
                fData[r] = (vData[r] - vocData[r]) + zRed[r] * i0;
            }
        } else if constexpr (N == 2) {
            const double i0 = iData[cols[0]];
            const double i1 = iData[cols[1]];
            for (int r = 0; r < K; ++r) {
                const double* LIVEWDF_RESTRICT zr = zRed + r * 2;
                fData[r] = (vData[r] - vocData[r]) + (zr[0] * i0 + zr[1] * i1);
            }
        } else if constexpr (N == 3) {
            const double i0 = iData[cols[0]];
            const double i1 = iData[cols[1]];
            const double i2 = iData[cols[2]];
            for (int r = 0; r < K; ++r) {
                const double* LIVEWDF_RESTRICT zr = zRed + r * 3;
                fData[r] = (vData[r] - vocData[r]) + (zr[0] * i0 + zr[1] * i1 + zr[2] * i2);
            }
        } else if constexpr (N == 4) {
            const double i0 = iData[cols[0]];
            const double i1 = iData[cols[1]];
            const double i2 = iData[cols[2]];
            const double i3 = iData[cols[3]];
            for (int r = 0; r < K; ++r) {
                const double* LIVEWDF_RESTRICT zr = zRed + r * 4;
                fData[r] = (vData[r] - vocData[r]) + (zr[0] * i0 + zr[1] * i1 + zr[2] * i2 + zr[3] * i3);
            }
        } else {
            alignas(32) double iRed[N];
            for (int j = 0; j < N; ++j)
                iRed[j] = iData[cols[j]];

            for (int r = 0; r < K; ++r) {
                double s = vData[r] - vocData[r];
                const double* LIVEWDF_RESTRICT zr = zRed + r * N;
                for (int j = 0; j < N; ++j) {
                    s += zr[j] * iRed[j];
                }
                fData[r] = s;
            }
        }
    }

    /// Fast residual using contiguous submatrix Zred of shape (K x nRed).
    static inline void residualIntoFast(
        const double* LIVEWDF_RESTRICT vData,
        const double* LIVEWDF_RESTRICT vocData,
        const double* LIVEWDF_RESTRICT zRed,
        const double* LIVEWDF_RESTRICT iData,
        double* LIVEWDF_RESTRICT fData,
        const int* LIVEWDF_RESTRICT cols,
        int nRed,
        int K) noexcept
    {
        switch (nRed) {
        case 1: return residualIntoFixed<1>(vData, vocData, zRed, iData, fData, cols, K);
        case 2: return residualIntoFixed<2>(vData, vocData, zRed, iData, fData, cols, K);
        case 3: return residualIntoFixed<3>(vData, vocData, zRed, iData, fData, cols, K);
        case 4: return residualIntoFixed<4>(vData, vocData, zRed, iData, fData, cols, K);
        case 5: return residualIntoFixed<5>(vData, vocData, zRed, iData, fData, cols, K);
        case 6: return residualIntoFixed<6>(vData, vocData, zRed, iData, fData, cols, K);
        case 7: return residualIntoFixed<7>(vData, vocData, zRed, iData, fData, cols, K);
        case 8: return residualIntoFixed<8>(vData, vocData, zRed, iData, fData, cols, K);
        case 9: return residualIntoFixed<9>(vData, vocData, zRed, iData, fData, cols, K);
        case 10: return residualIntoFixed<10>(vData, vocData, zRed, iData, fData, cols, K);
        case 11: return residualIntoFixed<11>(vData, vocData, zRed, iData, fData, cols, K);
        case 12: return residualIntoFixed<12>(vData, vocData, zRed, iData, fData, cols, K);
        case 13: return residualIntoFixed<13>(vData, vocData, zRed, iData, fData, cols, K);
        case 14: return residualIntoFixed<14>(vData, vocData, zRed, iData, fData, cols, K);
        case 15: return residualIntoFixed<15>(vData, vocData, zRed, iData, fData, cols, K);
        case 16: return residualIntoFixed<16>(vData, vocData, zRed, iData, fData, cols, K);
        case 17: return residualIntoFixed<17>(vData, vocData, zRed, iData, fData, cols, K);
        case 18: return residualIntoFixed<18>(vData, vocData, zRed, iData, fData, cols, K);
        default:
            alignas(32) double iRed[32];
            const int nGather = std::min(nRed, 32);
            for (int j = 0; j < nGather; ++j)
                iRed[j] = iData[cols[j]];

            for (int r = 0; r < K; ++r) {
                double s = vData[r] - vocData[r];
                const double* LIVEWDF_RESTRICT zr = zRed + r * nRed;
                for (int j = 0; j < nGather; ++j) {
                    s += zr[j] * iRed[j];
                }
                for (int j = nGather; j < nRed; ++j) {
                    s += zr[j] * iData[cols[j]];
                }
                fData[r] = s;
            }
            return;
        }
    }

    /// Jacobian: A = I + Z*J
    static inline void jacobianInto(
        const double* zData,
        const double* jData,
        double* aData,
        const RootDevice* devices,
        size_t devCount,
        int K) noexcept
    {
        for (int r = 0; r < K; ++r) {
            const double* zr = zData + r * K;
            double* ar = aData + r * K;
            for (int c = 0; c < K; ++c)
                ar[c] = (r == c) ? 1.0 : 0.0;
            for (size_t di = 0; di < devCount; ++di) {
                const auto& dev = devices[di];
                switch (dev.kind) {
                case DeviceKind::Triode: {
                    const int o = dev.portBase;
                    ar[o] += zr[o] * jData[o * K + o];
                    ar[o + 2] += zr[o] * jData[o * K + o + 2];
                    if (dev.numActiveRows == 2)
                        ar[o + 2] += zr[o + 2] * jData[(o + 2) * K + o + 2];
                    ar[o + 1] += zr[o + 1] * jData[(o + 1) * K + o + 1];
                    break;
                }
                case DeviceKind::Diode:
                case DeviceKind::ControlledRes: {
                    const int e = dev.portBase;
                    ar[e] += zr[e] * jData[e * K + e];
                    break;
                }
                case DeviceKind::Pentode: {
                    const int o = dev.portBase;
                    ar[o]     += zr[o] * jData[o * K + o]     + zr[o + 1] * jData[(o + 1) * K + o]     + zr[o + 2] * jData[(o + 2) * K + o];
                    ar[o + 1] += zr[o] * jData[o * K + o + 1] + zr[o + 1] * jData[(o + 1) * K + o + 1] + zr[o + 2] * jData[(o + 2) * K + o + 1];
                    ar[o + 2] += zr[o] * jData[o * K + o + 2] + zr[o + 1] * jData[(o + 1) * K + o + 2] + zr[o + 2] * jData[(o + 2) * K + o + 2];
                    break;
                }
                case DeviceKind::Jfet:
                case DeviceKind::Mosfet: {
                    const int o = dev.portBase;
                    ar[o] += zr[o] * jData[o * K + o];
                    ar[o + 1] += zr[o] * jData[o * K + o + 1];
                    break;
                }
                case DeviceKind::Bjt: {
                    // CE and BE both return through the emitter. The residual
                    // only multiplies these two independent current columns.
                    const int o = dev.portBase;
                    for (int c = o; c < o + 2; ++c)
                        ar[c] += zr[o] * jData[o * K + c] + zr[o + 1] * jData[(o + 1) * K + c];
                    break;
                }
                case DeviceKind::OpAmp: {
                    const int o = dev.portBase;
                    for (int q = 0; q < 3; ++q)
                        for (int c = 0; c < 3; ++c)
                            ar[o + c] += zr[o + q] * jData[(o + q) * K + (o + c)];
                    break;
                }
                }
            }
        }
    }

    /// Limit diode and grid ports using pnjlim rule
    static inline int limitDiodePorts(
        double* v,
        const double* vOld,
        const JunctionLimiter* junctionLimiters,
        size_t jlCount,
        const GridJunctionLimiter* gridJunctionLimiters,
        size_t gjlCount,
        long long& gridLimitHits) noexcept
    {
        int hits = 0;
        for (size_t i = 0; i < jlCount; ++i) {
            const auto& jl = junctionLimiters[i];
            bool limited = false;
            if (jl.negate) {
                const double oldMirrored = -vOld[jl.port];
                const double newMirrored = -v[jl.port];
                const double clamped = jl.model->limit(oldMirrored, newMirrored, limited);
                v[jl.port] = -clamped;
            } else {
                v[jl.port] = jl.model->limit(vOld[jl.port], v[jl.port], limited);
            }
            if (limited)
                ++hits;
        }
        for (size_t i = 0; i < gjlCount; ++i) {
            const auto& gl = gridJunctionLimiters[i];
            bool limited = false;
            v[gl.port] = gl.limitGrid(vOld[gl.port], v[gl.port], limited);
            if (limited) {
                ++hits;
                ++gridLimitHits;
            }
        }
        return hits;
    }

    /// Limit BJT junctions
    static inline int limitBjtJunctions(
        double* v,
        const double* vOld,
        const RootLayout& rootLayout,
        const BjtModel* bjts) noexcept
    {
        int hits = 0;
        for (const auto& dev : rootLayout.devices) {
            if (dev.kind != RootKind::Bjt)
                continue;
            const BjtModel& m = bjts[static_cast<size_t>(dev.compIndex)];
            const int o = dev.portBase;
            const double vbe = v[o + 1];
            const double bcOld = vOld[o + 1] - vOld[o];
            const double bcNew = vbe - v[o];
            bool limited = false;
            const double bc = m.pnp
                                  ? -m.diodeBc.limit(-bcOld, -bcNew, limited)
                                  : m.diodeBc.limit(bcOld, bcNew, limited);
            if (limited) {
                v[o] = vbe - bc;
                ++hits;
            }
            const double vbeDev = m.pnp ? -v[o + 1] : v[o + 1];
            const double vbeComp = m.compensateJunction(vbeDev, m.nfVt);
            if (vbeComp != vbeDev) {
                v[o + 1] = m.pnp ? -vbeComp : vbeComp;
                ++hits;
            }
            const double vbcDev = m.pnp ? -(v[o + 1] - v[o]) : (v[o + 1] - v[o]);
            const double vbcComp = m.compensateJunction(vbcDev, m.nrVt);
            if (vbcComp != vbcDev) {
                v[o] = v[o + 1] - (m.pnp ? -vbcComp : vbcComp);
                ++hits;
            }
        }
        return hits;
    }
};

} // namespace wdf::solver
