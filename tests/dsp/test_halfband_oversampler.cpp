#include <catch2.hpp>

#include <cmath>
#include <vector>

#include "dsp/HalfBandOversampler.h"

TEST_CASE("halfband oversampler X1 is passthrough", "[oversampling]") {
    wdf::HalfBandOversampler os(wdf::OversamplingFactor::X1);
    REQUIRE(os.ratio() == 1);

    for (int i = 0; i < 100; ++i) {
        const double input = std::sin(i * 0.1);
        const double output = os.process(input, [](double x) { return x; });
        REQUIRE(output == Approx(input).margin(1e-12));
    }
}

TEST_CASE("halfband oversampler X2 preserves DC unity gain", "[oversampling]") {
    wdf::HalfBandOversampler os(wdf::OversamplingFactor::X2);
    REQUIRE(os.ratio() == 2);

    // Feed constant DC = 1.0; filter settles to unity DC
    double out = 0.0;
    for (int i = 0; i < 2000; ++i) {
        out = os.process(1.0, [](double x) { return x; });
    }
    REQUIRE(out == Approx(1.0).margin(0.01));
}

TEST_CASE("halfband oversampler X4 preserves DC unity gain", "[oversampling]") {
    wdf::HalfBandOversampler os(wdf::OversamplingFactor::X4);
    REQUIRE(os.ratio() == 4);

    double out = 0.0;
    for (int i = 0; i < 4000; ++i) {
        out = os.process(1.0, [](double x) { return x; });
    }
    REQUIRE(out == Approx(1.0).margin(0.01));
}

TEST_CASE("halfband oversampler block processing matches sample-by-sample", "[oversampling]") {
    wdf::HalfBandOversampler os1(wdf::OversamplingFactor::X2);
    wdf::HalfBandOversampler os2(wdf::OversamplingFactor::X2);

    constexpr size_t N = 128;
    std::vector<double> in(N), outBlock(N), outSample(N);
    for (size_t i = 0; i < N; ++i) {
        in[i] = std::sin(2.0 * 3.141592653589793 * 440.0 * static_cast<double>(i) / 48000.0);
    }

    auto nonlinearFn = [](double x) {
        return std::tanh(2.0 * x);
    };

    os1.processBlock(in.data(), outBlock.data(), N, nonlinearFn);
    for (size_t i = 0; i < N; ++i) {
        outSample[i] = os2.process(in[i], nonlinearFn);
    }

    for (size_t i = 0; i < N; ++i) {
        REQUIRE(outBlock[i] == Approx(outSample[i]).margin(1e-12));
    }
}

TEST_CASE("halfband oversampler reset clears internal states", "[oversampling]") {
    wdf::HalfBandOversampler os(wdf::OversamplingFactor::X2);
    for (int i = 0; i < 500; ++i) {
        os.process(1.0, [](double x) { return x; });
    }
    os.reset();

    // After reset, response to zero input must be identically zero
    const double out = os.process(0.0, [](double x) { return x; });
    REQUIRE(out == Approx(0.0).margin(1e-12));
}
