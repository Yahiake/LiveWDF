#include <catch2.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "spice/Net.h"
#include "spice/SpiceNet.h"
#include "wdf/Engine.h"

// Phase 1e: Voltage-Controlled Voltage Source (VCVS / Gain Block / Op-Amp Buffer).

TEST_CASE("VCVS unity gain buffer preserves voltage and draws zero input current", "[vcvs][engine]") {
    // 2.5 V DC source -> buffer -> 10k load resistor
    const auto c = wdf::buildFromString(
        "V1 in 0 DC 2.5\n"
        "E1 out 0 in 0 1.0 1e-4\n"
        "Rload out 0 10k\n");

    REQUIRE(c.vcvs.size() == 1);
    REQUIRE(c.vcvs[0].gain == 1.0);
    REQUIRE(c.vcvs[0].ro == 1e-4);

    wdf::Engine engine(c, 48000.0);
    const int outIdx = engine.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    std::vector<double> node, rootV;
    REQUIRE(engine.solveDc(node, rootV));
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(2.5).epsilon(1e-4));
}

TEST_CASE("VCVS fixed gain block with finite output impedance divides across load", "[vcvs][engine]") {
    // Input = 1.2 V. Gain = 3.5. Ro = 1.0 Ohm. Load = 99 Ohm.
    // Thevenin open-circuit V = 3.5 * 1.2 = 4.2 V.
    // Loaded Vout = 4.2 * 99 / (1 + 99) = 4.158 V.
    const auto c = wdf::buildFromString(
        "V1 in 0 DC 1.2\n"
        "E1 out 0 in 0 3.5 1.0\n"
        "Rload out 0 99.0\n");

    wdf::Engine engine(c, 48000.0);
    const int outIdx = engine.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    std::vector<double> node, rootV;
    REQUIRE(engine.solveDc(node, rootV));
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(4.158).epsilon(1e-4));
}

TEST_CASE("VCVS operational amplifier with negative feedback forms inverting amplifier", "[vcvs][engine]") {
    // Vin = 1.0 V.
    // R1 = 10k from 'in' to 'inv'.
    // Rf = 20k from 'inv' to 'out'.
    // Op-amp: E_op out 0 0 inv 100000 0.1 (inverting gain A = 1e5).
    // Closed-loop gain = -Rf / R1 = -2.0. With A=1e5: Vout = -1.99994 V.
    // Node 'inv' is virtual ground: Vinv = -Vout / A = 20 uV.
    const auto c = wdf::buildFromString(
        "V1 in 0 DC 1.0\n"
        "R1 in inv 10k\n"
        "Rf inv out 20k\n"
        "E_op out 0 0 inv 100000 0.1\n");

    wdf::Engine engine(c, 48000.0);
    const int invIdx = engine.nodeIndex("inv");
    const int outIdx = engine.nodeIndex("out");
    REQUIRE(invIdx >= 0);
    REQUIRE(outIdx >= 0);

    std::vector<double> node, rootV;
    REQUIRE(engine.solveDc(node, rootV));
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(-1.99994).epsilon(1e-4));
    REQUIRE(std::abs(node[static_cast<size_t>(invIdx)]) < 1e-4); // Virtual ground < 100 uV
}

TEST_CASE("VCVS operates in linear scattering with zero iterations during audio rate step", "[vcvs][engine]") {
    // Gain block A = 2.0 driving an RC lowpass filter (R = 1k, C = 100n)
    const auto c = wdf::buildFromString(
        "V1 in 0 DC 0.0 AC 1.0 SIN(0.0 1.0 1000)\n"
        "E1 mid 0 in 0 2.0 1e-4\n"
        "R1 mid out 1k\n"
        "C1 out 0 100n\n");

    wdf::Engine engine(c, 48000.0);
    engine.reset();

    const int outIdx = engine.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    constexpr int kSamples = 480;
    for (int n = 0; n < kSamples; ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        const double vin = std::sin(2.0 * M_PI * 1000.0 * t);
        const auto& nodes = engine.step({vin});

        // Zero nonlinear roots -> zero Newton iterations, zero device evaluations
        REQUIRE(engine.newtonIterations() == 0);
        REQUIRE(engine.deviceEvaluations() == 0);
        REQUIRE(engine.lastConverged());

        // Output stays finite and bounded by gain * vin peak
        REQUIRE(std::abs(nodes[static_cast<size_t>(outIdx)]) <= 2.1);
    }
}

TEST_CASE("VCVS validation rejects non-physical negative or zero output resistance", "[vcvs][validation]") {
    wdf::Circuit c;
    c.addVCVS("out", "0", "in", "0", 2.0, 0.0, "E_zero");
    REQUIRE_THROWS_AS(wdf::compile(c, {}), std::runtime_error);

    wdf::Circuit cNeg;
    cNeg.addVCVS("out", "0", "in", "0", 2.0, -1.0, "E_neg");
    REQUIRE_THROWS_AS(wdf::compile(cNeg, {}), std::runtime_error);
}
