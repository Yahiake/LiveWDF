#pragma once

// The general three-terminal JFET, after Bernardini, Vergani and Sarti,
// "Wave Digital Modeling of Nonlinear 3-terminal Devices for Virtual Analog
// Applications", Circuits Systems and Signal Processing 39, 2020,
// DOI 10.1007/s00034-019-01331-7.
//
// This is deliberately *not* a one-port time-varying-resistance fit. Here the
// device is the paper's n-channel JFET constitutive law with an explicit gate,
// a drain-source current, and the paper's ohmic/saturation/cut-off regions.
//
// A P-channel JFET is the exact mirror of that law. With terminal variables
// still defined as vGS = vG - vS and vDS = vD - vS, the physical current is
//
//     Id_p(vGS, vDS) = -Id_n(-vGS, -vDS)
//
// and both derivatives are unchanged by the two sign flips. This is the same
// transformation SPICE applies to a PJF card, so the LTspice cards keep their
// negative `Vto` and are marked with `pChannel` instead of having their
// parameters rewritten.
//
// ---------------------------------------------------------------------------
// Constitutive law (paper equations 45-47)
// ---------------------------------------------------------------------------
//
// Let
//
//     vAB = vGS = vG - vS
//     vCB = vDS = vD - vS
//     phi1 = vAB - Vp
//     phi2 = vCB
//
// with Vp < 0. The paper writes the current from drain to source as
//
//   cut-off,    phi1 <= 0:
//       Id = 0                                                   (45)
//
//   ohmic,      phi1 > 0 and phi2 < phi1:
//       Id = 2 Is0 / Vp^2 * (phi1*phi2 - phi2^2/2) * (1 + lambda*phi2)
//                                                                 (46)
//
//   saturation, phi1 > 0 and phi2 >= phi1:
//       Id = Is0 * (1 - vAB/Vp)^2 * (1 + lambda*phi2)             (47)
//
// `Is0` is the saturation current at zero gate-source voltage. `lambda` is the
// channel-length modulation coefficient. In SPICE's square-law notation
// `Beta = Is0 / Vp^2`, so `Is0 = Beta * Vp^2`; the paper's ohmic coefficient
// `2 Is0 / Vp^2` is therefore `2*Beta`. The two are not interchangeable.
//
// Assumptions and limits, stated rather than hidden:
//
//   * gate current is zero (the paper's `iA = 0`); a gate resistor to the
//     reference node is a separate linear element, not part of this law;
//   * intrinsic capacitances are not part of this constitutive law; they are
//     represented by ordinary linear capacitors around the intrinsic device;
//   * the law is the paper's simplified square-law/JFET model, not a complete
//     physical model: no subthreshold conduction, no temperature dependence, no
//     impact ionization, no gate leakage;
//   * the paper does not give `Vds < 0` a separate reverse-conduction branch;
//     its inequalities put negative `Vds` in the ohmic branch. Applying (46)
//     literally there produces a large-current extrapolation root in some
//     passive networks. This implementation instead uses the standard
//     source/drain-reversed extension: for `Vds < 0`, swap the source and drain
//     roles and evaluate the same forward law. That is an extension, not a
//     claim that the paper derived reverse saturation. It is continuous in
//     current and both derivatives at `Vds = 0`.
//
// ---------------------------------------------------------------------------
// Wave-domain form
// ---------------------------------------------------------------------------
//
// The paper's Fig. 5(b) has three ports (gate, source, drain, all referred to
// the reference node) with
//
//     [iA iB iC]^T = [0 1 -1]^T Id.                              (51)
//
// The gate port therefore carries no current. In this engine a nonlinear root
// device owns a contiguous port group; the JFET group is the two-port
// reduction
//
//     port 0: drain-source, current +Id
//     port 1: gate-source,  current 0
//
// which is the same nonlinear branch as (51) after the gate port is referred
// to the source instead of ground. The paper's `Rgate` is then the ordinary
// linear resistor from gate to ground, added by `Circuit::addJfet`. This is a
// two-port *reduction* of the paper's three-port WD element, not an identical
// set of scattering coefficients: the port basis and the group's open-circuit
// voltages and impedance matrix are different, while the constitutive law and
// the zero gate current are unchanged.
//
// For a root network with open-circuit port voltages `voDS`, `voGS` and
// impedance matrix `Z`, the port equations are
//
//     vDS = voDS - Z[ds,ds] Id
//     vGS = voGS - Z[gs,ds] Id
//
// because the gate current is zero. Substituting these into (46) or (47) gives
// a scalar polynomial in Id:
//
//     a3 Id^3 + a2 Id^2 + a1 Id + a0 = 0.
//
// With `A = voDS`, `B = voGS - Vp`, `zd = Z[ds,ds]`, `zg = Z[gs,ds]`,
// `L0 = 1 + lambda*A`, `L1 = -lambda*zd`:
//
//   ohmic:
//     u0 = B*A - A^2/2
//     u1 = -B*zd - A*zg + A*zd
//     u2 = zg*zd - zd^2/2
//     a3 = k*u2*L1
//     a2 = k*(u2*L0 + u1*L1)
//     a1 = k*(u1*L0 + u0*L1) - 1
//     a0 = k*u0*L0,                  k = 2*Is0/Vp^2
//
//   saturation:
//     q2 = Is0*zg^2/Vp^2
//     q1 = -2*Is0*zg*B/Vp^2
//     q0 = Is0*B^2/Vp^2
//     a3 = q2*L1
//     a2 = q2*L0 + q1*L1
//     a1 = q1*L0 + q0*L1 - 1
//     a0 = q0*L0
//
// `lambda = 0` makes L1 = 0 and the cubic reduces to a quadratic in both
// regions. With `lambda != 0` the cubic term remains in general. `zg = 0` also
// removes the saturation cubic term, but not the ohmic one in general.
//
// The closed-form solver below solves both the saturation and ohmic cubics,
// rejects roots that violate their own region inequality, and requires each
// candidate to land in the forward or source/drain-reversed half-plane it was
// solved for. When more than one valid root remains, it chooses the root nearest
// the previous sample's current (the same branch-continuity seed the Newton path
// gets from its warm start); a cold start therefore chooses the smallest |Id|.
// A separate bracketed scalar solver is kept for tests and as an explicit
// fallback; it finds roots without using the polynomial and applies the same
// documented branch policy only after the independent root set is known.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>

#include "devices/PolynomialRoots.h"

namespace wdf {

enum class JfetRegion {
    Invalid,
    Cutoff,
    Ohmic,
    Saturation,
};

struct JfetParams {
    double Is0 = 5.367e-3;   // A, saturation current at Vgs = 0
    double Vp = -2.021;      // V, pinch-off voltage (negative for n-channel)
    double lambda = 4.0e-3;  // 1/V, channel-length modulation
    // Linear parasitics. `rd`/`rs` are series resistances, `rgate` is the
    // external gate-to-reference resistance, and the capacitances are ordinary
    // linear elements. They are not part of the constitutive law above.
    double rd = 1.0;         // ohm
    double rs = 1.0;         // ohm
    double cgs = 2.414e-12;  // F
    double cgd = 1.6e-12;    // F
    double cds = 0.0;        // F
    double rgate = 1.0e9;    // ohm
    // false: N-channel; true: P-channel. The constitutive parameters keep their
    // n-channel meaning; polarity is applied as the mirror in `evaluate` and
    // `solveGroup`.
    bool pChannel = false;

    bool valid() const noexcept {
        return std::isfinite(Is0) && std::isfinite(Vp) && std::isfinite(lambda) &&
               std::isfinite(rd) && std::isfinite(rs) && std::isfinite(cgs) &&
               std::isfinite(cgd) && std::isfinite(cds) && std::isfinite(rgate) &&
               Is0 > 0.0 && Vp < 0.0 && lambda >= 0.0 && rd >= 0.0 && rs >= 0.0 &&
               cgs >= 0.0 && cgd >= 0.0 && cds >= 0.0 && rgate >= 0.0;
    }
};

struct JfetEvaluation {
    double id = 0.0;      // A, drain -> source
    double did_ds = 0.0;  // d Id / d Vds
    double did_gs = 0.0;  // d Id / d Vgs
    JfetRegion region = JfetRegion::Invalid;
    bool valid = false;
};

struct JfetGroupSolution {
    bool ok = false;
    double id = 0.0;
    JfetRegion region = JfetRegion::Invalid;
    double residual = 0.0;
    bool usedReference = false;
};

/// All independent scalar roots found by the bracketed reference. The test
/// suite uses this to check set membership separately from the closed-form
/// solver's branch policy; the engine itself only needs `solveReference`.
struct JfetRootSet {
    std::array<JfetGroupSolution, 12> roots{};
    int count = 0;
};

namespace jfet_detail {
using poly::realCubicRoots;
using poly::realQuadraticRoots;
}  // namespace jfet_detail

struct JfetModel {
    JfetParams p;

    constexpr JfetModel() = default;
    constexpr explicit JfetModel(const JfetParams& params) : p(params) {}

    /// SPICE square-law mapping. `Beta` is `Is0/Vp^2`, not `Is0`; this helper
    /// makes that conversion once so callers cannot get it backwards.
    static constexpr JfetModel fromSpice(double beta, double vto, double lambda, double rd = 0.0,
                                         double rs = 0.0, double cgs = 0.0, double cgd = 0.0,
                                         double cds = 0.0, double rgate = 1.0e9,
                                         bool pChannel = false) noexcept {
        JfetParams q;
        q.Is0 = beta * vto * vto;
        q.Vp = vto;
        q.lambda = lambda;
        q.rd = rd;
        q.rs = rs;
        q.cgs = cgs;
        q.cgd = cgd;
        q.cds = cds;
        q.rgate = rgate;
        q.pChannel = pChannel;
        return JfetModel(q);
    }

    bool valid() const noexcept { return p.valid(); }

    double k() const noexcept { return 2.0 * p.Is0 / (p.Vp * p.Vp); }

    /// The paper's equations (45)-(47) in their forward orientation, with
    /// `vDS >= 0`. This is the constitutive law itself; `evaluate` below adds
    /// the documented source/drain-reversed extension for negative `vDS`.
    JfetEvaluation evaluateForward(double vgs, double vds) const noexcept {
        JfetEvaluation e;
        if (!valid() || !std::isfinite(vgs) || !std::isfinite(vds) || vds < 0.0)
            return e;

        const double vov = vgs - p.Vp;
        if (!(vov > 0.0)) {
            e.region = JfetRegion::Cutoff;
            e.valid = true;
            return e;
        }

        const double onePlusLambdaVds = 1.0 + p.lambda * vds;
        if (!std::isfinite(onePlusLambdaVds))
            return e;

        if (vds < vov) {
            const double u = vov * vds - 0.5 * vds * vds;
            e.id = k() * u * onePlusLambdaVds;
            e.did_ds = k() * ((vov - vds) * onePlusLambdaVds + p.lambda * u);
            e.did_gs = k() * vds * onePlusLambdaVds;
            e.region = JfetRegion::Ohmic;
        } else {
            const double a = 1.0 - vgs / p.Vp;
            e.id = p.Is0 * a * a * onePlusLambdaVds;
            e.did_gs = -2.0 * p.Is0 * a * onePlusLambdaVds / p.Vp;
            e.did_ds = p.Is0 * a * a * p.lambda;
            e.region = JfetRegion::Saturation;
        }

        e.valid = std::isfinite(e.id) && std::isfinite(e.did_ds) && std::isfinite(e.did_gs);
        if (!e.valid)
            e.region = JfetRegion::Invalid;
        return e;
    }

    /// The paper's law plus a source/drain-reversed extension.
    ///
    /// The paper writes the current from drain to source and only its
    /// forward-bias inequalities are meaningful for a physical n-channel
    /// device. Applying (46) literally at large negative `vDS` eventually
    /// produces a second, large-current root when a passive network's load line
    /// is folded in; that root is an extrapolation artifact, not a reverse
    /// saturation model. For `vDS < 0` this implementation therefore swaps the
    /// source and drain roles and evaluates the same law in its forward
    /// orientation:
    ///
    ///     vGS' = vG - vD = vGS - vDS
    ///     vDS' = vS - vD = -vDS
    ///     Id   = -Id'(vGS', vDS')
    ///
    /// This is the standard symmetric JFET extension. It agrees with the
    /// paper at `vDS = 0` (current and both derivatives are continuous), and it
    /// keeps every evaluation inside the law's actual forward domain. It is an
    /// extension, not a claim that the paper derived reverse saturation.
    JfetEvaluation evaluate(double vgs, double vds) const noexcept {
        if (!p.pChannel)
            return evaluateN(vgs, vds);

        const JfetEvaluation r = evaluateN(-vgs, -vds);
        if (!r.valid)
            return {};
        JfetEvaluation e = r;
        e.id = -r.id;
        // did/dvgs and did/dvds are unchanged: both the argument and the
        // returned current are negated, and the two signs cancel.
        e.valid = std::isfinite(e.id) && std::isfinite(e.did_ds) &&
                  std::isfinite(e.did_gs);
        if (!e.valid)
            e.region = JfetRegion::Invalid;
        return e;
    }

private:
    /// The n-channel law plus its source/drain-reversed extension. Public
    /// `evaluate` applies the P-channel mirror around this function.
    JfetEvaluation evaluateN(double vgs, double vds) const noexcept {
        if (!valid() || !std::isfinite(vgs) || !std::isfinite(vds))
            return {};
        if (vds >= 0.0)
            return evaluateForward(vgs, vds);

        const JfetEvaluation r = evaluateForward(vgs - vds, -vds);
        if (!r.valid)
            return {};

        JfetEvaluation e;
        e.id = -r.id;
        e.did_gs = -r.did_gs;
        e.did_ds = r.did_gs + r.did_ds;
        e.region = r.region;
        e.valid = std::isfinite(e.id) && std::isfinite(e.did_ds) && std::isfinite(e.did_gs);
        if (!e.valid)
            e.region = JfetRegion::Invalid;
        return e;
    }

public:

    /// Closed-form solve in one fixed orientation, with `vDS >= 0`.
    ///
    /// The paper's inequalities are a forward-conduction model. This helper
    /// rejects any polynomial root whose own port equation produces a negative
    /// `vDS`; `solveGroup` below obtains reverse conduction by calling it with
    /// the source and drain exchanged. `seedId` is the previous branch current
    /// (or zero at a cold start). When more than one valid root exists, the
    /// nearest root is the continuous branch; the old smallest-|Id| rule is
    /// exactly the `seedId == 0` special case.
    JfetGroupSolution solveForwardGroup(double voDs, double voGs, double zdd, double zgd,
                                        double seedId) const noexcept {
        JfetGroupSolution out;
        if (!valid() || !std::isfinite(voDs) || !std::isfinite(voGs) ||
            !std::isfinite(zdd) || !std::isfinite(zgd))
            return out;

        const double seed = std::isfinite(seedId) ? seedId : 0.0;
        const double Vp = p.Vp;
        const double B = voGs - Vp;
        const double L0 = 1.0 + p.lambda * voDs;
        const double L1 = -p.lambda * zdd;
        if (!std::isfinite(B) || !std::isfinite(L0) || !std::isfinite(L1))
            return out;

        double bestResidual = std::numeric_limits<double>::infinity();
        double bestId = 0.0;
        JfetRegion bestRegion = JfetRegion::Invalid;

        const auto better = [&](double id, double residual) {
            if (!std::isfinite(bestResidual))
                return true;
            const double d = std::abs(id - seed);
            const double bd = std::abs(bestId - seed);
            const double tol = 1.0e-14 * (1.0 + std::max(d, bd));
            if (d < bd - tol)
                return true;
            if (d > bd + tol)
                return false;
            // Equal distance to the seed: prefer the smaller-magnitude root,
            // then the better constitutive residual. This makes a cold start
            // deterministic without making the warm start depend on it.
            const double a = std::abs(id);
            const double b = std::abs(bestId);
            const double magTol = 1.0e-14 * (1.0 + std::max(a, b));
            if (a < b - magTol)
                return true;
            if (a > b + magTol)
                return false;
            return residual < bestResidual;
        };

        const auto consider = [&](double id, JfetRegion expected) {
            if (!std::isfinite(id))
                return;
            double vds = voDs - zdd * id;
            const double vgs = voGs - zgd * id;
            if (!std::isfinite(vds) || !std::isfinite(vgs))
                return;
            const double vdsTol = 1.0e-12 * (1.0 + std::abs(voDs) + std::abs(zdd * id));
            if (vds < -vdsTol)
                return;
            if (vds < 0.0)
                vds = 0.0;
            const JfetEvaluation e = evaluateForward(vgs, vds);
            // Both laws agree on the ohmic/saturation boundary, and rounding can put
            // a root there on the other cubic's side (zgd == zdd keeps every root on it).
            const bool onBoundary = expected != JfetRegion::Cutoff &&
                                    e.region != JfetRegion::Cutoff &&
                                    std::abs(vds - (vgs - Vp)) <= vdsTol;
            if (!e.valid || (e.region != expected && !onBoundary))
                return;
            const double residual = std::abs(e.id - id);
            if (residual > 1.0e-7 * (1.0 + std::abs(id)))
                return;
            if (better(id, residual)) {
                bestResidual = residual;
                bestId = id;
                bestRegion = e.region;
            }
        };

        // Cut-off candidate. Zero is the closest possible root to a cold start,
        // so the common cut-off case still avoids both cubics.
        consider(0.0, JfetRegion::Cutoff);
        if (bestRegion == JfetRegion::Cutoff && seed == 0.0) {
            out.ok = true;
            out.id = 0.0;
            out.region = JfetRegion::Cutoff;
            out.residual = 0.0;
            return out;
        }

        double roots[3];

        // Both cubics are solved when a warm start is present. A circuit with
        // gate feedback can have a valid root in each region; stopping after
        // the first region would make the selected branch depend on ordering
        // rather than on continuity.
        {
            const double invVp2 = 1.0 / (Vp * Vp);
            const double q2 = p.Is0 * zgd * zgd * invVp2;
            const double q1 = -2.0 * p.Is0 * zgd * B * invVp2;
            const double q0 = p.Is0 * B * B * invVp2;
            const double c3 = q2 * L1;
            const double c2 = q2 * L0 + q1 * L1;
            const double c1 = q1 * L0 + q0 * L1 - 1.0;
            const double c0 = q0 * L0;
            const int n = jfet_detail::realCubicRoots(c3, c2, c1, c0, roots);
            for (int i = 0; i < n; ++i)
                consider(roots[i], JfetRegion::Saturation);
        }
        {
            const double A = voDs;
            const double u0 = B * A - 0.5 * A * A;
            const double u1 = -B * zdd - A * zgd + A * zdd;
            const double u2 = zgd * zdd - 0.5 * zdd * zdd;
            const double kk = k();
            const double c3 = kk * u2 * L1;
            const double c2 = kk * (u2 * L0 + u1 * L1);
            const double c1 = kk * (u1 * L0 + u0 * L1) - 1.0;
            const double c0 = kk * u0 * L0;
            const int n = jfet_detail::realCubicRoots(c3, c2, c1, c0, roots);
            for (int i = 0; i < n; ++i)
                consider(roots[i], JfetRegion::Ohmic);
        }

        if (std::isfinite(bestResidual)) {
            out.ok = true;
            out.id = bestId;
            out.region = bestRegion;
            out.residual = bestResidual;
        }
        return out;
    }

    /// Closed-form two-port group solve. `voDs`/`voGs` are the group's
    /// open-circuit voltages after earlier groups are folded in; `zdd` is
    /// `Z[ds,ds]` and `zgd` is `Z[gs,ds]`. The gate current is zero by
    /// construction, so no `Z[gs,gs]` term appears.
    ///
    /// The forward and source/drain-reversed orientations are solved
    /// separately and each candidate is required to land in its own `vDS`
    /// half-plane. That removes the large-negative-`vDS` extrapolation root
    /// without selecting it away by magnitude. `seedId` chooses between any
    /// remaining multiple roots by branch continuity.
    JfetGroupSolution solveGroup(double voDs, double voGs, double zdd, double zgd,
                                 double seedId = 0.0) const noexcept {
        if (!valid() || !std::isfinite(voDs) || !std::isfinite(voGs) ||
            !std::isfinite(zdd) || !std::isfinite(zgd))
            return {};
        if (!p.pChannel)
            return solveGroupN(voDs, voGs, zdd, zgd, seedId);

        // Mirror the whole port equation: solve the N-channel group at the
        // negated open-circuit voltages and seed, then negate its current.
        // Z and the port topology stay in the physical orientation.
        const double seed = std::isfinite(seedId) ? seedId : 0.0;
        JfetGroupSolution out = solveGroupN(-voDs, -voGs, zdd, zgd, -seed);
        if (out.ok)
            out.id = -out.id;
        return out;
    }

private:
    JfetGroupSolution solveGroupN(double voDs, double voGs, double zdd, double zgd,
                                  double seedId) const noexcept {
        const double seed = std::isfinite(seedId) ? seedId : 0.0;

        // For a passive port (`zdd >= 0`) only one orientation can contain a
        // root. If voDs >= 0, a reverse-law current is negative and would make
        // vDS = voDs - zdd*Id larger than voDs, not negative; if voDs < 0, a
        // forward-law current is positive and would make vDS more negative.
        // This is the same half-plane argument as the full two-orientation
        // solve, not an approximation, and it avoids solving four cubics for
        // the common one-sided case. Keep the full solve for active/negative
        // port impedances, where that monotonicity argument does not hold.
        if (zdd >= 0.0) {
            if (voDs >= 0.0)
                return solveForwardGroup(voDs, voGs, zdd, zgd, seed);
            JfetGroupSolution reverse =
                solveForwardGroup(-voDs, voGs - voDs, zdd, zdd - zgd, -seed);
            if (reverse.ok)
                reverse.id = -reverse.id;
            return reverse;
        }

        const JfetGroupSolution forward = solveForwardGroup(voDs, voGs, zdd, zgd, seed);
        const JfetGroupSolution reverse =
            solveForwardGroup(-voDs, voGs - voDs, zdd, zdd - zgd, -seed);

        JfetGroupSolution best;
        bool have = false;
        const auto consider = [&](const JfetGroupSolution& candidate, double id, bool isForward) {
            if (!candidate.ok || !std::isfinite(id))
                return;
            if (!have) {
                best = candidate;
                best.id = id;
                have = true;
                return;
            }
            const double d = std::abs(id - seed);
            const double bd = std::abs(best.id - seed);
            const double tol = 1.0e-14 * (1.0 + std::max(d, bd));
            if (d < bd - tol) {
                best = candidate;
                best.id = id;
            } else if (d <= bd + tol && isForward && !best.usedReference) {
                // A zero-width tie at vDS = 0 is the same physical point. Keep
                // the forward orientation so the result is deterministic.
                best = candidate;
                best.id = id;
            }
        };
        consider(forward, forward.id, true);
        consider(reverse, -reverse.id, false);
        if (!have)
            return {};
        best.usedReference = false;
        return best;
    }

public:
    /// Independent bracketed root set for `i = f(vo - Z*i)`.
    ///
    /// It scans the supplied current interval, bisects every sign change, and
    /// accepts only points whose own constitutive-law residual is small. It is
    /// not a Newton iteration and it does not use the cubic. The returned set is
    /// used directly by tests; `solveReference` below applies the documented
    /// branch-continuity policy to it.
    JfetRootSet solveReferenceRoots(double voDs, double voGs, double zdd, double zgd,
                                    double xLo, double xHi, int scan = 256) const noexcept {
        JfetRootSet out;
        if (!valid() || !std::isfinite(voDs) || !std::isfinite(voGs) ||
            !std::isfinite(zdd) || !std::isfinite(zgd) || !std::isfinite(xLo) ||
            !std::isfinite(xHi) || !(xHi > xLo) || scan < 4)
            return out;

        const auto addRoot = [&](double id) {
            if (!std::isfinite(id))
                return;
            const double vds = voDs - zdd * id;
            const double vgs = voGs - zgd * id;
            const JfetEvaluation e = evaluate(vgs, vds);
            if (!e.valid || e.region == JfetRegion::Invalid)
                return;
            const double residual = std::abs(e.id - id);
            if (residual > 1.0e-9 * (1.0 + std::abs(id)))
                return;
            for (int i = 0; i < out.count; ++i) {
                const double existing = out.roots[static_cast<size_t>(i)].id;
                if (std::abs(existing - id) <= 1.0e-8 * (1.0 + std::abs(id))) {
                    if (residual < out.roots[static_cast<size_t>(i)].residual) {
                        out.roots[static_cast<size_t>(i)].id = id;
                        out.roots[static_cast<size_t>(i)].region = e.region;
                        out.roots[static_cast<size_t>(i)].residual = residual;
                    }
                    return;
                }
            }
            if (out.count >= static_cast<int>(out.roots.size()))
                return;
            JfetGroupSolution root;
            root.ok = true;
            root.id = id;
            root.region = e.region;
            root.residual = residual;
            out.roots[static_cast<size_t>(out.count++)] = root;
        };

        const auto g = [&](double id) {
            const double vds = voDs - zdd * id;
            const double vgs = voGs - zgd * id;
            const JfetEvaluation e = evaluate(vgs, vds);
            return e.valid ? (e.id - id) : std::numeric_limits<double>::quiet_NaN();
        };

        addRoot(0.0);

        const auto scanRange = [&](double lo, double hi, int count) {
            if (!(hi > lo) || count < 4)
                return;
            double prevX = lo;
            double prevG = g(prevX);
            for (int i = 1; i <= count; ++i) {
                const double x = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(count);
                const double gx = g(x);
                if (std::isfinite(gx) && std::abs(gx) <= 1.0e-12 * (1.0 + std::abs(x)))
                    addRoot(x);
                if (std::isfinite(prevG) && std::isfinite(gx) && prevG != 0.0 && gx != 0.0 &&
                    ((prevG < 0.0) != (gx < 0.0))) {
                    double a = prevX;
                    double b = x;
                    double ga = prevG;
                    for (int it = 0; it < 100; ++it) {
                        const double mid = 0.5 * (a + b);
                        const double gm = g(mid);
                        if (!std::isfinite(gm))
                            break;
                        addRoot(mid);
                        if (std::abs(gm) <= 1.0e-14 * (1.0 + std::abs(mid)))
                            break;
                        if ((ga < 0.0) != (gm < 0.0)) {
                            b = mid;
                        } else {
                            a = mid;
                            ga = gm;
                        }
                    }
                }
                prevX = x;
                prevG = gx;
            }
        };

        // The near scan keeps a small root visible even when the full interval
        // is dominated by a large gate-transimpedance bound. A second, dense
        // scan around zero is needed because a cubic can have two close roots
        // (for example 0.79 mA and 2.38 mA) that a coarse scan steps over
        // without seeing a sign change. The full scan still covers roots that
        // only exist far from the origin.
        double xNear = std::max(100.0 * p.Is0, 1.0e-12);
        if (std::abs(zdd) > 1.0e-9)
            xNear = std::max(xNear, 10.0 * std::abs(voDs) / std::abs(zdd));
        xNear = std::min(xNear, 1.0e6);
        const double nearLo = std::max(xLo, -xNear);
        const double nearHi = std::min(xHi, xNear);
        if (nearHi > nearLo)
            scanRange(nearLo, nearHi, std::max(64, scan / 2));
        const double xTiny = std::min(xNear, std::max(20.0 * p.Is0, 1.0e-6));
        const double tinyLo = std::max(xLo, -xTiny);
        const double tinyHi = std::min(xHi, xTiny);
        if (tinyHi > tinyLo)
            scanRange(tinyLo, tinyHi, std::max(512, scan * 2));
        scanRange(xLo, xHi, scan);
        return out;
    }

    JfetRootSet solveReferenceRoots(double voDs, double voGs, double zdd, double zgd) const noexcept {
        double xMax = 1000.0 * p.Is0;
        if (std::abs(zdd) > 1.0e-9)
            xMax = std::max(xMax, 10.0 * std::abs(voDs) / std::abs(zdd));
        // A zero (or negligible) gate transimpedance does not mean the current
        // is unbounded; it means the gate voltage does not constrain the root.
        // Dividing by 1e-9 here used to produce a 1e6-wide scan and miss the
        // physically relevant root near zero entirely.
        if (std::abs(zgd) > 1.0e-6)
            xMax = std::max(xMax, 10.0 * std::abs(voGs) / std::abs(zgd));
        xMax = std::min(xMax, 1.0e6);
        if (!(xMax > 0.0) || !std::isfinite(xMax))
            xMax = 1.0;
        return solveReferenceRoots(voDs, voGs, zdd, zgd, -xMax, xMax);
    }

    /// Select one root from the independently found reference set using the
    /// same continuity policy as the closed form. `seedId` is the previous
    /// branch current; a cold start (`seedId == 0`) chooses the smallest |Id|.
    JfetGroupSolution solveReference(double voDs, double voGs, double zdd, double zgd,
                                     double xLo, double xHi, int scan = 256,
                                     double seedId = 0.0) const noexcept {
        const JfetRootSet roots = solveReferenceRoots(voDs, voGs, zdd, zgd, xLo, xHi, scan);
        JfetGroupSolution best;
        if (roots.count == 0)
            return best;
        const double seed = std::isfinite(seedId) ? seedId : 0.0;
        bool have = false;
        for (int i = 0; i < roots.count; ++i) {
            const JfetGroupSolution& candidate = roots.roots[static_cast<size_t>(i)];
            if (!have) {
                best = candidate;
                have = true;
                continue;
            }
            const double d = std::abs(candidate.id - seed);
            const double bd = std::abs(best.id - seed);
            const double tol = 1.0e-14 * (1.0 + std::max(d, bd));
            if (d < bd - tol) {
                best = candidate;
            } else if (d <= bd + tol) {
                const double a = std::abs(candidate.id);
                const double b = std::abs(best.id);
                const double magTol = 1.0e-14 * (1.0 + std::max(a, b));
                if (a < b - magTol ||
                    (a <= b + magTol && candidate.residual < best.residual))
                    best = candidate;
            }
        }
        best.usedReference = false;
        return best;
    }

    JfetGroupSolution solveReferenceSeeded(double voDs, double voGs, double zdd, double zgd,
                                           double seedId) const noexcept {
        double xMax = 1000.0 * p.Is0;
        if (std::abs(zdd) > 1.0e-9)
            xMax = std::max(xMax, 10.0 * std::abs(voDs) / std::abs(zdd));
        if (std::abs(zgd) > 1.0e-6)
            xMax = std::max(xMax, 10.0 * std::abs(voGs) / std::abs(zgd));
        xMax = std::min(xMax, 1.0e6);
        if (!(xMax > 0.0) || !std::isfinite(xMax))
            xMax = 1.0;
        return solveReference(voDs, voGs, zdd, zgd, -xMax, xMax, 256, seedId);
    }

    JfetGroupSolution solveReference(double voDs, double voGs, double zdd, double zgd) const noexcept {
        return solveReferenceSeeded(voDs, voGs, zdd, zgd, 0.0);
    }
};

// The LTspice standard.jft 2N5952 card, restricted to the parameters this model
// actually reads. Source: LTspice `standard.jft`, model `2N5952`:
//   Beta=1.314m  Vto=-2.021  Lambda=4m  Rd=1  Rs=1  Cgd=1.6p  Cgs=2.414p
// Beta*Vto^2 = 5.367e-3 A, which is the paper's Is0. The card's junction,
// temperature, noise and gate-current parameters are outside this simplified
// law and are intentionally not represented.
inline constexpr JfetParams k2N5952Params{5.367e-3, -2.021, 4.0e-3, 1.0, 1.0,
                                          2.414e-12, 1.6e-12, 0.0, 1.0e9};
inline constexpr JfetModel k2N5952{k2N5952Params};

// Additional built-in presets from the LTspice `standard.jft` library.
// Source: `standard.jft` supplied with LTspice, Copyright (c) 2000-2022
// Analog Devices, Inc. All rights reserved. Only the parameter values read by
// this simplified model are reproduced; the library file is not redistributed.
// `fromSpice` applies the documented `Is0 = Beta * Vto^2` mapping. `Cds` is
// absent from these cards and therefore uses its documented default of zero.
// Junction, temperature, noise, gate-current and manufacturer fields are
// outside this model and are intentionally not represented.
//
// 2N5457: Beta=1.125m Vto=-1.372 Lambda=2.3m Rd=1 Rs=1 Cgs=4.627p Cgd=4p.
inline constexpr JfetModel k2N5457 = JfetModel::fromSpice(
    1.125e-3, -1.372, 2.3e-3, 1.0, 1.0, 4.627e-12, 4.0e-12);

// 2N5458 from the published jfet.lib card (VTO, BETA, LAMBDA and parasitics).
// https://www.diyaudio.com/community/threads/improvements-on-12ax7-12at7-el34-schematic.358653/page-3#post-6304821
inline constexpr JfetModel k2N5458 = JfetModel::fromSpice(
    488.9e-6, -2.882, 3.167e-3, 1.0, 1.0, 4.627e-12, 4.0e-12);

// MPF102: Beta=1.04m Vto=-3.41 Lambda=2m Rd=1 Rs=1 Cgs=2.414p Cgd=1.6p.
inline constexpr JfetModel kMPF102 = JfetModel::fromSpice(
    1.04e-3, -3.41, 2.0e-3, 1.0, 1.0, 2.414e-12, 1.6e-12);

// J201: Beta=1.07m Vto=-0.93 Lambda=6.75m Rd=10 Rs=12 Cgs=1p Cgd=1p.
inline constexpr JfetModel kJ201 = JfetModel::fromSpice(
    1.07e-3, -0.93, 6.75e-3, 10.0, 12.0, 1.0e-12, 1.0e-12);

// 2SK170 (source spelling `2sk170`): Beta=59.86m Vto=-0.5024 Lambda=1.923m
// Rd=4.151 Rs=4.151 Cgs=25.48p Cgd=20p.
inline constexpr JfetModel k2SK170 = JfetModel::fromSpice(
    59.86e-3, -0.5024, 1.923e-3, 4.151, 4.151, 25.48e-12, 20.0e-12);

// Toshiba 2SK184 datasheet, GR grade (2.6–6.5 mA IDSS).
// Fit the 4.2 mA transfer curve at VDS = 10 V, with VTO ≈ −0.55 V;
// lambda ≈ 0.003/V comes from the slope of the output characteristics.
// Cgs = Ciss − Crss (13 − 3 pF). The datasheet does not give Rd or Rs.
// https://aitendo3.sakura.ne.jp/aitendo_data/product_img/transisitor/2SK184/2SK184_toshiba.pdf
inline constexpr JfetModel k2SK184Gr = JfetModel::fromSpice(
    4.2e-3 / (1.03 * 0.55 * 0.55), -0.55, 0.003, 0.0, 0.0, 10.0e-12, 3.0e-12);

// J111: Beta=2.91m Vto=-4.047 Lambda=17.5m Rd=1 Rs=1 Cgs=5.74p Cgd=6.46p.
inline constexpr JfetModel kJ111 = JfetModel::fromSpice(
    2.91e-3, -4.047, 17.5e-3, 1.0, 1.0, 5.74e-12, 6.46e-12);

// 2N3819: Beta=1.304m Vto=-3 Lambda=2.25m Rd=1 Rs=1 Cgs=2.414p Cgd=1.6p.
inline constexpr JfetModel k2N3819 = JfetModel::fromSpice(
    1.304e-3, -3.0, 2.25e-3, 1.0, 1.0, 2.414e-12, 1.6e-12);

// 2N5484...
inline constexpr JfetModel k2N5484 = JfetModel::fromSpice(
    3.0e-3, -1.5, 10.0e-3, 10.0, 10.0, 4.0e-12, 4.0e-12);
inline constexpr JfetModel k2N5485 = JfetModel::fromSpice(
    3.5e-3, -2.0, 10.0e-3, 10.0, 10.0, 4.0e-12, 4.0e-12);
inline constexpr JfetModel k2N5486 = JfetModel::fromSpice(
    4.0e-3, -4.0, 10.0e-3, 10.0, 10.0, 4.0e-12, 4.0e-12);
inline constexpr JfetModel kBF245A = JfetModel::fromSpice(
    1.16621e-3, -1.7372, 17.7211e-3, 9.01678, 9.01678, 2.2e-12, 2.2e-12);
inline constexpr JfetModel kBF245B = JfetModel::fromSpice(
    1.09045e-3, -2.3085, 23.1754e-3, 7.77648, 7.77648, 2.0e-12, 2.2e-12);
inline constexpr JfetModel kBF245C = JfetModel::fromSpice(
    0.543157e-3, -5.0014, 27.1505e-3, 12.0869, 12.0869, 2.0e-12, 2.0e-12);
inline constexpr JfetModel kJ112 = JfetModel::fromSpice(
    5.695e-3, -2.057, 13.0e-3, 1.0, 1.0, 5.74e-12, 6.46e-12);
inline constexpr JfetModel kJ113 = JfetModel::fromSpice(
    9.109e-3, -1.382, 8.0e-3, 1.0, 1.0, 5.74e-12, 6.46e-12);
inline constexpr JfetModel kJ310 = JfetModel::fromSpice(
    3.384e-3, -3.409, 17.0e-3, 1.0, 1.0, 6.2e-12, 6.2e-12);

// J202: Beta=0.88m Vto=-1.81 Lambda=8.47m Rd=11 Rs=13 Cgs=1p Cgd=1p.
inline constexpr JfetModel kJ202 = JfetModel::fromSpice(
    0.88e-3, -1.81, 8.47e-3, 11.0, 13.0, 1.0e-12, 1.0e-12);

// P-channel cards from the same LTspice library. `PJF` is carried as the
// model's polarity flag; the card's Vto stays negative exactly as published.
// 2N5460..62: Beta=1.0m/1.5m/2.0m Vto=-3.4/-4.3/-5.4 Lambda=10m
// Rd=10 Rs=10 Cgs=5p Cgd=5p.
inline constexpr JfetModel k2N5460 = JfetModel::fromSpice(
    1.0e-3, -3.4, 10.0e-3, 10.0, 10.0, 5.0e-12, 5.0e-12, 0.0, 1.0e9, true);
inline constexpr JfetModel k2N5461 = JfetModel::fromSpice(
    1.5e-3, -4.3, 10.0e-3, 10.0, 10.0, 5.0e-12, 5.0e-12, 0.0, 1.0e9, true);
inline constexpr JfetModel k2N5462 = JfetModel::fromSpice(
    2.0e-3, -5.4, 10.0e-3, 10.0, 10.0, 5.0e-12, 5.0e-12, 0.0, 1.0e9, true);

// J175..177: Beta=1.031m/2.3m/3.3m Vto=-3.762/-1.82/-1.386
// Lambda=28m/21m/18.18m Rd=1 Rs=1 Cgs=9p Cgd=6.5p.
inline constexpr JfetModel kJ175 = JfetModel::fromSpice(
    1.031e-3, -3.762, 28.0e-3, 1.0, 1.0, 9.0e-12, 6.5e-12, 0.0, 1.0e9, true);
inline constexpr JfetModel kJ176 = JfetModel::fromSpice(
    2.3e-3, -1.82, 21.0e-3, 1.0, 1.0, 9.0e-12, 6.5e-12, 0.0, 1.0e9, true);
inline constexpr JfetModel kJ177 = JfetModel::fromSpice(
    3.3e-3, -1.386, 18.18e-3, 1.0, 1.0, 9.0e-12, 6.5e-12, 0.0, 1.0e9, true);

struct JfetEntry {
    const char* name;
    JfetModel model;
};

inline constexpr JfetEntry kJfetTable[] = {
    {"2N5952", k2N5952},
    {"2N5457", k2N5457},
    {"2N5458", k2N5458},
    {"MPF102", kMPF102},
    {"J201", kJ201},
    {"2SK170", k2SK170},
    {"2SK184-GR", k2SK184Gr},
    {"J111", kJ111},
    {"2N3819", k2N3819},
    {"2N5484", k2N5484},
    {"2N5485", k2N5485},
    {"2N5486", k2N5486},
    {"BF245A", kBF245A},
    {"BF245B", kBF245B},
    {"BF245C", kBF245C},
    {"J112",   kJ112},
    {"J113",   kJ113},
    {"J310",   kJ310},
    {"J202", kJ202},
    // P-channel cards. The picker shows them alongside the N-channel cards;
    // selecting one is what makes a placed JFET a P-channel device.
    {"2N5460", k2N5460},
    {"2N5461", k2N5461},
    {"2N5462", k2N5462},
    {"J175", kJ175},
    {"J176", kJ176},
    {"J177", kJ177},
    // 2N5459 (LTspice standard.jft 2N5459 / Siliconix)
    {"2N5459", JfetModel::fromSpice(377.6e-6, -4.473, 4.167e-3, 1.0, 1.0, 4.627e-12, 4.0e-12)},
    // LSK170 family (Linear Systems; LTspice standard.jft LSK170A/B/C)
    {"LSK170A", JfetModel::fromSpice(37.86e-3, -0.40, 4.78e-3, 11.0, 7.0, 40.7e-12, 39.9e-12)},
    {"LSK170B", JfetModel::fromSpice(35.07e-3, -0.54, 4.95e-3, 8.0, 7.0, 43.6e-12, 43.6e-12)},
    {"LSK170C", JfetModel::fromSpice(27.85e-3, -0.80, 12.24e-3, 12.0, 6.0, 42.3e-12, 42.2e-12)},
    // LSK189 family (Linear Systems; LTspice standard.jft LSK189A/B/C)
    {"LSK189A", JfetModel::fromSpice(2.2e-3, -1.13, 4.3e-3, 11.0, 30.0, 2.92e-12, 3.19e-12)},
    {"LSK189B", JfetModel::fromSpice(2.1e-3, -1.80, 5.7e-3, 11.0, 40.0, 2.92e-12, 3.19e-12)},
    {"LSK189C", JfetModel::fromSpice(2.1e-3, -2.78, 6.3e-3, 11.0, 55.0, 2.92e-12, 3.19e-12)},
    // JFE150 (Linear Systems; LTspice standard.jft NJFE150)
    {"JFE150", JfetModel::fromSpice(31.0e-3, -0.975, 16.95e-3, 3.5, 4.2, 20.3e-12, 6.3e-12)},
};
inline constexpr int kNumJfets = static_cast<int>(sizeof(kJfetTable) / sizeof(kJfetTable[0]));

inline bool jfetNameEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
        const char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
        if (ca != cb)
            return false;
    }
    return true;
}

inline const JfetEntry* findJfetByName(std::string_view name) noexcept {
    for (int i = 0; i < kNumJfets; ++i) {
        if (jfetNameEquals(name, kJfetTable[i].name))
            return &kJfetTable[i];
    }
    // SPICE users often write `J2N5952` for the model name as well as the
    // instance name; accept the leading J only as a convenience, never as a
    // silent fallback for an unknown model.
    if (!name.empty() && (name.front() == 'J' || name.front() == 'j'))
        return findJfetByName(name.substr(1));
    return nullptr;
}

}  // namespace wdf
