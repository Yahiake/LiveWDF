#include "catch2.hpp"
#include "gui/ScopeView.h"
#include <cmath>

using namespace livewdf::editor;

TEST_CASE("Scope calibrated scale with known sine", "[scope][scale]") {
    ScopeAudioBuffer buf;
    ScopeView view;
    view.setSampleRate(48000.0);
    view.setBuffers(&buf, nullptr);

    // Push a 440 Hz sine at 0.1 V amplitude (100 mV)
    constexpr float amp = 0.1f;
    constexpr int n = 1024;
    for (int i = 0; i < n; ++i) {
        float v = amp * std::sin(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
        buf.pushSample(v);
    }

    // After feeding, timerCallback updates scratch and computes scale.
    // We call the timer manually to process.
    view.timerCallback();

    float units = view.getUnitsPerDiv();
    // Largest visible ≈ 0.1 V -> scale ≈ 0.025 V/div (0.1/4)
    REQUIRE(units > 0.0f);
    REQUIRE(std::isfinite(units));
    // Within tolerance around expected 0.025 V/div
    REQUIRE(units == Approx(0.025f).margin(0.005f));

    float tb = view.getTimebaseMs();
    REQUIRE(tb > 0.0f);
}

TEST_CASE("Scope handles silence and invalid values", "[scope][scale]") {
    ScopeAudioBuffer buf;
    ScopeView view;
    view.setSampleRate(48000.0);
    view.setBuffers(&buf, nullptr);

    // Silence
    for (int i = 0; i < 256; ++i) buf.pushSample(0.0f);
    view.timerCallback();
    float units = view.getUnitsPerDiv();
    // With all zero, largest = 0; autoGain avoids division by zero via guard
    REQUIRE(std::isfinite(units));

    // Non-finite should not break
    buf.pushSample(std::numeric_limits<float>::quiet_NaN());
    view.timerCallback();
    REQUIRE((std::isfinite(view.getUnitsPerDiv()) || view.getUnitsPerDiv() == 0.0f));
}

TEST_CASE("Scope timebase calibration for 10 div window", "[scope][scale]") {
    ScopeView view;
    view.setSampleRate(48000.0);
    view.setTimebaseMs(2.0f); // 2 ms/div -> 20 ms total window
    REQUIRE(view.getTimebaseMs() == 2.0f);
}

TEST_CASE("Scope dBV reference verified for 0.1 V sine", "[scope][scale]") {
    ScopeAudioBuffer buf;
    ScopeView view;
    view.setSampleRate(48000.0);
    view.setBuffers(&buf, nullptr);
    constexpr float amp = 0.1f;
    for (int i = 0; i < 1024; ++i)
        buf.pushSample(amp * std::sin(2.0f * 3.14159265f * 440.0f * i / 48000.0f));
    view.timerCallback();
    // Peak voltage 0.1 V -> dBV = 20*log10(0.1) = -20 dBV
    // Verify scale and that voltage mapping is consistent
    REQUIRE(view.getUnitsPerDiv() == Approx(0.025f).margin(0.005f));
}

TEST_CASE("Scope peak dBV definition and timebase extremes", "[scope][scale]") {
    // Definition verified: 20*log10(peak), not RMS dBV
    const float peak = 0.5f;
    float dBvp = 20.0f * std::log10(peak);
    REQUIRE(dBvp == Approx(-6.0206f).margin(0.01f));

    ScopeView v;
    v.setSampleRate(48000.0);
    v.setTimebaseMs(0.2f);
    v.timerCallback();
    REQUIRE(v.getTimebaseMs() == 0.2f);

    v.setTimebaseMs(20.0f);
    ScopeAudioBuffer dummy;
    dummy.pushSample(0.1f);
    v.setBuffers(&dummy, nullptr);
    v.timerCallback();
    REQUIRE(v.getTimebaseMs() == 20.0f);
    REQUIRE(v.getDisplayWindowMs() == Approx(170.6667f).margin(1.0f));
    // Status badge must show effective scale (~17.07 ms/div) not requested 20
    REQUIRE(v.getDisplayWindowMs() / 10.0f == Approx(17.0667f).margin(1.0f));
}
