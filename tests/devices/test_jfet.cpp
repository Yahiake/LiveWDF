#include <catch2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "devices/Jfet.h"
#include "format/LiveWDFFile.h"
#include "format/TransistorPolarity.h"
#include "spice/SpiceNet.h"
#include "wdf/Engine.h"

// The general three-terminal JFET, after Bernardini, Vergani and Sarti (2020).
//
// The checks are deliberately split into independent layers:
//
//   1. the constitutive law and its analytic derivatives;
//   2. the scalar group solver against an independent bracketed reference over
//      a broad bias grid, with the original region inequalities checked on every
//      accepted point;
//   3. the polynomial root helper at degenerate coefficients and discriminants;
//   4. the engine's DC Newton path, the closed-form sample path, and their
//      agreement on a real circuit;
//   5. the parser/document path, including what must be refused.
//
// The reference solver does not use the polynomial. If both agree, that is
// evidence about the algebra; if they disagree, the test fails rather than
// choosing one silently.

namespace {

constexpr double kFs = 48000.0;

wdf::JfetModel modelWithLambda(double lambda) {
    return wdf::JfetModel::fromSpice(1.0e-3, -2.0, lambda, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0e9);
}

wdf::JfetModel cleanModel() { return modelWithLambda(0.0); }

bool regionInequalities(const wdf::JfetParams& p, wdf::JfetRegion region, double vgs, double vds) {
    // The device law's region names are in the forward orientation. For
    // negative VDS the implementation swaps source and drain, so the same
    // swap is applied here before checking the paper's inequalities.
    if (vds < 0.0) {
        vgs -= vds;
        vds = -vds;
    }
    const double vov = vgs - p.Vp;
    switch (region) {
    case wdf::JfetRegion::Cutoff:
        return !(vov > 0.0);
    case wdf::JfetRegion::Ohmic:
        return vov > 0.0 && vds < vov;
    case wdf::JfetRegion::Saturation:
        return vov > 0.0 && vds >= vov;
    case wdf::JfetRegion::Invalid:
        return false;
    }
    return false;
}

struct BiasCase {
    double voDs;
    double voGs;
    double zdd;
    double zgd;
};

}  // namespace

TEST_CASE("2SK184-GR preset reproduces its selected datasheet bias point", "[jfet][presets]") {
    const auto* part = wdf::findJfetByName("2SK184-GR");
    REQUIRE(part != nullptr);
    const auto atZeroGate = part->model.evaluate(0.0, 10.0);
    REQUIRE(atZeroGate.valid);
    CHECK(atZeroGate.id == Approx(4.2e-3));
    CHECK(atZeroGate.did_gs == Approx(15.3e-3).margin(0.1e-3));
    CHECK(part->model.evaluate(-0.6, 10.0).region == wdf::JfetRegion::Cutoff);
}

TEST_CASE("JFET equations 45-47 have the paper's regions and values", "[jfet][device]") {
    const wdf::JfetModel m = wdf::k2N5952;
    REQUIRE(m.valid());
    REQUIRE(m.p.Is0 == Approx(5.367e-3).epsilon(1e-12));
    REQUIRE(m.p.Vp == Approx(-2.021));
    REQUIRE(m.p.lambda == Approx(4.0e-3));

    const double k = 2.0 * m.p.Is0 / (m.p.Vp * m.p.Vp);

    // Cut-off: vAB <= Vp, Id exactly zero.
    for (double vgs : {-4.0, -3.0, m.p.Vp}) {
        const auto e = m.evaluate(vgs, 5.0);
        REQUIRE(e.valid);
        REQUIRE(e.region == wdf::JfetRegion::Cutoff);
        REQUIRE(e.id == 0.0);
        REQUIRE(e.did_ds == 0.0);
        REQUIRE(e.did_gs == 0.0);
    }

    // Ohmic: phi1 > 0, phi2 < phi1.
    {
        const double vgs = -1.0;
        const double vds = 0.4;
        const double vov = vgs - m.p.Vp;
        const double u = vov * vds - 0.5 * vds * vds;
        const double ref = k * u * (1.0 + m.p.lambda * vds);
        const auto e = m.evaluate(vgs, vds);
        REQUIRE(e.valid);
        REQUIRE(e.region == wdf::JfetRegion::Ohmic);
        REQUIRE(e.id == Approx(ref).margin(std::abs(ref) * 1e-14 + 1e-18));
        REQUIRE(regionInequalities(m.p, e.region, vgs, vds));
    }

    // Saturation: phi1 > 0, phi2 >= phi1.
    {
        const double vgs = -1.0;
        const double vds = 5.0;
        const double a = 1.0 - vgs / m.p.Vp;
        const double ref = m.p.Is0 * a * a * (1.0 + m.p.lambda * vds);
        const auto e = m.evaluate(vgs, vds);
        REQUIRE(e.valid);
        REQUIRE(e.region == wdf::JfetRegion::Saturation);
        REQUIRE(e.id == Approx(ref).margin(std::abs(ref) * 1e-14 + 1e-18));
        REQUIRE(regionInequalities(m.p, e.region, vgs, vds));
    }

    // The paper states strict inequalities and leaves equality unspecified. The
    // law is continuous there, so the implementation assigns equality to
    // saturation (and vGS = Vp to cutoff) for deterministic region reporting.
    {
        const double vgs = -1.0;
        const double vov = vgs - m.p.Vp;
        const auto below = m.evaluate(vgs, vov - 1e-9);
        const auto at = m.evaluate(vgs, vov);
        const auto above = m.evaluate(vgs, vov + 1e-9);
        REQUIRE(below.region == wdf::JfetRegion::Ohmic);
        REQUIRE(at.region == wdf::JfetRegion::Saturation);
        REQUIRE(above.region == wdf::JfetRegion::Saturation);
        REQUIRE(below.id == Approx(at.id).margin(1e-10));
        REQUIRE(at.id == Approx(above.id).margin(1e-10));
    }

    // Negative vDS uses the documented source/drain-reversed extension, not a
    // literal evaluation of (46) at negative phi2. The reverse-oriented law is
    // the same forward law with vGS' = vGS - vDS and vDS' = -vDS.
    {
        const double vgs = -1.0;
        const double vds = -1.0;
        const auto e = m.evaluate(vgs, vds);
        const auto r = m.evaluateForward(vgs - vds, -vds);
        REQUIRE(e.valid);
        REQUIRE(r.valid);
        REQUIRE(e.region == r.region);
        REQUIRE(e.id == Approx(-r.id).margin(std::abs(r.id) * 1e-13 + 1e-18));
        REQUIRE(e.did_gs == Approx(-r.did_gs).margin(std::abs(r.did_gs) * 1e-13 + 1e-18));
        REQUIRE(e.did_ds == Approx(r.did_gs + r.did_ds)
                                .margin((std::abs(r.did_gs) + std::abs(r.did_ds)) * 1e-13 +
                                        1e-18));
        REQUIRE(regionInequalities(m.p, e.region, vgs, vds));
    }

    // The extension must be continuous through vDS = 0 in current and in both
    // derivatives. A one-sided difference across zero is the check that catches
    // a wrong sign in the chain rule.
    {
        const double vgs = -1.0;
        const double h = 1e-7;
        const auto at = m.evaluate(vgs, 0.0);
        const auto plus = m.evaluate(vgs, h);
        const auto minus = m.evaluate(vgs, -h);
        REQUIRE(at.valid);
        REQUIRE(plus.valid);
        REQUIRE(minus.valid);
        REQUIRE(plus.id == Approx(-minus.id).margin(1e-12));
        REQUIRE(plus.did_ds == Approx(minus.did_ds).margin(1e-8));
        REQUIRE(plus.did_gs == Approx(minus.did_gs).margin(1e-8));
    }
}

TEST_CASE("JFET analytic derivatives match finite differences away from boundaries",
          "[jfet][device]") {
    const wdf::JfetModel m = wdf::k2N5952;
    const double h = 1e-6;

    const auto check = [&](double vgs, double vds) {
        const auto e = m.evaluate(vgs, vds);
        REQUIRE(e.valid);
        const auto gsP = m.evaluate(vgs + h, vds);
        const auto gsM = m.evaluate(vgs - h, vds);
        const auto dsP = m.evaluate(vgs, vds + h);
        const auto dsM = m.evaluate(vgs, vds - h);
        REQUIRE(gsP.valid);
        REQUIRE(gsM.valid);
        REQUIRE(dsP.valid);
        REQUIRE(dsM.valid);
        const double fdGs = (gsP.id - gsM.id) / (2.0 * h);
        const double fdDs = (dsP.id - dsM.id) / (2.0 * h);
        REQUIRE(e.did_gs == Approx(fdGs).margin(std::abs(fdGs) * 2e-6 + 1e-18));
        REQUIRE(e.did_ds == Approx(fdDs).margin(std::abs(fdDs) * 2e-6 + 1e-18));
    };

    check(-1.0, 0.4);   // ohmic
    check(-0.5, 0.2);   // ohmic
    check(-1.0, 4.0);   // saturation
    check(-0.25, 3.0);  // saturation
    check(-1.0, -1.0);  // reverse ohmic
    check(-0.5, -0.2);  // reverse ohmic
    check(-3.0, -1.0);  // reverse saturation
}

TEST_CASE("JFET closed form matches an independent bracketed solve across bias",
          "[jfet][solver]") {
    for (double lambda : {0.0, 4.0e-3}) {
        const wdf::JfetModel m = modelWithLambda(lambda);
        for (double voDs : {-1.0, 0.0, 0.5, 2.0, 5.0, 10.0}) {
            for (double voGs : {-4.0, -2.5, -2.0, -1.0, 0.0}) {
                for (double zdd : {10.0, 100.0, 1000.0}) {
                    for (double zgd : {0.0, 10.0, 100.0}) {
                        INFO("lambda=" << lambda << " voDs=" << voDs << " voGs=" << voGs
                                       << " zdd=" << zdd << " zgd=" << zgd);
                        const BiasCase bc{voDs, voGs, zdd, zgd};
                        const auto fast = m.solveGroup(bc.voDs, bc.voGs, bc.zdd, bc.zgd);
                        const auto ref = m.solveReference(bc.voDs, bc.voGs, bc.zdd, bc.zgd);
                        REQUIRE(fast.ok);
                        REQUIRE(ref.ok);
                        REQUIRE(fast.id == Approx(ref.id).margin(2e-9 * (1.0 + std::abs(ref.id))));

                        const double vds = bc.voDs - bc.zdd * fast.id;
                        const double vgs = bc.voGs - bc.zgd * fast.id;
                        // On the exact ohmic/saturation boundary the label is down to rounding.
                        const double vov = (vds < 0.0 ? vgs - vds : vgs) - m.p.Vp;
                        const bool onBoundary = fast.region != wdf::JfetRegion::Cutoff &&
                                                std::abs(std::abs(vds) - vov) <= 1e-12;
                        REQUIRE((onBoundary || regionInequalities(m.p, fast.region, vgs, vds)));
                        const auto e = m.evaluate(vgs, vds);
                        REQUIRE(e.valid);
                        REQUIRE((onBoundary || e.region == fast.region));
                        REQUIRE(e.id == Approx(fast.id).margin(2e-8 * (1.0 + std::abs(fast.id))));
                    }
                }
            }
        }
    }
}

TEST_CASE("JFET negative VDS uses the source/drain-reversed law", "[jfet][device]") {
    const wdf::JfetModel m = modelWithLambda(4.0e-3);
    for (double vgs : {-3.0, -2.0, -1.0, 0.0}) {
        for (double vds : {-0.01, -0.1, -1.0, -5.0}) {
            INFO("vgs=" << vgs << " vds=" << vds);
            const auto e = m.evaluate(vgs, vds);
            const auto r = m.evaluateForward(vgs - vds, -vds);
            REQUIRE(e.valid);
            REQUIRE(r.valid);
            REQUIRE(e.region == r.region);
            REQUIRE(e.id == Approx(-r.id).margin(std::abs(r.id) * 1e-13 + 1e-18));
            REQUIRE(e.did_gs == Approx(-r.did_gs)
                                    .margin(std::abs(r.did_gs) * 1e-13 + 1e-18));
            REQUIRE(e.did_ds == Approx(r.did_gs + r.did_ds)
                                    .margin((std::abs(r.did_gs) + std::abs(r.did_ds)) *
                                                1e-13 +
                                            1e-18));
            REQUIRE(regionInequalities(m.p, e.region, vgs, vds));
        }
    }
}

TEST_CASE("JFET reverse-bias group solve rejects the extrapolated far root",
          "[jfet][solver]") {
    // Regression: with zgd = 0 and a negative open-circuit drain voltage, a
    // literal evaluation of (46) at negative VDS produced a small
    // reverse-current root near zero and a far root at roughly +0.25 A. The
    // source/drain-reversed extension must keep only the near root.
    const wdf::JfetModel m = modelWithLambda(4.0e-3);
    const auto fast = m.solveGroup(-1.0, -1.0, 1000.0, 0.0);
    const auto ref = m.solveReference(-1.0, -1.0, 1000.0, 0.0);
    REQUIRE(fast.ok);
    REQUIRE(ref.ok);
    REQUIRE(std::abs(fast.id) < 1.0e-2);
    REQUIRE(fast.id == Approx(ref.id).margin(2e-9 * (1.0 + std::abs(ref.id))));
    REQUIRE(fast.region == wdf::JfetRegion::Ohmic);

    const auto roots = m.solveReferenceRoots(-1.0, -1.0, 1000.0, 0.0);
    REQUIRE(roots.count >= 1);
    for (int i = 0; i < roots.count; ++i)
        REQUIRE(std::abs(roots.roots[static_cast<size_t>(i)].id) < 1.0e-2);

    const double vds = -1.0 - 1000.0 * fast.id;
    const double vgs = -1.0;
    REQUIRE(vds < 0.0);
    REQUIRE(regionInequalities(m.p, fast.region, vgs, vds));
}

TEST_CASE("JFET group solve selects the root nearest the previous branch current",
          "[jfet][solver]") {
    // This bias has three forward roots. A cold start chooses the smallest
    // |Id|; a warm start must follow the branch it came from. The roots are
    // found by the independent bracketed reference, not by the cubic.
    const wdf::JfetModel m = wdf::k2N5952;
    const auto roots = m.solveReferenceRoots(0.65, -2.0, 100.0, -1000.0);
    REQUIRE(roots.count >= 2);

    const auto nearestRoot = [&](double seed) {
        double best = 0.0;
        double bestDist = std::numeric_limits<double>::infinity();
        for (int i = 0; i < roots.count; ++i) {
            const double id = roots.roots[static_cast<size_t>(i)].id;
            const double d = std::abs(id - seed);
            if (d < bestDist) {
                bestDist = d;
                best = id;
            }
        }
        return best;
    };

    for (double seed : {-1.0, 0.0, 0.001, 0.003, 1.0}) {
        INFO("seed=" << seed);
        const auto s = m.solveGroup(0.65, -2.0, 100.0, -1000.0, seed);
        REQUIRE(s.ok);
        const double expected = nearestRoot(seed);
        REQUIRE(s.id == Approx(expected).margin(2e-8 * (1.0 + std::abs(expected))));

        bool found = false;
        for (int i = 0; i < roots.count; ++i)
            if (std::abs(s.id - roots.roots[static_cast<size_t>(i)].id) <=
                2e-8 * (1.0 + std::abs(s.id)))
                found = true;
        REQUIRE(found);

        const double vds = 0.65 - 100.0 * s.id;
        const double vgs = -2.0 + 1000.0 * s.id;
        REQUIRE(regionInequalities(m.p, s.region, vgs, vds));
        const auto e = m.evaluate(vgs, vds);
        REQUIRE(e.valid);
        REQUIRE(e.region == s.region);
        REQUIRE(e.id == Approx(s.id).margin(2e-8 * (1.0 + std::abs(s.id))));
    }
}

TEST_CASE("JFET group solve keeps both orientations for an active port",
          "[jfet][solver]") {
    // A negative port impedance is outside the passive load-line argument used
    // by the one-orientation fast path. This case has one forward root and one
    // source/drain-reversed root; the cold-start nearest-current policy must
    // pick the smaller-magnitude one, and the independent reference must agree.
    const wdf::JfetModel m = wdf::k2N5952;
    const auto fast = m.solveGroup(-5.0, -4.0, -1000.0, -1000.0, 0.0);
    const auto ref = m.solveReference(-5.0, -4.0, -1000.0, -1000.0);
    REQUIRE(fast.ok);
    REQUIRE(ref.ok);
    REQUIRE(fast.id == Approx(ref.id).margin(2e-9 * (1.0 + std::abs(ref.id))));
    REQUIRE(fast.id == Approx(0.005640723606711065).margin(1e-9));

    const auto roots = m.solveReferenceRoots(-5.0, -4.0, -1000.0, -1000.0);
    REQUIRE(roots.count >= 2);
    bool sawPositive = false;
    bool sawNegative = false;
    for (int i = 0; i < roots.count; ++i) {
        const double id = roots.roots[static_cast<size_t>(i)].id;
        sawPositive = sawPositive || id > 0.0;
        sawNegative = sawNegative || id < 0.0;
    }
    REQUIRE(sawPositive);
    REQUIRE(sawNegative);
}

TEST_CASE("JFET group solve is continuous through VDS = 0", "[jfet][solver]") {
    const wdf::JfetModel m = modelWithLambda(4.0e-3);
    for (double startSeed : {-1.0e-3, -1.0e-6, 0.0, 1.0e-6, 1.0e-3}) {
        double seed = startSeed;
        double previous = 0.0;
        bool first = true;
        for (int k = -100; k <= 100; ++k) {
            const double voDs = 0.01 * static_cast<double>(k);
            INFO("seed0=" << startSeed << " voDs=" << voDs);
            const auto fast = m.solveGroup(voDs, -1.0, 1000.0, 0.0, seed);
            const auto ref = m.solveReferenceSeeded(voDs, -1.0, 1000.0, 0.0, seed);
            REQUIRE(fast.ok);
            REQUIRE(ref.ok);
            REQUIRE(fast.id == Approx(ref.id).margin(2e-9 * (1.0 + std::abs(ref.id))));

            const double vds = voDs - 1000.0 * fast.id;
            if (k < 0)
                REQUIRE(vds <= 1e-12);
            if (k > 0)
                REQUIRE(vds >= -1e-12);
            if (k == 0) {
                REQUIRE(std::abs(vds) <= 1e-12);
                REQUIRE(std::abs(fast.id) <= 1e-12);
            }
            REQUIRE(regionInequalities(m.p, fast.region, -1.0, vds));
            if (!first)
                REQUIRE(std::abs(fast.id - previous) < 1e-4);
            previous = fast.id;
            seed = fast.id;
            first = false;
        }
    }

    // Tiny excursions on both sides of zero must not produce a visible jump or
    // a wrong half-plane root. These are much smaller than the 0.01 V sweep.
    for (double voDs : {-1.0e-12, -1.0e-9, -1.0e-6, 0.0, 1.0e-6, 1.0e-9, 1.0e-12}) {
        INFO("tiny voDs=" << voDs);
        const auto fast = m.solveGroup(voDs, -1.0, 1000.0, 0.0, 0.0);
        const auto ref = m.solveReferenceSeeded(voDs, -1.0, 1000.0, 0.0, 0.0);
        REQUIRE(fast.ok);
        REQUIRE(ref.ok);
        REQUIRE(fast.id == Approx(ref.id).margin(2e-12 * (1.0 + std::abs(ref.id))));
        REQUIRE(std::abs(fast.id) <= 1.0e-8);
        const double vds = voDs - 1000.0 * fast.id;
        REQUIRE(std::abs(vds) <= std::abs(voDs) + 1.0e-12);
        REQUIRE(regionInequalities(m.p, fast.region, -1.0, vds));
    }
}

TEST_CASE("JFET group solve handles degenerate coefficients and extreme values",
          "[jfet][solver]") {
    // lambda = 0 removes the cubic term; zgd = 0 removes the saturation cubic
    // term and the gate coupling. Both must still solve.
    {
        const wdf::JfetModel m = cleanModel();
        const auto s = m.solveGroup(5.0, -1.0, 100.0, 0.0);
        REQUIRE(s.ok);
        REQUIRE(s.region == wdf::JfetRegion::Saturation);
    }
    {
        const wdf::JfetModel m = modelWithLambda(4e-3);
        const auto s = m.solveGroup(5.0, -1.0, 100.0, 0.0);
        REQUIRE(s.ok);
    }

    // Very small Is0 and a short-channel Vp: the polynomial coefficients are
    // tiny, so the root finder must scale rather than declare a zero cubic.
    {
        const wdf::JfetModel m =
            wdf::JfetModel::fromSpice(1e-9, -0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1e9);
        const auto s = m.solveGroup(1.0, -0.05, 1000.0, 100.0);
        REQUIRE(s.ok);
        REQUIRE(std::isfinite(s.id));
        const double vds = 1.0 - 1000.0 * s.id;
        const double vgs = -0.05 - 100.0 * s.id;
        REQUIRE(regionInequalities(m.p, s.region, vgs, vds));
    }

    // Extreme but finite terminal voltages must not produce NaN/Inf. They may
    // produce a valid root; if the law's own arithmetic overflows, the result
    // must say invalid rather than a plausible current.
    {
        const wdf::JfetModel m = wdf::k2N5952;
        for (const auto& bc : {BiasCase{1e3, 1e3, 100.0, 10.0},
                               BiasCase{-1e3, -1e3, 100.0, 10.0},
                               BiasCase{1e-9, -3.0, 1e-9, 1e-9}}) {
            const auto s = m.solveGroup(bc.voDs, bc.voGs, bc.zdd, bc.zgd);
            if (s.ok) {
                REQUIRE(std::isfinite(s.id));
                const double vds = bc.voDs - bc.zdd * s.id;
                const double vgs = bc.voGs - bc.zgd * s.id;
                REQUIRE(regionInequalities(m.p, s.region, vgs, vds));
            }
        }
    }

    // Invalid parameters and non-finite inputs fail explicitly.
    {
        wdf::JfetParams bad = wdf::k2N5952Params;
        bad.Vp = 1.0;
        const wdf::JfetModel m(bad);
        REQUIRE_FALSE(m.valid());
        const auto s = m.solveGroup(1.0, -1.0, 100.0, 10.0);
        REQUIRE_FALSE(s.ok);
    }
    {
        const wdf::JfetModel m = wdf::k2N5952;
        REQUIRE_FALSE(m.solveGroup(std::numeric_limits<double>::quiet_NaN(), -1.0, 100.0, 10.0).ok);
        REQUIRE_FALSE(m.solveGroup(1.0, std::numeric_limits<double>::infinity(), 100.0, 10.0).ok);
        REQUIRE_FALSE(m.solveReference(1.0, -1.0, 100.0, 10.0, 1.0, -1.0).ok);
    }
}

TEST_CASE("cubic and quadratic root helpers handle repeated and near-zero roots",
          "[jfet][solver]") {
    double r[3];
    const auto sorted = [&](int n) {
        std::vector<double> v(r, r + n);
        std::sort(v.begin(), v.end());
        return v;
    };

    REQUIRE(wdf::jfet_detail::realCubicRoots(1.0, -6.0, 11.0, -6.0, r) == 3);
    {
        const auto v = sorted(3);
        REQUIRE(v[0] == Approx(1.0).margin(1e-12));
        REQUIRE(v[1] == Approx(2.0).margin(1e-12));
        REQUIRE(v[2] == Approx(3.0).margin(1e-12));
    }

    // (x-1)^2 (x-2): one double root and one simple root.
    REQUIRE(wdf::jfet_detail::realCubicRoots(1.0, -4.0, 5.0, -2.0, r) == 2);
    {
        const auto v = sorted(2);
        REQUIRE(v[0] == Approx(1.0).margin(1e-10));
        REQUIRE(v[1] == Approx(2.0).margin(1e-10));
    }

    // (x-1)^3: a triple root.
    REQUIRE(wdf::jfet_detail::realCubicRoots(1.0, -3.0, 3.0, -1.0, r) == 1);
    REQUIRE(r[0] == Approx(1.0).margin(1e-10));

    // Scaling must not change which roots are found.
    REQUIRE(wdf::jfet_detail::realCubicRoots(1e-30, -6e-30, 11e-30, -6e-30, r) == 3);
    REQUIRE(wdf::jfet_detail::realCubicRoots(1e30, -6e30, 11e30, -6e30, r) == 3);

    // A zero leading coefficient degenerates to the quadratic path.
    REQUIRE(wdf::jfet_detail::realCubicRoots(0.0, 1.0, -3.0, 2.0, r) == 2);

    REQUIRE(wdf::jfet_detail::realQuadraticRoots(1.0, -2.0, 1.0, r) == 1);
    REQUIRE(r[0] == Approx(1.0).margin(1e-12));
    REQUIRE(wdf::jfet_detail::realQuadraticRoots(0.0, 1.0, -1.0, r) == 1);
    REQUIRE(r[0] == Approx(1.0).margin(1e-12));
    REQUIRE(wdf::jfet_detail::realQuadraticRoots(0.0, 0.0, 1.0, r) == 0);
}

TEST_CASE("the engine solves a JFET stage on both root paths", "[jfet][engine]") {
    const wdf::JfetModel m =
        wdf::JfetModel::fromSpice(1.0e-3, -2.0, 0.01, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0e9);

    wdf::Circuit c;
    c.addSrc("d", "0", 10.0, 0.0, {}, "Vdd");
    c.addRes("d", "jd", 1000.0, "R1");
    c.addSrc("g_src", "0", -1.0, 0.0, {}, "Vg");
    c.addRes("g_src", "g", 1000.0, "Rg");
    c.addJfet("jd", "g", "0", m, "J1");

    wdf::Engine e(c, kFs);
    REQUIRE(e.jfetCount() == 1);
    REQUIRE(e.rootCount() == 2);
    REQUIRE(e.explicitRoots());
    REQUIRE(e.rootLayout().devices.size() == 1);
    REQUIRE(e.rootLayout().devices[0].kind == wdf::RootKind::Jfet);

    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    REQUIRE(rootV.size() == 2);

    std::vector<double> i, J;
    e.currents(rootV, i, J);
    REQUIRE(i.size() == 2);
    const auto ev = m.evaluate(rootV[1], rootV[0]);
    REQUIRE(ev.valid);
    REQUIRE(ev.region == wdf::JfetRegion::Saturation);
    REQUIRE(i[0] == Approx(ev.id).margin(1e-10 * (1.0 + std::abs(ev.id))));
    REQUIRE(i[1] == 0.0);

    // The drain node is the intrinsic drain (rd = 0); the source is ground.
    // The independent port voltages must be the actual node differences, and
    // the drain resistor current must equal the terminal drain current.
    const int dIdx = e.nodeIndex("jd");
    const int gIdx = e.nodeIndex("g");
    REQUIRE(dIdx >= 0);
    REQUIRE(gIdx >= 0);
    REQUIRE(std::abs(node[static_cast<size_t>(dIdx)] - rootV[0]) < 1e-9);
    REQUIRE(std::abs(node[static_cast<size_t>(gIdx)] - rootV[1]) < 1e-9);
    REQUIRE(std::abs((10.0 - node[static_cast<size_t>(dIdx)]) / 1000.0 - i[0]) < 1e-12);
    REQUIRE(rootV[0] > 1.0);  // saturation, not the ohmic edge

    // The sample path takes the closed form. Compare it to forced Newton on a
    // driven transient; this exercises the group solve and the Newton Jacobian
    // on the same circuit rather than only the DC path.
    wdf::Engine closed(c, kFs);
    wdf::Engine newton(c, kFs);
    REQUIRE(closed.explicitRoots());
    REQUIRE(newton.setExplicitRoots(false));
    REQUIRE_FALSE(newton.explicitRoots());

    std::vector<double> nodeC, nodeN, rootC, rootN;
    REQUIRE(closed.solveDc(nodeC, rootC));
    REQUIRE(newton.solveDc(nodeN, rootN));
    closed.reset();
    newton.reset();

    for (int n = 0; n < 400; ++n) {
        const double vin = 0.25 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * n / kFs);
        const auto& yc = closed.step({vin});
        const auto& yn = newton.step({vin});
        REQUIRE(closed.lastConverged());
        REQUIRE(newton.lastConverged());
        REQUIRE(yc.size() == yn.size());
        for (size_t k = 0; k < yc.size(); ++k)
            REQUIRE(yc[k] == Approx(yn[k]).margin(1e-7));
    }
    REQUIRE(closed.jfetFallbackCount() == 0);
}

TEST_CASE("the engine follows a reverse-biased JFET stage on both root paths",
          "[jfet][engine][reverse]") {
    // Source at +5 V, drain returned to ground through 1 k, gate at +3 V:
    // VGS = -2 V and the DC solution has VDS = -1.778969 V. This is the
    // source/drain-reversed branch, not a literal negative-VDS extrapolation.
    const wdf::JfetModel m =
        wdf::JfetModel::fromSpice(1.0e-3, -2.0, 0.01, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0e9);

    wdf::Circuit c;
    c.addSrc("s", "0", 5.0, 0.0, {}, "Vdd");
    c.addRes("d", "0", 1000.0, "Rd");
    c.addSrc("g", "0", 3.0, 0.0, {}, "Vg");
    c.addJfet("d", "g", "s", m, "J1");

    wdf::Engine closed(c, kFs);
    wdf::Engine newton(c, kFs);
    REQUIRE(closed.jfetCount() == 1);
    REQUIRE(closed.rootCount() == 2);
    REQUIRE(newton.setExplicitRoots(false));

    std::vector<double> nodeC, nodeN, rootC, rootN;
    REQUIRE(closed.solveDc(nodeC, rootC));
    REQUIRE(newton.solveDc(nodeN, rootN));
    REQUIRE(nodeC.size() == nodeN.size());
    for (size_t k = 0; k < nodeC.size(); ++k)
        REQUIRE(nodeC[k] == Approx(nodeN[k]).margin(1e-7));

    // Confirm the operating point really is in the reverse half-plane and that
    // the port current is the reverse law's current, not a stale or invented
    // value. rootC[0] is VDS, rootC[1] is VGS.
    REQUIRE(rootC[0] < 0.0);
    REQUIRE(rootC[0] == Approx(-1.7789691545064195).margin(1e-5));
    const auto ev = m.evaluate(rootC[1], rootC[0]);
    REQUIRE(ev.valid);
    REQUIRE(ev.id < 0.0);
    REQUIRE(ev.region == wdf::JfetRegion::Saturation);

    // `reset()` must seed the reactive states and the branch current from this
    // DC point. A first step at the same source values should not jump.
    const int dcD = closed.nodeIndex("d");
    REQUIRE(dcD >= 0);
    closed.reset();
    newton.reset();
    const double closedDcStep = closed.step({5.0, 3.0})[static_cast<size_t>(dcD)];
    const double newtonDcStep = newton.step({5.0, 3.0})[static_cast<size_t>(dcD)];
    REQUIRE(closed.lastConverged());
    REQUIRE(newton.lastConverged());
    REQUIRE(closedDcStep == Approx(nodeC[static_cast<size_t>(dcD)]).margin(1e-9));
    REQUIRE(newtonDcStep == Approx(nodeN[static_cast<size_t>(dcD)]).margin(1e-9));

    closed.reset();
    newton.reset();
    for (int n = 0; n < 400; ++n) {
        const double gate = 3.0 + 0.1 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * n / kFs);
        const auto& yc = closed.step({5.0, gate});
        const auto& yn = newton.step({5.0, gate});
        REQUIRE(closed.lastConverged());
        REQUIRE(newton.lastConverged());
        REQUIRE(yc.size() == yn.size());
        for (size_t k = 0; k < yc.size(); ++k) {
            REQUIRE(std::isfinite(yc[k]));
            REQUIRE(std::isfinite(yn[k]));
            REQUIRE(yc[k] == Approx(yn[k]).margin(1e-7));
        }
    }
    REQUIRE(closed.jfetFallbackCount() == 0);
}

TEST_CASE("the SPICE parser builds JFETs from a limited NJF card", "[jfet][spice]") {
    const wdf::Circuit c = wdf::buildFromString(
        "Vdd d 0 10\n"
        "R1 d jd 1k\n"
        "Vg g 0 -1\n"
        "J1 jd g 0 2N5952\n");
    REQUIRE(c.jfet.size() == 1);
    REQUIRE(c.jfet[0].name == "J1");
    REQUIRE(c.jfet[0].model.p.Is0 == Approx(wdf::k2N5952Params.Is0));
    REQUIRE(c.jfet[0].model.p.Vp == Approx(wdf::k2N5952Params.Vp));
    // The built-in card's linear parasitics are added as ordinary elements.
    REQUIRE(c.res.size() >= 3);  // R1 + J1$rd + J1$rs
    REQUIRE(c.cap.size() >= 2);  // J1$cgs + J1$cgd

    const wdf::Circuit card = wdf::buildFromString(
        ".model MyJ NJF(Beta=1m Vto=-2 Lambda=0.01 Rd=2 Rs=3 Cgs=1p Cgd=2p Cds=3p)\n"
        "J1 d g 0 MyJ\n");
    REQUIRE(card.jfet.size() == 1);
    const auto& jm = card.jfet[0].model;
    REQUIRE(jm.p.Is0 == Approx(4e-3));
    REQUIRE(jm.p.Vp == Approx(-2.0));
    REQUIRE(jm.p.lambda == Approx(0.01));
    REQUIRE(jm.p.rd == Approx(2.0));
    REQUIRE(jm.p.rs == Approx(3.0));
    REQUIRE(jm.p.cgs == Approx(1e-12));
    REQUIRE(jm.p.cgd == Approx(2e-12));
    REQUIRE(jm.p.cds == Approx(3e-12));

    REQUIRE_THROWS_AS(wdf::buildFromString("J1 d g 0 NoSuchJfet\n"), std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString("J1 d g\n"), std::runtime_error);
    REQUIRE_THROWS_AS(
        wdf::buildFromString(".model Bad NJF(Beta=1m Vto=-2 Is=1n)\nJ1 d g 0 Bad\n"),
        std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString(".model Bad NJF(Vto=-2)\nJ1 d g 0 Bad\n"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString(".model Bad NJF(Beta=1m)\nJ1 d g 0 Bad\n"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString(".model Bad NJF(Beta=1m Vto=2)\nJ1 d g 0 Bad\n"),
                      std::runtime_error);
    REQUIRE(wdf::findJfetByName("J2N5952") != nullptr);
}

TEST_CASE("JFET NJF parameters have documented defaults, units, and behavior",
          "[jfet][spice][parameters]") {
    // Beta is not Is0. The parser must perform the square-law conversion
    // Is0 = Beta*Vto^2, including the Vto^2 factor.
    {
        const wdf::Circuit c = wdf::buildFromString(
            ".model M NJF(Beta=1.5m Vto=-3)\n"
            "J1 d g 0 M\n");
        REQUIRE(c.jfet.size() == 1);
        const auto& p = c.jfet[0].model.p;
        REQUIRE(p.Is0 == Approx(1.5e-3 * 9.0));
        REQUIRE(p.Vp == Approx(-3.0));
        REQUIRE(p.lambda == 0.0);
        REQUIRE(p.rd == 0.0);
        REQUIRE(p.rs == 0.0);
        REQUIRE(p.cgs == 0.0);
        REQUIRE(p.cgd == 0.0);
        REQUIRE(p.cds == 0.0);
        // The engine-level gate resistor is not a card parameter and is still
        // added; no series or intrinsic-capacitor elements are created.
        REQUIRE(c.res.size() == 1);
        REQUIRE(c.cap.empty());
    }

    // Suffix parsing and the exact mapping of every accepted card key.
    {
        const wdf::Circuit c = wdf::buildFromString(
            ".model M NJF(Beta=1.314m Vto=-2.021 Lambda=4m Rd=1 Rs=2 Cgs=2.414p "
            "Cgd=1.6p Cds=0.5p)\n"
            "J1 d g 0 M\n");
        const auto& p = c.jfet[0].model.p;
        REQUIRE(p.Is0 == Approx(1.314e-3 * 2.021 * 2.021));
        REQUIRE(p.Vp == Approx(-2.021));
        REQUIRE(p.lambda == Approx(4e-3));
        REQUIRE(p.rd == Approx(1.0));
        REQUIRE(p.rs == Approx(2.0));
        REQUIRE(p.cgs == Approx(2.414e-12));
        REQUIRE(p.cgd == Approx(1.6e-12));
        REQUIRE(p.cds == Approx(0.5e-12));
    }

    // Lambda is a real term in the constitutive law, not just a parsed field.
    {
        const wdf::JfetModel zero = wdf::JfetModel::fromSpice(1e-3, -2.0, 0.0);
        const wdf::JfetModel modulated = wdf::JfetModel::fromSpice(1e-3, -2.0, 0.1);
        const auto a = zero.evaluate(-1.0, 3.0);
        const auto b = modulated.evaluate(-1.0, 3.0);
        REQUIRE(a.valid);
        REQUIRE(b.valid);
        REQUIRE(b.id == Approx(a.id * (1.0 + 0.1 * 3.0)));
    }

    // Rd and Rs are series resistances around the intrinsic device. They must
    // change the DC operating point, not merely appear in the struct.
    {
        const auto make = [](const std::string& card) {
            return wdf::buildFromString(
                "Vdd d 0 10\n"
                "R1 d jd 1k\n"
                "Vg g 0 -1\n"
                "J1 jd g 0 M\n" + card);
        };
        const wdf::Circuit ideal = make(
            ".model M NJF(Beta=1m Vto=-2 Lambda=0.01 Rd=0 Rs=0)\n");
        const wdf::Circuit series = make(
            ".model M NJF(Beta=1m Vto=-2 Lambda=0.01 Rd=100 Rs=50)\n");
        wdf::Engine eIdeal(ideal, kFs), eSeries(series, kFs);
        std::vector<double> nI, nS, rI, rS;
        REQUIRE(eIdeal.solveDc(nI, rI));
        REQUIRE(eSeries.solveDc(nS, rS));
        const int jdI = eIdeal.nodeIndex("jd");
        const int jdS = eSeries.nodeIndex("jd");
        REQUIRE(jdI >= 0);
        REQUIRE(jdS >= 0);
        REQUIRE(nS[static_cast<size_t>(jdS)] > nI[static_cast<size_t>(jdI)]);
        REQUIRE(std::abs(nS[static_cast<size_t>(jdS)] - nI[static_cast<size_t>(jdI)]) > 1e-4);

        std::vector<double> iI, jI, iS, jS;
        eIdeal.currents(rI, iI, jI);
        eSeries.currents(rS, iS, jS);
        REQUIRE(iS[0] < iI[0]);
    }

    // Cgs, Cgd and Cds are linear capacitors around the intrinsic device. They
    // are open at DC and must each change a transient step response.
    {
        const auto run = [](const std::string& extra) {
            const wdf::Circuit c = wdf::buildFromString(
                "Vdd d 0 10\n"
                "R1 d jd 1k\n"
                "Vg g0 0 -1\n"
                "Rg g0 g 1k\n"
                "J1 jd g 0 M\n"
                ".model M NJF(Beta=1m Vto=-2 Lambda=0.01 " + extra + ")\n");
            wdf::Engine e(c, kFs);
            const int jd = e.nodeIndex("jd");
            REQUIRE(jd >= 0);
            e.reset();
            std::vector<double> out;
            out.reserve(200);
            for (int n = 0; n < 200; ++n) {
                const auto& y = e.step({10.0, -0.5});
                REQUIRE(e.lastConverged());
                out.push_back(y[static_cast<size_t>(jd)]);
            }
            return out;
        };
        const auto base = run("");
        for (const char* cap : {"Cgs=1n", "Cgd=1n", "Cds=1n"}) {
            INFO("cap=" << cap);
            const auto withCap = run(cap);
            double maxDiff = 0.0;
            for (size_t i = 0; i < base.size(); ++i)
                maxDiff = std::max(maxDiff, std::abs(withCap[i] - base[i]));
            REQUIRE(maxDiff > 1e-4);
        }
    }

    // Every accepted key is checked; everything else is rejected rather than
    // silently ignored. Rgate is deliberately not a SPICE NJF parameter.
    for (const char* bad : {
             ".model Bad NJF(Beta=1m Vto=-2 Is=1n)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=1m Vto=-2 CJO=1p)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=1m Vto=-2 Rgate=1G)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=0 Vto=-2)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=1m Vto=0)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=1m Vto=-2 Lambda=-1)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=1m Vto=-2 Rd=-1)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=1m Vto=-2 Rs=-1)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=1m Vto=-2 Cgs=-1p)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=1m Vto=-2 Cgd=-1p)\nJ1 d g 0 Bad\n",
             ".model Bad NJF(Beta=1m Vto=-2 Cds=-1p)\nJ1 d g 0 Bad\n",
         }) {
        INFO("bad card=" << bad);
        REQUIRE_THROWS_AS(wdf::buildFromString(bad), std::runtime_error);
    }
}

TEST_CASE("built-in JFET presets match their LTspice cards and evaluate",
          "[jfet][presets]") {
    struct Expected {
        const char* name;
        double beta;
        double vto;
        double lambda;
        double rd;
        double rs;
        double cgs;
        double cgd;
        bool pChannel;
    };
    const Expected expected[] = {
        {"2N5457", 1.125e-3, -1.372, 2.3e-3, 1.0, 1.0, 4.627e-12, 4.0e-12, false},
        {"MPF102", 1.04e-3, -3.41, 2.0e-3, 1.0, 1.0, 2.414e-12, 1.6e-12, false},
        {"J201", 1.07e-3, -0.93, 6.75e-3, 10.0, 12.0, 1.0e-12, 1.0e-12, false},
        {"2SK170", 59.86e-3, -0.5024, 1.923e-3, 4.151, 4.151, 25.48e-12, 20.0e-12, false},
        {"J111", 2.91e-3, -4.047, 17.5e-3, 1.0, 1.0, 5.74e-12, 6.46e-12, false},
        {"2N3819", 1.304e-3, -3.0, 2.25e-3, 1.0, 1.0, 2.414e-12, 1.6e-12, false},
        {"J202", 0.88e-3, -1.81, 8.47e-3, 11.0, 13.0, 1.0e-12, 1.0e-12, false},
        {"2N5460", 1.0e-3, -3.4, 10.0e-3, 10.0, 10.0, 5.0e-12, 5.0e-12, true},
        {"2N5461", 1.5e-3, -4.3, 10.0e-3, 10.0, 10.0, 5.0e-12, 5.0e-12, true},
        {"2N5462", 2.0e-3, -5.4, 10.0e-3, 10.0, 10.0, 5.0e-12, 5.0e-12, true},
        {"J175", 1.031e-3, -3.762, 28.0e-3, 1.0, 1.0, 9.0e-12, 6.5e-12, true},
        {"J176", 2.3e-3, -1.82, 21.0e-3, 1.0, 1.0, 9.0e-12, 6.5e-12, true},
        {"J177", 3.3e-3, -1.386, 18.18e-3, 1.0, 1.0, 9.0e-12, 6.5e-12, true},
    };

    for (const auto& e : expected) {
        INFO("model: " << e.name);
        const auto* entry = wdf::findJfetByName(e.name);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->model.valid());

        const auto& p = entry->model.p;
        CHECK(p.Is0 == Approx(e.beta * e.vto * e.vto).epsilon(1e-12));
        CHECK(p.Vp == Approx(e.vto));
        CHECK(p.lambda == Approx(e.lambda));
        CHECK(p.rd == Approx(e.rd));
        CHECK(p.rs == Approx(e.rs));
        CHECK(p.cgs == Approx(e.cgs));
        CHECK(p.cgd == Approx(e.cgd));
        CHECK(p.cds == 0.0);  // absent in the source cards
        CHECK(p.rgate == 1.0e9);
        CHECK(p.pChannel == e.pChannel);

        const double sign = e.pChannel ? -1.0 : 1.0;
        const auto cutoff =
            entry->model.evaluate(sign * (e.vto - 0.5), sign * 1.0);
        REQUIRE(cutoff.valid);
        CHECK(cutoff.region == wdf::JfetRegion::Cutoff);
        CHECK(cutoff.id == 0.0);
        CHECK(cutoff.did_ds == 0.0);
        CHECK(cutoff.did_gs == 0.0);

        const auto ohmic = entry->model.evaluate(sign * -0.1, sign * 0.1);
        REQUIRE(ohmic.valid);
        CHECK(ohmic.region == wdf::JfetRegion::Ohmic);
        CHECK(sign * ohmic.id > 0.0);
        CHECK(ohmic.did_ds > 0.0);
        CHECK(ohmic.did_gs > 0.0);
        CHECK(std::isfinite(ohmic.id));
        CHECK(std::isfinite(ohmic.did_ds));
        CHECK(std::isfinite(ohmic.did_gs));

        const double satVds = 2.0 * std::abs(e.vto);
        const auto saturation = entry->model.evaluate(0.0, sign * satVds);
        REQUIRE(saturation.valid);
        CHECK(saturation.region == wdf::JfetRegion::Saturation);
        CHECK(sign * saturation.id > 0.0);
        CHECK(saturation.did_ds > 0.0);
        CHECK(saturation.did_gs > 0.0);
        CHECK(std::isfinite(saturation.id));
        CHECK(std::isfinite(saturation.did_ds));
        CHECK(std::isfinite(saturation.did_gs));

        // A passive load line with negligible drop should find the same
        // saturation branch through the closed-form group solve.
        const auto solved =
            entry->model.solveGroup(sign * satVds, 0.0, 1.0, 0.0);
        REQUIRE(solved.ok);
        CHECK(solved.region == wdf::JfetRegion::Saturation);
        CHECK(sign * solved.id > 0.0);
        CHECK(std::isfinite(solved.id));
        CHECK(solved.residual < 1.0e-6 * (1.0 + std::abs(solved.id)));
    }
}

TEST_CASE("built-in JFET presets work in a common-source stage",
          "[jfet][presets][engine]") {
    for (const char* name :
         {"2N5457", "MPF102", "J201", "2SK170", "J111", "2N3819", "J202"}) {
        INFO("model: " << name);
        const std::string net = "VDD vdd 0 15\n"
                                "RD vdd d 100\n"
                                "VG g 0 0\n"
                                "J1 d g 0 " +
                                std::string(name) + "\n";
        const wdf::Circuit c = wdf::buildFromString(net);
        REQUIRE(c.jfet.size() == 1);
        const auto* entry = wdf::findJfetByName(name);
        REQUIRE(entry != nullptr);
        CHECK(c.jfet[0].model.p.Is0 == Approx(entry->model.p.Is0));
        CHECK(c.jfet[0].model.p.Vp == Approx(entry->model.p.Vp));

        wdf::Engine e(c, kFs);
        std::vector<double> node, rootV;
        REQUIRE(e.solveDc(node, rootV));
        const int d = e.nodeIndex("d");
        REQUIRE(d >= 0);
        REQUIRE(rootV.size() >= 2);
        CHECK(std::isfinite(node[static_cast<size_t>(d)]));
        const auto ev = c.jfet[0].model.evaluate(rootV[1], rootV[0]);
        REQUIRE(ev.valid);
        CHECK(ev.region == wdf::JfetRegion::Saturation);
        CHECK(ev.id > 0.0);
        CHECK(std::isfinite(ev.id));
        CHECK(std::isfinite(ev.did_ds));
        CHECK(std::isfinite(ev.did_gs));

        e.reset();
        const auto& y = e.step({15.0, 0.0});
        REQUIRE(e.lastConverged());
        CHECK(std::isfinite(y[static_cast<size_t>(d)]));
    }
}

TEST_CASE("a .livewdf JFET part converts to a circuit with the documented pins",
          "[jfet][document]") {
    wdf::LiveWDFDocument doc;
    wdf::SchematicComponent j;
    j.id = "J1";
    j.type = "jfet";
    j.pins = {{"drain", "jd"}, {"gate", "g"}, {"source", "0"}};
    j.properties = {{"model", "2N5952"}};
    doc.components.push_back(j);

    const wdf::Circuit c = doc.toCircuit();
    REQUIRE(c.jfet.size() == 1);
    REQUIRE(c.jfet[0].name == "J1");
    REQUIRE(c.jfet[0].dExt == "jd");
    REQUIRE(c.jfet[0].gExt == "g");
    REQUIRE(c.jfet[0].sExt == "0");
    REQUIRE(c.jfet[0].model.p.Is0 == Approx(wdf::k2N5952Params.Is0));

    wdf::LiveWDFDocument bad;
    wdf::SchematicComponent b;
    b.id = "J2";
    b.type = "jfet";
    b.pins = {{"drain", "d"}, {"gate", "g"}, {"source", "0"}};
    b.properties = {{"model", "NotARealJfet"}};
    bad.components.push_back(b);
    REQUIRE_THROWS_AS(bad.toCircuit(), std::runtime_error);
}

TEST_CASE("JFET preset selection and edited-value persistence",
          "[jfet][presets][editor]") {
    wdf::LiveWDFDocument doc;
    wdf::SchematicComponent j;
    j.id = "J1";
    j.type = "jfet";
    j.pins = {{"drain", "d"}, {"gate", "g"}, {"source", "0"}};
    j.properties = {{"model", "2N5952"}};
    const auto* jm = wdf::findJfetByName("2N5952");
    if (jm) {
        const auto& p = jm->model.p;
        j.values = {{"beta", p.Is0 / (p.Vp * p.Vp)}, {"vto", p.Vp},
                    {"lambda", p.lambda}, {"rd", p.rd}, {"rs", p.rs},
                    {"cgs", p.cgs}, {"cgd", p.cgd}, {"cds", p.cds}};
    }
    doc.components.push_back(j);
    REQUIRE(doc.components[0].properties.at("model") == "2N5952");
    REQUIRE(doc.components[0].values.at("beta") == Approx(5.367e-3 / (2.021 * 2.021)));

    // User edits parameter values; preset identity preserved.
    doc.components[0].values.at("vto") = -3.0;
    doc.components[0].values.at("lambda") = 0.02;
    REQUIRE(doc.components[0].properties.at("model") == "2N5952");

    const wdf::Circuit edited = doc.toCircuit();
    REQUIRE(edited.jfet.size() == 1);
    REQUIRE(edited.jfet[0].model.p.Vp == Approx(-3.0));
    REQUIRE(edited.jfet[0].model.p.lambda == Approx(0.02));

    // Serialization preserves edited parameters and preset identity.
    const std::string json = doc.toJson();
    wdf::LiveWDFDocument back;
    std::string err;
    REQUIRE(wdf::LiveWDFDocument::fromJson(json, back, err));
    REQUIRE(err.empty());
    REQUIRE(back.components[0].properties.at("model") == "2N5952");
    REQUIRE(back.components[0].values.at("vto") == Approx(-3.0));
}

TEST_CASE("a P-channel JFET is the exact mirror of the N-channel law",
          "[jfet][pchannel][equation]") {
    wdf::JfetModel n = wdf::kJ176;
    n.p.pChannel = false;
    wdf::JfetModel p = n;
    p.p.pChannel = true;

    for (double vgs : {-2.5, -1.0, -0.3, 0.0, 1.5}) {
        for (double vds : {-4.0, -1.0, -0.2, 0.0, 0.5, 3.0}) {
            const auto en = n.evaluate(vgs, vds);
            const auto ep = p.evaluate(-vgs, -vds);
            INFO("vgs " << vgs << " vds " << vds);
            REQUIRE(en.valid);
            REQUIRE(ep.valid);
            CHECK(ep.id == Approx(-en.id).margin(1e-15));
            CHECK(ep.did_gs == Approx(en.did_gs).margin(1e-12));
            CHECK(ep.did_ds == Approx(en.did_ds).margin(1e-12));
            CHECK(ep.region == en.region);
        }
    }

    // The two-port group solve must solve the same physical equation, not just
    // flip a label on the result.
    const wdf::JfetRootSet roots =
        p.solveReferenceRoots(-2.0, -1.0, 100.0, 0.0);
    REQUIRE(roots.count > 0);
    bool usedReference = false;
    for (int i = 0; i < roots.count; ++i) {
        if (std::abs(roots.roots[static_cast<size_t>(i)].id -
                     p.solveReference(-2.0, -1.0, 100.0, 0.0).id) < 1e-9)
            usedReference = true;
    }
    CHECK(usedReference);

    const auto fast = p.solveGroup(-5.0, -1.0, 1.0, 0.0);
    REQUIRE(fast.ok);
    CHECK(fast.id < 0.0);
    CHECK(fast.region == wdf::JfetRegion::Saturation);
    const auto ev = p.evaluate(-1.0, -5.0 - 1.0 * fast.id);
    REQUIRE(ev.valid);
    CHECK(fast.id == Approx(ev.id).margin(1e-12));

    for (const auto& bc : {std::array<double, 4>{-2.0, -1.0, 100.0, 0.0},
                           std::array<double, 4>{-4.0, -0.5, 1000.0, 50.0},
                           std::array<double, 4>{-0.8, -2.5, 220.0, -10.0}}) {
        const auto solved = p.solveGroup(bc[0], bc[1], bc[2], bc[3]);
        REQUIRE(solved.ok);
        const auto e = p.evaluate(bc[1] - bc[3] * solved.id,
                                  bc[0] - bc[2] * solved.id);
        REQUIRE(e.valid);
        CHECK(solved.id == Approx(e.id).margin(1e-12));
        CHECK(solved.id < 0.0);

        const auto ref = p.solveReference(bc[0], bc[1], bc[2], bc[3]);
        REQUIRE(ref.ok);
        CHECK(solved.id == Approx(ref.id).margin(1e-8 * (1.0 + std::abs(ref.id))));
    }
}

TEST_CASE("the engine solves a P-channel JFET common-source stage",
          "[jfet][pchannel][engine]") {
    constexpr double kFsLocal = 48000.0;
    const wdf::JfetModel m = wdf::k2N5460;
    REQUIRE(m.p.pChannel);

    wdf::Circuit c;
    c.addSrc("VDD", "0", 5.0, 0.0, {}, "Vdd");
    c.addSrc("VG", "0", 4.5, 0.0, {}, "Vg");
    c.addRes("D", "0", 10.0, "RD");
    c.addJfet("D", "VG", "VDD", m, "J1");

    wdf::Engine e(c, kFsLocal);
    REQUIRE(e.jfetCount() == 1);
    REQUIRE(e.explicitRoots());
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    REQUIRE(rootV.size() == 2);

    const int d = e.nodeIndex("D");
    REQUIRE(d >= 0);
    const double vD = node[static_cast<size_t>(d)];
    CHECK(vD > 0.0);
    CHECK(vD < 5.0);
    CHECK(rootV[0] < 0.0);  // drain below source
    CHECK(rootV[1] < 0.0);  // gate below source

    std::vector<double> i, J;
    e.currents(rootV, i, J);
    REQUIRE(i.size() == 2);
    CHECK(i[0] < 0.0);
    CHECK(i[1] == 0.0);
    CHECK(i[0] == Approx(-vD / 10.0).margin(1e-12));
    const auto ev = m.evaluate(rootV[1], rootV[0]);
    REQUIRE(ev.valid);
    CHECK(ev.region == wdf::JfetRegion::Saturation);
    CHECK(ev.id == Approx(i[0]).margin(1e-12 * (1.0 + std::abs(i[0]))));

    // The same stage on both root paths, with a driven gate.
    wdf::Engine closed(c, kFsLocal);
    wdf::Engine newton(c, kFsLocal);
    REQUIRE(closed.explicitRoots());
    REQUIRE(newton.setExplicitRoots(false));
    std::vector<double> nc, nr, nn, nr2;
    REQUIRE(closed.solveDc(nc, nr));
    REQUIRE(newton.solveDc(nn, nr2));
    closed.reset();
    newton.reset();
    for (int n = 0; n < 240; ++n) {
        const double vg = 4.5 + 0.1 * std::sin(2.0 * 3.14159265358979323846 *
                                               220.0 * n / kFsLocal);
        const auto& yc = closed.step({5.0, vg});
        const auto& yn = newton.step({5.0, vg});
        REQUIRE(closed.lastConverged());
        REQUIRE(newton.lastConverged());
        REQUIRE(yc.size() == yn.size());
        for (size_t k = 0; k < yc.size(); ++k)
            CHECK(yc[k] == Approx(yn[k]).margin(1e-7));
    }
    CHECK(closed.jfetFallbackCount() == 0);
}

TEST_CASE("a .livewdf P-channel JFET converts with its card polarity",
          "[jfet][pchannel][document]") {
    wdf::LiveWDFDocument doc;
    wdf::SchematicComponent j;
    j.id = "J1";
    j.type = "jfet";
    j.pins = {{"drain", "d"}, {"gate", "g"}, {"source", "0"}};
    j.properties = {{"model", "2N5460"}};
    doc.components.push_back(j);

    const wdf::Circuit c = doc.toCircuit();
    REQUIRE(c.jfet.size() == 1);
    CHECK(c.jfet[0].model.p.pChannel);
    CHECK(c.jfet[0].model.p.Vp == Approx(-3.4));

    // A custom P-channel part keeps its explicit polarity even with edited
    // n-channel-shaped values.
    wdf::LiveWDFDocument custom;
    wdf::SchematicComponent j2 = j;
    j2.id = "J2";
    j2.properties = {{"model", "Custom"},
                     {wdf::kPolarityProperty, wdf::kPChannel}};
    j2.values = {{"beta", 1.0e-3}, {"vto", -2.0}, {"lambda", 0.01},
                 {"rd", 1.0}, {"rs", 1.0}, {"cgs", 5.0e-12},
                 {"cgd", 5.0e-12}, {"cds", 0.0}};
    custom.components.push_back(j2);
    const wdf::Circuit cc = custom.toCircuit();
    REQUIRE(cc.jfet.size() == 1);
    CHECK(cc.jfet[0].model.p.pChannel);
    CHECK(cc.jfet[0].model.p.Vp == Approx(-2.0));
}
