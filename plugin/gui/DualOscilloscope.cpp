#include "DualOscilloscope.h"
#include "Fonts.h"
#include <cmath>

namespace livewdf {

DualOscilloscope::DualOscilloscope() {
    setOpaque(true);
    setLookAndFeel(&lookAndFeel_);

    inToggle_.setToggleState(true, juce::dontSendNotification);
    inToggle_.setColour(juce::ToggleButton::textColourId, juce::Colour(0xFF00E5FF));
    inToggle_.setColour(juce::ToggleButton::tickColourId, juce::Colour(0xFF00E5FF));
    inToggle_.onClick = [this] { showInput_ = inToggle_.getToggleState(); repaint(); };
    addAndMakeVisible(inToggle_);

    outToggle_.setToggleState(true, juce::dontSendNotification);
    outToggle_.setColour(juce::ToggleButton::textColourId, juce::Colour(0xFFFFAB00));
    outToggle_.setColour(juce::ToggleButton::tickColourId, juce::Colour(0xFFFFAB00));
    outToggle_.onClick = [this] { showOutput_ = outToggle_.getToggleState(); repaint(); };
    addAndMakeVisible(outToggle_);

    timeMinusBtn_.onClick = [this] {
        // Zooming *out* of the automatic timebase keeps it automatic: the scope
        // picks the window again from the signal, with the zoom applied.
        if (autoTimebase_)
            timebaseMs_ *= 1.5f;
        else
            setTimebaseMs(timebaseMs_ * 1.5f);
        repaint();
    };
    addAndMakeVisible(timeMinusBtn_);

    timePlusBtn_.onClick = [this] {
        // Zooming *in* is a decision about the window, so it takes the scope off
        // automatic -- the way a real scope's does when you touch the knob.
        if (autoTimebase_)
            autoTimebase_ = false;
        setTimebaseMs(timebaseMs_ / 1.5f);
        repaint();
    };
    addAndMakeVisible(timePlusBtn_);

    freezeBtn_.setClickingTogglesState(true);
    freezeBtn_.onClick = [this] {
        setFrozen(freezeBtn_.getToggleState());
    };
    addAndMakeVisible(freezeBtn_);

    startTimerHz(45); // 45 FPS UI refresh
}

DualOscilloscope::~DualOscilloscope() {
    stopTimer();
    setLookAndFeel(nullptr);
}

void DualOscilloscope::setBuffers(const ScopeAudioBuffer* inputBuffer, const ScopeAudioBuffer* outputBuffer) {
    inBuffer_ = inputBuffer;
    outBuffer_ = outputBuffer;
}

void DualOscilloscope::timerCallback() {
    if (frozen_) return;

    if (!inBuffer_ && !outBuffer_) return;

    // **The scope measures the signal before it draws it.** The window comes
    // from the signal's *own* period, so the waveform on screen is about three
    // cycles wide whatever the circuit is running at -- the host's music, a
    // 1 Hz generator, a 12 kHz ripple.
    if (autoTimebase_) {
        std::vector<float> estimate;
        const ScopeAudioBuffer* source = inBuffer_ ? inBuffer_ : outBuffer_;
        source->readRecent(estimate, 8192);

        const float ms = autoTimebaseFor(estimate);
        if (ms > 0.0f)
            timebaseMs_ += (ms - timebaseMs_) * 0.2f;
    }

    // Determine how many samples to display based on timebase
    // 10 divisions total
    const float totalTimeMs = timebaseMs_ * 10.0f;
    const size_t numSamples = static_cast<size_t>(std::clamp(
        (totalTimeMs * 0.001f) * static_cast<float>(sampleRate_), 64.0f, 8192.0f));

    // **The trigger: where the sweep starts.** A real scope holds the picture
    // still by starting every sweep at the same point of the signal. This one
    // looks for the signal *rising through its own middle*, and the trace is
    // drawn from there -- a snapshot rather than a scroll, with the input and
    // the output keeping the phase relationship they really have.
    //
    // Triggering on zero (rather than on the middle) is what made the trace
    // crawl: a signal that sits on a DC offset never crosses zero.
    size_t triggerOffset = 0;
    const ScopeAudioBuffer* trigBuf = inBuffer_ ? inBuffer_ : outBuffer_;
    if (trigBuf) {
        const size_t curPos = trigBuf->getWritePos();
        const size_t searchLen = 1024;

        double sum = 0.0;
        for (size_t i = 0; i < searchLen; ++i) {
            const size_t idx = (curPos + ScopeAudioBuffer::kCapacity - numSamples - searchLen + i) % ScopeAudioBuffer::kCapacity;
            sum += trigBuf->getSampleAt(idx);
        }
        const float level = static_cast<float>(sum / static_cast<double>(searchLen));

        bool found = false;
        float prev = 0.0f;
        for (size_t i = 0; i < searchLen; ++i) {
            const size_t idx = (curPos + ScopeAudioBuffer::kCapacity - numSamples - searchLen + i) % ScopeAudioBuffer::kCapacity;
            const float s = trigBuf->getSampleAt(idx);
            if (i > 0 && prev < level && s >= level) {
                triggerOffset = i;
                found = true;
                break;
            }
            prev = s;
        }
        if (!found) triggerOffset = 0;
    }

    if (inBuffer_) {
        inBuffer_->readRecent(inWaveScratch_, numSamples + 1024);
        if (triggerOffset + numSamples <= inWaveScratch_.size()) {
            std::vector<float> aligned(inWaveScratch_.begin() + static_cast<ptrdiff_t>(triggerOffset),
                                       inWaveScratch_.begin() + static_cast<ptrdiff_t>(triggerOffset + numSamples));
            inWaveScratch_ = std::move(aligned);
        } else {
            inWaveScratch_.resize(numSamples);
        }

        // Peak dB calculation
        float maxVal = 1e-6f;
        for (float s : inWaveScratch_) maxVal = std::max(maxVal, std::abs(s));
        inPeakDb_ = 20.0f * std::log10(maxVal);
        inPeakVolts_ = maxVal * inputScale_;
        for (auto& sample : inWaveScratch_) sample *= inputScale_;
    }

    if (outBuffer_) {
        outBuffer_->readRecent(outWaveScratch_, numSamples + 1024);
        if (triggerOffset + numSamples <= outWaveScratch_.size()) {
            std::vector<float> aligned(outWaveScratch_.begin() + static_cast<ptrdiff_t>(triggerOffset),
                                       outWaveScratch_.begin() + static_cast<ptrdiff_t>(triggerOffset + numSamples));
            outWaveScratch_ = std::move(aligned);
        } else {
            outWaveScratch_.resize(numSamples);
        }

        // Peak dB calculation
        float maxVal = 1e-6f;
        for (float s : outWaveScratch_) maxVal = std::max(maxVal, std::abs(s));
        outPeakDb_ = 20.0f * std::log10(maxVal);
        outPeakVolts_ = maxVal * outputScale_;
        for (auto& sample : outWaveScratch_) sample *= outputScale_;
    }

    // **The scope sizes itself to the biggest signal it is showing.** A trace
    // that is switched off is not counted: sizing the screen from a hidden
    // signal would zoom out for something nobody can see.
    float largest = 0.0f;
    if (showInput_)
        for (float s : inWaveScratch_) largest = std::max(largest, std::abs(s));
    if (showOutput_)
        for (float s : outWaveScratch_) largest = std::max(largest, std::abs(s));

    if (largest > 1e-9f) {
        const float target = 0.9f / largest;
        autoGain_ += (target - autoGain_) * 0.25f;
        unitsPerDiv_ = largest / 4.0f;
    } else {
        unitsPerDiv_ = 0.0f;
    }


    repaint();
}

float DualOscilloscope::estimatePeriodSamples(const std::vector<float>& samples) const {
    if (samples.size() < 16)
        return 0.0f;

    // The signal's mean, so the crossings are of the *middle* of the wave and
    // not of zero -- a signal sitting on a DC offset crosses zero at the wrong
    // places.
    double sum = 0.0;
    for (float s : samples)
        sum += s;
    const float mean = static_cast<float>(sum / static_cast<double>(samples.size()));

    // The rises through the mean: one per period.
    int rises = 0;
    float prev = samples.front() - mean;
    for (size_t i = 1; i < samples.size(); ++i) {
        const float cur = samples[i] - mean;
        if (prev < 0.0f && cur >= 0.0f)
            ++rises;
        prev = cur;
    }

    if (rises < 2)
        return 0.0f;

    return static_cast<float>(samples.size()) / static_cast<float>(rises);
}

float DualOscilloscope::autoTimebaseFor(const std::vector<float>& samples) const {
    const float periodSamples = estimatePeriodSamples(samples);
    if (periodSamples <= 0.0f)
        return 0.0f;

    // Three cycles across the ten divisions, so each division is a third of a
    // period -- the shape of the wave is what fills the screen.
    const float periodMs = (periodSamples / static_cast<float>(sampleRate_)) * 1000.0f;
    return juce::jlimit(0.2f, 20.0f, periodMs / 3.0f);
}

void DualOscilloscope::drawGraticule(juce::Graphics& g, juce::Rectangle<float> area) {
    // Dark CRT screen background
    g.setColour(juce::Colour(0xFF0D1117));
    g.fillRect(area);

    // Subtle cathode phosphor vignette / radial glow in center
    juce::ColourGradient vignette(juce::Colour(0x0C00E5FF), area.getCentreX(), area.getCentreY(),
                                  juce::Colours::transparentBlack, area.getX(), area.getY(), true);
    g.setGradientFill(vignette);
    g.fillRect(area);

    // 10 horizontal divisions, 8 vertical divisions
    const int xDivs = 10;
    const int yDivs = 8;
    const float dx = area.getWidth() / static_cast<float>(xDivs);
    const float dy = area.getHeight() / static_cast<float>(yDivs);

    // Minor grid lines
    g.setColour(juce::Colour(0x153A4758));
    for (int i = 1; i < xDivs; ++i) {
        const float x = area.getX() + static_cast<float>(i) * dx;
        g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
    }
    for (int j = 1; j < yDivs; ++j) {
        const float y = area.getY() + static_cast<float>(j) * dy;
        g.drawHorizontalLine(juce::roundToInt(y), area.getX(), area.getRight());
    }

    // Center crosshair axes (with tick subdivisions)
    const float midX = area.getCentreX();
    const float midY = area.getCentreY();

    g.setColour(juce::Colour(0x356C7D93));
    g.drawVerticalLine(juce::roundToInt(midX), area.getY(), area.getBottom());
    g.drawHorizontalLine(juce::roundToInt(midY), area.getX(), area.getRight());

    // Center axis ticks (5 ticks per div)
    g.setColour(juce::Colour(0x406C7D93));
    for (int i = 0; i <= xDivs * 5; ++i) {
        const float x = area.getX() + (static_cast<float>(i) * dx) / 5.0f;
        g.drawLine(x, midY - 2.0f, x, midY + 2.0f, 1.0f);
    }
    for (int j = 0; j <= yDivs * 5; ++j) {
        const float y = area.getY() + (static_cast<float>(j) * dy) / 5.0f;
        g.drawLine(midX - 2.0f, y, midX + 2.0f, y, 1.0f);
    }

    // Bezel border
    g.setColour(juce::Colour(0x40303846));
    g.drawRect(area, 1.0f);
}

void DualOscilloscope::drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                                    const std::vector<float>& samples, juce::Colour color, float gain) {
    if (samples.size() < 2) return;

    juce::Path path;
    const float midY = area.getCentreY();
    const float halfH = area.getHeight() * 0.45f;
    const float xStep = area.getWidth() / static_cast<float>(samples.size() - 1);

    for (size_t i = 0; i < samples.size(); ++i) {
        const float x = area.getX() + static_cast<float>(i) * xStep;
        const float val = std::clamp(samples[i] * gain, -1.8f, 1.8f);
        const float y = midY - val * halfH;

        if (i == 0) path.startNewSubPath(x, y);
        else path.lineTo(x, y);
    }

    // 1. Phosphor glow pass (wide, low opacity)
    g.setColour(color.withAlpha(0.20f));
    g.strokePath(path, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // 2. Core trace (crisp, high opacity)
    g.setColour(color.withAlpha(0.95f));
    g.strokePath(path, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void DualOscilloscope::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();

    drawGraticule(g, bounds);

    // Draw Input waveform (Cyan)
    if (showInput_ && !inWaveScratch_.empty()) {
        drawWaveform(g, bounds, inWaveScratch_, juce::Colour(0xFF00E5FF), inGain_ * autoGain_);
    }

    // Draw Output waveform (Warm Amber)
    if (showOutput_ && !outWaveScratch_.empty()) {
        drawWaveform(g, bounds, outWaveScratch_, juce::Colour(0xFFFFAB00), outGain_ * autoGain_);
    }

    // Top-left status badges: Timebase and Peak Levels in clean engineering monospace
    g.setFont(editor::Fonts::monoSemiBold(12.5f));

    // Timebase readout
    juce::String timeStr = juce::String(timebaseMs_, 1) + " ms/div" +
                            (autoTimebase_ ? " auto" : juce::String());
    g.setColour(juce::Colour(0xCC8B9BB4));
    g.drawText(timeStr, 8, 6, 140, 16, juce::Justification::left);

    // Input Peak readout (Cyan/Teal)
    if (showInput_) {
        g.setColour(juce::Colour(0xFF00E5FF));
        juce::String inStr = "IN: " + juce::String(inPeakVolts_, 3) + " V  " +
                             (inPeakDb_ < -80.0f ? "-inf" : juce::String(inPeakDb_, 1)) + " dB";
        g.drawText(inStr, 150, 6, 140, 16, juce::Justification::left);
    }

    // Output Peak readout (Amber)
    if (showOutput_) {
        g.setColour(juce::Colour(0xFFFFB800));
        juce::String outStr = "OUT: " + juce::String(outPeakVolts_, 3) + " V  " +
                              (outPeakDb_ < -80.0f ? "-inf" : juce::String(outPeakDb_, 1)) + " dB";
        g.drawText(outStr, 295, 6, 165, 16, juce::Justification::left);
    }

    // Units per division
    if (unitsPerDiv_ > 0.0f) {
        g.setColour(juce::Colour(0xCC8B9BB4));
        g.drawText(juce::String(unitsPerDiv_, 3) + " V/div", 8, 22, 120, 14,
                   juce::Justification::left);
    }
}

void DualOscilloscope::resized() {
    auto bounds = getLocalBounds();
    const int btnH = 22;
    const int topMargin = 4;

    // Right-aligned controls
    int right = bounds.getRight() - 6;

    freezeBtn_.setBounds(right - 55, topMargin, 55, btnH);
    right -= 60;

    outToggle_.setBounds(right - 48, topMargin, 48, btnH);
    right -= 50;

    inToggle_.setBounds(right - 42, topMargin, 42, btnH);
    right -= 46;

    timePlusBtn_.setBounds(right - 20, topMargin, 20, btnH);
    right -= 22;

    timeMinusBtn_.setBounds(right - 20, topMargin, 20, btnH);
}

} // namespace livewdf
