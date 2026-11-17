#pragma once

// The general three-terminal n-channel MOSFET, using the simplified square-law
// model of Bernardini, Vergani and Sarti, "Wave Digital Modeling of Nonlinear
// 3-terminal Devices for Virtual Analog Applications", Circuits, Systems and
// Signal Processing 39 (2020), DOI 10.1007/s00034-019-01331-7.
//
// This is the paper's n-channel MOSFET constitutive law, not a SPICE model card
// implementation. It is deliberately separate from the JFET model in `Jfet.h`:
// the JFET uses `Is0`/`Vp`, while this model uses the MOSFET transconductance
// parameter `K = mu_n C_ox W/L`, a threshold voltage `Vth`, and channel-length
// modulation `lambda`.
//
// A P-channel device is the exact mirror in the same terminal variables:
//
//     Id_p(vGS, vDS) = -Id_n(-vGS, -vDS)
//
// `Vth` is stored as a positive magnitude and `pChannel` supplies the sign
// convention. Both derivatives are unchanged by the two sign flips. This keeps
// one constitutive law and one closed-form solver rather than a second,
// silently different PMOS law.
//
// ---------------------------------------------------------------------------
// Constitutive law (paper equations 21-23)
// ---------------------------------------------------------------------------
//
// Let
//
//     vGS = vG - vS
//     vDS = vD - vS
//     Vov = vGS - Vth
//
// with the current `Id` defined from drain to source. The paper writes:
//
//   cut-off,    Vov <= 0:
//       Id = 0                                                   (21)
//
//   ohmic,      Vov > 0 and vDS < Vov:
//       Id = K (Vov*vDS - vDS^2/2) (1 + lambda*vDS)              (22)
//
//   saturation, Vov > 0 and vDS >= Vov:
//       Id = (K/2) Vov^2 (1 + lambda*vDS)                         (23)
//
// The paper states strict inequalities and leaves equality to the reader. The
// law is continuous at both boundaries. This implementation assigns equality
// `vDS == Vov` to saturation and `Vov == 0` to cut-off so region reporting is
// deterministic.
//
// Assumptions and limits, stated rather than hidden:
//
//   * gate current is zero; the gate is a control terminal, not a junction;
//   * intrinsic capacitances, body effect, subthreshold conduction, mobility
//     degradation, temperature dependence, impact ionization, and gate leakage
//     are not part of this law;
//   * the P-channel variant is the exact polarity mirror stated above, not a
//     second set of parameters;
//   * the paper does not define a reverse-conduction branch for `vDS < 0`. This
//     implementation uses the standard source/drain-reversed extension for the
//     pinned n-channel device:
//
//         vGS' = vGS - vDS = vGD
//         vDS' = -vDS
//         Id'  = the forward law at (vGS', vDS')
//         Id   = -Id'
//
//     This extension is not paper content. It is derived and tested here as a
//     separate implementation decision. It is continuous in current and both
//     first derivatives at `vDS = 0`.
//
// ---------------------------------------------------------------------------
// Wave-domain form
// ---------------------------------------------------------------------------
//
// The paper's Fig. 5(b) three-port law has `[iG iS iD]^T = [0 -Id +Id]^T`.
// As with the JFET, this engine uses a two-port reduction of that element:
//
//     port 0: drain-source, current +Id
//     port 1: gate-source,  current 0
//
// The gate is still a real port because its voltage controls the law and the
// network couples it through `Z`. The zero-current gate row is exact.
//
// For a root network with open-circuit port voltages `voDS`, `voGS` and
// impedance matrix `Z`, the port equations are
//
//     vDS = voDS - Z[ds,ds] Id
//     vGS = voGS - Z[gs,ds] Id
//
// because the gate current is zero. Substituting these into (22) or (23) gives
// a scalar cubic in Id. With
//
//     A  = voDS
//     B  = voGS - Vth
//     zd = Z[ds,ds]
//     zg = Z[gs,ds]
//     L0 = 1 + lambda*A
//     L1 = -lambda*zd
//
// the ohmic coefficients are
//
//     u0 = B*A - A^2/2
//     u1 = -B*zd - A*zg + A*zd = -B*zd + A*(zd - zg)
//     u2 = zg*zd - zd^2/2
//     c3 = K*u2*L1
//     c2 = K*(u2*L0 + u1*L1)
//     c1 = K*(u1*L0 + u0*L1) - 1
//     c0 = K*u0*L0
//
// and the saturation coefficients are
//
//     q2 = K*zg^2/2
//     q1 = -K*zg*B
//     q0 = K*B^2/2
//     c3 = q2*L1
//     c2 = q2*L0 + q1*L1
//     c1 = q1*L0 + q0*L1 - 1
//     c0 = q0*L0
//
// `lambda = 0` removes the cubic term. `zg = 0` removes the saturation cubic
// term, but not the ohmic one in general. The closed-form solver solves both
// region polynomials, rejects roots that violate their own region inequality or
// land in the wrong `vDS` half-plane, and keeps the root nearest the previous
// sample current when more than one survives. A cold start (`seedId == 0`)
// therefore chooses the smallest `|Id|`. That is a branch-continuity policy,
// not a claim from the paper.
//
// ---------------------------------------------------------------------------
// Source/drain-reversed group solve
// ---------------------------------------------------------------------------
//
// The forward group coefficients above are written in the orientation used by
// the paper. For `vDS < 0`, define the reversed orientation explicitly:
//
//     new drain  = old source
//     new source = old drain
//     Id_r       = current from old source to old drain = -Id
//
// The reversed terminal variables are
//
//     vGS_r = vG - new source = vG - old drain = vGD = vGS - vDS
//     vov_r = vGS_r - Vth = vGD - Vth = vGS - vDS - Vth
//     vDS_r = new drain - new source = old source - old drain = -vDS
//
// The LiveWDF port equations, still written for the original group, are
//
//     vDS = voDS - zd*Id
//     vGS = voGS - zg*Id
//
// Substituting Id = -Id_r and the reversed definitions gives the reversed
// open-circuit port equations
//
//     vDS_r = -voDS - zd*Id_r
//     vGS_r = (voGS - voDS) - (zd - zg)*Id_r
//
// so the reversed scalar solve uses
//
//     A_r  = -voDS
//     B_r  = (voGS - voDS) - Vth       [= voGD - Vth]
//     zd_r = zd
//     zg_r = zd - zg
//     L0_r = 1 + lambda*A_r
//     L1_r = -lambda*zd_r
//
// and the same region-polynomial algebra in the reversed variables. Only after
// the oriented root is accepted is the physical current restored as `Id=-Id_r`.
// This is *not* the forward polynomial with the current sign flipped: `A_r`,
// `B_r` and `zg_r` all change, and the region check is made in the reversed
// variables. The tests exercise the case `VGS < Vth < VGD`, where the forward
// orientation is in cut-off while the reversed orientation conducts.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include "devices/PolynomialRoots.h"

namespace wdf {

enum class MosfetRegion {
    Invalid,
    Cutoff,
    Ohmic,
    Saturation,
};

struct MosfetParams {
    // No model-card preset is implied by these defaults. `K == 0` makes the
    // default-constructed model invalid; callers must supply an explicit model.
    double K = 0.0;         // A/V^2, mu*C_ox*W/L
    double Vth = 0.0;       // V, threshold magnitude (positive for both types)
    double lambda = 0.0;    // 1/V, channel-length modulation
    bool pChannel = false;  // false: N-channel; true: P-channel

    bool valid() const noexcept {
        return std::isfinite(K) && std::isfinite(Vth) && std::isfinite(lambda) &&
               K > 0.0 && lambda >= 0.0;
    }
};

struct MosfetEvaluation {
    double id = 0.0;      // A, drain -> source
    double did_ds = 0.0;  // d Id / d Vds
    double did_gs = 0.0;  // d Id / d Vgs
    MosfetRegion region = MosfetRegion::Invalid;
    bool valid = false;
};

struct MosfetGroupSolution {
    bool ok = false;
    double id = 0.0;
    MosfetRegion region = MosfetRegion::Invalid;
    double residual = 0.0;
    bool usedReference = false;
};

/// All independent scalar roots found by the bracketed reference. Tests use the
/// set to check membership separately from the closed form's branch policy.
struct MosfetRootSet {
    std::array<MosfetGroupSolution, 12> roots{};
    int count = 0;
};

struct MosfetModel {
    MosfetParams p;

    constexpr MosfetModel() = default;
    constexpr explicit MosfetModel(const MosfetParams& params) : p(params) {}

    /// Direct K/Vth/lambda construction. This is the model used by the paper's
    /// equations and by `.livewdf` documents.
    static constexpr MosfetModel fromK(double K, double Vth, double lambda = 0.0,
                                       bool pChannel = false) noexcept {
        return MosfetModel(MosfetParams{K, Vth, lambda, pChannel});
    }

    /// SPICE Level-1 mapping: `KP = mu_n*C_ox` and `K = KP*W/L`. The caller
    /// supplies the instance or model-card width and length; nothing here
    /// supplies a fabricated default device.
    static constexpr MosfetModel fromSpice(double kp, double vto, double lambda = 0.0,
                                           double w = 1.0, double l = 1.0,
                                           bool pChannel = false) noexcept {
        return MosfetModel(MosfetParams{kp * w / l, std::abs(vto), lambda, pChannel});
    }

    bool valid() const noexcept { return p.valid(); }

    /// Region classification with one numerical boundary tolerance. The law is
    /// continuous at `vDS == Vov`, so this only decides which expression and
    /// which region label to use; it does not change the current at the
    /// boundary. The tolerance keeps a root computed from the port equations
    /// from being rejected as "ohmic" when the constitutive-law evaluator sees
    /// the same point as saturation by a few ulps.
    static MosfetRegion classifyRegion(double vov, double vds) noexcept {
        if (!(vov > 0.0))
            return MosfetRegion::Cutoff;
        const double tol = 1.0e-12 * (1.0 + std::abs(vov) + std::abs(vds));
        return vds < vov - tol ? MosfetRegion::Ohmic : MosfetRegion::Saturation;
    }

    /// The paper's equations (21)-(23) in their forward orientation, with
    /// `vDS >= 0`. This is the constitutive law itself; `evaluate` below adds
    /// the separately derived source/drain-reversed extension for negative
    /// `vDS`.
    MosfetEvaluation evaluateForward(double vgs, double vds) const noexcept {
        MosfetEvaluation e;
        if (!valid() || !std::isfinite(vgs) || !std::isfinite(vds) || vds < 0.0)
            return e;

        const double vov = vgs - p.Vth;
        if (!std::isfinite(vov))
            return e;
        if (!(vov > 0.0)) {
            e.region = MosfetRegion::Cutoff;
            e.valid = true;
            return e;
        }

        const double onePlusLambdaVds = 1.0 + p.lambda * vds;
        if (!std::isfinite(onePlusLambdaVds))
            return e;

        e.region = classifyRegion(vov, vds);
        if (e.region == MosfetRegion::Ohmic) {
            const double u = vov * vds - 0.5 * vds * vds;
            e.id = p.K * u * onePlusLambdaVds;
            e.did_ds = p.K * ((vov - vds) * onePlusLambdaVds + p.lambda * u);
            e.did_gs = p.K * vds * onePlusLambdaVds;
        } else {
            e.id = 0.5 * p.K * vov * vov * onePlusLambdaVds;
            e.did_gs = p.K * vov * onePlusLambdaVds;
            e.did_ds = 0.5 * p.K * vov * vov * p.lambda;
        }

        e.valid = std::isfinite(e.id) && std::isfinite(e.did_ds) && std::isfinite(e.did_gs);
        if (!e.valid)
            e.region = MosfetRegion::Invalid;
        return e;
    }

    /// The paper's forward law plus the source/drain-reversed extension defined
    /// in the header comment. This is an implementation decision, not a branch
    /// derived by the paper.
    MosfetEvaluation evaluate(double vgs, double vds) const noexcept {
        if (!p.pChannel)
            return evaluateN(vgs, vds);

        const MosfetEvaluation r = evaluateN(-vgs, -vds);
        if (!r.valid)
            return {};
        MosfetEvaluation e = r;
        e.id = -r.id;
        e.valid = std::isfinite(e.id) && std::isfinite(e.did_ds) &&
                  std::isfinite(e.did_gs);
        if (!e.valid)
            e.region = MosfetRegion::Invalid;
        return e;
    }

private:
    /// The n-channel law plus its source/drain-reversed extension. Public
    /// `evaluate` applies the P-channel mirror around this function.
    MosfetEvaluation evaluateN(double vgs, double vds) const noexcept {
        if (!valid() || !std::isfinite(vgs) || !std::isfinite(vds))
            return {};
        if (vds >= 0.0)
            return evaluateForward(vgs, vds);

        const MosfetEvaluation r = evaluateForward(vgs - vds, -vds);
        if (!r.valid)
            return {};

        MosfetEvaluation e;
        e.id = -r.id;
        e.did_gs = -r.did_gs;
        e.did_ds = r.did_gs + r.did_ds;
        e.region = r.region;
        e.valid = std::isfinite(e.id) && std::isfinite(e.did_ds) && std::isfinite(e.did_gs);
        if (!e.valid)
            e.region = MosfetRegion::Invalid;
        return e;
    }

public:

    /// Closed-form solve in one fixed orientation, using the oriented variables
    /// `A = voDS`, `B = voGS - Vth`, `zd = Z[ds,ds]`, `zg = Z[gs,ds]`. The
    /// returned current is the oriented current. `solveForwardGroup` supplies
    /// the paper orientation; `solveReverseGroup` supplies the transformed
    /// variables derived above.
    MosfetGroupSolution solveOrientedGroup(double A, double B, double zd, double zg,
                                           double seedId) const noexcept {
        MosfetGroupSolution out;
        if (!valid() || !std::isfinite(A) || !std::isfinite(B) ||
            !std::isfinite(zd) || !std::isfinite(zg))
            return out;

        const double seed = std::isfinite(seedId) ? seedId : 0.0;
        const double voG = B + p.Vth;
        const double L0 = 1.0 + p.lambda * A;
        const double L1 = -p.lambda * zd;
        if (!std::isfinite(voG) || !std::isfinite(L0) || !std::isfinite(L1))
            return out;

        double bestResidual = std::numeric_limits<double>::infinity();
        double bestId = 0.0;
        MosfetRegion bestRegion = MosfetRegion::Invalid;

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

        const auto consider = [&](double id, MosfetRegion expected) {
            if (!std::isfinite(id))
                return;
            double vds = A - zd * id;
            const double vov = B - zg * id;
            const double vgs = voG - zg * id;
            if (!std::isfinite(vds) || !std::isfinite(vov) || !std::isfinite(vgs))
                return;
            const double vdsTol = 1.0e-12 * (1.0 + std::abs(A) + std::abs(zd * id));
            if (vds < -vdsTol)
                return;
            if (vds < 0.0)
                vds = 0.0;
            const MosfetRegion actualRegion = classifyRegion(vov, vds);
            if (actualRegion != expected)
                return;
            const MosfetEvaluation e = evaluateForward(vgs, vds);
            if (!e.valid || e.region != expected)
                return;
            const double residual = std::abs(e.id - id);
            if (residual > 1.0e-7 * (1.0 + std::abs(id)))
                return;
            if (better(id, residual)) {
                bestResidual = residual;
                bestId = id;
                bestRegion = expected;
            }
        };

        // Zero is the closest possible root to a cold start, so the common
        // cut-off case avoids both cubics.
        consider(0.0, MosfetRegion::Cutoff);
        if (bestRegion == MosfetRegion::Cutoff && seed == 0.0) {
            out.ok = true;
            out.id = 0.0;
            out.region = MosfetRegion::Cutoff;
            out.residual = 0.0;
            return out;
        }

        double roots[3];

        // Saturation cubic.
        {
            const double q2 = 0.5 * p.K * zg * zg;
            const double q1 = -p.K * zg * B;
            const double q0 = 0.5 * p.K * B * B;
            const double c3 = q2 * L1;
            const double c2 = q2 * L0 + q1 * L1;
            const double c1 = q1 * L0 + q0 * L1 - 1.0;
            const double c0 = q0 * L0;
            const int n = poly::realCubicRoots(c3, c2, c1, c0, roots);
            for (int i = 0; i < n; ++i)
                consider(roots[i], MosfetRegion::Saturation);
        }

        // Ohmic cubic.
        {
            const double u0 = B * A - 0.5 * A * A;
            const double u1 = -B * zd - A * zg + A * zd;
            const double u2 = zg * zd - 0.5 * zd * zd;
            const double c3 = p.K * u2 * L1;
            const double c2 = p.K * (u2 * L0 + u1 * L1);
            const double c1 = p.K * (u1 * L0 + u0 * L1) - 1.0;
            const double c0 = p.K * u0 * L0;
            const int n = poly::realCubicRoots(c3, c2, c1, c0, roots);
            for (int i = 0; i < n; ++i)
                consider(roots[i], MosfetRegion::Ohmic);
        }

        if (std::isfinite(bestResidual)) {
            out.ok = true;
            out.id = bestId;
            out.region = bestRegion;
            out.residual = bestResidual;
        }
        return out;
    }

    /// Paper orientation, `vDS >= 0`: `A=voDS`, `B=voGS-Vth`.
    MosfetGroupSolution solveForwardGroup(double voDs, double voGs, double zdd, double zgd,
                                          double seedId) const noexcept {
        if (!valid() || !std::isfinite(voDs) || !std::isfinite(voGs) ||
            !std::isfinite(zdd) || !std::isfinite(zgd))
            return {};
        return solveOrientedGroup(voDs, voGs - p.Vth, zdd, zgd, seedId);
    }

    /// Source/drain-reversed orientation, derived explicitly from the terminal
    /// interchange in the header comment. The returned current is restored to
    /// the original drain-to-source direction.
    MosfetGroupSolution solveReverseGroup(double voDs, double voGs, double zdd, double zgd,
                                          double seedId) const noexcept {
        if (!valid() || !std::isfinite(voDs) || !std::isfinite(voGs) ||
            !std::isfinite(zdd) || !std::isfinite(zgd))
            return {};

        // Reversed terminal definitions:
        //     Ar  = -voDs
        //     Br  = (voGs - voDs) - Vth   = voGD - Vth
        //     zddr = zdd
        //     zgdr = zdd - zgd
        //     seedr = -seed
        // The oriented law is evaluated in these variables; only afterwards is
        // the physical drain-to-source current restored as Id = -Idr.
        const double Ar = -voDs;
        const double Br = (voGs - voDs) - p.Vth;
        const double zddr = zdd;
        const double zgdr = zdd - zgd;
        const double seedr = -seedId;
        MosfetGroupSolution out =
            solveOrientedGroup(Ar, Br, zddr, zgdr, seedr);
        if (out.ok)
            out.id = -out.id;
        return out;
    }

    /// Closed-form two-port group solve. The forward and reversed orientations
    /// are solved separately and each candidate is required to land in its own
    /// `vDS` half-plane. `seedId` chooses between any remaining multiple roots
    /// by branch continuity.
    MosfetGroupSolution solveGroup(double voDs, double voGs, double zdd, double zgd,
                                   double seedId = 0.0) const noexcept {
        if (!valid() || !std::isfinite(voDs) || !std::isfinite(voGs) ||
            !std::isfinite(zdd) || !std::isfinite(zgd))
            return {};
        if (!p.pChannel)
            return solveGroupN(voDs, voGs, zdd, zgd, seedId);

        // The P-channel mirror applied to the complete two-port group solve.
        // The port impedance matrix is unchanged; only the independent
        // voltages, seed and solved current change sign.
        const double seed = std::isfinite(seedId) ? seedId : 0.0;
        MosfetGroupSolution out = solveGroupN(-voDs, -voGs, zdd, zgd, -seed);
        if (out.ok)
            out.id = -out.id;
        return out;
    }

private:
    MosfetGroupSolution solveGroupN(double voDs, double voGs, double zdd, double zgd,
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
            return solveReverseGroup(voDs, voGs, zdd, zgd, seed);
        }

        const MosfetGroupSolution forward = solveForwardGroup(voDs, voGs, zdd, zgd, seed);
        const MosfetGroupSolution reverse = solveReverseGroup(voDs, voGs, zdd, zgd, seed);

        MosfetGroupSolution best;
        bool have = false;
        const auto consider = [&](const MosfetGroupSolution& candidate, bool isForward) {
            if (!candidate.ok || !std::isfinite(candidate.id))
                return;
            if (!have) {
                best = candidate;
                have = true;
                return;
            }
            const double d = std::abs(candidate.id - seed);
            const double bd = std::abs(best.id - seed);
            const double tol = 1.0e-14 * (1.0 + std::max(d, bd));
            if (d < bd - tol) {
                best = candidate;
            } else if (d <= bd + tol && isForward && !best.usedReference) {
                // A zero-width tie at vDS = 0 is the same physical point. Keep
                // the forward orientation so the result is deterministic.
                best = candidate;
            }
        };
        consider(forward, true);
        consider(reverse, false);
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
    MosfetRootSet solveReferenceRoots(double voDs, double voGs, double zdd, double zgd,
                                      double xLo, double xHi, int scan = 256) const noexcept {
        MosfetRootSet out;
        if (!valid() || !std::isfinite(voDs) || !std::isfinite(voGs) ||
            !std::isfinite(zdd) || !std::isfinite(zgd) || !std::isfinite(xLo) ||
            !std::isfinite(xHi) || !(xHi > xLo) || scan < 4)
            return out;

        const auto addRoot = [&](double id) {
            if (!std::isfinite(id))
                return;
            const double vds = voDs - zdd * id;
            const double vgs = voGs - zgd * id;
            const MosfetEvaluation e = evaluate(vgs, vds);
            if (!e.valid || e.region == MosfetRegion::Invalid)
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
            MosfetGroupSolution root;
            root.ok = true;
            root.id = id;
            root.region = e.region;
            root.residual = residual;
            out.roots[static_cast<size_t>(out.count++)] = root;
        };

        const auto g = [&](double id) {
            const double vds = voDs - zdd * id;
            const double vgs = voGs - zgd * id;
            const MosfetEvaluation e = evaluate(vgs, vds);
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
        // that a coarse scan steps over without seeing a sign change. The full
        // scan still covers roots that only exist far from the origin.
        double xNear = std::max(100.0 * p.K * (1.0 + std::abs(voGs - p.Vth) + std::abs(voDs)),
                                1.0e-12);
        if (std::abs(zdd) > 1.0e-9)
            xNear = std::max(xNear, 10.0 * std::abs(voDs) / std::abs(zdd));
        xNear = std::min(xNear, 1.0e6);
        const double nearLo = std::max(xLo, -xNear);
        const double nearHi = std::min(xHi, xNear);
        if (nearHi > nearLo)
            scanRange(nearLo, nearHi, std::max(64, scan / 2));
        const double xTiny = std::min(xNear, std::max(20.0 * p.K, 1.0e-6));
        const double tinyLo = std::max(xLo, -xTiny);
        const double tinyHi = std::min(xHi, xTiny);
        if (tinyHi > tinyLo)
            scanRange(tinyLo, tinyHi, std::max(512, scan * 2));
        scanRange(xLo, xHi, scan);
        return out;
    }

    MosfetRootSet solveReferenceRoots(double voDs, double voGs, double zdd, double zgd) const noexcept {
        double xMax = 1000.0 * p.K;
        if (std::abs(zdd) > 1.0e-9)
            xMax = std::max(xMax, 10.0 * std::abs(voDs) / std::abs(zdd));
        if (std::abs(zgd) > 1.0e-6)
            xMax = std::max(xMax, 10.0 * std::abs(voGs) / std::abs(zgd));
        xMax = std::min(xMax, 1.0e6);
        if (!(xMax > 0.0) || !std::isfinite(xMax))
            xMax = 1.0;
        return solveReferenceRoots(voDs, voGs, zdd, zgd, -xMax, xMax);
    }

    /// Select one root from the independently found reference set using the
    /// same continuity policy as the closed form.
    MosfetGroupSolution solveReference(double voDs, double voGs, double zdd, double zgd,
                                       double xLo, double xHi, int scan = 256,
                                       double seedId = 0.0) const noexcept {
        const MosfetRootSet roots = solveReferenceRoots(voDs, voGs, zdd, zgd, xLo, xHi, scan);
        MosfetGroupSolution best;
        if (roots.count == 0)
            return best;
        const double seed = std::isfinite(seedId) ? seedId : 0.0;
        bool have = false;
        for (int i = 0; i < roots.count; ++i) {
            const MosfetGroupSolution& candidate = roots.roots[static_cast<size_t>(i)];
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

    MosfetGroupSolution solveReferenceSeeded(double voDs, double voGs, double zdd, double zgd,
                                             double seedId) const noexcept {
        double xMax = 1000.0 * p.K;
        if (std::abs(zdd) > 1.0e-9)
            xMax = std::max(xMax, 10.0 * std::abs(voDs) / std::abs(zdd));
        if (std::abs(zgd) > 1.0e-6)
            xMax = std::max(xMax, 10.0 * std::abs(voGs) / std::abs(zgd));
        xMax = std::min(xMax, 1.0e6);
        if (!(xMax > 0.0) || !std::isfinite(xMax))
            xMax = 1.0;
        return solveReference(voDs, voGs, zdd, zgd, -xMax, xMax, 256, seedId);
    }

    MosfetGroupSolution solveReference(double voDs, double voGs, double zdd, double zgd) const noexcept {
        return solveReferenceSeeded(voDs, voGs, zdd, zgd, 0.0);
    }
};

}  // namespace wdf
