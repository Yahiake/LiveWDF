#include <catch2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "devices/BucketBrigade.h"
#include "spice/SpiceExport.h"
#include "wdf/Engine.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kClock = 100000.0;

wdf::BucketBrigadeModel clockedModel() {
    wdf::BucketBrigadeModel model;
    model.stages = 1024;
    model.clockHz = kClock;
    model.periodSecondsPerVolt = 3e-6;
    model.inputBias = 0.0;
    model.outputBias = 0.0;
    model.inputSwing = 1.0;
    model.gain = 1.0;
    return model;
}

} // namespace

TEST_CASE("BBD samples every clock event above the host rate", "[bbd][dsp]") {
    const auto model = clockedModel();
    wdf::BucketBrigadeState state(model);
    constexpr double slope = 20.0;

    // 25 clock periods per 12 host frames. For a linear ramp, interpolation
    // at the clock event recovers the exact input value at that event.
    for (int frame = 1; frame <= 1200; ++frame) {
        const auto clockEdge = (25 * frame) / 12;
        const auto expected = std::max(0.0, slope * (clockEdge - 512.0) / kClock);
        const auto actual = state.step(slope * frame / kSampleRate, 0.0, 1.0 / kSampleRate);
        REQUIRE(actual == Approx(expected).margin(1e-12));
    }

    state.reset(0.0);
    REQUIRE(state.outputCurrent() == 0.0);
    REQUIRE(model.clockPeriod(1.0) == Approx(13e-6).margin(1e-12));
    int firstWetFrame = 0;
    for (int frame = 1; frame <= 400; ++frame) {
        const auto actual = state.step(0.2, 1.0, 1.0 / kSampleRate);
        if (actual > 0.1) {
            firstWetFrame = frame;
            break;
        }
    }
    // The first shifted charge reaches output on edge 513.
    REQUIRE(firstWetFrame == static_cast<int>(std::ceil(513.0 * 13e-6 * kSampleRate)));
}

TEST_CASE("engine BBD resets its wet path and recirculates delayed audio", "[bbd][engine]") {
    const auto model = clockedModel();
    wdf::Circuit circuit;
    circuit.addSrc("dry", "0");
    circuit.addSrc("clock", "0");
    circuit.addRes("dry", "mix", 10000.0);
    circuit.addVCVS("feedback", "0", "wet", "0", 1.0, 1.0, "FeedbackBuffer");
    circuit.addRes("feedback", "mix", 100000.0);
    circuit.addRes("wet", "0", 100000.0);
    circuit.addBucketBrigade("mix", "wet", "clock", "0", model, "BBD1");

    wdf::Engine engine(circuit, kSampleRate);
    const auto wet = engine.nodeIndex("wet");
    REQUIRE(wet >= 0);

    for (int reset = 0; reset < 2; ++reset) {
        engine.reset();
        std::array<double, 3> peaks{};
        int firstWetFrame = -1;
        for (int frame = 0; frame < 1000; ++frame) {
            const std::array<double, 2> sourceValues{
                frame >= 30 && frame < 51 ? 0.6 : 0.0, 0.0
            };
            const auto actual = std::abs(engine.step(sourceValues.data(), sourceValues.size())[wet]);
            if (firstWetFrame < 0 && actual > 1e-4)
                firstWetFrame = frame;
            for (int echo = 0; echo < 3; ++echo) {
                if (frame >= 30 + 240 * (echo + 1) && frame < 51 + 265 * (echo + 1))
                    peaks[echo] = std::max(peaks[echo], actual);
            }
        }
        // Capturing a charge and presenting the held output to the following
        // electrical solve quantizes the onset to one host frame.
        REQUIRE(firstWetFrame >= 30 + 246);
        REQUIRE(firstWetFrame <= 30 + 247);
        REQUIRE(peaks[0] > 0.4);
        REQUIRE(peaks[1] / peaks[0] == Approx(0.086).margin(0.005));
        REQUIRE(peaks[2] / peaks[1] == Approx(0.086).margin(0.005));
    }

    const auto deck = wdf::spiceExport(circuit, kSampleRate, 0.05);
    REQUIRE(deck.ok);
    REQUIRE(deck.diagnostics.find("transient wet path omitted") != std::string::npos);
}

TEST_CASE("BBD settles and seeds the wet history at an off-center DC bias",
          "[bbd][engine][dc]") {
    auto model = clockedModel();
    model.inputBias = 0.2;
    model.outputBias = 0.1;
    constexpr double load = 100000.0;

    wdf::Circuit circuit;
    circuit.addSrc("dry", "0", 0.42);
    circuit.addRes("dry", "mix", 10000.0);
    circuit.addVCVS("feedback", "0", "wet", "0", 1.0, 1.0, "FeedbackBuffer");
    circuit.addRes("feedback", "mix", 100000.0);
    circuit.addRes("wet", "0", load);
    circuit.addBucketBrigade("mix", "wet", "0", "0", model);
    wdf::Engine engine(circuit, kSampleRate);

    const int wet = engine.nodeIndex("wet");
    const int mix = engine.nodeIndex("mix");
    REQUIRE(wet >= 0);
    REQUIRE(mix >= 0);
    std::vector<double> dc, roots;
    REQUIRE(engine.solveDc(dc, roots));
    const double expected = model.outputVoltage(dc[mix]) / (1.0 + model.outputOhms / load);
    REQUIRE(dc[wet] == Approx(expected).margin(1e-8));

    // Charge the line with unrelated audio first; reset must use the solved
    // DC bias, independently of the contents of the previous wet history.
    const std::array<double, 1> audioSource{0.7};
    for (int n = 0; n < 1000; ++n)
        (void)engine.step(audioSource.data(), audioSource.size());
    engine.reset();
    const std::array<double, 1> dcSource{0.42};
    for (int n = 0; n < 1000; ++n) {
        const auto sample = engine.step(dcSource.data(), dcSource.size())[wet];
        REQUIRE(sample == Approx(expected).margin(1e-8));
    }
}
