#pragma once

// Dual-trace oscilloscope, drawn in a dark-CRT style.
//
// Copied from the plugin's `DualOscilloscope` (see the note in editor/CMakeLists
// and docs/PLAN.md on the copy-as-we-go decision). It is copied rather than shared
// because the editor's scope will grow editor-only controls (probe selection,
// DC/AC coupling, trigger source) that the plugin's should not carry. The buffer
// half is pure DSP with no JUCE-plugin dependency, so it moves across as-is.

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace livewdf::editor {

class ScopeAudioBuffer {
public:
    static constexpr size_t kCapacity = 16384;

    ScopeAudioBuffer() : buffer_(kCapacity, 0.0f), writePos_(0) {}

    void pushSample(float sample) noexcept {
        // The scope is a display, not the audio path. A rogue non-finite sample
        // is a solver problem to log and fix; it must not poison the trace and
        // the peak readout, which would then say "inf V" until the sample
        // scrolled out of the window.
        const size_t pos = writePos_.load(std::memory_order_relaxed);
        buffer_[pos] = std::isfinite(sample) ? sample : 0.0f;
        writePos_.store((pos + 1) % kCapacity, std::memory_order_release);
    }

    /// Drop the history, for a new circuit. The scope should start on the
    /// engine that is running now, not the one that was running before it.
    void clear() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        writePos_.store(0, std::memory_order_release);
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

class ScopeView : public juce::Component, public juce::Timer {
public:
    ScopeView();
    ~ScopeView() override;

    void setBuffers(const ScopeAudioBuffer* inputBuffer, const ScopeAudioBuffer* outputBuffer);

    void setSampleRate(double sampleRate) { sampleRate_ = sampleRate; }
    void setTimebaseMs(float msPerDiv) {
        timebaseMs_ = juce::jlimit(0.2f, 20.0f, msPerDiv);
        autoTimebase_ = false;
    }
    void setInputGain(float gain) { inGain_ = gain; }
    void setOutputGain(float gain) { outGain_ = gain; }
    void setFrozen(bool frozen) { frozen_ = frozen; }
    bool isFrozen() const { return frozen_; }
    float getTimebaseMs() const { return timebaseMs_; }
    float getUnitsPerDiv() const { return unitsPerDiv_; }
    float getDisplayWindowMs() const { return displayWindowMs_; }

    /// What the second trace is showing.
    ///
    /// It is the circuit's output by default, and the *scope probe's* net when
    /// the sheet has a probe -- the probe is the editor's "look here", so the
    /// trace has to say what it is looking at, or the scope is showing a signal
    /// nobody can name.
    void setOutputLabel(const juce::String& label) {
        outputLabel_ = label.isEmpty() ? juce::String("OUT") : label;
        outToggle_.setButtonText(outputLabel_);
        repaint();
    }

    /// **A probe trace**: a scope probe on the sheet, the buffer it is written
    /// to, and the colour it is drawn in.
    ///
    /// The probes are the sheet's meters: each one names a net, and the scope
    /// draws that net's signal on its own colour -- so a sheet with two probes
    /// shows two signals at once, and neither is mistaken for the input or the
    /// output.
    struct ProbeTrace {
        juce::String label;
        ScopeAudioBuffer* buffer = nullptr;
        juce::Colour colour { 0xFFFF4FD8 };
    };

    /// The probe traces the scope draws. Replaces the previous set.
    void setProbeTraces(std::vector<ProbeTrace> traces);

    /// Forget the measured waveforms and readouts. The buffers themselves are
    /// owned by the editor, so this only resets what the scope has drawn and
    /// measured from them; the editor clears the buffers alongside it.
    void clearTraces();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

private:
    const ScopeAudioBuffer* inBuffer_ = nullptr;
    const ScopeAudioBuffer* outBuffer_ = nullptr;

    double sampleRate_ = 48000.0;

    /// Milliseconds per division.
    ///
    /// **The scope chooses this for itself** (see `autoTimebase_`), so whatever
    /// frequency the circuit is running at -- the editor's 440 Hz tone, a 1 Hz
    /// generator, a 12 kHz ripple -- the waveform on the screen is about three
    /// cycles wide and can be read. A scope that showed nine cycles of a sine
    /// was a picture of a texture; three cycles is a waveform whose shape a
    /// person can see.
    float timebaseMs_ = 0.7f;

    /// Whether the scope is choosing its own timebase. The `+`/`-` buttons take
    /// it off automatic, the way a real scope's does when you touch the knob.
    bool autoTimebase_ = true;
    float inGain_ = 1.0f;
    float outGain_ = 1.0f;
    bool frozen_ = false;

    bool showInput_ = true;
    bool showOutput_ = true;

    /// What the second trace shows: "OUT", or the scope probe's name.
    juce::String outputLabel_ { "OUT" };

    std::vector<float> inWaveScratch_;
    std::vector<float> outWaveScratch_;

    /// The probe traces, their waveforms, and the button that switches each one
    /// on and off.
    std::vector<ProbeTrace> probeTraces_;
    std::vector<std::vector<float>> probeWaves_;
    std::vector<std::unique_ptr<juce::ToggleButton>> probeButtons_;

    // Live metering readouts
    float inPeakDb_ = -100.0f;
    float outPeakDb_ = -100.0f;

    /// **The peak amplitude of each trace, in volts.**
    ///
    /// The decibels are how loud a signal is; the volts are how big it is. A
    /// reader asks "what voltage should I put in?" and only the volts answer
    /// that -- so both are shown.
    float inPeakVolts_ = 0.0f;
    float outPeakVolts_ = 0.0f;
    std::vector<float> probePeakVolts_;

    /// The gain that makes the *largest enabled* trace fill the screen.
    ///
    /// **The traces that are switched off are not counted.** A scope that sized
    /// itself from a hidden trace would zoom out for a signal nobody can see,
    /// and the trace that is on would be a flat line in the middle of the
    /// screen -- the same unreadable picture the fixed gain produced, only
    /// quieter.
    float autoGain_ = 1.0f;

    /// The volts (or the unit the trace is in) per vertical division, for the
    /// readout. Zero when there is nothing to scale.
    float unitsPerDiv_ = 0.0f;
    float displayWindowMs_ = 0.0f;

    juce::TextButton timeMinusBtn_ { "-" };
    juce::TextButton timePlusBtn_ { "+" };
    juce::TextButton freezeBtn_ { "Freeze" };
    juce::ToggleButton inToggle_ { "IN" };
    juce::ToggleButton outToggle_ { "OUT" };

    void drawGraticule(juce::Graphics& g, juce::Rectangle<float> area);
    void drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                      const std::vector<float>& samples, juce::Colour color, float gain);

    /// Whether probe `index` is switched on. A trace that is off is not drawn
    /// and does not count towards the scale.
    bool probeVisible(size_t index) const {
        if (index >= probeButtons_.size() || !probeButtons_[index])
            return false;
        return probeButtons_[index]->getToggleState();
    }

    /// The signal's period, in samples, from its zero crossings.
    ///
    /// The scope measures the signal rather than assuming a frequency: a sheet
    /// running at 1 Hz and one running at 12 kHz both get a readable waveform.
    /// Returns 0 when there is no period to find.
    float estimatePeriodSamples(const std::vector<float>& samples) const;

    /// The timebase the signal asks for: about three cycles across the screen.
    ///
    /// Returns 0 when the signal has no period to measure.
    float autoTimebaseFor(const std::vector<float>& samples) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScopeView)
};

} // namespace livewdf::editor
