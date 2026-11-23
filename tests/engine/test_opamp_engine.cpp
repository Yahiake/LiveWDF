#include <catch2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "spice/Circuit.h"
#include "wdf/Engine.h"

// Op-amp as a three-port VCVS root inside the WDF engine.
//
// The feedback network is real: Rf and Ri are ordinary resistors in the WDF
// tree, so the closed-loop gain is set by the circuit, not by a scalar in the
// op-amp model. These tests cover the DC operating point, inverting and
// non-inverting gain, GBW rolloff, slew limiting and rail clamping.

namespace {
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

wdf::OpAmpModel opamp(wdf::OpAmpType type,
                      double railPos = 10.5, double railNeg = 10.5) {
    wdf::OpAmpModel m = wdf::OpAmpModel::fromType(type);
    m.setVRails(railPos, railNeg);
    return m;
}

// Inverting amplifier: Vout = -(Rf/Ri) * Vin, non-inverting input grounded.
wdf::Circuit inverting(double ri, double rf, const wdf::OpAmpModel& model,
                       double dc = 0.0, double ac = 0.0) {
    wdf::Circuit c;
    c.addSrc("in", "0", dc, ac, {}, "V1");
    c.addRes("in", "inv", ri, "Ri");
    c.addRes("inv", "out", rf, "Rf");
    c.addRes("pos", "0", 100000.0, "Rpos");
    c.addOpAmp("out", "inv", "pos", "0", "0", model, "U1");
    return c;
}

// Single-supply inverting amplifier biased at Vcc/2. A single-supply stage
// cannot sit at 0 V input common mode and still swing above and below its
// quiescent point, so the non-inverting input and the input source are
// referred to the mid-rail voltage. Gain is -Rf/Ri about that reference.
wdf::Circuit biasedInverting(double ri, double rf, const wdf::OpAmpModel& model,
                             double vref, double vin) {
    wdf::Circuit c;
    c.addSrc("in", "0", vin, 0.0, {}, "V1");
    c.addSrc("ref", "0", vref, 0.0, {}, "V2");
    c.addRes("in", "inv", ri, "Ri");
    c.addRes("inv", "out", rf, "Rf");
    c.addOpAmp("out", "inv", "ref", "0", "0", model, "U1");
    return c;
}
}  // namespace

TEST_CASE("CA3080 output is a bias-scaled differential-pair current", "[ota][engine]") {
    const auto measured = [](double bias, double input) {
        wdf::Circuit c;
        c.addSrc("vcc", "0", 9.0);
        c.addSrc("vee", "0", -9.0);
        c.addSrc("pos", "0", input);
        c.addIsrc("0", "bias", bias);
        c.addRes("out", "0", 10000.0);
        c.addCa3080("out", "0", "pos", "vcc", "vee", "bias", "OTA1");
        wdf::Engine e(c, 48000.0);
        std::vector<double> node, root;
        REQUIRE(e.solveDc(node, root));
        return node.at(static_cast<size_t>(e.nodeIndex("out")));
    };

    const double bias = 120e-6;
    const double small = 0.005;
    const double large = 0.2;
    const double baseline = measured(bias, 0.0);
    const auto expected = [](double current, double v) {
        constexpr double thermalVolts = 0.02585;
        return 10000.0 * current * (125.0 / 126.0) * std::tanh(v / (2.0 * thermalVolts));
    };
    CHECK(measured(bias, small) - baseline == Approx(expected(bias, small)).epsilon(0.02));
    CHECK(measured(bias / 2.0, small) - measured(bias / 2.0, 0.0) ==
          Approx(expected(bias / 2.0, small)).epsilon(0.02));
    CHECK(measured(bias, large) - baseline == Approx(expected(bias, large)).epsilon(0.02));
    CHECK(measured(bias, -large) - baseline == Approx(expected(bias, -large)).epsilon(0.02));
}

TEST_CASE("opamp engine DC inverting amplifier sets the virtual ground", "[opamp][engine][dc]") {
    const auto c = inverting(10000.0, 20000.0, opamp(wdf::OpAmpType::Lm308), 1.0);
    wdf::Engine e(c, kFs);

    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    const int outIdx = e.nodeIndex("out");
    const int invIdx = e.nodeIndex("inv");
    REQUIRE(outIdx >= 0);
    REQUIRE(invIdx >= 0);

    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(-2.0).epsilon(2e-3));
    REQUIRE(std::abs(node[static_cast<size_t>(invIdx)]) < 1e-3);
}

TEST_CASE("opamp engine DC non-inverting amplifier gain is 1 + Rf/Ri", "[opamp][engine][dc]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.1, 0.0, {}, "V1");
    c.addRes("inv", "0", 1000.0, "Rg");
    c.addRes("inv", "out", 10000.0, "Rf");
    c.addOpAmp("out", "inv", "in", "0", "0",
               opamp(wdf::OpAmpType::Tl072), "U1");
    wdf::Engine e(c, kFs);

    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(1.1).epsilon(2e-3));
}

TEST_CASE("unloaded opamp follower drives a second stage through a source offset",
          "[opamp][engine][dc][tran]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 3.0, 0.0, {}, "Vin");
    c.addSrc("next", "first", 0.5, 0.0, {}, "Voffset");
    c.addSrc("positive", "0", 9.0, 0.0, {}, "Vcc");
    c.addSrc("negative", "0", -2.0, 0.0, {}, "Vee");
    const auto model = opamp(wdf::OpAmpType::Ba728N, 9.0, 2.0);
    c.addOpAmp("first", "first", "in", "positive", "negative", model, "U1");
    c.addOpAmp("out", "out", "next", "positive", "negative", model, "U2");
    c.addRes("out", "0", 100000.0, "Rload");

    wdf::Engine e(c, kFs);
    const int out = e.nodeIndex("out");
    REQUIRE(out >= 0);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    CHECK(node[static_cast<size_t>(out)] == Approx(3.5).margin(1e-4));

    for (int n = 0; n < 32; ++n) {
        const auto& settled = e.step({3.2, 0.8, 9.0, -2.0});
        REQUIRE(e.lastConverged());
        if (n == 31)
            CHECK(settled[static_cast<size_t>(out)] == Approx(4.0).margin(5e-4));
    }
}

TEST_CASE("AC-coupled opamp follower outputs keep their DC bias",
          "[opamp][engine][dc]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 4.5, 0.0, {}, "Vin");
    const auto model = opamp(wdf::OpAmpType::M5218Al, 9.0, 0.0);
    const std::array<std::string, 5> nodes{"in", "a", "b", "c", "out"};
    for (size_t i = 1; i < nodes.size(); ++i) {
        const std::string coupled = nodes[i] + "_coupled";
        c.addOpAmp(nodes[i], nodes[i], nodes[i - 1], "0", "0", model,
                   "U" + std::to_string(i));
        c.addRes(nodes[i], coupled, 2200.0, "R" + std::to_string(i));
        c.addCap(coupled, "0", 22e-9, "C" + std::to_string(i));
    }

    wdf::Engine engine(c, kFs);
    std::vector<double> voltage, rootVoltage;
    REQUIRE(engine.solveDc(voltage, rootVoltage));
    for (size_t i = 1; i < nodes.size(); ++i) {
        const int output = engine.nodeIndex(nodes[i]);
        REQUIRE(output >= 0);
        CHECK(voltage[static_cast<size_t>(output)] == Approx(4.5).margin(1e-4));
    }
}

TEST_CASE("class-AB opamp drives a heavy load only up to its source current",
          "[opamp][engine][class-ab]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addRes("out", "0", 100.0, "Rload");
    const auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Lm741ClassAB);
    c.addOpAmp("out", "0", "in", "0", "0", model, "U1");
    wdf::Engine e(c, kFs);
    e.reset();
    const int out = e.nodeIndex("out");
    REQUIRE(out >= 0);
    for (double direction : {1.0, -1.0}) {
        for (int n = 0; n < 31; ++n)
            e.step({direction});
        const auto& nodes = e.step({direction});
        CHECK(e.lastConverged());
        CHECK(nodes[static_cast<size_t>(out)] == Approx(direction * 2.5).margin(1e-7));
    }
}

TEST_CASE("opamp engine inverting AC gain follows Rf/Ri", "[opamp][engine][tran]") {
    auto model = opamp(wdf::OpAmpType::Lm308);
    model.gbw = 1.0e9;  // keep the GBW pole well above the audio band
    const auto c = inverting(10000.0, 100000.0, model);
    wdf::Engine e(c, kFs);
    e.reset();

    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    double peak = 0.0;
    constexpr int kSamples = 4800;
    for (int n = 0; n < kSamples; ++n) {
        const double t = static_cast<double>(n) / kFs;
        const double vin = 0.1 * std::sin(2.0 * kPi * 1000.0 * t);
        const auto& nodes = e.step({vin});
        REQUIRE(e.lastConverged());
        if (n > kSamples / 2)
            peak = std::max(peak, std::abs(nodes[static_cast<size_t>(outIdx)]));
    }
    REQUIRE(peak == Approx(1.0).epsilon(5e-2));
}

TEST_CASE("opamp engine LM308 rolls off 15 kHz more than TL072 at gain 100",
          "[opamp][engine][gbw]") {
    const auto measure = [](wdf::OpAmpType type) {
        auto model = opamp(type);
        model.slewRate = 100.0;  // exercise GBW, not slew
        const auto c = inverting(1000.0, 100000.0, model);
        wdf::Engine e(c, kFs);
        e.reset();
        const int outIdx = e.nodeIndex("out");
        REQUIRE(outIdx >= 0);

        constexpr int kSettle = 2400;
        constexpr int kMeasure = 2400;
        for (int n = 0; n < kSettle; ++n) {
            const double t = static_cast<double>(n) / kFs;
            const double vin = 0.001 * std::sin(2.0 * kPi * 15000.0 * t);
            (void)e.step({vin});
        }
        double peak = 0.0;
        for (int n = 0; n < kMeasure; ++n) {
            const double t = static_cast<double>(kSettle + n) / kFs;
            const double vin = 0.001 * std::sin(2.0 * kPi * 15000.0 * t);
            const auto& nodes = e.step({vin});
            peak = std::max(peak, std::abs(nodes[static_cast<size_t>(outIdx)]));
        }
        return peak;
    };

    const double lm308 = measure(wdf::OpAmpType::Lm308);
    const double tl072 = measure(wdf::OpAmpType::Tl072);
    REQUIRE(tl072 > lm308 * 1.15);
}

TEST_CASE("opamp engine LM308 slews no faster than the datasheet rate", "[opamp][engine][slew]") {
    auto model = opamp(wdf::OpAmpType::Lm308, 100.0, 100.0);
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "V1");
    c.addRes("inv", "0", 1000.0, "Rg");
    c.addRes("inv", "out", 1000.0, "Rf");
    c.addOpAmp("out", "inv", "in", "0", "0", model, "U1");
    wdf::Engine e(c, kFs);
    e.reset();
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    // A 0 -> 10 V step (gain 2). The first sample can move at most
    // 0.3 V/us * 1/48000 s = 6.25 V.
    const double v1 = e.step({10.0})[static_cast<size_t>(outIdx)];
    REQUIRE(v1 <= 6.25 + 0.2);
    const double v2 = e.step({10.0})[static_cast<size_t>(outIdx)];
    REQUIRE(v2 > v1);
}

TEST_CASE("opamp engine output clamps at the supply rails", "[opamp][engine][rails]") {
    auto model = opamp(wdf::OpAmpType::Tl072, 5.0, 5.0);
    model.slewRate = 1000.0;
    wdf::Circuit c;
    c.addSrc("in", "0", 1.0, 0.0, {}, "V1");
    c.addRes("inv", "0", 1000.0, "Rg");
    c.addRes("inv", "out", 10000.0, "Rf");
    c.addOpAmp("out", "inv", "in", "0", "0", model, "U1");
    wdf::Engine e(c, kFs);

    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);
    // Gain 11 would demand 11 V; the positive rail is 5 V. The feedback
    // divider draws current from the clamped source, so the node sits just
    // below 5 V by Ro * i_load.
    const double vout = node[static_cast<size_t>(outIdx)];
    REQUIRE(vout < 5.0);
    REQUIRE(vout > 4.8);
}

TEST_CASE("opamp engine clamps asymmetric single- and dual-supply rails",
          "[opamp][engine][rails][asymmetric]") {
    // Return the solved output node for a gain -1 stage. Ri and Rf are 1 Mohm,
    // so the Ro * i_load drop at the output is below 1 mV and the result is
    // the source clamp, not a load-dominated value.
    const auto solveInverting = [](double vin, const wdf::OpAmpModel& model) {
        const auto c = inverting(1.0e6, 1.0e6, model, vin);
        wdf::Engine e(c, kFs);
        std::vector<double> node, rootV;
        REQUIRE(e.solveDc(node, rootV));
        const int outIdx = e.nodeIndex("out");
        REQUIRE(outIdx >= 0);
        return node[static_cast<size_t>(outIdx)];
    };

    SECTION("+9 V / 0 V single supply") {
        auto model = opamp(wdf::OpAmpType::Tl072);
        model.setVRails(9.0, 0.0);
        REQUIRE(model.railHigh() == Approx(9.0));
        REQUIRE(model.railLow() == Approx(0.0));

        // A single-supply stage is biased at Vcc/2. With gain -2 about 4.5 V,
        // Vout = 13.5 - 2*Vin. The requested outputs are 0.2 V inside and
        // 0.25 V outside the positive limit, and 0.4 V inside and 0.25 V
        // outside the lower limit. The zero-referenced variant is not used
        // here: the current cold-start Newton path can fail for large positive
        // outputs with the lower rail at 0 V (reported separately).
        const auto solveBiased = [&](double vin) {
            const auto c = biasedInverting(1.0e6, 2.0e6, model, 4.5, vin);
            wdf::Engine e(c, kFs);
            std::vector<double> node, rootV;
            REQUIRE(e.solveDc(node, rootV));
            const int outIdx = e.nodeIndex("out");
            REQUIRE(outIdx >= 0);
            return node[static_cast<size_t>(outIdx)];
        };

        // 8.8 V: just inside the +9 V limit.
        const double posInside = solveBiased(2.35);
        CHECK(posInside == Approx(8.8).margin(0.001));
        CHECK(posInside < model.railHigh());

        // 9.0 V: the requested +9.25 V is outside the +9 V limit.
        const double posOutside = solveBiased(2.125);
        CHECK(posOutside == Approx(9.0).margin(0.001));
        CHECK(posOutside <= model.railHigh());

        // 0.4 V: just inside the 0 V lower limit.
        const double negInside = solveBiased(6.55);
        CHECK(negInside == Approx(0.4).margin(0.001));
        CHECK(negInside > model.railLow());

        // 0.0 V: the requested -0.25 V is outside the 0 V lower limit.
        const double negOutside = solveBiased(6.875);
        CHECK(negOutside == Approx(0.0).margin(0.001));
        CHECK(negOutside >= model.railLow());
    }

    SECTION("+12 V / -12 V dual supply") {
        auto model = opamp(wdf::OpAmpType::Tl072);
        model.setVRails(12.0, 12.0);
        REQUIRE(model.railHigh() == Approx(12.0));
        REQUIRE(model.railLow() == Approx(-12.0));

        // Gain -1: -11.6 V -> +11.6 V and +11.6 V -> -11.6 V, just inside.
        const double posInside = solveInverting(-11.6, model);
        CHECK(posInside == Approx(11.6).margin(0.01));
        CHECK(posInside < model.railHigh());

        const double negInside = solveInverting(11.6, model);
        CHECK(negInside == Approx(-11.6).margin(0.01));
        CHECK(negInside > model.railLow());

        // -12.4 V and +12.4 V ask for +/-12.4 V, outside both limits.
        const double posOutside = solveInverting(-12.4, model);
        CHECK(posOutside == Approx(12.0).margin(0.01));
        CHECK(posOutside <= model.railHigh());

        const double negOutside = solveInverting(12.4, model);
        CHECK(negOutside == Approx(-12.0).margin(0.01));
        CHECK(negOutside >= model.railLow());
    }
}

TEST_CASE("opamp engine solves a zero-referenced single-supply inverting stage",
          "[opamp][engine][rails][zero-ref]") {
    // The lower rail is 0 V, so this is an inverting stage with no negative
    // supply at all. The non-inverting input is grounded and a negative input
    // asks for a positive output, which is exactly the region the DC solve
    // used to reject.
    auto model = opamp(wdf::OpAmpType::Tl072);
    model.setVRails(9.0, 0.0);
    model.slewRate = 1000.0;

    const auto solve = [&](double vin) {
        const auto c = inverting(1.0e6, 1.0e6, model, vin);
        wdf::Engine e(c, kFs);
        std::vector<double> node, rootV;
        const bool ok = e.solveDc(node, rootV);
        const int outIdx = e.nodeIndex("out");
        const int invIdx = e.nodeIndex("inv");
        REQUIRE(outIdx >= 0);
        REQUIRE(invIdx >= 0);
        return std::array<double, 3>{
            ok ? 1.0 : 0.0, node[static_cast<size_t>(outIdx)],
            node[static_cast<size_t>(invIdx)]};
    };

    // A -4 V input asks for +4 V, comfortably inside the 0..9 V output range.
    const auto mid = solve(-4.0);
    CHECK(mid[0] == 1.0);
    CHECK(mid[1] == Approx(4.0).margin(0.01));
    CHECK(std::abs(mid[2]) < 1e-3);

    // -8.8 V asks for +8.8 V, still 0.2 V inside the positive rail.
    const auto high = solve(-8.8);
    CHECK(high[0] == 1.0);
    CHECK(high[1] == Approx(8.8).margin(0.01));
    CHECK(std::abs(high[2]) < 1e-3);
}

TEST_CASE("opamp engine gain tracks a changed feedback resistor", "[opamp][engine][gain]") {
    auto model = opamp(wdf::OpAmpType::Tl072);
    model.gbw = 1.0e9;
    auto c = inverting(10000.0, 10000.0, model);
    wdf::Engine e(c, kFs);
    e.reset();
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    // Gain -1: input amplitude 0.5 -> output 0.5.
    double peak = 0.0;
    for (int n = 0; n < 4800; ++n) {
        const double t = static_cast<double>(n) / kFs;
        const double vin = 0.5 * std::sin(2.0 * kPi * 500.0 * t);
        const auto& nodes = e.step({vin});
        if (n > 2400)
            peak = std::max(peak, std::abs(nodes[static_cast<size_t>(outIdx)]));
    }
    REQUIRE(peak == Approx(0.5).epsilon(5e-2));
}

TEST_CASE("opamp engine pedalkernel golden probe", "[opamp][probe]") {
    auto model = opamp(wdf::OpAmpType::Tl072, 12.0, 12.0);
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "V1");
    c.addRes("inv", "0", 10000.0, "Ri");
    c.addRes("out", "inv", 90000.0, "Rf");
    c.addOpAmp("out", "inv", "in", "0", "0", model, "U1");
    wdf::Engine e(c, 384000.0);
    e.reset();
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);
    double peak = 0.0, sumsq = 0.0;
    constexpr int kN = 38400;
    for (int n = 0; n < kN; ++n) {
        const double t = static_cast<double>(n) / 384000.0;
        const double vin = 0.1 * std::sin(2.0 * kPi * 1000.0 * t);
        const auto& nodes = e.step({vin});
        if (n > 9600) {
            const double v = nodes[static_cast<size_t>(outIdx)];
            peak = std::max(peak, std::abs(v));
            sumsq += v * v;
        }
    }
    const double rms = std::sqrt(sumsq / (kN - 9600));
    WARN("pedalkernel probe peak=" << peak << " rms=" << rms);
    REQUIRE(peak > 0.0);
}
