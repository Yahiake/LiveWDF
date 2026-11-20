#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstddef>

#include "wdf/DenseTopology.h"
#include "wdf/DenseSolve.h"

#if defined(_MSC_VER)
#define LIVEWDF_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define LIVEWDF_RESTRICT __restrict__
#else
#define LIVEWDF_RESTRICT
#endif

namespace wdf::solver {

class RootJacobian {
public:
    /// Assemble the full reduced Newton system: M = I + J*Z restricted to active ports into M,
    /// and -J*F into r.
    static inline void reducedAssemble(
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT jData,
        const double* LIVEWDF_RESTRICT fData,
        const int* LIVEWDF_RESTRICT colOf,
        int nN,
        int K,
        const RootDevice* devices,
        size_t devCount,
        double* LIVEWDF_RESTRICT M,
        double* LIVEWDF_RESTRICT r) noexcept
    {
        for (size_t di = 0; di < devCount; ++di) {
            const auto& dev = devices[di];
            switch (dev.kind) {
            case DeviceKind::Triode: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double dvpk = jData[o * K + o];
                const double dvgk = jData[o * K + o + 2];
                const double* LIVEWDF_RESTRICT zp = zData + o * K;
                const double* LIVEWDF_RESTRICT zgi = zData + (o + 2) * K;
                double* LIVEWDF_RESTRICT rowP = M + rBase * nN;
                for (int j = 0; j < nN; ++j) {
                    const int c = colOf[j];
                    rowP[j] = dvpk * zp[c] + dvgk * zgi[c];
                }
                rowP[rBase] += 1.0;
                r[rBase] = -(dvpk * fData[o] + dvgk * fData[o + 2]);
                if (dev.numActiveRows == 2) {
                    const double dig = jData[(o + 2) * K + o + 2];
                    double* LIVEWDF_RESTRICT rowG = rowP + nN;
                    for (int j = 0; j < nN; ++j)
                        rowG[j] = dig * zgi[colOf[j]];
                    rowG[rBase + 1] += 1.0;
                    r[rBase + 1] = -(dig * fData[o + 2]);
                }
                break;
            }
            case DeviceKind::Diode:
            case DeviceKind::ControlledRes: {
                const int e = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double d = jData[e * K + e];
                const double* LIVEWDF_RESTRICT ze = zData + e * K;
                double* LIVEWDF_RESTRICT row = M + rBase * nN;
                for (int j = 0; j < nN; ++j)
                    row[j] = d * ze[colOf[j]];
                row[rBase] += 1.0;
                r[rBase] = -(d * fData[e]);
                break;
            }
            case DeviceKind::Pentode: {
                const int base = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double dip_dvpk = jData[base * K + base];
                const double dip_dvgk = jData[base * K + base + 1];

                const double dig_dvgk = jData[(base + 1) * K + base + 1];

                const double* LIVEWDF_RESTRICT zp = zData + base * K;
                const double* LIVEWDF_RESTRICT zg = zData + (base + 1) * K;

                double* LIVEWDF_RESTRICT rowP = M + rBase * nN;
                double* LIVEWDF_RESTRICT rowG = M + (rBase + 1) * nN;

                for (int j = 0; j < nN; ++j) {
                    const int c = colOf[j];
                    rowP[j] = dip_dvpk * zp[c] + dip_dvgk * zg[c];
                    rowG[j] = dig_dvgk * zg[c];
                }
                rowP[rBase] += 1.0;
                rowG[rBase + 1] += 1.0;

                r[rBase] = -(dip_dvpk * fData[base] + dip_dvgk * fData[base + 1]);
                r[rBase + 1] = -(dig_dvgk * fData[base + 1]);
                break;
            }
            case DeviceKind::Jfet:
            case DeviceKind::Mosfet: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double jdd = jData[o * K + o];
                const double jdg = jData[o * K + o + 1];
                const double* LIVEWDF_RESTRICT zp = zData + o * K;
                const double* LIVEWDF_RESTRICT zg = zData + (o + 1) * K;
                double* LIVEWDF_RESTRICT rowD = M + rBase * nN;
                double* LIVEWDF_RESTRICT rowG = M + (rBase + 1) * nN;
                for (int j = 0; j < nN; ++j) {
                    const int c = colOf[j];
                    rowD[j] = jdd * zp[c] + jdg * zg[c];
                    rowG[j] = 0.0;
                }
                rowD[rBase] += 1.0;
                rowG[rBase + 1] += 1.0;
                r[rBase] = -(jdd * fData[o] + jdg * fData[o + 1]);
                r[rBase + 1] = 0.0;
                break;
            }
            case DeviceKind::Bjt: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double j00 = jData[o * K + o];
                const double j01 = jData[o * K + o + 1];
                const double j10 = jData[(o + 1) * K + o];
                const double j11 = jData[(o + 1) * K + o + 1];
                const double* LIVEWDF_RESTRICT zp = zData + o * K;
                const double* LIVEWDF_RESTRICT zg = zData + (o + 1) * K;
                double* LIVEWDF_RESTRICT rowC = M + rBase * nN;
                double* LIVEWDF_RESTRICT rowB = M + (rBase + 1) * nN;
                for (int j = 0; j < nN; ++j) {
                    const int c = colOf[j];
                    rowC[j] = j00 * zp[c] + j01 * zg[c];
                    rowB[j] = j10 * zp[c] + j11 * zg[c];
                }
                rowC[rBase] += 1.0;
                rowB[rBase + 1] += 1.0;
                r[rBase] = -(j00 * fData[o] + j01 * fData[o + 1]);
                r[rBase + 1] = -(j10 * fData[o] + j11 * fData[o + 1]);
                break;
            }
            case DeviceKind::OpAmp: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                for (int rr = 0; rr < 3; ++rr) {
                    double* LIVEWDF_RESTRICT row = M + (rBase + rr) * nN;
                    for (int j = 0; j < nN; ++j)
                        row[j] = 0.0;
                    double rf = 0.0;
                    for (int q = 0; q < 3; ++q) {
                        const double jrq = jData[(o + rr) * K + (o + q)];
                        if (jrq == 0.0)
                            continue;
                        const double* LIVEWDF_RESTRICT zq = zData + (o + q) * K;
                        for (int j = 0; j < nN; ++j)
                            row[j] += jrq * zq[colOf[j]];
                        rf += jrq * fData[o + q];
                    }
                    row[rBase + rr] += 1.0;
                    r[rBase + rr] = -rf;
                }
                break;
            }
            }
        }
    }

    /// Overload with active-set reduction: `nAct` active ports (<= nRed_).
    static inline void reducedAssembleActive(
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT jData,
        const double* LIVEWDF_RESTRICT fData,
        const int* LIVEWDF_RESTRICT colOf,
        const int* LIVEWDF_RESTRICT redToActive,
        int nN,
        int K,
        const RootDevice* devices,
        size_t devCount,
        double* LIVEWDF_RESTRICT M,
        double* LIVEWDF_RESTRICT r) noexcept
    {
        for (size_t di = 0; di < devCount; ++di) {
            const auto& dev = devices[di];
            switch (dev.kind) {
            case DeviceKind::Triode: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double dvpk = jData[o * K + o];
                const double dvgk = jData[o * K + o + 2];
                const double* LIVEWDF_RESTRICT zp = zData + o * K;
                const double* LIVEWDF_RESTRICT zgi = zData + (o + 2) * K;
                const int rowPIdx = redToActive[static_cast<size_t>(rBase)];
                const int rowGIdx = dev.numActiveRows == 2 ? redToActive[static_cast<size_t>(rBase + 1)] : -1;
                double* LIVEWDF_RESTRICT rowP = M + rowPIdx * nN;
                for (int j = 0; j < nN; ++j) {
                    const int c = colOf[j];
                    rowP[j] = dvpk * zp[c] + dvgk * zgi[c];
                }
                rowP[rowPIdx] += 1.0;
                r[rowPIdx] = -(dvpk * fData[o] + dvgk * fData[o + 2]);
                if (rowGIdx >= 0) {
                    const double dig = jData[(o + 2) * K + o + 2];
                    double* LIVEWDF_RESTRICT rowG = M + rowGIdx * nN;
                    for (int j = 0; j < nN; ++j)
                        rowG[j] = dig * zgi[colOf[j]];
                    rowG[rowGIdx] += 1.0;
                    r[rowGIdx] = -(dig * fData[o + 2]);
                }
                break;
            }
            case DeviceKind::Diode:
            case DeviceKind::ControlledRes: {
                const int e = dev.portBase;
                const int rBase = dev.activeRowBase;
                const int rowIdx = redToActive[static_cast<size_t>(rBase)];
                const double d = jData[e * K + e];
                const double* LIVEWDF_RESTRICT ze = zData + e * K;
                double* LIVEWDF_RESTRICT row = M + rowIdx * nN;
                for (int j = 0; j < nN; ++j)
                    row[j] = d * ze[colOf[j]];
                row[rowIdx] += 1.0;
                r[rowIdx] = -(d * fData[e]);
                break;
            }
            case DeviceKind::Pentode: {
                const int base = dev.portBase;
                const int rowIdxP = redToActive[static_cast<size_t>(dev.activeRowBase)];
                const int rowIdxG = redToActive[static_cast<size_t>(dev.activeRowBase + 1)];
                const double dip_dvpk = jData[base * K + base];
                const double dip_dvgk = jData[base * K + base + 1];

                const double dig_dvgk = jData[(base + 1) * K + base + 1];

                const double* LIVEWDF_RESTRICT zp = zData + base * K;
                const double* LIVEWDF_RESTRICT zg = zData + (base + 1) * K;

                double* LIVEWDF_RESTRICT rowP = M + rowIdxP * nN;
                double* LIVEWDF_RESTRICT rowG = M + rowIdxG * nN;

                for (int j = 0; j < nN; ++j) {
                    const int c = colOf[j];
                    rowP[j] = dip_dvpk * zp[c] + dip_dvgk * zg[c];
                    rowG[j] = dig_dvgk * zg[c];
                }
                rowP[rowIdxP] += 1.0;
                rowG[rowIdxG] += 1.0;

                r[rowIdxP] = -(dip_dvpk * fData[base] + dip_dvgk * fData[base + 1]);
                r[rowIdxG] = -(dig_dvgk * fData[base + 1]);
                break;
            }
            case DeviceKind::Jfet:
            case DeviceKind::Mosfet: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const int rowDIdx = redToActive[static_cast<size_t>(rBase)];
                const int rowGIdx = redToActive[static_cast<size_t>(rBase + 1)];
                const double jdd = jData[o * K + o];
                const double jdg = jData[o * K + o + 1];
                const double* LIVEWDF_RESTRICT zp = zData + o * K;
                const double* LIVEWDF_RESTRICT zg = zData + (o + 1) * K;
                double* LIVEWDF_RESTRICT rowD = M + rowDIdx * nN;
                double* LIVEWDF_RESTRICT rowG = (rowGIdx >= 0) ? M + rowGIdx * nN : nullptr;
                for (int j = 0; j < nN; ++j) {
                    const int c = colOf[j];
                    rowD[j] = jdd * zp[c] + jdg * zg[c];
                    if (rowG)
                        rowG[j] = 0.0;
                }
                rowD[rowDIdx] += 1.0;
                if (rowG)
                    rowG[rowGIdx] += 1.0;
                r[rowDIdx] = -(jdd * fData[o] + jdg * fData[o + 1]);
                if (rowG)
                    r[rowGIdx] = 0.0;
                break;
            }
            case DeviceKind::Bjt: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const int rowCIdx = redToActive[static_cast<size_t>(rBase)];
                const int rowBIdx = redToActive[static_cast<size_t>(rBase + 1)];
                const double j00 = jData[o * K + o];
                const double j01 = jData[o * K + o + 1];
                const double j10 = jData[(o + 1) * K + o];
                const double j11 = jData[(o + 1) * K + o + 1];
                const double* LIVEWDF_RESTRICT zp = zData + o * K;
                const double* LIVEWDF_RESTRICT zg = zData + (o + 1) * K;
                double* LIVEWDF_RESTRICT rowC = M + rowCIdx * nN;
                double* LIVEWDF_RESTRICT rowB = M + rowBIdx * nN;
                for (int j = 0; j < nN; ++j) {
                    const int c = colOf[j];
                    rowC[j] = j00 * zp[c] + j01 * zg[c];
                    rowB[j] = j10 * zp[c] + j11 * zg[c];
                }
                rowC[rowCIdx] += 1.0;
                rowB[rowBIdx] += 1.0;
                r[rowCIdx] = -(j00 * fData[o] + j01 * fData[o + 1]);
                r[rowBIdx] = -(j10 * fData[o] + j11 * fData[o + 1]);
                break;
            }
            case DeviceKind::OpAmp: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                for (int rr = 0; rr < 3; ++rr) {
                    double* LIVEWDF_RESTRICT row = M + (rBase + rr) * nN;
                    for (int j = 0; j < nN; ++j)
                        row[j] = 0.0;
                    double rf = 0.0;
                    for (int q = 0; q < 3; ++q) {
                        const double jrq = jData[(o + rr) * K + (o + q)];
                        if (jrq == 0.0)
                            continue;
                        const double* LIVEWDF_RESTRICT zq = zData + (o + q) * K;
                        for (int j = 0; j < nN; ++j)
                            row[j] += jrq * zq[colOf[j]];
                        rf += jrq * fData[o + q];
                    }
                    row[rBase + rr] += 1.0;
                    r[rBase + rr] = -rf;
                }
                break;
            }
            }
        }
    }

    /// Solve the system and expand back: dv = -F - Z*u
    static inline bool reducedFinish(
        double* LIVEWDF_RESTRICT M,
        double* LIVEWDF_RESTRICT r,
        int nN,
        const double* LIVEWDF_RESTRICT zData,
        const double* LIVEWDF_RESTRICT fData,
        const int* LIVEWDF_RESTRICT colOf,
        int K,
        double* LIVEWDF_RESTRICT dvData) noexcept
    {
        if (!detail::solveData(M, r, nN))
            return false;

        for (int k = 0; k < K; ++k) {
            const double* LIVEWDF_RESTRICT zk = zData + k * K;
            double s = -fData[k];
            for (int j = 0; j < nN; ++j) {
                s -= zk[colOf[j]] * r[j];
            }
            dvData[k] = s;
        }
        return true;
    }

    /// Fixed-size unrolled assembly of reduced system M = I + J*Zred and r = -J*F.
    template <int N>
    static inline void reducedAssembleFixed(
        const double* LIVEWDF_RESTRICT zRed,
        const double* LIVEWDF_RESTRICT jData,
        const double* LIVEWDF_RESTRICT fData,
        int K,
        const RootDevice* devices,
        size_t devCount,
        double* LIVEWDF_RESTRICT M,
        double* LIVEWDF_RESTRICT r) noexcept
    {
        for (size_t di = 0; di < devCount; ++di) {
            const auto& dev = devices[di];
            switch (dev.kind) {
            case DeviceKind::Triode: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double dvpk = jData[o * K + o];
                const double dvgk = jData[o * K + o + 2];
                const double* LIVEWDF_RESTRICT zp = zRed + o * N;
                const double* LIVEWDF_RESTRICT zgi = zRed + (o + 2) * N;
                double* LIVEWDF_RESTRICT rowP = M + rBase * N;
                for (int j = 0; j < N; ++j) {
                    rowP[j] = dvpk * zp[j] + dvgk * zgi[j];
                }
                rowP[rBase] += 1.0;
                r[rBase] = -(dvpk * fData[o] + dvgk * fData[o + 2]);
                if (dev.numActiveRows == 2) {
                    const double dig = jData[(o + 2) * K + o + 2];
                    double* LIVEWDF_RESTRICT rowG = rowP + N;
                    for (int j = 0; j < N; ++j)
                        rowG[j] = dig * zgi[j];
                    rowG[rBase + 1] += 1.0;
                    r[rBase + 1] = -(dig * fData[o + 2]);
                }
                break;
            }
            case DeviceKind::Diode:
            case DeviceKind::ControlledRes: {
                const int e = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double d = jData[e * K + e];
                const double* LIVEWDF_RESTRICT ze = zRed + e * N;
                double* LIVEWDF_RESTRICT row = M + rBase * N;
                for (int j = 0; j < N; ++j)
                    row[j] = d * ze[j];
                row[rBase] += 1.0;
                r[rBase] = -(d * fData[e]);
                break;
            }
            case DeviceKind::Pentode: {
                const int base = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double dip_dvpk = jData[base * K + base];
                const double dip_dvgk = jData[base * K + base + 1];
                const double dig_dvgk = jData[(base + 1) * K + base + 1];

                const double* LIVEWDF_RESTRICT zp = zRed + base * N;
                const double* LIVEWDF_RESTRICT zg = zRed + (base + 1) * N;

                double* LIVEWDF_RESTRICT rowP = M + rBase * N;
                double* LIVEWDF_RESTRICT rowG = M + (rBase + 1) * N;

                for (int j = 0; j < N; ++j) {
                    rowP[j] = dip_dvpk * zp[j] + dip_dvgk * zg[j];
                    rowG[j] = dig_dvgk * zg[j];
                }
                rowP[rBase] += 1.0;
                rowG[rBase + 1] += 1.0;

                r[rBase] = -(dip_dvpk * fData[base] + dip_dvgk * fData[base + 1]);
                r[rBase + 1] = -(dig_dvgk * fData[base + 1]);
                break;
            }
            case DeviceKind::Jfet:
            case DeviceKind::Mosfet: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double jdd = jData[o * K + o];
                const double jdg = jData[o * K + o + 1];
                const double* LIVEWDF_RESTRICT zp = zRed + o * N;
                const double* LIVEWDF_RESTRICT zg = zRed + (o + 1) * N;
                double* LIVEWDF_RESTRICT rowD = M + rBase * N;
                double* LIVEWDF_RESTRICT rowG = M + (rBase + 1) * N;
                for (int j = 0; j < N; ++j) {
                    rowD[j] = jdd * zp[j] + jdg * zg[j];
                    rowG[j] = 0.0;
                }
                rowD[rBase] += 1.0;
                rowG[rBase + 1] += 1.0;
                r[rBase] = -(jdd * fData[o] + jdg * fData[o + 1]);
                r[rBase + 1] = 0.0;
                break;
            }
            case DeviceKind::Bjt: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double j00 = jData[o * K + o];
                const double j01 = jData[o * K + o + 1];
                const double j10 = jData[(o + 1) * K + o];
                const double j11 = jData[(o + 1) * K + o + 1];
                const double* LIVEWDF_RESTRICT zp = zRed + o * N;
                const double* LIVEWDF_RESTRICT zg = zRed + (o + 1) * N;
                double* LIVEWDF_RESTRICT rowC = M + rBase * N;
                double* LIVEWDF_RESTRICT rowB = M + (rBase + 1) * N;
                for (int j = 0; j < N; ++j) {
                    rowC[j] = j00 * zp[j] + j01 * zg[j];
                    rowB[j] = j10 * zp[j] + j11 * zg[j];
                }
                rowC[rBase] += 1.0;
                rowB[rBase + 1] += 1.0;
                r[rBase] = -(j00 * fData[o] + j01 * fData[o + 1]);
                r[rBase + 1] = -(j10 * fData[o] + j11 * fData[o + 1]);
                break;
            }
            case DeviceKind::OpAmp: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                for (int rr = 0; rr < 3; ++rr) {
                    double* LIVEWDF_RESTRICT row = M + (rBase + rr) * N;
                    for (int j = 0; j < N; ++j)
                        row[j] = 0.0;
                    double rf = 0.0;
                    for (int q = 0; q < 3; ++q) {
                        const double jrq = jData[(o + rr) * K + (o + q)];
                        if (jrq == 0.0)
                            continue;
                        const double* LIVEWDF_RESTRICT zq = zRed + (o + q) * N;
                        for (int j = 0; j < N; ++j)
                            row[j] += jrq * zq[j];
                        rf += jrq * fData[o + q];
                    }
                    row[rBase + rr] += 1.0;
                    r[rBase + rr] = -rf;
                }
                break;
            }
            }
        }
    }

    /// Fast assembly using contiguous submatrix Zred of shape (K x nN),
    /// where column j of Zred corresponds to reduced column j.
    /// Eliminates all indirect colOf[j] memory lookups.
    static inline void reducedAssembleFast(
        const double* LIVEWDF_RESTRICT zRed,
        const double* LIVEWDF_RESTRICT jData,
        const double* LIVEWDF_RESTRICT fData,
        int nN,
        int K,
        const RootDevice* devices,
        size_t devCount,
        double* LIVEWDF_RESTRICT M,
        double* LIVEWDF_RESTRICT r) noexcept
    {
        switch (nN) {
        case 1: return reducedAssembleFixed<1>(zRed, jData, fData, K, devices, devCount, M, r);
        case 2: return reducedAssembleFixed<2>(zRed, jData, fData, K, devices, devCount, M, r);
        case 3: return reducedAssembleFixed<3>(zRed, jData, fData, K, devices, devCount, M, r);
        case 4: return reducedAssembleFixed<4>(zRed, jData, fData, K, devices, devCount, M, r);
        case 5: return reducedAssembleFixed<5>(zRed, jData, fData, K, devices, devCount, M, r);
        case 6: return reducedAssembleFixed<6>(zRed, jData, fData, K, devices, devCount, M, r);
        case 7: return reducedAssembleFixed<7>(zRed, jData, fData, K, devices, devCount, M, r);
        case 8: return reducedAssembleFixed<8>(zRed, jData, fData, K, devices, devCount, M, r);
        case 9: return reducedAssembleFixed<9>(zRed, jData, fData, K, devices, devCount, M, r);
        case 10: return reducedAssembleFixed<10>(zRed, jData, fData, K, devices, devCount, M, r);
        case 11: return reducedAssembleFixed<11>(zRed, jData, fData, K, devices, devCount, M, r);
        case 12: return reducedAssembleFixed<12>(zRed, jData, fData, K, devices, devCount, M, r);
        case 13: return reducedAssembleFixed<13>(zRed, jData, fData, K, devices, devCount, M, r);
        case 14: return reducedAssembleFixed<14>(zRed, jData, fData, K, devices, devCount, M, r);
        case 15: return reducedAssembleFixed<15>(zRed, jData, fData, K, devices, devCount, M, r);
        case 16: return reducedAssembleFixed<16>(zRed, jData, fData, K, devices, devCount, M, r);
        case 17: return reducedAssembleFixed<17>(zRed, jData, fData, K, devices, devCount, M, r);
        case 18: return reducedAssembleFixed<18>(zRed, jData, fData, K, devices, devCount, M, r);
        default:
            break;
        }

        for (size_t di = 0; di < devCount; ++di) {
            const auto& dev = devices[di];
            switch (dev.kind) {
            case DeviceKind::Triode: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double dvpk = jData[o * K + o];
                const double dvgk = jData[o * K + o + 2];
                const double* LIVEWDF_RESTRICT zp = zRed + o * nN;
                const double* LIVEWDF_RESTRICT zgi = zRed + (o + 2) * nN;
                double* LIVEWDF_RESTRICT rowP = M + rBase * nN;
                for (int j = 0; j < nN; ++j) {
                    rowP[j] = dvpk * zp[j] + dvgk * zgi[j];
                }
                rowP[rBase] += 1.0;
                r[rBase] = -(dvpk * fData[o] + dvgk * fData[o + 2]);
                if (dev.numActiveRows == 2) {
                    const double dig = jData[(o + 2) * K + o + 2];
                    double* LIVEWDF_RESTRICT rowG = rowP + nN;
                    for (int j = 0; j < nN; ++j)
                        rowG[j] = dig * zgi[j];
                    rowG[rBase + 1] += 1.0;
                    r[rBase + 1] = -(dig * fData[o + 2]);
                }
                break;
            }
            case DeviceKind::Diode:
            case DeviceKind::ControlledRes: {
                const int e = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double d = jData[e * K + e];
                const double* LIVEWDF_RESTRICT ze = zRed + e * nN;
                double* LIVEWDF_RESTRICT row = M + rBase * nN;
                for (int j = 0; j < nN; ++j)
                    row[j] = d * ze[j];
                row[rBase] += 1.0;
                r[rBase] = -(d * fData[e]);
                break;
            }
            case DeviceKind::Pentode: {
                const int base = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double dip_dvpk = jData[base * K + base];
                const double dip_dvgk = jData[base * K + base + 1];
                const double dig_dvgk = jData[(base + 1) * K + base + 1];

                const double* LIVEWDF_RESTRICT zp = zRed + base * nN;
                const double* LIVEWDF_RESTRICT zg = zRed + (base + 1) * nN;

                double* LIVEWDF_RESTRICT rowP = M + rBase * nN;
                double* LIVEWDF_RESTRICT rowG = M + (rBase + 1) * nN;

                for (int j = 0; j < nN; ++j) {
                    rowP[j] = dip_dvpk * zp[j] + dip_dvgk * zg[j];
                    rowG[j] = dig_dvgk * zg[j];
                }
                rowP[rBase] += 1.0;
                rowG[rBase + 1] += 1.0;

                r[rBase] = -(dip_dvpk * fData[base] + dip_dvgk * fData[base + 1]);
                r[rBase + 1] = -(dig_dvgk * fData[base + 1]);
                break;
            }
            case DeviceKind::Jfet:
            case DeviceKind::Mosfet: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double jdd = jData[o * K + o];
                const double jdg = jData[o * K + o + 1];
                const double* LIVEWDF_RESTRICT zp = zRed + o * nN;
                const double* LIVEWDF_RESTRICT zg = zRed + (o + 1) * nN;
                double* LIVEWDF_RESTRICT rowD = M + rBase * nN;
                double* LIVEWDF_RESTRICT rowG = M + (rBase + 1) * nN;
                for (int j = 0; j < nN; ++j) {
                    rowD[j] = jdd * zp[j] + jdg * zg[j];
                    rowG[j] = 0.0;
                }
                rowD[rBase] += 1.0;
                rowG[rBase + 1] += 1.0;
                r[rBase] = -(jdd * fData[o] + jdg * fData[o + 1]);
                r[rBase + 1] = 0.0;
                break;
            }
            case DeviceKind::Bjt: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                const double j00 = jData[o * K + o];
                const double j01 = jData[o * K + o + 1];
                const double j10 = jData[(o + 1) * K + o];
                const double j11 = jData[(o + 1) * K + o + 1];
                const double* LIVEWDF_RESTRICT zp = zRed + o * nN;
                const double* LIVEWDF_RESTRICT zg = zRed + (o + 1) * nN;
                double* LIVEWDF_RESTRICT rowC = M + rBase * nN;
                double* LIVEWDF_RESTRICT rowB = M + (rBase + 1) * nN;
                for (int j = 0; j < nN; ++j) {
                    rowC[j] = j00 * zp[j] + j01 * zg[j];
                    rowB[j] = j10 * zp[j] + j11 * zg[j];
                }
                rowC[rBase] += 1.0;
                rowB[rBase + 1] += 1.0;
                r[rBase] = -(j00 * fData[o] + j01 * fData[o + 1]);
                r[rBase + 1] = -(j10 * fData[o] + j11 * fData[o + 1]);
                break;
            }
            case DeviceKind::OpAmp: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                for (int rr = 0; rr < 3; ++rr) {
                    double* LIVEWDF_RESTRICT row = M + (rBase + rr) * nN;
                    for (int j = 0; j < nN; ++j)
                        row[j] = 0.0;
                    double rf = 0.0;
                    for (int q = 0; q < 3; ++q) {
                        const double jrq = jData[(o + rr) * K + (o + q)];
                        if (jrq == 0.0)
                            continue;
                        const double* LIVEWDF_RESTRICT zq = zRed + (o + q) * nN;
                        for (int j = 0; j < nN; ++j)
                            row[j] += jrq * zq[j];
                        rf += jrq * fData[o + q];
                    }
                    row[rBase + rr] += 1.0;
                    r[rBase + rr] = -rf;
                }
                break;
            }
            }
        }
    }

    /// Fast back-substitution using unrolled fixed-size kernel.
    template <int N>
    static inline void backsubFixed(
        const double* LIVEWDF_RESTRICT r,
        const double* LIVEWDF_RESTRICT zRed,
        const double* LIVEWDF_RESTRICT fData,
        int K,
        double* LIVEWDF_RESTRICT dvData) noexcept
    {
        if constexpr (N == 1) {
            const double r0 = r[0];
            for (int k = 0; k < K; ++k)
                dvData[k] = -fData[k] - zRed[k] * r0;
        } else if constexpr (N == 2) {
            const double r0 = r[0], r1 = r[1];
            for (int k = 0; k < K; ++k) {
                const double* LIVEWDF_RESTRICT zk = zRed + k * 2;
                dvData[k] = -fData[k] - (zk[0] * r0 + zk[1] * r1);
            }
        } else if constexpr (N == 3) {
            const double r0 = r[0], r1 = r[1], r2 = r[2];
            for (int k = 0; k < K; ++k) {
                const double* LIVEWDF_RESTRICT zk = zRed + k * 3;
                dvData[k] = -fData[k] - (zk[0] * r0 + zk[1] * r1 + zk[2] * r2);
            }
        } else if constexpr (N == 4) {
            const double r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3];
            for (int k = 0; k < K; ++k) {
                const double* LIVEWDF_RESTRICT zk = zRed + k * 4;
                dvData[k] = -fData[k] - (zk[0] * r0 + zk[1] * r1 + zk[2] * r2 + zk[3] * r3);
            }
        } else {
            for (int k = 0; k < K; ++k) {
                const double* LIVEWDF_RESTRICT zk = zRed + k * N;
                double s = -fData[k];
                for (int j = 0; j < N; ++j)
                    s -= zk[j] * r[j];
                dvData[k] = s;
            }
        }
    }

    /// Fast back-substitution dispatcher across sizes.
    static inline void backsubFast(
        const double* LIVEWDF_RESTRICT r,
        const double* LIVEWDF_RESTRICT zRed,
        const double* LIVEWDF_RESTRICT fData,
        int nN,
        int K,
        double* LIVEWDF_RESTRICT dvData) noexcept
    {
        switch (nN) {
        case 1: backsubFixed<1>(r, zRed, fData, K, dvData); return;
        case 2: backsubFixed<2>(r, zRed, fData, K, dvData); return;
        case 3: backsubFixed<3>(r, zRed, fData, K, dvData); return;
        case 4: backsubFixed<4>(r, zRed, fData, K, dvData); return;
        case 5: backsubFixed<5>(r, zRed, fData, K, dvData); return;
        case 6: backsubFixed<6>(r, zRed, fData, K, dvData); return;
        case 7: backsubFixed<7>(r, zRed, fData, K, dvData); return;
        case 8: backsubFixed<8>(r, zRed, fData, K, dvData); return;
        case 9: backsubFixed<9>(r, zRed, fData, K, dvData); return;
        case 10: backsubFixed<10>(r, zRed, fData, K, dvData); return;
        case 11: backsubFixed<11>(r, zRed, fData, K, dvData); return;
        case 12: backsubFixed<12>(r, zRed, fData, K, dvData); return;
        case 13: backsubFixed<13>(r, zRed, fData, K, dvData); return;
        case 14: backsubFixed<14>(r, zRed, fData, K, dvData); return;
        case 15: backsubFixed<15>(r, zRed, fData, K, dvData); return;
        case 16: backsubFixed<16>(r, zRed, fData, K, dvData); return;
        case 17: backsubFixed<17>(r, zRed, fData, K, dvData); return;
        case 18: backsubFixed<18>(r, zRed, fData, K, dvData); return;
        default:
            for (int k = 0; k < K; ++k) {
                const double* LIVEWDF_RESTRICT zk = zRed + k * nN;
                double s = -fData[k];
                for (int j = 0; j < nN; ++j)
                    s -= zk[j] * r[j];
                dvData[k] = s;
            }
            return;
        }
    }

    /// Efficiently computes r = -J * f restricted to active rows, without touching M.
    static inline void reducedRhs(
        const double* LIVEWDF_RESTRICT jData,
        const double* LIVEWDF_RESTRICT fData,
        int K,
        const RootDevice* devices,
        size_t devCount,
        double* LIVEWDF_RESTRICT r) noexcept
    {
        for (size_t di = 0; di < devCount; ++di) {
            const auto& dev = devices[di];
            switch (dev.kind) {
            case DeviceKind::Triode: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                r[rBase] = -(jData[o * K + o] * fData[o] + jData[o * K + o + 2] * fData[o + 2]);
                if (dev.numActiveRows == 2) {
                    r[rBase + 1] = -(jData[(o + 2) * K + o + 2] * fData[o + 2]);
                }
                break;
            }
            case DeviceKind::Diode:
            case DeviceKind::ControlledRes: {
                const int e = dev.portBase;
                const int rBase = dev.activeRowBase;
                r[rBase] = -(jData[e * K + e] * fData[e]);
                break;
            }
            case DeviceKind::Pentode: {
                const int base = dev.portBase;
                const int rBase = dev.activeRowBase;
                r[rBase] = -(jData[base * K + base] * fData[base] + jData[base * K + base + 1] * fData[base + 1]);
                r[rBase + 1] = -(jData[(base + 1) * K + base + 1] * fData[base + 1]);
                break;
            }
            case DeviceKind::Jfet:
            case DeviceKind::Mosfet: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                r[rBase] = -(jData[o * K + o] * fData[o] + jData[o * K + o + 1] * fData[o + 1]);
                break;
            }
            case DeviceKind::Bjt: {
                const int o = dev.portBase;
                const int rBase = dev.activeRowBase;
                r[rBase] = -(jData[o * K + o] * fData[o] + jData[o * K + o + 1] * fData[o + 1]);
                r[rBase + 1] = -(jData[(o + 1) * K + o] * fData[o] + jData[(o + 1) * K + o + 1] * fData[o + 1]);
                break;
            }
            case DeviceKind::OpAmp:
                break;
            }
        }
    }

    /// Fixed-size unrolled solve and back-substitution using contiguous Zred submatrix.
    template <int N>
    static inline bool reducedFinishFixed(
        double* LIVEWDF_RESTRICT M,
        double* LIVEWDF_RESTRICT r,
        const double* LIVEWDF_RESTRICT zRed,
        const double* LIVEWDF_RESTRICT fData,
        int K,
        double* LIVEWDF_RESTRICT dvData) noexcept
    {
        if constexpr (N == 1) {
            if (!detail::solve1(M, r)) return false;
        } else if constexpr (N == 2) {
            if (!detail::solve2(M, r)) return false;
        } else if constexpr (N == 3) {
            if (!detail::solve3(M, r)) return false;
        } else if constexpr (N == 4) {
            if (!detail::solve4(M, r)) return false;
        } else {
            if (!detail::solveFixed<N>(M, r)) return false;
        }
        backsubFixed<N>(r, zRed, fData, K, dvData);
        return true;
    }

    /// Fast back-substitution using contiguous Zred submatrix.
    static inline bool reducedFinishFast(
        double* LIVEWDF_RESTRICT M,
        double* LIVEWDF_RESTRICT r,
        int nN,
        const double* LIVEWDF_RESTRICT zRed,
        const double* LIVEWDF_RESTRICT fData,
        int K,
        double* LIVEWDF_RESTRICT dvData) noexcept
    {
        switch (nN) {
        case 1: return reducedFinishFixed<1>(M, r, zRed, fData, K, dvData);
        case 2: return reducedFinishFixed<2>(M, r, zRed, fData, K, dvData);
        case 3: return reducedFinishFixed<3>(M, r, zRed, fData, K, dvData);
        case 4: return reducedFinishFixed<4>(M, r, zRed, fData, K, dvData);
        case 5: return reducedFinishFixed<5>(M, r, zRed, fData, K, dvData);
        case 6: return reducedFinishFixed<6>(M, r, zRed, fData, K, dvData);
        case 7: return reducedFinishFixed<7>(M, r, zRed, fData, K, dvData);
        case 8: return reducedFinishFixed<8>(M, r, zRed, fData, K, dvData);
        case 9: return reducedFinishFixed<9>(M, r, zRed, fData, K, dvData);
        case 10: return reducedFinishFixed<10>(M, r, zRed, fData, K, dvData);
        case 11: return reducedFinishFixed<11>(M, r, zRed, fData, K, dvData);
        case 12: return reducedFinishFixed<12>(M, r, zRed, fData, K, dvData);
        case 13: return reducedFinishFixed<13>(M, r, zRed, fData, K, dvData);
        case 14: return reducedFinishFixed<14>(M, r, zRed, fData, K, dvData);
        case 15: return reducedFinishFixed<15>(M, r, zRed, fData, K, dvData);
        case 16: return reducedFinishFixed<16>(M, r, zRed, fData, K, dvData);
        case 17: return reducedFinishFixed<17>(M, r, zRed, fData, K, dvData);
        case 18: return reducedFinishFixed<18>(M, r, zRed, fData, K, dvData);
        default:
            if (!detail::solveData(M, r, nN))
                return false;

            for (int k = 0; k < K; ++k) {
                const double* LIVEWDF_RESTRICT zk = zRed + k * nN;
                double s = -fData[k];
                for (int j = 0; j < nN; ++j) {
                    s -= zk[j] * r[j];
                }
                dvData[k] = s;
            }
            return true;
        }
    }
};

} // namespace wdf::solver
