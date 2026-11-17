#pragma once

// Which root-port groups can be solved independently of one another.
//
// Why this exists: the engine puts every nonlinear device behind a multiport
// root, so two devices cost a joint Newton where two per-device closed forms
// would cost two sqrts (or two omegas). Whether the root *can* be split is a
// property of the compiled transfer, not a matter of taste, and it is the
// premise the group solve rests on -- so it lives here, where both the instrument
// (which prints it) and the test (which asserts it) can read it, instead of in
// either one of them.
//
// The residual for root port `r` is
//
//     F[r] = v[r] - vOc[r] + sum_c Z[r][c] * i_c
//
// and a *group* is one device: a triode `t` owns ports `3t + {0,1,2}` (plate,
// external grid, internal grid) and a diode `q` owns port `3*nTri + q`. So "can
// group g be solved without group h?" is exactly "does any of group g's equations
// mention a current belonging to group h", i.e. is `(Z*J)[r][c]` zero for every
// row `r` of g and every column `c` of h.
//
// `Z` alone is *not* the answer, and that is why both blocks are reported: an
// interstage coupling capacitor makes `Z` nonzero in **both** directions (it is
// reciprocal), yet a triode's grid ports carry no current of their own, so `J`'s
// grid entries are zero and the backwards coupling is annihilated. Groups
// of a cap-coupled cascade are then block-triangular in device order and solving
// them in sequence is *exact* -- not the handoff approximation a hardcoded
// per-stage design makes.
//
// `J` is block-diagonal (each device's Jacobian is local to it) and its diagonal
// is strictly positive on every port that can drive current, so the product
// collapses to a `Z`-only lookup against the single *live* column of `h`
// (`base(h)`): the plate for a triode, the port itself for a diode. Every other
// port of a device is passive -- it carries no current, so its column cannot
// make a residual depend on another group.
//
// The coupling is two-way when the shared node carries current (a long-tailed
// pair's common cathode resistor, or two diodes spanning the same node pair), so
// the probe is evaluated over the fixture's own drive rather than only at the
// operating point. It is untimed; nothing here is on an audio path.

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "spice/Net.h"
#include "support/CircuitCases.h"
#include "wdf/Engine.h"

namespace lwdf_test {

struct RootGroupCoupling {
    int nTri = 0;  ///< triodes; group `t` owns root ports `3t .. 3t+2`
    int nD = 0;    ///< diodes;  group `nTri + q` owns root port `3*nTri + q`
    int nCtrl = 0; ///< controlled resistors; group `nTri + nD + c` owns root port `3*nTri + nD + c`
    int nPent = 0; ///< pentodes; group `nTri + nD + nCtrl + pt` owns root ports `... + 3*pt + {0,1,2}`
    int nJfet = 0; ///< JFETs; group after pentodes owns two ports (drain-source, gate-source)

    int nGroups() const { return nTri + nD + nCtrl + nPent + nJfet; }

    /// First root port of a group, and its only live column -- the same identity
    /// the engine's `EngineSolve::groupBase` uses.
    int base(int g) const {
        if (g < nTri) return 3 * g;
        if (g < nTri + nD) return 3 * nTri + (g - nTri);
        if (g < nTri + nD + nCtrl) return 3 * nTri + nD + (g - nTri - nD);
        if (g < nTri + nD + nCtrl + nPent)
            return 3 * nTri + nD + nCtrl + 3 * (g - nTri - nD - nCtrl);
        return 3 * nTri + nD + nCtrl + 3 * nPent +
               2 * (g - nTri - nD - nCtrl - nPent);
    }

    /// Root ports a group owns: three for a triode, one for a diode, three for a
    /// pentode, two for a JFET.
    int ports(int g) const {
        if (g < nTri) return 3;
        if (g < nTri + nD + nCtrl) return 1;
        if (g < nTri + nD + nCtrl + nPent) return 3;
        return 2;
    }

    /// `nGroups * nGroups`, block `(g, h)`: max |Z[r][c]| over rows of group g and
    /// columns of h. The raw impedance coupling.
    std::vector<double> z;
    /// The same blocks of |(Z*J)[r][c]|. This is the one that decides.
    std::vector<double> zj;
    /// `max` over rows of group g of `|Z[r][base(h)]|` -- `pk_h` for a triode,
    /// the diode's own port for a diode. This is what a compile-time eligibility
    /// test can read, because `J` contributes only a strictly positive diagonal
    /// factor there.
    std::vector<double> zLive;

    /// The compile-time prediction of `block(zj, g, h) != 0`. Every group has
    /// exactly one live column -- its plate/drain/anode port -- so the
    /// prediction is that column alone.
    bool predictedCoupled(int g, int h) const {
        return block(zLive, g, h) != 0.0;
    }

    double block(const std::vector<double>& m, int g, int h) const {
        const int n = nGroups();
        return m[static_cast<size_t>(g) * static_cast<size_t>(n) + static_cast<size_t>(h)];
    }
    /// Largest block strictly above the diagonal -- nonzero means the groups in
    /// this order depend on each other and need iteration between them.
    double maxAboveDiagonal() const {
        double m = 0.0;
        for (int g = 0; g < nGroups(); ++g)
            for (int h = g + 1; h < nGroups(); ++h)
                m = std::max(m, block(zj, g, h));
        return m;
    }
    /// Smallest diagonal block -- the scale `maxAboveDiagonal` is read against.
    /// A "small" off-diagonal is only meaningful next to a healthy diagonal.
    double minDiagonal() const {
        double m = 0.0;
        for (int g = 0; g < nGroups(); ++g)
            m = (g == 0) ? block(zj, g, g) : std::min(m, block(zj, g, g));
        return m;
    }
};

/// Group the root ports by device and measure the coupling between every pair,
/// driving the circuit with the fixture's own input (untimed).
inline RootGroupCoupling rootGroupCoupling(const wdf::Circuit& c, const CircuitCase& tc, double fs,
                                           int nSamples = 4000) {
    RootGroupCoupling out;
    wdf::Engine e(c, fs);
    const int K = e.rootCount();
    out.nTri = static_cast<int>(e.compiled().tri.size());
    out.nD = static_cast<int>(e.compiled().diode.size());
    out.nCtrl = static_cast<int>(e.compiled().ctrlRes.size());
    out.nPent = static_cast<int>(e.compiled().pent.size());
    out.nJfet = static_cast<int>(e.compiled().jfet.size());
    // A self-check on this file's own reading of the port layout: `K` is the
    // triodes' three ports plus the diodes' one each, plus one per controlled resistor,
    // plus three per pentode, plus two per JFET.
    if (K != 3 * out.nTri + out.nD + out.nCtrl + 3 * out.nPent + 2 * out.nJfet)
        throw std::runtime_error("RootGroups: root port layout mismatch");

    const int nG = out.nGroups();
    const size_t n2 = static_cast<size_t>(nG) * static_cast<size_t>(nG);
    out.z.assign(n2, 0.0);
    out.zj.assign(n2, 0.0);
    out.zLive.assign(n2, 0.0);
    if (nG < 2)
        return out;

    const std::vector<double>& Z = e.rootImpedance();

    // The compile-time part: `Z` is fixed until a value moves, so the single
    // live column `J` leaves alive is read off it once, not per sample.
    const int firstJfetGroup = out.nTri + out.nD + out.nCtrl + out.nPent;
    for (int g = 0; g < nG; ++g) {
        for (int h = 0; h < nG; ++h) {
            double ml = 0.0;
            const int rowBegin = out.base(g);
            const int rowEnd = rowBegin + out.ports(g);
            // A JFET's gate row carries no current; only its drain-source row
            // can make a residual depend on another group's live column.
            const int liveRowEnd = (g >= firstJfetGroup) ? rowBegin + 1 : rowEnd;
            for (int r = rowBegin; r < liveRowEnd; ++r)
                ml = std::max(ml, std::fabs(Z[static_cast<size_t>(r) * K + out.base(h)]));
            const size_t at = static_cast<size_t>(g) * static_cast<size_t>(nG) +
                              static_cast<size_t>(h);
            out.zLive[at] = ml;
        }
    }
    const std::vector<std::array<int, 2>>& ports = e.rootPorts();
    std::vector<double> v(static_cast<size_t>(K), 0.0);
    std::vector<double> i, J;
    std::vector<double> ZJ(static_cast<size_t>(K) * static_cast<size_t>(K), 0.0);

    for (int n = 0; n < nSamples; ++n) {
        const std::vector<double>& y = e.step(sampleSources(c, tc, n));
        for (int r = 0; r < K; ++r) {
            const std::array<int, 2>& p = ports[static_cast<size_t>(r)];
            v[static_cast<size_t>(r)] = (p[0] >= 0 ? y[static_cast<size_t>(p[0])] : 0.0) -
                                        (p[1] >= 0 ? y[static_cast<size_t>(p[1])] : 0.0);
        }
        e.currents(v, i, J);
        for (int r = 0; r < K; ++r) {
            for (int col = 0; col < K; ++col) {
                double s = 0.0;
                for (int m = 0; m < K; ++m)
                    s += Z[static_cast<size_t>(r) * K + m] * J[static_cast<size_t>(m) * K + col];
                ZJ[static_cast<size_t>(r) * K + col] = s;
            }
        }
        for (int g = 0; g < nG; ++g) {
            for (int h = 0; h < nG; ++h) {
                double mz = 0.0, mzj = 0.0;
                for (int r = out.base(g); r < out.base(g) + out.ports(g); ++r) {
                    for (int col = out.base(h); col < out.base(h) + out.ports(h); ++col) {
                        mz = std::max(mz, std::fabs(Z[static_cast<size_t>(r) * K + col]));
                        mzj = std::max(mzj, std::fabs(ZJ[static_cast<size_t>(r) * K + col]));
                    }
                }
                const size_t at = static_cast<size_t>(g) * static_cast<size_t>(nG) +
                                  static_cast<size_t>(h);
                out.z[at] = std::max(out.z[at], mz);
                out.zj[at] = std::max(out.zj[at], mzj);
            }
        }
    }
    return out;
}

}  // namespace lwdf_test
