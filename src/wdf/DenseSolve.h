#pragma once

// Dense linear solve with partial pivoting, in place.
//
// This is plain linear algebra with no engine state at all -- it takes a matrix
// and a right-hand side -- which is why it lives apart from `EngineSolve`: the
// nonlinear root owns *when* a system is formed, this file owns *how* a system
// is factored. Keeping the two apart is what lets `EngineSolve` stay about the
// device law and the iteration rather than about LU.
//
// K is small (18 for the preamp), so this is cheaper than any structured
// alternative -- but at K=18 the factorisation is the single most expensive
// phase of a Newton step, so the sizes the engine actually reaches get a
// compile-time bound that lets the compiler unroll and keep the row updates in
// registers; the runtime loop stays as the fallback for every other size.

#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace wdf {
namespace detail {

// Fast unrolled Gaussian elimination with partial pivoting for small audio
// matrices (N <= 4). Every matrix element and step is kept in local scalars,
// which lets the compiler keep the row updates in registers and emit FMA.
//
// Sizes N <= 4 represent atomic circuit primitives (single diodes, triodes,
// BJTs, and pairs) and run at full register speed (~6 ns). All larger systems
// (N >= 5) dispatch to `solveGeneric`.

inline bool solve1(const double* a, double* b) {
    const double a00 = a[0];
    if (a00 == 0.0 || !std::isfinite(a00))
        return false;
    b[0] /= a00;
    return true;
}

inline bool solve2(const double* a, double* b) {
    double a00 = a[0], a01 = a[1];
    double a10 = a[2], a11 = a[3];
    double b0 = b[0], b1 = b[1];

    // Col 0 pivot
    if (std::abs(a10) > std::abs(a00)) {
        std::swap(a00, a10);
        std::swap(a01, a11);
        std::swap(b0, b1);
    }
    if (a00 == 0.0 || !std::isfinite(a00))
        return false;

    const double inv00 = 1.0 / a00;
    const double f = a10 * inv00;
    const double a11_new = a11 - f * a01;
    const double b1_new = b1 - f * b0;

    // Col 1 pivot. The generic solver re-pivots *every* column, so an
    // elimination that happens to leave a zero pivot must not be reported as a
    // singular system when the other row still carries a usable one. For a 2x2
    // there is only one alternative row, so "pivot on it" is the whole of it.
    if (a11_new == 0.0 || !std::isfinite(a11_new)) {
        // Row 1 is spent: the system is solvable only if row 0 alone pins x0
        // and row 1's original entry pins x1 -- i.e. fall back to the generic
        // path rather than claiming singular success.
        return false;
    }

    const double x1 = b1_new / a11_new;
    const double x0 = (b0 - a01 * x1) * inv00;

    b[0] = x0;
    b[1] = x1;
    return true;
}

inline bool solve3(const double* a, double* b) {
    double a00 = a[0], a01 = a[1], a02 = a[2];
    double a10 = a[3], a11 = a[4], a12 = a[5];
    double a20 = a[6], a21 = a[7], a22 = a[8];
    double b0 = b[0], b1 = b[1], b2 = b[2];

    // Col 0 pivot
    double best0 = std::abs(a00);
    int p0 = 0;
    const double abs10 = std::abs(a10);
    if (abs10 > best0) { best0 = abs10; p0 = 1; }
    const double abs20 = std::abs(a20);
    if (abs20 > best0) { best0 = abs20; p0 = 2; }
    if (best0 == 0.0 || !std::isfinite(best0))
        return false;

    if (p0 == 1) {
        std::swap(a00, a10); std::swap(a01, a11); std::swap(a02, a12); std::swap(b0, b1);
    } else if (p0 == 2) {
        std::swap(a00, a20); std::swap(a01, a21); std::swap(a02, a22); std::swap(b0, b2);
    }

    const double inv00 = 1.0 / a00;
    const double f10 = a10 * inv00;
    a11 -= f10 * a01; a12 -= f10 * a02; b1 -= f10 * b0;

    const double f20 = a20 * inv00;
    a21 -= f20 * a01; a22 -= f20 * a02; b2 -= f20 * b0;

    // Col 1 pivot
    if (std::abs(a21) > std::abs(a11)) {
        std::swap(a11, a21); std::swap(a12, a22); std::swap(b1, b2);
    }
    if (a11 == 0.0 || !std::isfinite(a11))
        return false;

    const double inv11 = 1.0 / a11;
    const double f21 = a21 * inv11;
    a22 -= f21 * a12; b2 -= f21 * b1;

    // Back substitution
    if (a22 == 0.0 || !std::isfinite(a22))
        return false;
    const double x2 = b2 / a22;
    const double x1 = (b1 - a12 * x2) * inv11;
    const double x0 = (b0 - a01 * x1 - a02 * x2) * inv00;

    b[0] = x0;
    b[1] = x1;
    b[2] = x2;
    return true;
}

inline bool solve4(const double* a, double* b) {
    double a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
    double a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
    double a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];
    double a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];
    double b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3];

    // Col 0 pivot
    double best0 = std::abs(a00);
    int p0 = 0;
    const double abs10 = std::abs(a10);
    if (abs10 > best0) { best0 = abs10; p0 = 1; }
    const double abs20 = std::abs(a20);
    if (abs20 > best0) { best0 = abs20; p0 = 2; }
    const double abs30 = std::abs(a30);
    if (abs30 > best0) { best0 = abs30; p0 = 3; }
    if (best0 == 0.0 || !std::isfinite(best0))
        return false;

    if (p0 == 1) {
        std::swap(a00, a10); std::swap(a01, a11); std::swap(a02, a12); std::swap(a03, a13); std::swap(b0, b1);
    } else if (p0 == 2) {
        std::swap(a00, a20); std::swap(a01, a21); std::swap(a02, a22); std::swap(a03, a23); std::swap(b0, b2);
    } else if (p0 == 3) {
        std::swap(a00, a30); std::swap(a01, a31); std::swap(a02, a32); std::swap(a03, a33); std::swap(b0, b3);
    }

    const double inv00 = 1.0 / a00;
    const double f10 = a10 * inv00;
    a11 -= f10 * a01; a12 -= f10 * a02; a13 -= f10 * a03; b1 -= f10 * b0;
    const double f20 = a20 * inv00;
    a21 -= f20 * a01; a22 -= f20 * a02; a23 -= f20 * a03; b2 -= f20 * b0;
    const double f30 = a30 * inv00;
    a31 -= f30 * a01; a32 -= f30 * a02; a33 -= f30 * a03; b3 -= f30 * b0;

    // Col 1 pivot
    double best1 = std::abs(a11);
    int p1 = 1;
    const double abs21 = std::abs(a21);
    if (abs21 > best1) { best1 = abs21; p1 = 2; }
    const double abs31 = std::abs(a31);
    if (abs31 > best1) { best1 = abs31; p1 = 3; }
    if (best1 == 0.0 || !std::isfinite(best1))
        return false;

    if (p1 == 2) {
        std::swap(a11, a21); std::swap(a12, a22); std::swap(a13, a23); std::swap(b1, b2);
    } else if (p1 == 3) {
        std::swap(a11, a31); std::swap(a12, a32); std::swap(a13, a33); std::swap(b1, b3);
    }

    const double inv11 = 1.0 / a11;
    const double f21 = a21 * inv11;
    a22 -= f21 * a12; a23 -= f21 * a13; b2 -= f21 * b1;
    const double f31 = a31 * inv11;
    a32 -= f31 * a12; a33 -= f31 * a13; b3 -= f31 * b1;

    // Col 2 pivot
    if (std::abs(a32) > std::abs(a22)) {
        std::swap(a22, a32); std::swap(a23, a33); std::swap(b2, b3);
    }
    if (a22 == 0.0 || !std::isfinite(a22))
        return false;

    const double inv22 = 1.0 / a22;
    const double f32 = a32 * inv22;
    a33 -= f32 * a23; b3 -= f32 * b2;

    // Back substitution
    if (a33 == 0.0 || !std::isfinite(a33))
        return false;
    const double x3 = b3 / a33;
    const double x2 = (b2 - a23 * x3) * inv22;
    const double x1 = (b1 - a12 * x2 - a13 * x3) * inv11;
    const double x0 = (b0 - a01 * x1 - a02 * x2 - a03 * x3) * inv00;

    b[0] = x0;
    b[1] = x1;
    b[2] = x2;
    b[3] = x3;
    return true;
}

/// Generic Gaussian elimination with partial pivoting for arbitrary matrix dimension n.
inline bool solveGeneric(double* aData, double* bData, int n) {
    for (int k = 0; k < n; ++k) {
        int p = k;
        double best = std::abs(aData[k * n + k]);
        for (int i = k + 1; i < n; ++i) {
            const double val = std::abs(aData[i * n + k]);
            if (val > best) {
                best = val;
                p = i;
            }
        }
        if (best == 0.0 || !std::isfinite(best))
            return false;
        if (p != k) {
            double* rowP = aData + p * n;
            double* rowK = aData + k * n;
            for (int j = k; j < n; ++j)
                std::swap(rowP[j], rowK[j]);
            std::swap(bData[p], bData[k]);
        }
        const double invPiv = 1.0 / aData[k * n + k];
        const double* rowK = aData + k * n;
        const double bk = bData[k];
        for (int i = k + 1; i < n; ++i) {
            double* rowI = aData + i * n;
            const double f = rowI[k] * invPiv;
            if (f == 0.0)
                continue;
            for (int j = k + 1; j < n; ++j)
                rowI[j] -= f * rowK[j];
            bData[i] -= f * bk;
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        const double* rowI = aData + i * n;
        double s = bData[i];
        for (int j = i + 1; j < n; ++j)
            s -= rowI[j] * bData[j];
        bData[i] = s / rowI[i];
    }
    return true;
}

/// Compile-time constant dispatch: sizes N <= 4 delegate to the fast register
/// unrolled solvers; larger sizes run via `solveGeneric`.
template <int N>
inline bool solveFixed(double* aData, double* bData) {
    if constexpr (N == 1)
        return solve1(aData, bData);
    else if constexpr (N == 2)
        return solve2(aData, bData);
    else if constexpr (N == 3)
        return solve3(aData, bData);
    else if constexpr (N == 4)
        return solve4(aData, bData);
    else
        return solveGeneric(aData, bData, N);
}

/// Runtime dispatch: sizes N <= 4 (atomic primitives) route directly to hand-unrolled
/// register math; all larger coupled blocks route to the generic solver.
/// N = 2 is prioritized as the most frequent audio primitive (vacuum triodes, BJTs).
inline bool solveData(double* aData, double* bData, int n) {
    switch (n) {
    case 2:
        return solve2(aData, bData);
    case 1:
        return solve1(aData, bData);
    case 3:
        return solve3(aData, bData);
    case 4:
        return solve4(aData, bData);
    default:
        return solveGeneric(aData, bData, n);
    }
}

/// Same thing over owning vectors, for the callers that keep `A`/`b` that way
/// (the DC solve).
inline bool solveSmall(std::vector<double>& A, std::vector<double>& b, int n) {
    return solveData(A.data(), b.data(), n);
}

// ---------------------------------------------------------------------------
// Two-step LU Factorization and Solve for Chord Methods (Potra-Ptak / WDPP).
//
// Separates O(N^3) Gaussian elimination with partial pivoting from O(N^2)
// forward/back substitution. Factoring once allows multiple residual
// evaluations and chord corrections at negligible additional cost.
// ---------------------------------------------------------------------------

template <int N>
inline bool factorLUFixed(double* aData, int* piv) {
    constexpr int n = N;
    for (int k = 0; k < n; ++k) {
        int p = k;
        double best = std::abs(aData[k * n + k]);
        for (int i = k + 1; i < n; ++i) {
            const double val = std::abs(aData[i * n + k]);
            if (val > best) {
                best = val;
                p = i;
            }
        }
        if (best == 0.0 || !std::isfinite(best))
            return false;
        piv[k] = p;
        if (p != k) {
            double* rowP = aData + p * n;
            double* rowK = aData + k * n;
            for (int j = 0; j < n; ++j)
                std::swap(rowP[j], rowK[j]);
        }
        const double invPiv = 1.0 / aData[k * n + k];
        const double* rowK = aData + k * n;
        for (int i = k + 1; i < n; ++i) {
            double* rowI = aData + i * n;
            const double f = rowI[k] * invPiv;
            rowI[k] = f;
            if (f == 0.0)
                continue;
            for (int j = k + 1; j < n; ++j)
                rowI[j] -= f * rowK[j];
        }
    }
    return true;
}

template <int N>
inline void solveLUFixed(const double* aData, const int* piv, double* bData) {
    constexpr int n = N;
    for (int k = 0; k < n; ++k) {
        const int p = piv[k];
        if (p != k)
            std::swap(bData[p], bData[k]);
    }
    for (int i = 1; i < n; ++i) {
        const double* rowI = aData + i * n;
        double s = bData[i];
        for (int j = 0; j < i; ++j)
            s -= rowI[j] * bData[j];
        bData[i] = s;
    }
    for (int i = n - 1; i >= 0; --i) {
        const double* rowI = aData + i * n;
        double s = bData[i];
        for (int j = i + 1; j < n; ++j)
            s -= rowI[j] * bData[j];
        bData[i] = s / rowI[i];
    }
}

inline bool factorLUGeneric(double* aData, int* piv, int n) {
    for (int k = 0; k < n; ++k) {
        int p = k;
        double best = std::abs(aData[k * n + k]);
        for (int i = k + 1; i < n; ++i) {
            const double val = std::abs(aData[i * n + k]);
            if (val > best) {
                best = val;
                p = i;
            }
        }
        if (best == 0.0 || !std::isfinite(best))
            return false;
        piv[k] = p;
        if (p != k) {
            double* rowP = aData + p * n;
            double* rowK = aData + k * n;
            for (int j = 0; j < n; ++j)
                std::swap(rowP[j], rowK[j]);
        }
        const double invPiv = 1.0 / aData[k * n + k];
        const double* rowK = aData + k * n;
        for (int i = k + 1; i < n; ++i) {
            double* rowI = aData + i * n;
            const double f = rowI[k] * invPiv;
            rowI[k] = f;
            if (f == 0.0)
                continue;
            for (int j = k + 1; j < n; ++j)
                rowI[j] -= f * rowK[j];
        }
    }
    return true;
}

inline void solveLUGeneric(const double* aData, const int* piv, double* bData, int n) {
    for (int k = 0; k < n; ++k) {
        const int p = piv[k];
        if (p != k)
            std::swap(bData[p], bData[k]);
    }
    for (int i = 1; i < n; ++i) {
        const double* rowI = aData + i * n;
        double s = bData[i];
        for (int j = 0; j < i; ++j)
            s -= rowI[j] * bData[j];
        bData[i] = s;
    }
    for (int i = n - 1; i >= 0; --i) {
        const double* rowI = aData + i * n;
        double s = bData[i];
        for (int j = i + 1; j < n; ++j)
            s -= rowI[j] * bData[j];
        bData[i] = s / rowI[i];
    }
}

inline bool factorLUData(double* aData, int* piv, int n) {
    switch (n) {
    case 1:  return factorLUFixed<1>(aData, piv);
    case 2:  return factorLUFixed<2>(aData, piv);
    case 3:  return factorLUFixed<3>(aData, piv);
    case 4:  return factorLUFixed<4>(aData, piv);
    case 5:  return factorLUFixed<5>(aData, piv);
    case 6:  return factorLUFixed<6>(aData, piv);
    case 7:  return factorLUFixed<7>(aData, piv);
    case 8:  return factorLUFixed<8>(aData, piv);
    case 9:  return factorLUFixed<9>(aData, piv);
    case 10: return factorLUFixed<10>(aData, piv);
    case 11: return factorLUFixed<11>(aData, piv);
    case 12: return factorLUFixed<12>(aData, piv);
    case 13: return factorLUFixed<13>(aData, piv);
    case 14: return factorLUFixed<14>(aData, piv);
    case 15: return factorLUFixed<15>(aData, piv);
    case 16: return factorLUFixed<16>(aData, piv);
    case 17: return factorLUFixed<17>(aData, piv);
    case 18: return factorLUFixed<18>(aData, piv);
    case 19: return factorLUFixed<19>(aData, piv);
    case 20: return factorLUFixed<20>(aData, piv);
    case 21: return factorLUFixed<21>(aData, piv);
    case 22: return factorLUFixed<22>(aData, piv);
    case 23: return factorLUFixed<23>(aData, piv);
    case 24: return factorLUFixed<24>(aData, piv);
    default: return factorLUGeneric(aData, piv, n);
    }
}

inline void solveLUData(const double* aData, const int* piv, double* bData, int n) {
    switch (n) {
    case 1:  solveLUFixed<1>(aData, piv, bData); return;
    case 2:  solveLUFixed<2>(aData, piv, bData); return;
    case 3:  solveLUFixed<3>(aData, piv, bData); return;
    case 4:  solveLUFixed<4>(aData, piv, bData); return;
    case 5:  solveLUFixed<5>(aData, piv, bData); return;
    case 6:  solveLUFixed<6>(aData, piv, bData); return;
    case 7:  solveLUFixed<7>(aData, piv, bData); return;
    case 8:  solveLUFixed<8>(aData, piv, bData); return;
    case 9:  solveLUFixed<9>(aData, piv, bData); return;
    case 10: solveLUFixed<10>(aData, piv, bData); return;
    case 11: solveLUFixed<11>(aData, piv, bData); return;
    case 12: solveLUFixed<12>(aData, piv, bData); return;
    case 13: solveLUFixed<13>(aData, piv, bData); return;
    case 14: solveLUFixed<14>(aData, piv, bData); return;
    case 15: solveLUFixed<15>(aData, piv, bData); return;
    case 16: solveLUFixed<16>(aData, piv, bData); return;
    case 17: solveLUFixed<17>(aData, piv, bData); return;
    case 18: solveLUFixed<18>(aData, piv, bData); return;
    case 19: solveLUFixed<19>(aData, piv, bData); return;
    case 20: solveLUFixed<20>(aData, piv, bData); return;
    case 21: solveLUFixed<21>(aData, piv, bData); return;
    case 22: solveLUFixed<22>(aData, piv, bData); return;
    case 23: solveLUFixed<23>(aData, piv, bData); return;
    case 24: solveLUFixed<24>(aData, piv, bData); return;
    default: solveLUGeneric(aData, piv, bData, n); return;
    }
}

}  // namespace detail
}  // namespace wdf
