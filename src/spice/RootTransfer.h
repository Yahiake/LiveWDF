#pragma once

// R-type scattering: element ports, root ports, and the transfer solve.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "spice/Dense.h"

namespace wdf {

// --------------------------------------------------------------------------- //
// R-type scattering (ported from ampsim2 mna/PortNetwork.h via proto/net.py)
// --------------------------------------------------------------------------- //

/// An element port reduced to a conductance: a Thevenin source behind 1/g.
struct ElemPort {
    int a = -1, b = -1;
    double g = 0.0;
};

/// A directed pair of element ports coupled by mutual conductance `g`.
struct MutualPortPair {
    int pi = -1;
    int pj = -1;
    double g = 0.0;
};

/// Transconductance from controlling nodes (ia, ib) to output nodes (oa, ob) for a VCVS.
struct VCVSTransconductance {
    int oa = -1, ob = -1;
    int ia = -1, ib = -1;
    double gm = 0.0;
};

/// Resize a dense matrix to `rows x cols` and zero it, reusing the storage when
/// the shape already matches. Setup-only.
inline void resetDense(Dense& v, int rows, int cols) {
    if (static_cast<int>(v.size()) != rows)
        v.assign(static_cast<size_t>(rows), std::vector<double>(static_cast<size_t>(cols), 0.0));
    for (auto& row : v) {
        if (static_cast<int>(row.size()) != cols)
            row.assign(static_cast<size_t>(cols), 0.0);
        else
            std::fill(row.begin(), row.end(), 0.0);
    }
}

/// As `rootTransfer`, but writing into caller-owned buffers so a component-value
/// change can re-derive the transfer without allocating (see `Engine::rebuild`).
inline bool rootTransferInto(const std::vector<ElemPort>& elements,
                             const std::vector<std::array<int, 2>>& roots, int N, Dense& Y,
                             Dense& rhs, Dense& Xe, Dense& Xr,
                             const std::vector<MutualPortPair>& mutual = {},
                             const std::vector<VCVSTransconductance>& vcvs = {},
                             double* backwardError = nullptr, FactoredTransfer* factors = nullptr,
                             const std::vector<int>& liveCols = {}) {
    const int P = static_cast<int>(elements.size());
    const int K = static_cast<int>(roots.size());

    resetDense(Y, N, N);
    resetDense(rhs, N, P + K);
    for (const auto& e : elements) {
        if (e.a >= 0)
            Y[e.a][e.a] += e.g;
        if (e.b >= 0)
            Y[e.b][e.b] += e.g;
        if (e.a >= 0 && e.b >= 0) {
            Y[e.a][e.b] -= e.g;
            Y[e.b][e.a] -= e.g;
        }
    }
    for (const auto& m : mutual) {
        const int ai = elements[static_cast<size_t>(m.pi)].a;
        const int bi = elements[static_cast<size_t>(m.pi)].b;
        const int aj = elements[static_cast<size_t>(m.pj)].a;
        const int bj = elements[static_cast<size_t>(m.pj)].b;
        if (ai >= 0 && aj >= 0) Y[ai][aj] += m.g;
        if (ai >= 0 && bj >= 0) Y[ai][bj] -= m.g;
        if (bi >= 0 && aj >= 0) Y[bi][aj] -= m.g;
        if (bi >= 0 && bj >= 0) Y[bi][bj] += m.g;

        if (ai >= 0) rhs[ai][m.pj] += m.g;
        if (bi >= 0) rhs[bi][m.pj] -= m.g;
    }
    for (const auto& v : vcvs) {
        if (v.oa >= 0) {
            if (v.ia >= 0) Y[v.oa][v.ia] -= v.gm;
            if (v.ib >= 0) Y[v.oa][v.ib] += v.gm;
        }
        if (v.ob >= 0) {
            if (v.ia >= 0) Y[v.ob][v.ia] += v.gm;
            if (v.ib >= 0) Y[v.ob][v.ib] -= v.gm;
        }
    }
    auto inject = [&](int col, int a, int b, double value) {
        if (a >= 0)
            rhs[a][col] += value;
        if (b >= 0)
            rhs[b][col] -= value;
    };
    for (int j = 0; j < P; ++j)
        inject(j, elements[j].a, elements[j].b, elements[j].g);
    for (int r = 0; r < K; ++r)
        inject(P + r, roots[r][0], roots[r][1], 1.0);

    // Debug-only network dump (LIVEWDF_DEBUG_NET): the assembled conductance
    // matrix plus the port tables that built it, so a singular transfer can be
    // traced to the dependent rows and the elements that produced them.
    if (std::getenv("LIVEWDF_DEBUG_NET")) {
        std::printf("NETDUMP N=%d P=%d K=%d\n", N, P, K);
        for (int j = 0; j < P; ++j)
            std::printf("NETDUMP elem %2d a=%2d b=%2d g=%.9g\n", j, elements[j].a, elements[j].b, elements[j].g);
        for (const auto& m : mutual)
            std::printf("NETDUMP mutual pi=%d pj=%d g=%.9g\n", m.pi, m.pj, m.g);
        for (const auto& v : vcvs)
            std::printf("NETDUMP vcvs oa=%d ob=%d ia=%d ib=%d gm=%.9g\n", v.oa, v.ob, v.ia, v.ib, v.gm);
        for (int r = 0; r < K; ++r)
            std::printf("NETDUMP root %d a=%d b=%d\n", r, roots[r][0], roots[r][1]);
        for (int i = 0; i < N; ++i) {
            std::printf("NETDUMP yrow %2d:", i);
            for (int j = 0; j < N; ++j)
                std::printf(" %.9g", Y[i][j]);
            std::printf("\n");
        }
    }

    if (factors)
        factors->orderSystem(Y, rhs);
    Dense yOriginal, rhsOriginal;
    if (backwardError) {
        yOriginal = Y;      // setup-only diagnostic: keep the unfactored system
        rhsOriginal = rhs;
    }
    if (factors)
        factors->prepare(rhs, liveCols);
    if (!solveInPlace(Y, rhs, factors))
        return false;
    if (factors)
        factors->finish(Y);
    if (backwardError)
        *backwardError = maxBackwardError(yOriginal, rhsOriginal, rhs);

    resetDense(Xe, N, P);
    resetDense(Xr, N, K);
    for (int n = 0; n < N; ++n) {
        const int node = factors ? factors->order[n] : n;
        for (int j = 0; j < P; ++j)
            Xe[node][j] = rhs[n][j];
        for (int r = 0; r < K; ++r)
            Xr[node][r] = rhs[n][P + r];
    }
    return true;
}

/// Transfer matrices of a network with P element ports and K root ports over the
/// same N nodes. Node voltages are `v = Xe*b - Xr*i`, with `b` the element
/// reflected waves and `i` the root currents drawn into each root's node+.
/// Capacitors enter with G = 0 (open, for a DC solve) or G = 2C/T for a step.
inline bool rootTransfer(const std::vector<ElemPort>& elements,
                         const std::vector<std::array<int, 2>>& roots, int N, Dense& Xe, Dense& Xr,
                         const std::vector<MutualPortPair>& mutual = {},
                         const std::vector<VCVSTransconductance>& vcvs = {},
                         double* backwardError = nullptr) {
    Dense Y, rhs;
    return rootTransferInto(elements, roots, N, Y, rhs, Xe, Xr, mutual, vcvs, backwardError);
}

} // namespace wdf
