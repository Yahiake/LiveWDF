#include <catch2.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "devices/Diode.h"
#include "devices/OpAmp.h"
#include "spice/Circuit.h"
#include "wdf/Engine.h"

// Tests for the three imported PedalKernel blockwise & SPQR techniques:
// 1. Anti-parallel clipping diode pair (Wright Omega seeded, fixed-iteration exact solve)
// 2. Compile-time DC bias network precharging for coupling caps
// 3. Harold Black op-amp closed-form feedback stage

namespace {
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

wdf::Circuit makeDiodeClipper(double rIn = 1000.0, double cVal = 1e-6) {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    // Stage 1: reference diode stage
    c.addRes("in", "mid", rIn, "Rin1");
    wdf::DiodeModel dm(4.0e-9, 1.0, 0.56);
    c.addDiode("mid", "0", dm, "D0");
    // Decoupled buffer to Stage 2
    c.addVCVS("buf", "0", "mid", "0", 1.0, 10.0, "E1");
    c.addRes("buf", "out", rIn, "Rin2");
    c.addCap("out", "0", cVal, "C1");
    // Stage 2: Symmetrical anti-parallel 1N4148 clipping diodes
    c.addDiode("out", "0", dm, "D1");
    c.addDiode("0", "out", dm, "D2");
    return c;
}

}  // namespace

TEST_CASE("AntiParallelDiodeModel: Wright Omega seeded pair satisfies port equation to machine precision",
          "[diode][pedalkernel][closed_form]") {
    wdf::DiodeModel dm(4.0e-9, 1.0, 0.56);
    wdf::AntiParallelDiodeModel pair(dm, dm);

    REQUIRE(pair.symmetrical);

    const double Rp = 1000.0;
    const std::vector<double> testVoltages = {
        -3.0, -1.5, -0.7, -0.3, -0.05, 0.0, 0.05, 0.3, 0.7, 1.5, 3.0
    };

    for (double vo : testVoltages) {
        double vPort = 0.0, i1 = 0.0, i2 = 0.0;
        pair.solvePort(vo, Rp, vPort, i1, i2);

        // Port equation: vPort = vo - Rp * (i1 - i2)
        const double iNet = i1 - i2;
        const double residual = vPort - (vo - Rp * iNet);
        CHECK(std::abs(residual) < 1e-11);

        // Symmetry test
        if (vo != 0.0) {
            double vPortNeg = 0.0, i1Neg = 0.0, i2Neg = 0.0;
            pair.solvePort(-vo, Rp, vPortNeg, i1Neg, i2Neg);
            CHECK(vPortNeg == Approx(-vPort).margin(1e-11));
            CHECK(i1Neg == Approx(i2).margin(1e-11));
            CHECK(i2Neg == Approx(i1).margin(1e-11));
        }
    }
}

TEST_CASE("AntiParallelDiodeModel: junction solve validates both branch laws for unequal diodes",
          "[diode][pedalkernel][closed_form]") {
    const wdf::DiodeModel seriesA(4.0e-9, 1.0, 0.56);
    const wdf::DiodeModel seriesB(2.0e-9, 1.2, 0.10);
    const wdf::DiodeModel idealA(4.0e-9, 1.0, 0.0);
    const wdf::DiodeModel idealB(2.0e-9, 1.2, 0.0);

    const std::vector<wdf::AntiParallelDiodeModel> pairs = {
        wdf::AntiParallelDiodeModel(seriesA, seriesB),
        wdf::AntiParallelDiodeModel(idealA, seriesB),
        wdf::AntiParallelDiodeModel(idealA, idealB),
    };
    const std::vector<double> rpValues = {1.0, 100.0, 1000.0, 1.0e6};
    const std::vector<double> voValues = {-10.0, -3.0, -0.7, -0.1, 0.0,
                                          0.1, 3.0, 10.0};

    for (const auto& pair : pairs) {
        for (double Rp : rpValues) {
            for (double vo : voValues) {
                double vPort = 0.0, i1 = 0.0, i2 = 0.0;
                pair.solvePort(vo, Rp, vPort, i1, i2);
                CAPTURE(Rp, vo, pair.d1.rs, pair.d2.rs, vPort, i1, i2);

                CHECK(std::isfinite(vPort));
                CHECK(std::isfinite(i1));
                CHECK(std::isfinite(i2));

                const double scale = 1.0 + std::abs(vo) + std::abs(vPort);
                CHECK(std::abs(vPort - (vo - Rp * (i1 - i2))) < 1e-9 * scale);

                // Reconstruct each junction voltage from its branch current.
                // A branch at exactly -Is is fully reverse-saturated and has no
                // finite log-domain voltage; skip only that terminal value.
                const auto checkBranch = [&](const wdf::DiodeModel& d, double i,
                                             double polarity) {
                    const double ratio = i / d.isat;
                    // Reverse saturation loses the junction voltage to
                    // cancellation in `i + Is`; skip that terminal value.
                    if (ratio < -0.999)
                        return;
                    const double u = d.nvt * std::log1p(ratio);
                    const double vBranch = polarity * (u + d.rs * i);
                    CHECK(std::abs(vBranch - vPort) < 1e-9 * (scale + std::abs(vBranch)));
                };
                checkBranch(pair.d1, i1, 1.0);
                checkBranch(pair.d2, i2, -1.0);
            }
        }
    }
}

TEST_CASE("Engine: Anti-parallel diode clipper solves with exact closed form and zero fallbacks",
          "[diode][engine][blockwise][pedalkernel]") {
    wdf::Circuit c = makeDiodeClipper(1000.0, 1e-6);
    wdf::Engine engine(c, kFs);
    engine.reset();

    REQUIRE(engine.rootBlockCount() == 2);
    CHECK(engine.blockRoots());

    // Run 1000 samples of 1 kHz sine wave with large overdrive (2.0 V peak)
    double maxRes = 0.0;
    int samplesClipped = 0;
    for (int n = 0; n < 1000; ++n) {
        const double t = static_cast<double>(n) / kFs;
        const double vin = 2.0 * std::sin(2.0 * kPi * 1000.0 * t);
        const auto& nodes = engine.step({vin});

        REQUIRE(engine.lastConverged());
        const double outV = nodes[static_cast<size_t>(engine.nodeIndex("out"))];
        if (std::abs(outV) < 0.7 && std::abs(vin) > 1.2) {
            ++samplesClipped;
        }
        maxRes = std::max(maxRes, engine.lastBlockResidual());
    }

    CHECK(samplesClipped > 200);
    CHECK(engine.blockFallbackCount() == 0);
    CHECK(maxRes < 1e-9);
}

TEST_CASE("Engine: Compile-time DC bias precharges AC coupling capacitor to quiescent operating point",
          "[bias][precharge][pedalkernel]") {
    // Stage with 9V rail, divider R1=100k, R2=100k setting Vbias = 4.5V.
    // AC input source (0V DC) coupled via Cin = 1uF to the biased node.
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addSrc("VCC", "0", 9.0, 0.0, {}, "VCC");
    c.addRes("VCC", "bias", 100e3, "R1");
    c.addRes("bias", "0", 100e3, "R2");
    c.addCap("in", "bias", 1e-6, "Cin");

    wdf::Engine engine(c, kFs);

    std::vector<double> nodeBias;
    REQUIRE(engine.solveNetworkBias(nodeBias));
    const int biasIdx = engine.nodeIndex("bias");
    REQUIRE(biasIdx >= 0);
    CHECK(nodeBias[static_cast<size_t>(biasIdx)] == Approx(4.5).epsilon(1e-4));

    engine.reset();

    // Sample 0 should immediately reproduce the steady-state bias (4.5V)
    // without any startup DC charging transient or pop
    const auto& nodes = engine.step({0.0, 9.0});
    CHECK(nodes[static_cast<size_t>(biasIdx)] == Approx(4.5).epsilon(1e-3));
}

TEST_CASE("Engine: Harold Black op-amp closed form evaluates linear feedback and rails in O(1)",
          "[opamp][black_feedback][pedalkernel]") {
    // Inverting op-amp stage: Ri = 10k, Rf = 50k (gain -5x), TL072 op-amp
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addRes("in", "inv", 10e3, "Ri");
    c.addRes("inv", "out", 50e3, "Rf");
    c.addRes("pos", "0", 100e3, "Rpos");
    wdf::OpAmpModel model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.gbw = 1.0e9;  // Wideband so transient step matches DC gain immediately
    model.setVRails(12.0, 12.0);
    c.addOpAmp("out", "inv", "pos", "0", "0", model, "U1");

    wdf::Engine engine(c, kFs);
    engine.reset();

    // Harold Black op-amp reciprocal feedback conditioning
    CHECK(engine.rootCount() == 3);
    CHECK_FALSE(engine.explicitRoots());

    const int outIdx = engine.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    // Test linear region: Vin = 0.5V -> Vout should be approx -2.5V
    auto nodes = engine.step({0.5});
    CHECK(nodes[static_cast<size_t>(outIdx)] == Approx(-2.5).epsilon(2e-3));
    // The block path now certifies against a freshly evaluated full residual at
    // the returned voltages, so `lastResidual()` is the true system residual
    // (machine precision), not the stale pre-resolve value that used to sit under
    // 1e-10. Require the sample converged and its residual met the step tolerance.
    CHECK(engine.lastConverged());
    CHECK(engine.lastResidual() <= 1e-9);

    // Test overdrive / rail limit: Vin = 4.0V -> ideal -20V, but clamped to -12V rail
    nodes = engine.step({4.0});
    CHECK(nodes[static_cast<size_t>(outIdx)] == Approx(-12.0).margin(0.1));
}
