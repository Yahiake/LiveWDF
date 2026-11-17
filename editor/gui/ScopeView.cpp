#include "ScopeView.h"
#include "Fonts.h"
#include <cmath>

namespace livewdf::editor {

ScopeView::ScopeView() {
    setOpaque(true);

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

ScopeView::~ScopeView() {
    stopTimer();
}

void ScopeView::setBuffers(const ScopeAudioBuffer* inputBuffer, const ScopeAudioBuffer* outputBuffer) {
    inBuffer_ = inputBuffer;
    outBuffer_ = outputBuffer;
}

void ScopeView::clearTraces() {
    inWaveScratch_.clear();
    outWaveScratch_.clear();
    for (auto& wave : probeWaves_)
        wave.clear();

    inPeakDb_ = -100.0f;
    outPeakDb_ = -100.0f;
    inPeakVolts_ = 0.0f;
    outPeakVolts_ = 0.0f;
    std::fill(probePeakVolts_.begin(), probePeakVolts_.end(), 0.0f);
    unitsPerDiv_ = 0.0f;
    autoGain_ = 1.0f;
    repaint();
}

namespace {

/// The colours the probe traces are drawn in, in the order the probes appear
/// on the sheet. They are chosen to be told apart from the input's cyan and
/// the output's amber at a glance, and from each other.
const juce::Colour kProbeColours[] = {
    juce::Colour(0xFFFF4FD8),  // magenta
    juce::Colour(0xFF7CFF6B),  // green
    juce::Colour(0xFFB388FF),  // violet
    juce::Colour(0xFFFFD54F),  // yellow
    juce::Colour(0xFF4FC3F7),  // sky
    juce::Colour(0xFFFF8A65),  // orange-red
};

}  // namespace

void ScopeView::setProbeTraces(std::vector<ProbeTrace> traces) {
    // The old buttons leave the parent before they go, so a re-set does not
    // leave buttons behind on the toolbar.
    for (auto& button : probeButtons_) {
        if (button)
            removeChildComponent(button.get());
    }
    probeButtons_.clear();
    probeWaves_.clear();

    probeTraces_ = std::move(traces);
    probeWaves_.resize(probeTraces_.size());
    probePeakVolts_.assign(probeTraces_.size(), 0.0f);

    for (size_t i = 0; i < probeTraces_.size(); ++i) {
        auto& trace = probeTraces_[i];
        if (trace.colour == juce::Colour(0xFFFF4FD8) && i > 0)
            trace.colour = kProbeColours[i % (sizeof(kProbeColours) / sizeof(kProbeColours[0]))];

        auto button = std::make_unique<juce::ToggleButton>(trace.label);
        button->setToggleState(true, juce::dontSendNotification);
        button->setColour(juce::ToggleButton::textColourId, trace.colour);
        button->setColour(juce::ToggleButton::tickColourId, trace.colour);
        button->onClick = [this] { repaint(); };
        addAndMakeVisible(*button);
        probeButtons_.push_back(std::move(button));
    }

    resized();
    repaint();
}

void ScopeView::timerCallback() {
    if (frozen_) return;

    if (!inBuffer_ && !outBuffer_) return;

    // **The scope measures the signal before it draws it.**
    //
    // The window is chosen from the signal's *own* period, so the waveform on
    // screen is about three cycles wide whatever the circuit is running at --
    // the editor's 440 Hz tone, a 1 Hz generator, a 12 kHz ripple. Assuming a
    // frequency instead is what made the trace unreadably fast.
    size_t numSamples = 0;
    if (autoTimebase_) {
        // A long window to measure from: at 1 Hz a 7 ms window holds less than
        // a tenth of a cycle, and there is no period to find in it.
        std::vector<float> estimate;
        const ScopeAudioBuffer* source = inBuffer_ ? inBuffer_ : outBuffer_;
        source->readRecent(estimate, 8192);

        const float ms = autoTimebaseFor(estimate);
        if (ms > 0.0f) {
            // Smoothed, so the window does not jump on every period estimate.
            timebaseMs_ += (ms - timebaseMs_) * 0.2f;
        }
    }

    // Determine how many samples to display based on timebase
    // 10 divisions total
    const float totalTimeMs = timebaseMs_ * 10.0f;
    numSamples = static_cast<size_t>(std::clamp(
        (totalTimeMs * 0.001f) * static_cast<float>(sampleRate_), 64.0f, 8192.0f));
    displayWindowMs_ = (numSamples / static_cast<float>(sampleRate_)) * 1000.0f;

    // **The trigger: where the sweep starts.**
    //
    // A real scope holds the picture still by starting every sweep at the same
    // point of the signal. This one does the same -- it looks for the signal
    // *rising through its own middle*, and the trace is drawn from there. The
    // picture is then a snapshot, not a scroll, and the input and the output
    // keep the phase relationship they really have.
    //
    // Triggering on zero (rather than on the middle) is what made the trace
    // crawl: a signal that sits on a DC offset never crosses zero, so the
    // trigger never fired and the display ran free.
    size_t triggerOffset = 0;
    const ScopeAudioBuffer* trigBuf = inBuffer_ ? inBuffer_ : outBuffer_;
    if (trigBuf) {
        const size_t curPos = trigBuf->getWritePos();
        const size_t searchLen = 1024;

        // The signal's middle, over the search window: the level the trace
        // crosses on its way up.
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
        inPeakVolts_ = maxVal;
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
        outPeakVolts_ = maxVal;
    }

    // **The probe traces**, read the same way as the other two, each into its
    // own waveform.
    for (size_t p = 0; p < probeTraces_.size(); ++p) {
        auto& trace = probeTraces_[p];
        if (trace.buffer == nullptr)
            continue;

        trace.buffer->readRecent(probeWaves_[p], numSamples + 1024);
        if (triggerOffset + numSamples <= probeWaves_[p].size()) {
            std::vector<float> aligned(probeWaves_[p].begin() + static_cast<ptrdiff_t>(triggerOffset),
                                       probeWaves_[p].begin() + static_cast<ptrdiff_t>(triggerOffset + numSamples));
            probeWaves_[p] = std::move(aligned);
        } else {
            probeWaves_[p].resize(numSamples);
        }

        // The probe's peak, in volts -- what the meter reads.
        float peak = 0.0f;
        for (float s : probeWaves_[p]) peak = std::max(peak, std::abs(s));
        if (p < probePeakVolts_.size())
            probePeakVolts_[p] = peak;
    }

    // **The scope sizes itself to the biggest signal it is showing.**
    //
    // The peaks of the traces that are *on* -- the input, the output, and any
    // probe trace -- decide the scale, so a 30 dB gain stage does not draw a
    // clipped line at the top of the screen and a quiet stage does not draw a
    // flat one in the middle.
    //
    // A trace that is switched off is **not** counted: sizing the screen from a
    // hidden signal would zoom out for something nobody can see.
    float largest = 0.0f;
    if (showInput_)
        for (float s : inWaveScratch_) largest = std::max(largest, std::abs(s));
    if (showOutput_)
        for (float s : outWaveScratch_) largest = std::max(largest, std::abs(s));
    for (size_t p = 0; p < probeTraces_.size(); ++p) {
        if (!probeVisible(p))
            continue;
        for (float s : probeWaves_[p]) largest = std::max(largest, std::abs(s));
    }

    if (largest > 1e-9f) {
        // The trace fills 90% of the half-height, so it is readable and still
        // has headroom at the top.
        const float target = 0.9f / largest;
        // Smoothed, so the scale does not jump on every peak.
        autoGain_ += (target - autoGain_) * 0.25f;
        unitsPerDiv_ = largest / 4.0f;
    } else {
        unitsPerDiv_ = 0.0f;
    }

    // **Each meter's reading is on its button**: the probe's name and the
    // volts at that point of the circuit. A meter that only shows a wave has
    // to be read against the grid; a meter that shows its number does not.
    for (size_t p = 0; p < probeButtons_.size(); ++p) {
        if (!probeButtons_[p])
            continue;
        const float volts = (p < probePeakVolts_.size()) ? probePeakVolts_[p] : 0.0f;
        const juce::String text = probeTraces_[p].label + " " + juce::String(volts, 2) + "V";
        if (probeButtons_[p]->getButtonText() != text) {
            probeButtons_[p]->setButtonText(text);
            resized();
        }
    }

    repaint();
}

float ScopeView::estimatePeriodSamples(const std::vector<float>& samples) const {
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

    // Fewer than two crossings is not a period to measure.
    if (rises < 2)
        return 0.0f;

    return static_cast<float>(samples.size()) / static_cast<float>(rises);
}

float ScopeView::autoTimebaseFor(const std::vector<float>& samples) const {
    const float periodSamples = estimatePeriodSamples(samples);
    if (periodSamples <= 0.0f)
        return 0.0f;

    // Three cycles across the ten divisions, so each division is a third of a
    // period -- the shape of the wave is what fills the screen.
    const float periodMs = (periodSamples / static_cast<float>(sampleRate_)) * 1000.0f;
    return juce::jlimit(0.2f, 20.0f, periodMs / 3.0f);
}

void ScopeView::drawGraticule(juce::Graphics& g, juce::Rectangle<float> area) {
    // --- Bezel frame: a physical surround, like a Tektronix scope housing ---
    // The bezel is the outermost band; the CRT area is inset from it. The shadow
    // between them is what makes it read as hardware instead of a widget.
    const float bezelW = 3.0f;
    const float bezelR = 5.0f;

    // Bezel surface — slightly lighter than the window ground, like brushed
    // aluminium around a CRT. Two-tone: the top edge catches light, the bottom
    // edge is in shadow.
    {
        juce::ColourGradient bezelFill(
            juce::Colour(0xFF22272e), area.getX(), area.getY(),
            juce::Colour(0xFF181c22), area.getX(), area.getBottom(), false);
        g.setGradientFill(bezelFill);
        g.fillRoundedRectangle(area, bezelR);
    }

    // Inner shadow: a dark ring between the bezel and the phosphor, the gap you
    // see when you look at a real scope at an angle.
    const auto crt = area.reduced(bezelW);
    g.setColour(juce::Colour(0xFF06080b));
    g.fillRoundedRectangle(crt.expanded(1.0f), bezelR - 1.0f);

    // --- CRT phosphor area ---
    g.setColour(juce::Colour(0xFF0a0e14));
    g.fillRoundedRectangle(crt, bezelR - 2.0f);

    // Subtle cathode phosphor vignette — the centre is brighter than the edges,
    // like a CRT beam that is strongest head-on.
    {
        juce::ColourGradient vignette(
            juce::Colour(0x0800E5FF), crt.getCentreX(), crt.getCentreY(),
            juce::Colours::transparentBlack, crt.getX(), crt.getY(), true);
        g.setGradientFill(vignette);
        g.fillRoundedRectangle(crt, bezelR - 2.0f);
    }

    // 10 horizontal divisions, 8 vertical divisions
    const int xDivs = 10;
    const int yDivs = 8;
    const float dx = crt.getWidth() / static_cast<float>(xDivs);
    const float dy = crt.getHeight() / static_cast<float>(yDivs);

    // Minor grid lines — dotted, like a real scope graticule etched into glass.
    g.setColour(juce::Colour(0x153A4758));
    for (int i = 1; i < xDivs; ++i) {
        const float x = crt.getX() + static_cast<float>(i) * dx;
        // Dotted: 2px dash, 4px gap — reads as etching, not as drawn lines.
        for (float y = crt.getY(); y < crt.getBottom(); y += 6.0f)
            g.fillRect(x - 0.5f, y, 1.0f, 2.0f);
    }
    for (int j = 1; j < yDivs; ++j) {
        const float y = crt.getY() + static_cast<float>(j) * dy;
        for (float x = crt.getX(); x < crt.getRight(); x += 6.0f)
            g.fillRect(x, y - 0.5f, 2.0f, 1.0f);
    }

    // Center crosshair axes — solid, brighter than the grid, with tick marks.
    const float midX = crt.getCentreX();
    const float midY = crt.getCentreY();

    g.setColour(juce::Colour(0x306C7D93));
    g.drawVerticalLine(juce::roundToInt(midX), crt.getY(), crt.getBottom());
    g.drawHorizontalLine(juce::roundToInt(midY), crt.getX(), crt.getRight());

    // Center axis ticks (5 ticks per div) — the fine graduation marks.
    g.setColour(juce::Colour(0x456C7D93));
    const float tickShort = 2.0f;
    const float tickLong = 4.0f;
    for (int i = 0; i <= xDivs * 5; ++i) {
        const float x = crt.getX() + (static_cast<float>(i) * dx) / 5.0f;
        const float len = (i % 5 == 0) ? tickLong : tickShort;
        g.drawLine(x, midY - len, x, midY + len, 1.0f);
    }
    for (int j = 0; j <= yDivs * 5; ++j) {
        const float y = crt.getY() + (static_cast<float>(j) * dy) / 5.0f;
        const float len = (j % 5 == 0) ? tickLong : tickShort;
        g.drawLine(midX - len, y, midX + len, y, 1.0f);
    }

    // Bezel highlight — a thin bright line along the top-left edge, like light
    // catching the chamfer of a machined aluminium frame.
    g.setColour(juce::Colour(0x18ffffff));
    g.drawRoundedRectangle(area.reduced(0.5f), bezelR, 1.0f);

    // Inner bezel edge — a hairline shadow on the inside, the depth cue that
    // says the CRT is recessed.
    g.setColour(juce::Colour(0x30000000));
    g.drawRoundedRectangle(crt, bezelR - 2.0f, 1.0f);
}

void ScopeView::drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
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

void ScopeView::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();

    drawGraticule(g, bounds);

    // The CRT area where waveforms and annotations are drawn — inset from the
    // bezel by the same amount the graticule uses, so the traces sit inside the
    // phosphor and never bleed over the bezel frame.
    const auto crt = bounds.reduced(3.0f);

    // Numeric time-axis annotations (verified 48 kHz, timebaseMs per div).
    g.setFont(Fonts::mono(10.0f));
    g.setColour(juce::Colour(0xAABDC7D1));
    const float totalMs = displayWindowMs_ > 0.0f ? displayWindowMs_ : timebaseMs_ * 10.0f;
    for (int i = 0; i <= 10; i += 2) {
        float x = crt.getX() + (crt.getWidth() / 10.0f) * i;
        float ms = (totalMs / 10.0f) * i;
        juce::String lbl = (ms == (int)ms) ? juce::String((int)ms) + "ms" : juce::String(ms, 1) + "ms";
        g.drawText(lbl, static_cast<int>(std::round(x - 22.0f)), static_cast<int>(std::round(crt.getBottom() - 15.0f)), 44, 14, juce::Justification::centred);
    }
    // Left vertical scale markings.
    if (unitsPerDiv_ > 0.0f && std::isfinite(unitsPerDiv_)) {
        for (int j = -3; j <= 3; ++j) {
            float y = crt.getCentreY() - (crt.getHeight() / 8.0f) * j;
            float v = j * unitsPerDiv_;
            juce::String lbl = juce::String(v, 2) + "V";
            g.setColour(juce::Colour(0xAABDC7D1));
            g.drawText(lbl, static_cast<int>(crt.getX()) + 2, static_cast<int>(std::round(y - 7.0f)), 54, 14, juce::Justification::left);
        }
    }

    // Draw Input waveform (Cyan)
    if (showInput_ && !inWaveScratch_.empty()) {
        drawWaveform(g, crt, inWaveScratch_, juce::Colour(0xFF00E5FF), inGain_ * autoGain_);
    }

    // Draw Output waveform (Warm Amber)
    if (showOutput_ && !outWaveScratch_.empty()) {
        drawWaveform(g, crt, outWaveScratch_, juce::Colour(0xFFFFAB00), outGain_ * autoGain_);
    }

    // The probe traces.
    for (size_t p = 0; p < probeTraces_.size(); ++p) {
        if (!probeVisible(p) || probeWaves_[p].empty())
            continue;
        drawWaveform(g, crt, probeWaves_[p], probeTraces_[p].colour, autoGain_);
    }

    // --- Readout badges: drawn on the bezel, outside the CRT ---
    // Timebase — top-left, monospace, like a real scope's on-screen display.
    g.setFont(Fonts::monoSemiBold(11.5f));

    juce::String effectiveMsDiv = (displayWindowMs_ > 0.0f)
        ? juce::String(displayWindowMs_ / 10.0f, 2)
        : juce::String(timebaseMs_, 2);
    juce::String timeStr = effectiveMsDiv + " ms/div" +
                            (autoTimebase_ ? " (auto)" : juce::String());
    g.setColour(juce::Colour(0xFFE0D8E5));
    g.drawText(timeStr, static_cast<int>(crt.getX()) + 4, 5, 160, 14, juce::Justification::left);

    // Channel readouts — compact badges, same style as hardware OSD.
    if (showInput_) {
        g.setColour(juce::Colour(0xFF00E5FF));
        juce::String inStr = "IN " + juce::String(inPeakVolts_, 3) + "V  " +
                             (inPeakDb_ < -80.0f ? "-inf" : juce::String(inPeakDb_, 1)) + " dBV";
        g.drawText(inStr, static_cast<int>(crt.getX()) + 140, 5, 150, 14, juce::Justification::left);
    }

    if (showOutput_) {
        g.setColour(juce::Colour(0xFFFFAB00));
        juce::String outStr = outputLabel_ + " " + juce::String(outPeakVolts_, 3) + "V  " +
                              (outPeakDb_ < -80.0f ? "-inf" : juce::String(outPeakDb_, 1)) + " dBV";
        g.drawText(outStr, static_cast<int>(crt.getX()) + 290, 5, 165, 14, juce::Justification::left);
    }

    // Freeze indicator
    if (frozen_) {
        g.setColour(juce::Colour(0xFFFF4FD8));
        g.setFont(Fonts::bold(12.0f));
        g.drawText("FREEZE", static_cast<int>(crt.getRight()) - 65, 5, 60, 14, juce::Justification::right);
    }

    // Calibrated V/div — bottom-left, above the time axis. It used to sit at
    // the same y as the timebase readout, so the two painted over each other.
    if (unitsPerDiv_ > 0.0f && std::isfinite(unitsPerDiv_)) {
        g.setColour(juce::Colour(0xCCE0D8E5));
        g.setFont(Fonts::monoSemiBold(11.5f));
        juce::String scaleStr = juce::String(unitsPerDiv_, 3) + " V/div";
        g.drawText(scaleStr,
                   static_cast<int>(crt.getX()) + 4,
                   static_cast<int>(crt.getBottom()) - 30,
                   120, 14, juce::Justification::left);
    }
}

void ScopeView::resized() {
    auto bounds = getLocalBounds();
    const int btnH = 18;
    const int topMargin = 5;

    // Right-aligned controls
    int right = bounds.getRight() - 6;

    freezeBtn_.setBounds(right - 55, topMargin, 55, btnH);
    right -= 60;

    // The second trace's label is as wide as the name it has to show: "OUT", or
    // the scope probe's id, which can be any length. A fixed width would cut the
    // name off, and a name nobody can read is worse than no name.
    const int outWidth = juce::jmax(48, outToggle_.getButtonText().length() * 8 + 12);
    outToggle_.setBounds(right - outWidth, topMargin, outWidth, btnH);
    right -= outWidth + 2;

    inToggle_.setBounds(right - 42, topMargin, 42, btnH);
    right -= 46;

    timePlusBtn_.setBounds(right - 20, topMargin, 20, btnH);
    right -= 22;

    timeMinusBtn_.setBounds(right - 20, topMargin, 20, btnH);
    right -= 22;

    // **The probe buttons**, in a row under the controls: each one is a meter
    // on the sheet, and its button says which trace it switches.
    int probeX = 8;
    const int probeY = 21;
    for (size_t i = 0; i < probeButtons_.size(); ++i) {
        if (!probeButtons_[i])
            continue;
        const int w = juce::jmax(28, probeButtons_[i]->getButtonText().length() * 7 + 12);
        probeButtons_[i]->setBounds(probeX, probeY, w, 16);
        probeX += w + 4;
    }
}

} // namespace livewdf::editor
