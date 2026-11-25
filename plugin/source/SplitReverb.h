#pragma once

#include <juce_dsp/juce_dsp.h>
#include <memory>

namespace livewdf {

class SplitReverb final {
public:
    // Prepare and retire instances outside the audio callback. The IR is at
    // the host rate, and channels match the prepared mono or stereo layout.
    SplitReverb(juce::AudioBuffer<float> responseAtHostRate, const juce::dsp::ProcessSpec&);
    ~SplitReverb();

    // Replaces wet with convolution only, starting at the IR's first sample.
    // Offline processing takes ownership of the late engine between jobs.
    void process(juce::dsp::AudioBlock<float> wet, bool offline) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace livewdf
