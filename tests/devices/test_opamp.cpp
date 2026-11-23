#include <catch2.hpp>

#include <cmath>
#include <limits>

#include "devices/OpAmp.h"

// Op-amp model and one-port root: the PedalKernel contract.
//
// The three-port engine path is tested in tests/engine/test_opamp_engine.cpp.
// These tests pin the model cards and the standalone `OpAmpRoot` ordering:
// gain -> GBW lowpass -> optional soft clip -> rail clamp -> slew limit.

namespace {
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double drive(wdf::OpAmpRoot& root, double a) {
    const double b = root.process(a, 10000.0);
    return 0.5 * (a + b);
}
}  // namespace

TEST_CASE("opamp model cards match the PedalKernel datasheet values", "[opamp][model]") {
    const auto lm308 = wdf::OpAmpModel::fromType(wdf::OpAmpType::Lm308);
    REQUIRE(lm308.openLoopGain == Approx(300000.0));
    REQUIRE(lm308.gbw == Approx(1.0e6));
    REQUIRE(lm308.slewRate == Approx(0.3));
    REQUIRE(lm308.outputImpedance == Approx(200.0));
    REQUIRE(lm308.outputCapacitance == Approx(30e-12));

    const auto tl072 = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    REQUIRE(tl072.openLoopGain == Approx(200000.0));
    REQUIRE(tl072.gbw == Approx(3.0e6));
    REQUIRE(tl072.slewRate == Approx(13.0));
    REQUIRE(tl072.outputImpedance == Approx(75.0));

    const auto ne5532 = wdf::OpAmpModel::fromType(wdf::OpAmpType::Ne5532);
    REQUIRE(ne5532.gbw == Approx(10.0e6));
    REQUIRE(ne5532.slewRate == Approx(9.0));
    REQUIRE(ne5532.outputImpedance == Approx(50.0));

    REQUIRE(wdf::opAmpTypeFromString("TL072") == wdf::OpAmpType::Tl072);
    REQUIRE(wdf::opAmpTypeFromString("lm308") == wdf::OpAmpType::Lm308);
    REQUIRE(wdf::opAmpTypeFromString("4558") == wdf::OpAmpType::Jrc4558);
    REQUIRE(wdf::opAmpTypeFromString("rc4558") == wdf::OpAmpType::Rc4558);
    REQUIRE(wdf::opAmpTypeFromString(
                wdf::opAmpTypeName(wdf::OpAmpType::Rc4558)) ==
            wdf::OpAmpType::Rc4558);
    REQUIRE(wdf::opAmpTypeFromString("unknown-part") == wdf::OpAmpType::Generic);
}

TEST_CASE("MCI2001 card reproduces its published 600 ohm output rating", "[opamp][model]") {
    const auto type = wdf::opAmpTypeFromString("MCI-2001");
    REQUIRE(std::string_view(wdf::opAmpTypeName(type)) == "MCI2001");
    const auto m = wdf::OpAmpModel::fromType(type);
    CHECK(20 * std::log10(m.openLoopGain) == Approx(90.0));
    CHECK(m.gbw / std::pow(10.0, 50.0 / 20.0) == Approx(20000.0));
    CHECK(m.slewRate == Approx(2.3));
    const double loadedPeak = m.railHigh() * 600.0 / (600.0 + m.outputImpedance);
    CHECK(10 * std::log10(loadedPeak * loadedPeak / (2.0 * 600.0 * 0.001)) == Approx(24.0).margin(0.01));
    CHECK(wdf::opAmpSupplyMax(type) == 50.0);
}

TEST_CASE("opamp GBW lowpass coefficient is gain dependent and Nyquist safe", "[opamp][gbw]") {
    const auto tl072 = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    const double lowGain = tl072.gbwCoeff(10.0, kFs);
    const double highGain = tl072.gbwCoeff(1000.0, kFs);
    REQUIRE(lowGain > highGain);  // higher gain -> lower closed-loop bandwidth
    REQUIRE(lowGain > 0.0);
    REQUIRE(highGain > 0.0);
    // f_cl >= fs/2 is transparent.
    REQUIRE(tl072.gbwCoeff(0.5, kFs) == Approx(1.0));
    REQUIRE(tl072.gbwCoeff(1.0, kFs) == Approx(1.0));
}

TEST_CASE("pedal-specific opamp cards use published typical dynamics", "[opamp][model]") {
    const auto expected = GENERATE(
        table<const char*, double, double, double>({
            {"M5218AL", 316227.766, 7.0e6, 3.0},
            {"TL022", 10000.0, 0.5e6, 0.5},
            {"MC33174", 500000.0, 1.8e6, 2.1},
            {"MC33179", 200000.0, 5.0e6, 2.0},
            {"BA728N", 100000.0, 0.7e6, 0.7},
            {"TL061", 6000.0, 1.0e6, 3.5},
        }));
    const auto [name, gain, gbw, slewRate] = expected;
    const auto type = wdf::opAmpTypeFromString(name);
    REQUIRE(type != wdf::OpAmpType::Generic);
    REQUIRE(std::string_view(wdf::opAmpTypeName(type)) == name);

    const auto model = wdf::OpAmpModel::fromType(type);
    REQUIRE(model.openLoopGain == Approx(gain));
    REQUIRE(model.gbw == Approx(gbw));
    REQUIRE(model.slewRate == Approx(slewRate));
}

TEST_CASE("opamp signed bounds allow positive single-supply headroom", "[opamp][model]") {
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::M5218Al);
    model.setVRailBounds(1.5, 7.5);
    CHECK(model.evalStatic(-0.1) == 1.5);
    CHECK(model.evalStatic(0.1) == 7.5);
    CHECK(model.evalStatic(4.5 / model.openLoopGain) == Approx(4.5));
}

TEST_CASE("LM324-N datasheet card supports single-supply rails", "[opamp][model]") {
    const auto type = wdf::opAmpTypeFromString("LM324-N");
    REQUIRE(type == wdf::OpAmpType::Lm324);
    REQUIRE(wdf::opAmpTypeFromString(wdf::opAmpTypeName(type)) == type);
    REQUIRE(wdf::opAmpSupplyMax(type) == Approx(32.0));

    auto model = wdf::OpAmpModel::fromType(type);
    CHECK(model.openLoopGain == Approx(100000.0));
    CHECK(model.gbw == Approx(1.0e6));
    CHECK(model.slewRate == Approx(0.5));
    model.setVRailBounds(0.005, 7.0);
    CHECK(model.evalStatic(-0.0001) == Approx(0.005));
    CHECK(model.evalStatic(0.0001) == Approx(7.0));
    CHECK(model.evalStatic(40e-6) == Approx(4.0));
}

TEST_CASE("MiSTer class-AB stage has a small-signal slope and bounded drive", "[opamp][model][class-ab]") {
    const auto type = wdf::opAmpTypeFromString("LM741-Class-AB");
    REQUIRE(type == wdf::OpAmpType::Lm741ClassAB);
    const auto model = wdf::OpAmpModel::fromType(type);
    REQUIRE(model.valid());
    double slope = 0.0;
    CHECK(model.outputCurrent(0.0, slope) == 0.0);
    CHECK(slope == Approx(1.0 / model.outputImpedance));
    CHECK(model.outputCurrent(1.0, slope) == Approx(25e-3));
    CHECK(slope == 0.0);
    CHECK(model.outputCurrent(-1.0, slope) == Approx(-25e-3));
    CHECK(slope == 0.0);

    double unused = 0.0;
    const double below = model.outputCurrent(0.13 - 1e-6, unused);
    const double above = model.outputCurrent(0.13 + 1e-6, unused);
    model.outputCurrent(0.13, slope);
    CHECK(slope == Approx((above - below) / 2e-6).epsilon(1e-7));

    const auto legacy = wdf::OpAmpModel::fromType(wdf::OpAmpType::Lm741);
    CHECK_FALSE(legacy.hasClassABOutput());
    CHECK(legacy.outputCurrent(1.0, slope) == Approx(1.0 / 75.0));
}

TEST_CASE("opamp LM308 rolls off high frequencies more than TL072", "[opamp][gbw]") {
    // Same test as PedalKernel: gain 100, 15 kHz. LM308 f_cl = 10 kHz,
    // TL072 f_cl = 30 kHz, so the LM308 output must be visibly smaller.
    auto lmModel = wdf::OpAmpModel::fromType(wdf::OpAmpType::Lm308);
    lmModel.slewRate = 100.0;   // keep the slew clamp out of the measurement
    lmModel.setVRails(1000.0, 1000.0);
    auto tlModel = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    tlModel.slewRate = 100.0;
    tlModel.setVRails(1000.0, 1000.0);

    auto lm = wdf::OpAmpRoot::inverting(lmModel, 100.0);
    auto tl = wdf::OpAmpRoot::inverting(tlModel, 100.0);
    lm.setSampleRate(kFs);
    tl.setSampleRate(kFs);

    const auto measure = [](wdf::OpAmpRoot& root) {
        const double freq = 15000.0;
        for (int s = 0; s < 500; ++s) {
            const double v = 0.001 * std::sin(2.0 * kPi * freq * s / kFs);
            drive(root, 2.0 * v);
        }
        double peak = 0.0;
        for (int s = 500; s < 600; ++s) {
            const double v = 0.001 * std::sin(2.0 * kPi * freq * s / kFs);
            peak = std::max(peak, std::abs(drive(root, 2.0 * v)));
        }
        return peak;
    };

    const double peakLm = measure(lm);
    const double peakTl = measure(tl);
    REQUIRE(peakTl > peakLm * 1.1);
}

TEST_CASE("opamp LM308 slew limits a step to the datasheet rate", "[opamp][slew]") {
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Lm308);
    model.setVRails(100.0, 100.0);
    auto root = wdf::OpAmpRoot::nonInverting(model, 1.0);
    root.setSampleRate(kFs);

    for (int i = 0; i < 100; ++i) {
        root.setVp(0.0);
        drive(root, 0.0);
    }
    root.setVp(10.0);
    const double v1 = drive(root, 0.0);
    const double maxDv = 0.3e6 / kFs;  // 6.25 V per sample
    REQUIRE(v1 == Approx(maxDv).epsilon(1e-9));
    REQUIRE(v1 < 10.0);

    // It takes ceil(10/6.25) = 2 samples to reach the target.
    const double v2 = drive(root, 0.0);
    REQUIRE(v2 == Approx(10.0).epsilon(1e-9));
}

TEST_CASE("opamp output clamps to the asymmetric supply rails", "[opamp][rails]") {
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.slewRate = 1000.0;  // do not let slew mask the rail clamp
    model.setVRails(2.0, 1.0);
    auto root = wdf::OpAmpRoot::nonInverting(model, 10.0);
    root.setSampleRate(kFs);

    root.setVp(1.0);  // target +10 V, positive rail +2 V
    REQUIRE(drive(root, 0.0) == Approx(2.0).epsilon(1e-9));

    root.setVp(-1.0);  // target -10 V, negative rail -1 V
    REQUIRE(drive(root, 0.0) == Approx(-1.0).epsilon(1e-9));
}

TEST_CASE("opamp inverting and non-inverting static gains", "[opamp][gain]") {
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.gbw = 1.0e9;       // coefficient = 1: no bandwidth rolloff
    model.slewRate = 1.0e6;  // no slew limiting
    model.setVRails(100.0, 100.0);

    auto inv = wdf::OpAmpRoot::inverting(model, 10.0);
    inv.setSampleRate(kFs);
    REQUIRE(drive(inv, 2.0 * 0.2) == Approx(-2.0).epsilon(1e-9));

    auto non = wdf::OpAmpRoot::nonInverting(model, 11.0);
    non.setSampleRate(kFs);
    non.setVp(0.2);
    REQUIRE(drive(non, 0.0) == Approx(2.2).epsilon(1e-9));
}

TEST_CASE("opamp soft clip limits feedback-diode output without hard corners", "[opamp][softclip]") {
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.gbw = 1.0e9;
    model.slewRate = 1.0e6;
    model.setVRails(100.0, 100.0);

    auto root = wdf::OpAmpRoot::nonInverting(model, 10.0);
    root.setSampleRate(kFs);
    root.setSoftClip(0.7);
    root.setVp(1.0);  // target 10 V, tanh-limited toward 0.7 V

    const double v = drive(root, 0.0);
    REQUIRE(v > 0.69);
    REQUIRE(v < 0.71);
}
