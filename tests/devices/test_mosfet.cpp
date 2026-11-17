#include <catch2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "devices/Mosfet.h"
#include "devices/MosfetPresets.h"
#include "format/LiveWDFFile.h"
#include "format/TransistorPolarity.h"
#include "spice/SpiceNet.h"
#include "wdf/Engine.h"

// The general three-terminal n-channel MOSFET, after Bernardini, Vergani and
// Sarti (2020), equations (21)-(23).
//
// The checks are split by layer:
//
//   1. the paper's constitutive law, including the equality policy;
//   2. the separately derived source/drain-reversed extension and its
//      derivatives through VDS = 0;
//   3. the two-port group solve against a bracketed reference that never uses
//      the cubic;
//   4. root-selection and region transitions;
//   5. the engine's Newton and closed-form paths on a real stage;
//   6. the limited Level-1 `M`/NMOS parser path and `.livewdf` conversion.
//
// The paper does not contain the reverse branch. Every reverse test is therefore
// written against the terminal interchange (`vGS' = vGD`, `vDS' = -vDS`,
// `Id_r = -Id`) rather than being presented as a paper equation.

namespace {

constexpr double kFs = 48000.0;

wdf::MosfetModel modelWithLambda(double lambda) {
    return wdf::MosfetModel::fromK(1.0e-3, 1.0, lambda);
}

wdf::MosfetModel cleanModel() { return modelWithLambda(0.0); }

bool regionInequalities(const wdf::MosfetParams& p, wdf::MosfetRegion region, double vgs,
                        double vds) {
    // Region names are in the forward orientation. For negative VDS the
    // implementation's extension swaps source and drain, so the same swap is
    // applied before checking the paper's inequalities.
    if (vds < 0.0) {
        vgs -= vds;
        vds = -vds;
    }
    const double vov = vgs - p.Vth;
    const double tol = 1.0e-12 * (1.0 + std::abs(vov) + std::abs(vds));
    switch (region) {
    case wdf::MosfetRegion::Cutoff:
        return !(vov > 0.0);
    case wdf::MosfetRegion::Ohmic:
        return vov > 0.0 && vds < vov - tol;
    case wdf::MosfetRegion::Saturation:
        return vov > 0.0 && vds >= vov - tol;
    case wdf::MosfetRegion::Invalid:
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

double nearRoot(const wdf::MosfetRootSet& roots, double seed) {
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
}

}  // namespace

TEST_CASE("MOSFET equations 21-23 have the paper's regions and values",
          "[mosfet][device]") {
    const wdf::MosfetModel m = modelWithLambda(0.1);
    REQUIRE(m.valid());
    REQUIRE(m.p.K == Approx(1.0e-3));
    REQUIRE(m.p.Vth == Approx(1.0));
    REQUIRE(m.p.lambda == Approx(0.1));

    // Cut-off: Vov <= 0. The gate can be above ground while still below Vth.
    for (double vgs : {-1.0, 0.0, 0.5, m.p.Vth}) {
        const auto e = m.evaluateForward(vgs, 5.0);
        REQUIRE(e.valid);
        REQUIRE(e.region == wdf::MosfetRegion::Cutoff);
        REQUIRE(e.id == 0.0);
        REQUIRE(e.did_ds == 0.0);
        REQUIRE(e.did_gs == 0.0);
    }

    // Ohmic: Vov > 0 and VDS < Vov.
    {
        const double vgs = 3.0;
        const double vds = 0.5;
        const double vov = vgs - m.p.Vth;
        const double u = vov * vds - 0.5 * vds * vds;
        const double ref = m.p.K * u * (1.0 + m.p.lambda * vds);
        const auto e = m.evaluateForward(vgs, vds);
        REQUIRE(e.valid);
        REQUIRE(e.region == wdf::MosfetRegion::Ohmic);
        REQUIRE(e.id == Approx(ref).margin(std::abs(ref) * 1e-14 + 1e-18));
        REQUIRE(regionInequalities(m.p, e.region, vgs, vds));
    }

    // Saturation: Vov > 0 and VDS >= Vov.
    {
        const double vgs = 3.0;
        const double vds = 5.0;
        const double vov = vgs - m.p.Vth;
        const double ref = 0.5 * m.p.K * vov * vov * (1.0 + m.p.lambda * vds);
        const auto e = m.evaluateForward(vgs, vds);
        REQUIRE(e.valid);
        REQUIRE(e.region == wdf::MosfetRegion::Saturation);
        REQUIRE(e.id == Approx(ref).margin(std::abs(ref) * 1e-14 + 1e-18));
        REQUIRE(regionInequalities(m.p, e.region, vgs, vds));
    }

    // The paper has strict inequalities. Equality at VDS = Vov is assigned to
    // saturation and equality at Vov = 0 to cut-off for deterministic reporting.
    {
        const double vgs = 3.0;
        const double vov = vgs - m.p.Vth;
        const auto below = m.evaluateForward(vgs, vov - 1e-9);
        const auto at = m.evaluateForward(vgs, vov);
        const auto above = m.evaluateForward(vgs, vov + 1e-9);
        REQUIRE(below.region == wdf::MosfetRegion::Ohmic);
        REQUIRE(at.region == wdf::MosfetRegion::Saturation);
        REQUIRE(above.region == wdf::MosfetRegion::Saturation);
        REQUIRE(below.id == Approx(at.id).margin(1e-10));
        REQUIRE(at.id == Approx(above.id).margin(1e-10));
    }

    // Lambda is a real term in both conducting regions.
    {
        const wdf::MosfetModel zero = cleanModel();
        const auto ohmic = m.evaluateForward(3.0, 0.5);
        const auto ohmicZero = zero.evaluateForward(3.0, 0.5);
        REQUIRE(ohmic.id == Approx(ohmicZero.id * (1.0 + 0.1 * 0.5)));
        const auto sat = m.evaluateForward(3.0, 5.0);
        const auto satZero = zero.evaluateForward(3.0, 5.0);
        REQUIRE(sat.id == Approx(satZero.id * (1.0 + 0.1 * 5.0)));
    }

    // Negative VDS is the source/drain-reversed extension, not a literal
    // evaluation of (22) at negative VDS.
    {
        const double vgs = 0.5;
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
}

TEST_CASE("MOSFET derivatives match finite differences away from boundaries",
          "[mosfet][device]") {
    const wdf::MosfetModel m = modelWithLambda(0.05);
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
        REQUIRE(e.did_gs == Approx(fdGs).margin(std::abs(fdGs) * 3e-6 + 1e-18));
        REQUIRE(e.did_ds == Approx(fdDs).margin(std::abs(fdDs) * 3e-6 + 1e-18));
    };

    check(3.0, 0.4);   // forward ohmic
    check(2.0, 0.2);   // forward ohmic
    check(3.0, 4.0);   // forward saturation
    check(1.5, 3.0);   // forward saturation
    check(0.5, -1.0);  // reverse saturation: VGS < Vth, VGD > Vth
    check(0.5, -0.4);  // reverse cutoff by VGD
    check(3.0, -0.4);  // reverse ohmic
    check(3.0, -4.0);  // reverse saturation
}

TEST_CASE("MOSFET reverse extension covers the VGS/VGD threshold asymmetry",
          "[mosfet][device][reverse]") {
    const wdf::MosfetModel m = modelWithLambda(0.05);

    // VGS is below threshold, but VGD is above it. The forward orientation is
    // cut off; the reversed orientation conducts from the old source to the old
    // drain, so Id is negative.
    {
        const double vgs = 0.5;
        const double vds = -1.0;
        REQUIRE(vgs < m.p.Vth);
        REQUIRE(vgs - vds > m.p.Vth);
        const auto e = m.evaluate(vgs, vds);
        REQUIRE(e.valid);
        REQUIRE(e.id < 0.0);
        REQUIRE(e.region == wdf::MosfetRegion::Saturation);
        // Raising the gate raises the reversed overdrive and makes the
        // source-to-drain current more negative.
        REQUIRE(e.did_gs < 0.0);
        REQUIRE(regionInequalities(m.p, e.region, vgs, vds));
    }

    // Small positive and negative VDS with VGS below threshold. Both are
    // cut off until VGD crosses Vth on the negative side. This catches a
    // one-sided threshold test or a reverse branch keyed on VGS alone.
    {
        const double vgs = 0.5;
        for (double vds : {-0.1, 0.0, 0.1}) {
            const double vgd = vgs - vds;
            INFO("vds=" << vds << " vgd=" << vgd);
            const auto e = m.evaluate(vgs, vds);
            REQUIRE(e.valid);
            REQUIRE(vgd < m.p.Vth);
            REQUIRE(e.region == wdf::MosfetRegion::Cutoff);
            REQUIRE(e.id == 0.0);
        }
        const auto reverse = m.evaluate(0.5, -0.8);
        REQUIRE(reverse.valid);
        REQUIRE(reverse.id < 0.0);
        REQUIRE(reverse.region == wdf::MosfetRegion::Saturation);
    }

    // Transition through VDS = 0: current and both first derivatives are
    // continuous. The forward side uses (22); the negative side uses the
    // transformed terminal variables.
    {
        const double vgs = 3.0;
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

        // The analytic slope at zero is the same from both sides.
        const double expected = m.p.K * (vgs - m.p.Vth);
        REQUIRE(at.did_ds == Approx(expected).margin(1e-12));
        REQUIRE(plus.did_ds == Approx(expected).margin(1e-6));
        REQUIRE(minus.did_ds == Approx(expected).margin(1e-6));
    }
}

TEST_CASE("MOSFET closed form matches an independent bracketed solve across bias",
          "[mosfet][solver]") {
    for (double lambda : {0.0, 0.05}) {
        const wdf::MosfetModel m = modelWithLambda(lambda);
        for (double voDs : {-5.0, -1.0, 0.0, 0.5, 2.0, 5.0}) {
            for (double voGs : {-1.0, 0.5, 1.0, 2.0, 4.0}) {
                for (double zdd : {10.0, 100.0, 1000.0}) {
                    for (double zgd : {0.0, 10.0, 100.0}) {
                        INFO("lambda=" << lambda << " voDs=" << voDs << " voGs=" << voGs
                                       << " zdd=" << zdd << " zgd=" << zgd);
                        const BiasCase bc{voDs, voGs, zdd, zgd};
                        const auto fast = m.solveGroup(bc.voDs, bc.voGs, bc.zdd, bc.zgd);
                        const auto ref = m.solveReference(bc.voDs, bc.voGs, bc.zdd, bc.zgd);
                        REQUIRE(fast.ok);
                        REQUIRE(ref.ok);
                        REQUIRE(fast.id == Approx(ref.id)
                                               .margin(2e-9 * (1.0 + std::abs(ref.id))));

                        const double vds = bc.voDs - bc.zdd * fast.id;
                        const double vgs = bc.voGs - bc.zgd * fast.id;
                        REQUIRE(regionInequalities(m.p, fast.region, vgs, vds));
                        const auto e = m.evaluate(vgs, vds);
                        REQUIRE(e.valid);
                        REQUIRE(e.region == fast.region);
                        REQUIRE(e.id == Approx(fast.id)
                                            .margin(2e-8 * (1.0 + std::abs(fast.id))));
                    }
                }
            }
        }
    }
}

TEST_CASE("MOSFET reverse group solve uses VGD, -VDS and transformed impedance",
          "[mosfet][solver][reverse]") {
    const wdf::MosfetModel m = modelWithLambda(0.05);
    const double th = m.p.Vth;

    // Forward orientation is cut off (VGS = 0.5 < Vth) but the reversed
    // orientation sees VGD = 1.5 > Vth. The root exists only through the
    // explicit source/drain interchange.
    for (double voDs : {-1.0, -0.75, -2.0}) {
        for (double zdd : {100.0, 1000.0}) {
            for (double zgd : {0.0, 50.0}) {
                INFO("voDs=" << voDs << " zdd=" << zdd << " zgd=" << zgd);
                const auto fast = m.solveGroup(voDs, 0.5, zdd, zgd);
                const auto ref = m.solveReference(voDs, 0.5, zdd, zgd);
                REQUIRE(fast.ok);
                REQUIRE(ref.ok);
                REQUIRE(fast.id < 0.0);
                REQUIRE(fast.id == Approx(ref.id)
                                       .margin(2e-9 * (1.0 + std::abs(ref.id))));

                const double vds = voDs - zdd * fast.id;
                const double vgs = 0.5 - zgd * fast.id;
                REQUIRE(vds < 0.0);
                REQUIRE(vgs < th);       // forward orientation is cut off
                REQUIRE(vgs - vds > th); // reversed orientation conducts
                REQUIRE(regionInequalities(m.p, fast.region, vgs, vds));

                // The same solve written in the reversed coordinates must give
                // the same oriented current. This checks A_r = -voDS,
                // B_r = voGS - voDS - Vth and zgd_r = zdd - zgd explicitly.
                const auto reversed = m.solveForwardGroup(-voDs, 0.5 - voDs, zdd, zdd - zgd,
                                                          -0.0);
                REQUIRE(reversed.ok);
                REQUIRE(fast.id == Approx(-reversed.id)
                                        .margin(2e-9 * (1.0 + std::abs(reversed.id))));
            }
        }
    }
}

TEST_CASE("MOSFET group solve is continuous through VDS = 0", "[mosfet][solver]") {
    const wdf::MosfetModel m = modelWithLambda(0.05);
    for (double startSeed : {-1.0e-3, -1.0e-6, 0.0, 1.0e-6, 1.0e-3}) {
        double seed = startSeed;
        double previous = 0.0;
        bool first = true;
        for (int k = -100; k <= 100; ++k) {
            const double voDs = 0.01 * static_cast<double>(k);
            INFO("seed0=" << startSeed << " voDs=" << voDs);
            const auto fast = m.solveGroup(voDs, 3.0, 1000.0, 0.0, seed);
            const auto ref = m.solveReferenceSeeded(voDs, 3.0, 1000.0, 0.0, seed);
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
            REQUIRE(regionInequalities(m.p, fast.region, 3.0, vds));
            if (!first)
                REQUIRE(std::abs(fast.id - previous) < 1e-4);
            previous = fast.id;
            seed = fast.id;
            first = false;
        }
    }

    for (double voDs : {-1.0e-12, -1.0e-9, -1.0e-6, 0.0, 1.0e-6, 1.0e-9, 1.0e-12}) {
        INFO("tiny voDs=" << voDs);
        const auto fast = m.solveGroup(voDs, 3.0, 1000.0, 0.0, 0.0);
        const auto ref = m.solveReferenceSeeded(voDs, 3.0, 1000.0, 0.0, 0.0);
        REQUIRE(fast.ok);
        REQUIRE(ref.ok);
        REQUIRE(fast.id == Approx(ref.id).margin(2e-12 * (1.0 + std::abs(ref.id))));
        REQUIRE(std::abs(fast.id) <= 1.0e-8);
        const double vds = voDs - 1000.0 * fast.id;
        REQUIRE(std::abs(vds) <= std::abs(voDs) + 1e-12);
        REQUIRE(regionInequalities(m.p, fast.region, 3.0, vds));
    }
}

TEST_CASE("MOSFET group solve keeps both orientations for an active port",
          "[mosfet][solver]") {
    const wdf::MosfetModel m = modelWithLambda(0.05);
    const auto fast = m.solveGroup(5.0, 3.5, -1000.0, 0.0, 0.0);
    const auto ref = m.solveReference(5.0, 3.5, -1000.0, 0.0);
    REQUIRE(fast.ok);
    REQUIRE(ref.ok);
    REQUIRE(fast.id == Approx(ref.id).margin(2e-9 * (1.0 + std::abs(ref.id))));

    const auto roots = m.solveReferenceRoots(5.0, 3.5, -1000.0, 0.0);
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

TEST_CASE("MOSFET group solve handles degenerate coefficients and extreme values",
          "[mosfet][solver]") {
    // lambda = 0 removes the cubic term; zgd = 0 removes the saturation cubic
    // term and the gate coupling.
    {
        const wdf::MosfetModel m = cleanModel();
        const auto s = m.solveGroup(5.0, 3.0, 100.0, 0.0);
        REQUIRE(s.ok);
        REQUIRE(s.region == wdf::MosfetRegion::Saturation);
    }
    {
        const wdf::MosfetModel m = modelWithLambda(0.05);
        const auto s = m.solveGroup(5.0, 3.0, 100.0, 0.0);
        REQUIRE(s.ok);
    }

    // Very small K and a small threshold: the polynomial coefficients are tiny,
    // so the root finder must scale rather than declare a zero cubic.
    {
        const wdf::MosfetModel m = wdf::MosfetModel::fromK(1e-9, 0.1, 0.0);
        const auto s = m.solveGroup(1.0, 0.15, 1000.0, 100.0);
        REQUIRE(s.ok);
        REQUIRE(std::isfinite(s.id));
        const double vds = 1.0 - 1000.0 * s.id;
        const double vgs = 0.15 - 100.0 * s.id;
        REQUIRE(regionInequalities(m.p, s.region, vgs, vds));
    }

    // Extreme but finite terminal voltages must not produce NaN/Inf. If the
    // arithmetic overflows, the result must say invalid rather than invent a
    // plausible current.
    {
        const wdf::MosfetModel m = modelWithLambda(0.05);
        for (const auto& bc : {BiasCase{1e3, 1e3, 100.0, 10.0},
                               BiasCase{-1e3, -1e3, 100.0, 10.0},
                               BiasCase{1e-9, 0.5, 1e-9, 1e-9}}) {
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
        wdf::MosfetParams bad;
        bad.K = 1e-3;
        bad.Vth = 1.0;
        bad.lambda = -1.0;
        const wdf::MosfetModel m(bad);
        REQUIRE_FALSE(m.valid());
        const auto s = m.solveGroup(1.0, 2.0, 100.0, 10.0);
        REQUIRE_FALSE(s.ok);
    }
    {
        const wdf::MosfetModel m = modelWithLambda(0.05);
        REQUIRE_FALSE(
            m.solveGroup(std::numeric_limits<double>::quiet_NaN(), 2.0, 100.0, 10.0).ok);
        REQUIRE_FALSE(
            m.solveGroup(1.0, std::numeric_limits<double>::infinity(), 100.0, 10.0).ok);
        REQUIRE_FALSE(m.solveReference(1.0, 2.0, 100.0, 10.0, 1.0, -1.0).ok);
    }
}

TEST_CASE("the engine solves a MOSFET stage on both root paths", "[mosfet][engine]") {
    const wdf::MosfetModel m = modelWithLambda(0.01);

    wdf::Circuit c;
    c.addSrc("d", "0", 10.0, 0.0, {}, "Vdd");
    c.addRes("d", "md", 1000.0, "R1");
    c.addSrc("g_src", "0", 3.0, 0.0, {}, "Vg");
    c.addRes("g_src", "g", 1000.0, "Rg");
    c.addMosfet("md", "g", "0", m, "M1");

    wdf::Engine e(c, kFs);
    REQUIRE(e.mosfetCount() == 1);
    REQUIRE(e.rootCount() == 2);
    REQUIRE(e.explicitRoots());
    REQUIRE(e.rootLayout().devices.size() == 1);
    REQUIRE(e.rootLayout().devices[0].kind == wdf::RootKind::Mosfet);

    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    REQUIRE(rootV.size() == 2);

    std::vector<double> i, J;
    e.currents(rootV, i, J);
    REQUIRE(i.size() == 2);
    const auto ev = m.evaluate(rootV[1], rootV[0]);
    REQUIRE(ev.valid);
    REQUIRE(ev.region == wdf::MosfetRegion::Saturation);
    REQUIRE(i[0] == Approx(ev.id).margin(1e-10 * (1.0 + std::abs(ev.id))));
    REQUIRE(i[1] == 0.0);

    // The source is ground, so the independent port voltages are the drain and
    // gate node voltages. The drain resistor current must equal the terminal
    // drain current, and the gate carries no current.
    const int dIdx = e.nodeIndex("md");
    const int gIdx = e.nodeIndex("g");
    REQUIRE(dIdx >= 0);
    REQUIRE(gIdx >= 0);
    REQUIRE(std::abs(node[static_cast<size_t>(dIdx)] - rootV[0]) < 1e-9);
    REQUIRE(std::abs(node[static_cast<size_t>(gIdx)] - rootV[1]) < 1e-9);
    REQUIRE(std::abs((10.0 - node[static_cast<size_t>(dIdx)]) / 1000.0 - i[0]) < 1e-12);
    REQUIRE(rootV[0] > 1.0);

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
    REQUIRE(closed.mosfetFallbackCount() == 0);
}

TEST_CASE("the engine follows a reverse-biased MOSFET stage on both root paths",
          "[mosfet][engine][reverse]") {
    // Source at +5 V, drain returned to ground through 1 k, gate at +1.5 V:
    // VGS = -3.5 V < Vth, while VGD = +2.5 V > Vth. The only conducting
    // orientation is source-to-drain, so the DC current must be negative.
    const wdf::MosfetModel m = modelWithLambda(0.01);

    wdf::Circuit c;
    c.addSrc("s", "0", 5.0, 0.0, {}, "Vdd");
    c.addRes("d", "0", 1000.0, "Rd");
    c.addSrc("g", "0", 1.5, 0.0, {}, "Vg");
    c.addMosfet("d", "g", "s", m, "M1");

    wdf::Engine closed(c, kFs);
    wdf::Engine newton(c, kFs);
    REQUIRE(closed.mosfetCount() == 1);
    REQUIRE(closed.rootCount() == 2);
    REQUIRE(newton.setExplicitRoots(false));

    std::vector<double> nodeC, nodeN, rootC, rootN;
    REQUIRE(closed.solveDc(nodeC, rootC));
    REQUIRE(newton.solveDc(nodeN, rootN));
    REQUIRE(nodeC.size() == nodeN.size());
    for (size_t k = 0; k < nodeC.size(); ++k)
        REQUIRE(nodeC[k] == Approx(nodeN[k]).margin(1e-7));

    REQUIRE(rootC[0] < 0.0);
    const double vgs = rootC[1];
    REQUIRE(vgs < m.p.Vth);
    REQUIRE(vgs - rootC[0] > m.p.Vth);
    const auto ev = m.evaluate(vgs, rootC[0]);
    REQUIRE(ev.valid);
    REQUIRE(ev.id < 0.0);
    REQUIRE(ev.region == wdf::MosfetRegion::Saturation);

    const int dcD = closed.nodeIndex("d");
    REQUIRE(dcD >= 0);
    closed.reset();
    newton.reset();
    const double closedDcStep = closed.step({5.0, 1.5})[static_cast<size_t>(dcD)];
    const double newtonDcStep = newton.step({5.0, 1.5})[static_cast<size_t>(dcD)];
    REQUIRE(closed.lastConverged());
    REQUIRE(newton.lastConverged());
    REQUIRE(closedDcStep == Approx(nodeC[static_cast<size_t>(dcD)]).margin(1e-9));
    REQUIRE(newtonDcStep == Approx(nodeN[static_cast<size_t>(dcD)]).margin(1e-9));

    closed.reset();
    newton.reset();
    for (int n = 0; n < 400; ++n) {
        const double gate =
            1.5 + 0.05 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * n / kFs);
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
    REQUIRE(closed.mosfetFallbackCount() == 0);
}

TEST_CASE("a P-channel MOSFET is the exact mirror of the N-channel law",
          "[mosfet][pchannel][equation]") {
    wdf::MosfetModel n = wdf::MosfetModel::fromK(20.0e-3, 2.0, 0.01);
    wdf::MosfetModel p = n;
    p.p.pChannel = true;

    for (double vgs : {-4.0, -2.5, -1.0, 0.0, 2.5}) {
        for (double vds : {-5.0, -1.0, -0.1, 0.0, 0.5, 4.0}) {
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

    // Group solve: check the solved current against the same physical law and
    // against the independent bracketed reference, with negative terminal
    // voltages throughout.
    for (const auto& bc : {std::array<double, 4>{-3.0, -3.0, 100.0, 0.0},
                           std::array<double, 4>{-5.0, -3.0, 1000.0, 25.0},
                           std::array<double, 4>{-1.0, -4.0, 220.0, -10.0}}) {
        INFO("voDs " << bc[0] << " voGs " << bc[1]);
        const auto s = p.solveGroup(bc[0], bc[1], bc[2], bc[3]);
        REQUIRE(s.ok);
        CHECK(s.id < 0.0);
        const auto e = p.evaluate(bc[1] - bc[3] * s.id,
                                  bc[0] - bc[2] * s.id);
        REQUIRE(e.valid);
        CHECK(e.id == Approx(s.id).margin(1e-12));
        const auto ref = p.solveReference(bc[0], bc[1], bc[2], bc[3]);
        REQUIRE(ref.ok);
        CHECK(s.id == Approx(ref.id).margin(1e-8 * (1.0 + std::abs(ref.id))));
    }
}

TEST_CASE("the engine solves a P-channel MOSFET common-source stage",
          "[mosfet][pchannel][engine]") {
    constexpr double kFsLocal = 48000.0;
    const auto* preset = wdf::findMosfetPreset("IRF9540N");
    REQUIRE(preset != nullptr);
    REQUIRE(preset->pChannel);
    const wdf::MosfetModel m = wdf::MosfetModel::fromK(
        preset->K, preset->Vth, preset->lambda, true);

    wdf::Circuit c;
    c.addSrc("VDD", "0", 5.0, 0.0, {}, "Vdd");
    c.addSrc("VG", "0", 1.0, 0.0, {}, "Vg");
    c.addRes("D", "0", 1000.0, "RD");
    c.addMosfet("D", "VG", "VDD", m, "M1");

    wdf::Engine e(c, kFsLocal);
    REQUIRE(e.mosfetCount() == 1);
    REQUIRE(e.explicitRoots());
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    REQUIRE(rootV.size() == 2);

    const int d = e.nodeIndex("D");
    REQUIRE(d >= 0);
    const double vD = node[static_cast<size_t>(d)];
    CHECK(rootV[0] == Approx(vD - 5.0).margin(1e-9));
    CHECK(rootV[1] == Approx(-4.0).margin(1e-12));

    std::vector<double> i, J;
    e.currents(rootV, i, J);
    REQUIRE(i.size() == 2);
    CHECK(i[0] < 0.0);
    CHECK(i[1] == 0.0);
    CHECK(i[0] == Approx(-vD / 1000.0).margin(1e-12));
    const auto ev = m.evaluate(rootV[1], rootV[0]);
    REQUIRE(ev.valid);
    CHECK(ev.region == wdf::MosfetRegion::Saturation);
    CHECK(ev.id == Approx(i[0]).margin(1e-12 * (1.0 + std::abs(i[0]))));

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
        const double vg = 1.0 + 0.1 * std::sin(2.0 * 3.14159265358979323846 *
                                               220.0 * n / kFsLocal);
        const auto& yc = closed.step({5.0, vg});
        const auto& yn = newton.step({5.0, vg});
        REQUIRE(closed.lastConverged());
        REQUIRE(newton.lastConverged());
        REQUIRE(yc.size() == yn.size());
        for (size_t k = 0; k < yc.size(); ++k)
            CHECK(yc[k] == Approx(yn[k]).margin(1e-7));
    }
    CHECK(closed.mosfetFallbackCount() == 0);
}

TEST_CASE("MOSFET group solve crosses cutoff and ohmic/saturation boundaries",
          "[mosfet][solver][regions]") {
    const wdf::MosfetModel m = modelWithLambda(0.05);

    // Gate sweep across Vth. The cold start is cut off; as Vov becomes positive
    // the forward saturation root appears continuously. The seed is carried
    // from sample to sample, as in the engine's sample path.
    {
        double seed = 0.0;
        double previous = 0.0;
        bool first = true;
        bool sawCutoff = false;
        bool sawSaturation = false;
        for (int k = 0; k <= 100; ++k) {
            const double voGs = 0.8 + 0.004 * static_cast<double>(k);
            INFO("gate sweep voGs=" << voGs);
            const auto s = m.solveGroup(5.0, voGs, 1000.0, 0.0, seed);
            const auto ref = m.solveReferenceSeeded(5.0, voGs, 1000.0, 0.0, seed);
            REQUIRE(s.ok);
            REQUIRE(ref.ok);
            REQUIRE(s.id == Approx(ref.id).margin(2e-9 * (1.0 + std::abs(ref.id))));
            const double vds = 5.0 - 1000.0 * s.id;
            const double vgs = voGs;
            REQUIRE(regionInequalities(m.p, s.region, vgs, vds));
            if (s.region == wdf::MosfetRegion::Cutoff)
                sawCutoff = true;
            if (s.region == wdf::MosfetRegion::Saturation)
                sawSaturation = true;
            if (!first)
                REQUIRE(s.id >= previous - 1e-12);
            if (!first)
                REQUIRE(std::abs(s.id - previous) < 5e-5);
            previous = s.id;
            seed = s.id;
            first = false;
        }
        REQUIRE(sawCutoff);
        REQUIRE(sawSaturation);
    }

    // Drain sweep across vDS = Vov at a fixed gate. With a small drain
    // impedance the operating point passes from ohmic to saturation.
    {
        double seed = 0.0;
        double previous = 0.0;
        bool first = true;
        bool sawOhmic = false;
        bool sawSaturation = false;
        for (int k = 0; k <= 120; ++k) {
            const double voDs = 1.80 + 0.004 * static_cast<double>(k);
            INFO("drain sweep voDs=" << voDs);
            const auto s = m.solveGroup(voDs, 3.0, 10.0, 0.0, seed);
            const auto ref = m.solveReferenceSeeded(voDs, 3.0, 10.0, 0.0, seed);
            REQUIRE(s.ok);
            REQUIRE(ref.ok);
            REQUIRE(s.id == Approx(ref.id).margin(2e-9 * (1.0 + std::abs(ref.id))));
            const double vds = voDs - 10.0 * s.id;
            REQUIRE(regionInequalities(m.p, s.region, 3.0, vds));
            if (s.region == wdf::MosfetRegion::Ohmic)
                sawOhmic = true;
            if (s.region == wdf::MosfetRegion::Saturation)
                sawSaturation = true;
            if (!first)
                REQUIRE(std::abs(s.id - previous) < 2e-6);
            previous = s.id;
            seed = s.id;
            first = false;
        }
        REQUIRE(sawOhmic);
        REQUIRE(sawSaturation);
    }
}

TEST_CASE("MOSFET group solve selects the root nearest the previous branch current",
          "[mosfet][solver]") {
    // This bias has three independently found roots: one reversed root and two
    // forward roots. A cold start chooses the smallest |Id|; a warm start must
    // follow the branch it came from. The roots come from the bracketed
    // reference, not from the cubic.
    const wdf::MosfetModel m = modelWithLambda(0.05);
    const double voDs = 2.0;
    const double voGs = 1.5;
    const double zdd = -1000.0;
    const double zgd = -100.0;
    const auto roots = m.solveReferenceRoots(voDs, voGs, zdd, zgd);
    REQUIRE(roots.count >= 3);

    for (double seed : {-0.01, -0.0049234, -0.001, 0.0, 0.0001, 0.000146656,
                        0.01, 0.0472405, 0.1}) {
        INFO("seed=" << seed);
        const auto s = m.solveGroup(voDs, voGs, zdd, zgd, seed);
        REQUIRE(s.ok);
        const double expected = nearRoot(roots, seed);
        REQUIRE(s.id == Approx(expected).margin(2e-8 * (1.0 + std::abs(expected))));

        bool found = false;
        for (int i = 0; i < roots.count; ++i)
            if (std::abs(s.id - roots.roots[static_cast<size_t>(i)].id) <=
                2e-8 * (1.0 + std::abs(s.id)))
                found = true;
        REQUIRE(found);

        const double vds = voDs - zdd * s.id;
        const double vgs = voGs - zgd * s.id;
        REQUIRE(regionInequalities(m.p, s.region, vgs, vds));
        const auto e = m.evaluate(vgs, vds);
        REQUIRE(e.valid);
        REQUIRE(e.region == s.region);
        REQUIRE(e.id == Approx(s.id).margin(2e-8 * (1.0 + std::abs(s.id))));
    }

    // The cold start really is the smallest-magnitude root, not an accident of
    // the seed list above.
    double smallest = std::numeric_limits<double>::infinity();
    for (int i = 0; i < roots.count; ++i)
        smallest = std::min(smallest, std::abs(roots.roots[static_cast<size_t>(i)].id));
    const double cold = m.solveGroup(voDs, voGs, zdd, zgd, 0.0).id;
    REQUIRE(std::abs(cold) == Approx(smallest).margin(2e-8 * (1.0 + smallest)));
}

TEST_CASE("the SPICE parser builds MOSFETs from a limited Level-1 NMOS card",
          "[mosfet][spice]") {
    const wdf::Circuit c = wdf::buildFromString(
        ".model NM NMOS(Level=1 KP=1m VTO=1 Lambda=0.01 W=1 L=1)\n"
        "Vdd d 0 10\n"
        "R1 d md 1k\n"
        "Vg g 0 3\n"
        "M1 md g 0 0 NM\n");
    REQUIRE(c.mosfet.size() == 1);
    REQUIRE(c.mosfet[0].name == "M1");
    REQUIRE(c.mosfet[0].model.p.K == Approx(1.0e-3));
    REQUIRE(c.mosfet[0].model.p.Vth == Approx(1.0));
    REQUIRE(c.mosfet[0].model.p.lambda == Approx(0.01));

    // W/L are a ratio: KP=2m with W=2, L=1 gives K=4m.
    {
        const wdf::Circuit card = wdf::buildFromString(
            ".model M NMOS(KP=2m VTO=0.5 W=2 L=1)\nM1 d g 0 0 M\n");
        REQUIRE(card.mosfet.size() == 1);
        REQUIRE(card.mosfet[0].model.p.K == Approx(4.0e-3));
        REQUIRE(card.mosfet[0].model.p.Vth == Approx(0.5));
        REQUIRE(card.mosfet[0].model.p.lambda == 0.0);
    }

    // Instance W/L overrides the card.
    {
        const wdf::Circuit instance = wdf::buildFromString(
            ".model M NMOS(KP=1m VTO=1 W=1 L=1)\nM1 d g 0 0 M W=2 L=4\n");
        REQUIRE(instance.mosfet.size() == 1);
        REQUIRE(instance.mosfet[0].model.p.K == Approx(0.5e-3));
    }

    // With neither card nor instance W/L, both dimensions are explicitly 1.
    // This is LiveWDF's documented choice, not an unstated SPICE default.
    {
        const wdf::Circuit defaults =
            wdf::buildFromString(".model M NMOS(KP=1m VTO=1)\nM1 d g 0 0 M\n");
        REQUIRE(defaults.mosfet.size() == 1);
        REQUIRE(defaults.mosfet[0].model.p.K == Approx(1.0e-3));
    }

    // W and L are resolved independently: card W with instance L.
    {
        const wdf::Circuit mixed =
            wdf::buildFromString(".model M NMOS(KP=1m VTO=1 W=2)\nM1 d g 0 0 M L=4\n");
        REQUIRE(mixed.mosfet.size() == 1);
        REQUIRE(mixed.mosfet[0].model.p.K == Approx(0.5e-3));
    }

    REQUIRE_THROWS_AS(wdf::buildFromString("M1 d g 0 0 NoSuchModel\n"), std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString("M1 d g\n"), std::runtime_error);
    REQUIRE_THROWS_AS(
        wdf::buildFromString(".model M NMOS(KP=1m VTO=1)\nM1 d g 0 d M\n"),
        std::runtime_error);  // body effect
    REQUIRE_THROWS_AS(
        wdf::buildFromString(".model M NMOS(Level=2 KP=1m VTO=1)\nM1 d g 0 0 M\n"),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        wdf::buildFromString(".model M NMOS(KP=1m VTO=1 CJO=1p)\nM1 d g 0 0 M\n"),
        std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString(".model M NMOS(VTO=1)\nM1 d g 0 0 M\n"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString(".model M NMOS(KP=1m)\nM1 d g 0 0 M\n"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(
        wdf::buildFromString(".model M NMOS(KP=0 VTO=1)\nM1 d g 0 0 M\n"),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        wdf::buildFromString(".model M NMOS(KP=1m VTO=1 Lambda=-1)\nM1 d g 0 0 M\n"),
        std::runtime_error);
}

TEST_CASE("MOSFET model values map from KP and W/L without a preset",
          "[mosfet][spice][parameters]") {
    const wdf::MosfetModel direct = wdf::MosfetModel::fromK(1.0e-3, 1.0, 0.05);
    const wdf::MosfetModel spice = wdf::MosfetModel::fromSpice(2.0e-3, 1.0, 0.05, 0.5, 1.0);
    REQUIRE(spice.p.K == Approx(direct.p.K));
    REQUIRE(spice.p.Vth == Approx(direct.p.Vth));
    REQUIRE(spice.p.lambda == Approx(direct.p.lambda));

    // With W=L the mapping is the identity; this is what the ngspice oracle
    // uses to match the paper's K directly against SPICE's KP.
    const wdf::MosfetModel unit = wdf::MosfetModel::fromSpice(1.0e-3, 1.0, 0.05, 1.0, 1.0);
    REQUIRE(unit.p.K == Approx(1.0e-3));
}

TEST_CASE("a .livewdf MOSFET preset selects its own polarity",
          "[mosfet][pchannel][document]") {
    wdf::LiveWDFDocument doc;
    wdf::SchematicComponent m;
    m.id = "M1";
    m.type = "mosfet";
    m.pins = {{"drain", "d"}, {"gate", "g"}, {"source", "0"}};
    m.properties = {{"preset", "IRF9540N"}};
    doc.components.push_back(m);

    const wdf::Circuit c = doc.toCircuit();
    REQUIRE(c.mosfet.size() == 1);
    CHECK(c.mosfet[0].model.p.pChannel);
    CHECK(c.mosfet[0].model.p.Vth == Approx(3.77));

    // The card is authoritative for polarity: an explicit polarity on a named
    // N-channel card does not silently compile it as the wrong device, even
    // when the part carries edited numbers.
    wdf::LiveWDFDocument named = doc;
    named.components.front().properties["preset"] = "2N7000";
    named.components.front().properties[wdf::kPolarityProperty] = wdf::kPChannel;
    named.components.front().values = {{"k", 5.0e-3}, {"vth", 1.1}, {"lambda", 0.02}};
    const wdf::Circuit nc = named.toCircuit();
    REQUIRE(nc.mosfet.size() == 1);
    CHECK_FALSE(nc.mosfet[0].model.p.pChannel);
    CHECK(nc.mosfet[0].model.p.K == Approx(5.0e-3));
    CHECK(nc.mosfet[0].model.p.Vth == Approx(1.1));
    CHECK(nc.mosfet[0].model.p.lambda == Approx(0.02));

    // A Custom part uses its explicit polarity and editable values.
    wdf::LiveWDFDocument custom;
    wdf::SchematicComponent cm = m;
    cm.id = "M2";
    cm.properties = {{"preset", "Custom"},
                     {wdf::kPolarityProperty, wdf::kPChannel}};
    cm.values = {{"k", 20.0e-3}, {"vth", 2.0}, {"lambda", 0.01}};
    custom.components.push_back(cm);
    const wdf::Circuit cc = custom.toCircuit();
    REQUIRE(cc.mosfet.size() == 1);
    CHECK(cc.mosfet[0].model.p.pChannel);
    CHECK(cc.mosfet[0].model.p.K == Approx(20.0e-3));

    // A hand-edited part with no preset is custom too: the explicit polarity
    // is the only thing that can select the channel, and the converter must
    // read it exactly as the artwork selector does.
    wdf::LiveWDFDocument missingPreset = custom;
    missingPreset.components.front().properties.erase("preset");
    const wdf::Circuit mp = missingPreset.toCircuit();
    REQUIRE(mp.mosfet.size() == 1);
    CHECK(mp.mosfet[0].model.p.pChannel);
    CHECK(mp.mosfet[0].model.p.Vth == Approx(2.0));
}
