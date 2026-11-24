#include "PluginProcessor.h"

#include "format/AcSource.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace livewdf {

namespace {

void conditionInput(juce::dsp::AudioBlock<float> block, float gain, bool foldToMono) noexcept {
    block.multiplyBy(gain);
    if (foldToMono && block.getNumChannels() > 1) {
        float* left = block.getChannelPointer(0);
        float* right = block.getChannelPointer(1);
        for (size_t i = 0; i < block.getNumSamples(); ++i) {
            const float mono = 0.5f * (left[i] + right[i]);
            left[i] = mono;
            right[i] = mono;
        }
    }
}

}  // namespace

void LiveWDFPluginProcessor::processDryDelay(juce::dsp::AudioBlock<float> block,
                                             bool replace, int latency) noexcept {
    if (latency != dryDelaySamples_) {
        dryDelaySamples_ = latency;
        dryDelay_.reset();
        dryDelay_.setDelay(static_cast<float>(latency));
    }
    if (latency == 0)
        return;

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
        float* data = block.getChannelPointer(ch);
        for (size_t i = 0; i < block.getNumSamples(); ++i) {
            dryDelay_.pushSample(static_cast<int>(ch), data[i]);
            const float delayed = dryDelay_.popSample(static_cast<int>(ch));
            if (replace)
                data[i] = delayed;
        }
    }
}

void LiveWDFPluginProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer,
                                                   juce::MidiBuffer& midiMessages) {
    juce::ignoreUnused(midiMessages);
    const int inputChannels = std::min(getMainBusNumInputChannels(), buffer.getNumChannels());
    for (int ch = inputChannels; ch < buffer.getNumChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());
    if (inputChannels > 0)
        processDryDelay(juce::dsp::AudioBlock<float>(buffer).getSubsetChannelBlock(0, inputChannels),
                        true, processingLatencySamples_.load(std::memory_order_relaxed));
}

void LiveWDFPluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) {
    // Denormals for the whole callback. The WDF state is a network of
    // capacitors and tube junctions; as a note decays the state can reach
    // subnormal magnitudes, and x86 then slows down by orders of magnitude
    // inside the audio block. The offline engine never notices -- it has no
    // deadline -- which is exactly why this belongs to the plugin and not to
    // the engine. Cathodyne's processBlock opens with the same guard.
    juce::ScopedNoDenormals noDenormals;

    juce::ignoreUnused(midiMessages);

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels == 0 || numSamples == 0) return;

    // A bounce can apply pending processing options itself. Host latency
    // notifications are sent by the control thread.
    if (isNonRealtime())
        applyPendingCircuitOptions();

    const bool fastLive = !shouldForceAccurate() && *apvts_.getRawParameterValue("solve_quality") > 0.5f;

    // Read audio gain parameters
    const float inputTrimDb = *apvts_.getRawParameterValue("input_trim");
    const float outputMasterDb = *apvts_.getRawParameterValue("output_master");
    const float mixPercent = *apvts_.getRawParameterValue("mix");
    const bool stereoRequested = *apvts_.getRawParameterValue("stereo") > 0.5f;

    const float inputGain = juce::Decibels::decibelsToGain(inputTrimDb);
    const float outputGain = juce::Decibels::decibelsToGain(outputMasterDb);
    const float wetMix = mixPercent * 0.01f;
    const float dryMix = 1.0f - wetMix;

    const bool wantStereo = stereoRequested && numChannels > 1;

    // Try to acquire engine lock (never block audio thread)
    std::unique_lock<std::mutex> lock(engineMutex_, std::try_to_lock);
    const int latency = processingLatencySamples_.load(std::memory_order_relaxed);
    auto* activeOversampler = lock.owns_lock() ? activePolyphaseOversampler() : nullptr;
    juce::dsp::AudioBlock<float> block(buffer);
    const bool canProcess = lock.owns_lock() && leftEngine_ && outNodeIndex_ >= 0
        && !(activeOversampler && numSamples > samplesPerBlock_);

    // Keep untrimmed separate channels in the history, including at Mix 100%.
    // Host bypass can then return the delayed original input immediately.
    const bool dryAvailable = canProcess && dryMix > 0.001f && dryBuffer_.getNumSamples() >= numSamples;
    if (latency == 0)
        conditionInput(block, inputGain, !wantStereo);
    if (dryAvailable) {
        auto dry = juce::dsp::AudioBlock<float>(dryBuffer_)
            .getSubsetChannelBlock(0, block.getNumChannels()).getSubBlock(0, block.getNumSamples());
        dry.copyFrom(block);
        processDryDelay(dry, true, latency);
        if (latency != 0)
            conditionInput(dry, inputGain, !wantStereo);
    } else {
        processDryDelay(block, !canProcess, latency);
    }
    if (latency != 0)
        conditionInput(block, inputGain, !wantStereo);

    inputScopeBuffer_.pushBlock(buffer.getReadPointer(0), static_cast<size_t>(numSamples));
    if (!canProcess) {
        buffer.applyGain(outputGain);
        outputScopeBuffer_.pushBlock(buffer.getReadPointer(0), static_cast<size_t>(numSamples));
        return;
    }

    const double solveTolerance = fastLive
        ? wdf::Engine::kFastLiveTolerance : wdf::Engine::kDefaultTolerance;
    constexpr int maxIterations = wdf::Engine::kDefaultMaxIterations;

    // The output scale: the volts the circuit makes at full input, read off
    // the Output terminal, and the reciprocal of it.
    //
    // The circuit is not changed and nothing is limited or clipped -- the
    // output is simply *scaled*, by the number the user wrote. Without it, a
    // preamp that makes 100 volts hands 100 to the audio buffer, which is a
    // digital full-scale overload and a very loud click.
    //
    // Read under the lock, which is held for the rest of the block, because it
    // is written when a document is loaded.
    const float outputScale =
        1.0f / static_cast<float>(outputFullScaleVolts_ > 0.0 ? outputFullScaleVolts_ : 1.0);

    // Stereo needs a live right engine. Mono deliberately leaves it untouched,
    // which is where the CPU saving comes from: one engine instead of two.
    const bool processStereo = wantStereo && rightEngine_ != nullptr;

    // In Fast Live, finish a gesture in the callback that first receives it.
    // Measure the ramp in host frames for every oversampling setting.
    const int normalRamp = std::max(1, static_cast<int>(sampleRate_ * .015));
    const int rampSamples = fastLive && !isNonRealtime() ? std::min(numSamples, normalRamp) : normalRamp;
    readLivePotTargets(rampSamples);
    applySettledLiveTransfer(processStereo);
    leftEngine_->useMixedAffineReduction(fastLive);
    leftEngine_->useCircuitPlan(fastLive);
    if (processStereo) {
        rightEngine_->useMixedAffineReduction(fastLive);
        rightEngine_->useCircuitPlan(fastLive);
    }

    std::copy(baseSourceValues_.begin(), baseSourceValues_.end(), svLeft_.begin());
    if (processStereo)
        std::copy(baseSourceValues_.begin(), baseSourceValues_.end(), svRight_.begin());
    const int inIdx = inSourceIndex_;
    const int outIdx = outNodeIndex_;
    const std::vector<double>* lastNodesL = nullptr;
    float maxAbsOut = 0.0f;

    if (useHalfBand_) {
        float* leftData = buffer.getWritePointer(0);
        float* rightData = (numChannels > 1) ? buffer.getWritePointer(1) : nullptr;

        for (int s = 0; s < numSamples; ++s) {
            advanceLivePots();
            const double inL = static_cast<double>(leftData[s]);
            const double outL = halfBandOversamplerL_.process(inL, [&](double osIn) -> double {
                std::copy(baseSourceValues_.begin(), baseSourceValues_.end(), svLeft_.begin());
                if (!acSources_.empty()) {
                    for (const auto& ac : acSources_) {
                        if (ac.index < 0 || ac.index >= static_cast<int>(svLeft_.size()))
                            continue;
                        svLeft_[static_cast<size_t>(ac.index)] = wdf::sourceValueAt(
                            baseSourceValues_[static_cast<size_t>(ac.index)], ac.ac, ac.freq,
                            acSampleRate_, acSampleCounter_);
                    }
                    ++acSampleCounter_;
                }
                if (inIdx >= 0 && inIdx < static_cast<int>(svLeft_.size()))
                    svLeft_[static_cast<size_t>(inIdx)] = osIn;
                const auto& nodesL = leftEngine_->step(svLeft_, maxIterations, solveTolerance);
                lastNodesL = &nodesL;
                return (outIdx >= 0 && outIdx < static_cast<int>(nodesL.size()))
                    ? nodesL[static_cast<size_t>(outIdx)] * outputScale
                    : 0.0;
            });
            leftData[s] = static_cast<float>(outL);
            maxAbsOut = std::max(maxAbsOut, std::abs(leftData[s]));

            if (processStereo && rightData) {
                const double inR = static_cast<double>(rightData[s]);
                const double outR = halfBandOversamplerR_.process(inR, [&](double osIn) -> double {
                    std::copy(baseSourceValues_.begin(), baseSourceValues_.end(), svRight_.begin());
                    if (!acSources_.empty()) {
                        for (const auto& ac : acSources_) {
                            if (ac.index < 0 || ac.index >= static_cast<int>(svRight_.size()))
                                continue;
                            svRight_[static_cast<size_t>(ac.index)] = wdf::sourceValueAt(
                                baseSourceValues_[static_cast<size_t>(ac.index)], ac.ac, ac.freq,
                                acSampleRate_, acSampleCounter_);
                        }
                    }
                    if (inIdx >= 0 && inIdx < static_cast<int>(svRight_.size()))
                        svRight_[static_cast<size_t>(inIdx)] = osIn;
                    const auto& nodesR = rightEngine_->step(svRight_, maxIterations, solveTolerance);
                    return (outIdx >= 0 && outIdx < static_cast<int>(nodesR.size()))
                        ? nodesR[static_cast<size_t>(outIdx)] * outputScale
                        : 0.0;
                });
                rightData[s] = static_cast<float>(outR);
                maxAbsOut = std::max(maxAbsOut, std::abs(rightData[s]));
            } else if (rightData) {
                rightData[s] = leftData[s];
            }
        }
    } else {
        juce::dsp::AudioBlock<float> oversampledBlock = block;

        if (activeOversampler) {
            oversampledBlock = activeOversampler->processSamplesUp(block);
        }

        const size_t osSamples = oversampledBlock.getNumSamples();
        float* leftData = oversampledBlock.getChannelPointer(0);
        float* rightData = (numChannels > 1) ? oversampledBlock.getChannelPointer(1) : nullptr;

        // Process sample by sample through WDF engine
        const int oversampling = activeOversampler ? currentOversamplingFactor_ : 1;
        int phase = 0;
        for (size_t s = 0; s < osSamples; ++s) {
            if (phase == 0)
                advanceLivePots();
            if (++phase == oversampling)
                phase = 0;
            if (!acSources_.empty()) {
                for (const auto& ac : acSources_) {
                    if (ac.index < 0 || ac.index >= static_cast<int>(svLeft_.size()))
                        continue;
                    const auto value = wdf::sourceValueAt(
                        baseSourceValues_[static_cast<size_t>(ac.index)], ac.ac, ac.freq,
                        acSampleRate_, acSampleCounter_);

                    svLeft_[static_cast<size_t>(ac.index)] = value;
                    if (processStereo)
                        svRight_[static_cast<size_t>(ac.index)] = value;
                }
                ++acSampleCounter_;
            }

            // Left Channel
            if (inIdx >= 0 && inIdx < static_cast<int>(svLeft_.size())) {
                svLeft_[static_cast<size_t>(inIdx)] = static_cast<double>(leftData[s]);
            }
            const auto& nodesL = leftEngine_->step(svLeft_, maxIterations, solveTolerance);
            lastNodesL = &nodesL;
            const float outL = (outIdx >= 0 && outIdx < static_cast<int>(nodesL.size()))
                                   ? static_cast<float>(nodesL[static_cast<size_t>(outIdx)]) * outputScale
                                   : 0.0f;
            leftData[s] = outL;
            maxAbsOut = std::max(maxAbsOut, std::abs(outL));

            if (processStereo && rightData) {
                if (inIdx >= 0 && inIdx < static_cast<int>(svRight_.size())) {
                    svRight_[static_cast<size_t>(inIdx)] = static_cast<double>(rightData[s]);
                }
                const auto& nodesR = rightEngine_->step(svRight_, maxIterations, solveTolerance);
                const float outR = (outIdx >= 0 && outIdx < static_cast<int>(nodesR.size()))
                                       ? static_cast<float>(nodesR[static_cast<size_t>(outIdx)]) * outputScale
                                       : 0.0f;
                rightData[s] = outR;
                maxAbsOut = std::max(maxAbsOut, std::abs(outR));
            } else if (rightData) {
                rightData[s] = outL;
            }
        }
    }

    // Read live node probe voltages for UI from the last processed sample.
    // **Do not step again here.** An extra step advances the engine's state and
    // makes the probe readout a different circuit from the one just heard.
    {
        std::unique_lock<std::mutex> pLock(probeMutex_, std::try_to_lock);
        if (pLock.owns_lock() && lastNodesL != nullptr) {
            for (size_t i = 0; i < probeNodes_.size(); ++i) {
                const auto node = static_cast<size_t>(probeNodes_[i].second);
                if (node < lastNodesL->size())
                    probeVoltages_[i] = (*lastNodesL)[node];
            }
        }
    }

    signalActivity_.store(std::min(1.0f, maxAbsOut * 0.5f), std::memory_order_relaxed);

    // Downsample if oversampling was active
    if (activeOversampler) {
        activeOversampler->processSamplesDown(block);
    } else if (useHalfBand_) {
        juce::dsp::ProcessContextReplacing<float> context(block);
        halfBandPadding_.process(context);
    }

    // Apply output master gain & dry/wet mix
    for (int ch = 0; ch < numChannels; ++ch) {
        float* channelData = buffer.getWritePointer(ch);
        const float* dryData = dryAvailable ? dryBuffer_.getReadPointer(ch) : nullptr;

        for (int i = 0; i < numSamples; ++i) {
            float outSample = channelData[i] * outputGain;
            if (dryData) {
                outSample = dryData[i] * dryMix + outSample * wetMix;
            }
            // Last line of defence: a host mutes the whole track on a single
            // NaN/inf sample, and a plugin should never hand one to it. The
            // engine rejects non-physical states too; this also covers a bad
            // input buffer or a dry-path sample from the host.
            if (!std::isfinite(outSample)) {
                outSample = 0.0f;
            }
            channelData[i] = outSample;
        }
    }

    // Push output audio into circular scope buffer
    outputScopeBuffer_.pushBlock(buffer.getReadPointer(0), static_cast<size_t>(numSamples));
}

std::map<std::string, double> LiveWDFPluginProcessor::getLiveProbeVoltages() const {
    std::lock_guard<std::mutex> lock(probeMutex_);
    std::map<std::string, double> voltages;
    for (size_t i = 0; i < probeNodes_.size(); ++i)
        voltages[probeNodes_[i].first] = probeVoltages_[i];
    return voltages;
}

} // namespace livewdf
