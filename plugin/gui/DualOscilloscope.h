#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UiLookAndFeel.h"
#include <vector>
#include <atomic>

namespace livewdf {

class ScopeAudioBuffer {
public:
    static constexpr size_t kCapacity = 16384;

    ScopeAudioBuffer() : buffer_(kCapacity, 0.0f), writePos_(0) {}

    void pushSample(float sample) noexcept {
        const size_t pos = writePos_.load(std::memory_order_relaxed);
        buffer_[pos] = sample;
        writePos_.store((pos + 1) % kCapacity, std::memory_order_release);
    }

    void pushBlock(const float* data, size_t numSamples) noexcept {
        for (size_t i = 0; i < numSamples; ++i) {
            pushSample(data[i]);
        }
    }

    // Read the most recent N samples into dest
    void readRecent(std::vector<float>& dest, size_t numSamples) const {
        if (dest.size() != numSamples)
            dest.resize(numSamples);

        const size_t currentPos = writePos_.load(std::memory_order_acquire);
        for (size_t i = 0; i < numSamples; ++i) {
            const size_t idx = (currentPos + kCapacity - numSamples + i) % kCapacity;
            dest[i] = buffer_[idx];
        }
    }

    size_t getWritePos() const noexcept {
        return writePos_.load(std::memory_order_relaxed);
    }

    float getSampleAt(size_t index) const noexcept {
        return buffer_[index % kCapacity];
    }

private:
    std::vector<float> buffer_;
    std::atomic<size_t> writePos_;
};

class DualOscilloscope : public juce::Component, public juce::Timer {
public:
    DualOscilloscope();
    ~DualOscilloscope() override;

    void setBuffers(const ScopeAudioBuffer* inputBuffer, const ScopeAudioBuffer* outputBuffer);

    /// The scope is a display, not the audio path: an unknown rate (0 before
    /// the host prepares the plugin) must not turn the timebase into a divide
    /// by zero. Keep the last known rate until the host supplies a real one.
    void setSampleRate(double sampleRate) {
        if (sampleRate > 0.0)
            sampleRate_ = sampleRate;
    }
    void setTimebaseMs(float msPerDiv) {
        timebaseMs_ = juce::jlimit(0.2f, 20.0f, msPerDiv);
        autoTimebase_ = false;
    }
    void setTerminalScales(float in, float out) { inputScale_ = in; outputScale_ = out; }
    void setInputGain(float gain) { inGain_ = gain; }
    void setOutputGain(float gain) { outGain_ = gain; }
    void setFrozen(bool frozen) { frozen_ = frozen; }
    bool isFrozen() const { return frozen_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

private:
    const ScopeAudioBuffer* inBuffer_ = nullptr;
    const ScopeAudioBuffer* outBuffer_ = nullptr;

    double sampleRate_ = 48000.0;

    /// Milliseconds per division. **The scope chooses this for itself** (see
    /// `autoTimebase_`), so whatever the circuit is running at, the waveform on
    /// screen is about three cycles wide and can be read.
    float timebaseMs_ = 0.7f;

    /// Whether the scope is choosing its own timebase. The `+`/`-` buttons take
    /// it off automatic, the way a real scope's does when you touch the knob.
    bool autoTimebase_ = true;

    float inputScale_ = 1.0f, outputScale_ = 1.0f;
    float inGain_ = 1.0f;
    float outGain_ = 1.0f;
    bool frozen_ = false;

    bool showInput_ = true;
    bool showOutput_ = true;

    std::vector<float> inWaveScratch_;
    std::vector<float> outWaveScratch_;

    // Live metering readouts
    float inPeakDb_ = -100.0f;
    float outPeakDb_ = -100.0f;

    /// The peak amplitude of each trace, in volts -- how big the signal is, as
    /// opposed to how loud it is.
    float inPeakVolts_ = 0.0f;
    float outPeakVolts_ = 0.0f;

    /// The gain that makes the largest *shown* trace fill the screen.
    float autoGain_ = 1.0f;

    /// The units per vertical division, for the readout.
    float unitsPerDiv_ = 0.0f;

    juce::TextButton timeMinusBtn_ { "-" };
    juce::TextButton timePlusBtn_ { "+" };
    juce::TextButton freezeBtn_ { "Freeze" };
    juce::ToggleButton inToggle_ { "IN" };
    juce::ToggleButton outToggle_ { "OUT" };

    editor::UiLookAndFeel lookAndFeel_;

    void drawGraticule(juce::Graphics& g, juce::Rectangle<float> area);
    void drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                      const std::vector<float>& samples, juce::Colour color, float gain);

    /// The signal's period, in samples, from its zero crossings.
    float estimatePeriodSamples(const std::vector<float>& samples) const;

    /// The timebase the signal asks for: about three cycles across the screen.
    float autoTimebaseFor(const std::vector<float>& samples) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DualOscilloscope)
};

} // namespace livewdf
