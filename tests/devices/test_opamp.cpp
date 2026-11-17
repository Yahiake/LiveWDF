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
