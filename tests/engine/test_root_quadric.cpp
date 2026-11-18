#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "wdf/Engine.h"
#include "spice/SpiceNet.h"
#include "devices/Triode.h"
#include "support/QuadricStage.h"

// The exact closed-form root for a single quadric triode
// (`Engine::solveRootGroups`, docs/PERFORMANCE.md).
//
// The claim to defend is not "it runs" but "it is the same solve": the closed
// form must reproduce the multiport Newton, which was itself validated against
// ngspice and wdf_compiler generated code in the earlier increments. So these
// tests build one circuit, run both paths sample-for-sample, and demand they
// agree to round-off -- plus that the eligibility gate is exactly as advertised.

namespace {

constexpr double kFs = 48000.0;

// A single quadric triode: the closed-form case, since a triode's grid ports
// carry no current of their own.
wdf::Circuit singleTriodeStage() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    c.addCap("in", "i", 100e-9);
    c.addRes("i", "0", 1e6);
    c.addRes("i", "g", 20e3);
    c.addRes("k", "0", 1e3);
    c.addCap("k", "0", 10e-6);
    c.addRes("e", "p", 100e3);
    c.addCap("p", "o", 10e-9);
    c.addRes("o", "0", 1e6);
    wdf::TriodeModel model = wdf::k12ax7Quadric();
    c.addTri("g", "p", "k", "g", model);
    return c;
}

double inputAt(int n) {
    return 0.35 * std::sin(2.0 * 3.14159265358979323846 * 330.0 * static_cast<double>(n) / kFs);
}

}  // namespace

TEST_CASE("closed-form root activates for a quadric triode", "[phase1][root]") {
    {
        wdf::Engine e(singleTriodeStage(), kFs);
        REQUIRE(e.allowsExplicitRoot());
        REQUIRE(e.explicitRoots());
    }
    {
        // Force the other way: eligibility is a setup decision, not a per-sample
        // one.
        wdf::Engine e(singleTriodeStage(), kFs);
        REQUIRE(e.setExplicitRoots(false));
        REQUIRE_FALSE(e.explicitRoots());
        REQUIRE(e.setExplicitRoots(true));
        REQUIRE(e.explicitRoots());
    }
}

TEST_CASE("closed-form root reproduces the multiport Newton", "[phase1][root][tran]") {
    constexpr int kSamples = 4000;

    wdf::Engine closed(singleTriodeStage(), kFs);
    wdf::Engine newton(singleTriodeStage(), kFs);
    REQUIRE(newton.setExplicitRoots(false));
    REQUIRE(closed.explicitRoots());

    std::vector<double> nodeC, rootC, nodeN, rootN;
    REQUIRE(closed.solveDc(nodeC, rootC));
    REQUIRE(newton.solveDc(nodeN, rootN));
    closed.reset();
    newton.reset();

    std::vector<double> sv(2, 0.0);
    sv[1] = 250.0;
    double maxDiff = 0.0;
    int closedConv = 0;
    int newtonConv = 0;
    for (int n = 0; n < kSamples; ++n) {
        sv[0] = inputAt(n);
        const std::vector<double>& a = closed.step(sv);
        const std::vector<double>& b = newton.step(sv);
        for (size_t k = 0; k < a.size(); ++k)
            maxDiff = std::max(maxDiff, std::abs(a[k] - b[k]));
        if (closed.lastConverged())
            ++closedConv;
        if (newton.lastConverged())
            ++newtonConv;
    }

    INFO("maxDiff = " << maxDiff);
    REQUIRE(closedConv == kSamples);
    REQUIRE(newtonConv == kSamples);
    // Exact by construction; the floor is double round-off in the V-space solve.
    REQUIRE(maxDiff < 1e-9);
}

TEST_CASE("the plate law respects the paper's own Vpk >= 0 domain", "[phase1][device][paper]") {
    // Table 3's 12AX7 fit puts the cutoff (`c0 = kP/(2*kP2)`, ~92.2 V) far
    // below the cathode, so `q > 0` with `Vpk < 0` is reachable: at Vpk =
    // -50 V, Vgk = 0 the raw surface would conduct (q ~ 42 V). The paper's
    // reference element does not: `triode_stage_quadric.m` solves the quadric
    // and tests the *solved* voltage, projecting when `Vpk < 0`.
    const wdf::TriodeModel m = wdf::k12ax7Quadric();
    {
        const wdf::TriodeModel::Currents c = m.evaluate(0.0, -50.0);
        REQUIRE(c.ip == 0.0);
        REQUIRE(c.dvpk == 0.0);
        REQUIRE(c.dvgk == 0.0);
    }
    // The excursion `memory/task007_negative_vpk_origin.md` measured on the
    // Mark deck: a plate well below the cathode goes cold, not outgassed.
    {
        const wdf::TriodeModel::Currents c = m.evaluate(2.9, -52.8);
        REQUIRE(c.ip == 0.0);
    }
    // Boundary: the paper projects on a strict test (`if (Vpk < 0)`), so
    // exactly on the cathode the surface's own root is kept.
    {
        const wdf::TriodeModel::Currents c = m.evaluate(0.0, 0.0);
        REQUIRE(c.ip > 0.0);
    }
}

TEST_CASE("engine reproduces the authors' Octave reference stage", "[phase1][root][tran][paper]") {
    constexpr int kSamples = 4000;

    // `support/QuadricStage.h` is a line-for-line port (MIT, header notice) of
    // the authors' own Octave reference for this stage, `triode_stage_quadric
    // .m` with their Table 4 values; `singleTriodeStage` is the same network
    // decomposed and solved by our engine.
    wdf::QuadricCommonCathode ref;
    ref.params = wdf::kQuadric12AX7;
    REQUIRE(ref.prepare(kFs));

    const wdf::QuadricCommonCathodeBias bias = wdf::quadricCommonCathodeBias(
        wdf::kQuadric12AX7, 250.0, 100e3, 1e3);
    INFO("reference DC bias: Vp0 = " << bias.vp << " V");

    wdf::Engine closed(singleTriodeStage(), kFs);
    std::vector<double> node, rootV;
    REQUIRE(closed.solveDc(node, rootV));
    const int pIdx = closed.nodeIndex("p");
    REQUIRE(pIdx >= 0);
    const int oIdx = closed.nodeIndex("o");
    REQUIRE(oIdx >= 0);
    // The decomposed network settles at the authors' closed-form bias.
    REQUIRE(node[static_cast<size_t>(pIdx)] == Approx(bias.vp).margin(1e-6));
    closed.reset();

    std::vector<double> sv(2, 0.0);
    sv[1] = 250.0;
    double maxDiff = 0.0;
    int converged = 0;
    for (int n = 0; n < kSamples; ++n) {
        sv[0] = inputAt(n);
        const std::vector<double>& y = closed.step(sv);
        const double yr = ref.process(inputAt(n));
        maxDiff = std::max(maxDiff, std::abs(y[static_cast<size_t>(oIdx)] - yr));
        if (closed.lastConverged())
            ++converged;
    }
    INFO("max |engine closed - reference| = " << maxDiff << " V over "
         << kSamples << " samples");
    REQUIRE(converged == kSamples);
    // On this deck `Vpk` stays deep inside the domain (DC plate ~146.8 V,
    // input swing a few volts), so this gate pins the surface and the DC
    // bias against the paper itself; the domain branch has the device-level
    // gate above.
    REQUIRE(maxDiff < 1e-9);
}

TEST_CASE("a square-driven cascade keeps the second plate above its cathode", "[phase1][root][paper]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    c.addRes("in", "g1", 1e3);
    c.addRes("k1", "0", 1.5e3);
    c.addRes("e", "p1", 100e3);
    c.addCap("p1", "x", 22e-9);
    c.addRes("x", "0", 1e6);
    c.addRes("x", "g2", 1e3);
    c.addRes("k2", "0", 1.5e3);
    c.addRes("e", "p2", 100e3);
    c.addTri("g1", "p1", "k1", "g1", wdf::k12ax7Quadric());
    c.addTri("g2", "p2", "k2", "g2", wdf::k12ax7Quadric());
    for (const bool closedForm : {true, false}) {
        wdf::Engine e(c, kFs);
        REQUIRE(e.setExplicitRoots(closedForm));
        const auto p2 = static_cast<size_t>(e.nodeIndex("p2"));
        const auto k2 = static_cast<size_t>(e.nodeIndex("k2"));
        double lowest = 0.0;
        for (int n = 0; n < 4800; ++n) {
            const std::vector<double>& y = e.step({(n / 240) % 2 ? -5.0 : 5.0, 250.0});
            lowest = std::min(lowest, y[p2] - y[k2]);
        }
        CAPTURE(closedForm, lowest);
        REQUIRE(lowest >= -1e-6);
    }
}
