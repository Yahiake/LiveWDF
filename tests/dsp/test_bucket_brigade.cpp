#include <catch2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
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
    model.transferLoss = 0.0;
    model.kneeWidth = 0.0;
    model.evenOrder = 0.0;
    model.slowClockLossDb = 0.0;
    return model;
}

} // namespace

TEST_CASE("BBD samples every clock event above the host rate", "[bbd][dsp]") {
    const auto model = clockedModel();
    wdf::BucketBrigadeState state(model);
    constexpr double slope = 20.0;

    // 25 clock periods per 12 host frames. Past the ramp's corner the
    // band-limited staircase is the ramp delayed by 512.5 clock periods.
    for (int frame = 1; frame <= 1200; ++frame) {
        const auto actual = state.step(slope * frame / kSampleRate, 0.0, 1.0 / kSampleRate);
        if (frame >= 300)
            REQUIRE(actual == Approx(slope * (frame / kSampleRate - 512.5 / kClock)).margin(1e-7));
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
    // The first shifted charge reaches output on edge 513, the band-limited
    // step crossing half way within a frame of it.
    REQUIRE(std::abs(firstWetFrame - 513.0 * 13e-6 * kSampleRate) < 1.0);
}

TEST_CASE("MN3005 transfer loss matches the data sheet's treble loss at a 40 kHz clock", "[bbd][dsp]") {
    wdf::BucketBrigadeModel model;
    model.clockHz = 40000.0;
    model.inputBias = model.outputBias = 0.0;
    model.gain = 1.0;
    // The output is the band-limited staircase, so the data sheet's -1.9 dB
    // at 10 kHz includes the held output's -0.9 dB.
    const auto level = [&](double hz) {
        wdf::BucketBrigadeState state(model);
        std::complex<double> sum;
        for (int n = 0; n < 6048; ++n) {
            const auto phase = 2.0 * M_PI * hz * n / model.clockHz;
            const auto out = state.step(std::sin(phase), 0.0, 1.0 / model.clockHz);
            if (n >= 4048)
                sum += out * std::polar(1.0, -phase);
        }
        return std::abs(sum);
    };
    REQUIRE(20.0 * std::log10(level(10000.0) / level(1000.0)) == Approx(-1.9).margin(0.1));
}

TEST_CASE("MN3005 transfer has the data sheet's distortion at its rated input", "[bbd][dsp]") {
    const wdf::BucketBrigadeModel model;
    // 1.0 Vrms, where the data sheet's typical THD curve reads 0.35%.
    std::array<std::complex<double>, 6> harmonics{};
    for (int n = 0; n < 1024; ++n) {
        const auto phase = 2.0 * M_PI * n / 1024.0;
        const auto out = model.outputVoltage(model.inputBias + std::sqrt(2.0) * std::sin(phase));
        for (std::size_t h = 0; h < harmonics.size(); ++h)
            harmonics[h] += out * std::polar(1.0, -phase * (h + 1.0));
    }
    double distortion = 0.0;
    for (std::size_t h = 1; h < harmonics.size(); ++h)
        distortion += std::norm(harmonics[h]);
    REQUIRE(std::sqrt(distortion) / std::abs(harmonics[0]) == Approx(0.0035).margin(0.001));
}

TEST_CASE("BBD output keeps the clock images of a tone out of the host band", "[bbd][dsp]") {
    auto model = clockedModel();
    model.clockHz = 41800.0;
    wdf::BucketBrigadeState state(model);
    // A 3 kHz tone's image at fcp - f = 38.8 kHz folds to 9.2 kHz at 48 kHz.
    std::complex<double> tone, image;
    for (int n = 0; n < 60000; ++n) {
        const auto out = state.step(0.5 * std::sin(2.0 * M_PI * 3000.0 * n / kSampleRate), 0.0, 1.0 / kSampleRate);
        if (n >= 12000) {
            tone += out * std::polar(1.0, -2.0 * M_PI * 3000.0 * n / kSampleRate);
            image += out * std::polar(1.0, -2.0 * M_PI * 9200.0 * n / kSampleRate);
        }
    }
    REQUIRE(20.0 * std::log10(std::abs(image) / std::abs(tone)) < -60.0);
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
            if (firstWetFrame < 0 && actual > 0.2)
                firstWetFrame = frame;
            for (int echo = 0; echo < 3; ++echo) {
                if (frame >= 30 + 240 * (echo + 1) && frame < 51 + 265 * (echo + 1))
                    peaks[echo] = std::max(peaks[echo], actual);
            }
        }
        // The band-limited pulse crosses half height at the delayed edge,
        // quantized to one host frame.
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

TEST_CASE("an invalid BBD clock holds its last charge and resumes on the next valid sample", "[bbd][dsp]") {
    auto model = clockedModel();
    model.stages = 4;
    wdf::BucketBrigadeState reference(model), device(model);
    const double period = 1.0 / kSampleRate;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    for (int sample = 0; sample < 8; ++sample) {
        const double input = .1 * sample;
        const double expected = reference.step(input, 0.0, period);
        REQUIRE(device.step(input, 0.0, period) == expected);
        REQUIRE(device.step(.9, nan, period) == expected);
        REQUIRE(device.step(nan, 0.0, period) == expected);
        REQUIRE(device.step(.9, 0.0, -period) == expected);
        REQUIRE(device.step(.9, 0.0, 1e10) == expected);
    }
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
