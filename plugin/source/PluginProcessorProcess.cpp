#include "PluginProcessor.h"

#include "format/AcSource.h"

#include <algorithm>
#include <chrono>
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
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);
    const int inputChannels = std::min(getMainBusNumInputChannels(), buffer.getNumChannels());
    for (int ch = inputChannels; ch < buffer.getNumChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());
    if (inputChannels > 0)
        processDryDelay(juce::dsp::AudioBlock<float>(buffer).getSubsetChannelBlock(0, inputChannels),
                        true, processingLatencySamples_.load(std::memory_order_relaxed));
    if (buffer.getNumChannels() > 0)
        outputEffects_.process(juce::dsp::AudioBlock<float>(buffer), {}, isNonRealtime());
}

OutputEffects::Settings LiveWDFPluginProcessor::outputEffectSettings() const noexcept {
    const auto load = [](const std::atomic<float>* parameter) { return parameter->load(std::memory_order_relaxed); };
    return {load(cabinetEnabled_) > 0.5f, load(echoLevel_) * 0.01f, load(reverbLevel_) * 0.01f,
            load(echoDelay_), load(echoFeedback_) * 0.01f, load(reverbPreDelay_)};
}

void LiveWDFPluginProcessor::processOutputEffects(juce::dsp::AudioBlock<float> block) noexcept {
    outputEffects_.process(block, outputEffectSettings(), isNonRealtime());
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

    for (int ch = getMainBusNumInputChannels(); ch < numChannels; ++ch)
        buffer.clear(ch, 0, numSamples);

    // A bounce can apply pending processing options itself. Host latency
    // notifications are sent by the control thread.
    if (isNonRealtime()) {
        outputEffects_.reclaim();
        outputEffects_.applyReverbSize(reverbSize_->load(std::memory_order_relaxed) * 0.01f, true);
        applyPendingCircuitOptions();
    }

    const bool fastLive = !shouldForceAccurate() && *apvts_.getRawParameterValue("solve_quality") > 0.5f;

    // Read audio gain parameters
    const float inputTrimDb = *apvts_.getRawParameterValue("input_trim");
    const float outputMasterDb = *apvts_.getRawParameterValue("output_master");
    const float mixPercent = *apvts_.getRawParameterValue("mix");
    const bool stereoRequested = *apvts_.getRawParameterValue("stereo") > 0.5f;

    const float inputGain = juce::Decibels::decibelsToGain(inputTrimDb);
    const float outputGain = juce::Decibels::decibelsToGain(outputMasterDb);
    const float wetMix = mixPercent * 0.01f;
    const bool chainInput = *apvts_.getRawParameterValue("chain_input") > 0.5f;
    // A powered amp ends at speaker level, which is no input for a next LiveWDF.
    const bool matchOutput = outputAtSpeakerLevel() || *apvts_.getRawParameterValue("match_output") > 0.5f;

    const bool wantStereo = stereoRequested && numChannels > 1;

    // Try to acquire engine lock (never block audio thread)
    std::unique_lock<std::mutex> lock(engineMutex_, std::try_to_lock);
    const double presetInput = inputVoltsPerFullScale_.load(std::memory_order_relaxed);
    constexpr double chainVolts = wdf::LiveWDFDocument::kChainVoltsPerFullScale;
    const double inputScale = chainInput ? chainVolts : presetInput;
    // Carry a dry signal through the same change of units as a wire around the circuit.
    // At the matched output a bypassed preset keeps its original audition level.
    const float dryScale = static_cast<float>(inputScale / (matchOutput ? presetInput : chainVolts));
    const int latency = processingLatencySamples_.load(std::memory_order_relaxed);
    auto* activeOversampler = lock.owns_lock() ? activePolyphaseOversampler() : nullptr;
    juce::dsp::AudioBlock<float> block(buffer);
    const bool compiledPreview = fastLive && *apvts_.getRawParameterValue("compiled_preview") > 0.5f;
    // A heavy circuit's full solver can run several times slower than real time,
    // so once it misses the deadline Preview treats the wait for its first plan like a rebuild.
    const bool previewLoading = lock.owns_lock() && leftEngine_ && compiledPreview && !isNonRealtime() &&
        outputOnlyPreviewAllowed_ && !previewPrepared_ && previewLoadOverruns_ >= 4;
    const bool canProcess = lock.owns_lock() && leftEngine_ && outNodeIndex_ >= 0
        && !(activeOversampler && numSamples > samplesPerBlock_) && !previewLoading;

    // Keep untrimmed separate channels in the history, including at Mix 100%.
    // Host bypass can then return the delayed original input immediately.
    const bool dryAvailable = canProcess && wetMix < 0.999f && dryBuffer_.getNumSamples() >= numSamples;
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
    if (previewLoading) {
        readLivePotTargets(numSamples);
        for (int s = 0; s < numSamples; ++s)
            advanceLivePots();
        applySettledLiveTransfer(wantStereo && rightEngine_);
        captureSettledTransferIfRequested();
    }
    if (!canProcess) {
        // A voltage-chain return may have a large dry conversion (e.g. a mic preamp).
        // During a rebuild only pass the dry contribution the player actually requested.
        const bool passThrough = (!chainInput && matchOutput) || (lock.owns_lock() && !leftEngine_);
        const float fallbackGain = passThrough ? outputGain : (1.0f - wetMix) * dryScale;
        buffer.applyGain(fallbackGain);
        processOutputEffects(block);
        outputScopeBuffer_.pushBlock(buffer.getReadPointer(0), static_cast<size_t>(numSamples));
        return;
    }

    const auto solveStart = std::chrono::steady_clock::now();
    const double solveTolerance = fastLive
        ? wdf::Engine::kFastLiveTolerance : wdf::Engine::kDefaultTolerance;
    constexpr int maxIterations = wdf::Engine::kDefaultMaxIterations;

    const double modeTrim = compiledPreview ? outputPreviewTrim_.load(std::memory_order_relaxed)
        : fastLive ? outputFastTrim_.load(std::memory_order_relaxed) : 1.0;
    const float outputScale = static_cast<float>(1.0 / (matchOutput ? getCircuitOutputFullScaleVolts() * modeTrim : chainVolts));
    const float dryMix = (1.0f - wetMix) * dryScale;

    // Stereo needs a live right engine. Mono deliberately leaves it untouched,
    // which is where the CPU saving comes from: one engine instead of two.
    const bool processStereo = wantStereo && rightEngine_ != nullptr;
    // Two cores needs the worker: one engine step per hand-over, at most two in a host sample.
    const int pipeSteps = activeOversampler ? currentOversamplingFactor_ : 1;
    // Only a circuit with power tubes has a cut; anything else keeps its one-core Preview routes.
    const bool twoCores = !processStereo && numChannels > 1 && compiledPreview && rightEngine_ && !useHalfBand_ && pipeSteps <= 2 &&
        leftEngine_->pentodeCount() > 0 && *apvts_.getRawParameterValue("two_cores") > 0.5f;
    const bool secondEngine = processStereo || twoCores;

    // In Fast Live, finish a gesture in the callback that first receives it.
    // Measure the ramp in host frames for every oversampling setting.
    const int normalRamp = std::max(1, static_cast<int>(sampleRate_ * .015));
    const int rampSamples = fastLive && !isNonRealtime() ? std::min(numSamples, normalRamp) : normalRamp;
    readLivePotTargets(rampSamples);
    applySettledLiveTransfer(secondEngine);
    audioTransferRevision_.store(potsMoving_ || leftEngine_->hasAdjustedLiveTransfer()
        ? adjustedAudioTransfer : leftEngine_->linearRebuilds(), std::memory_order_relaxed);
    captureSettledTransferIfRequested();
    leftEngine_->useCompiledOptical(compiledPreview);
    if (secondEngine) rightEngine_->useCompiledOptical(compiledPreview);
    const bool tryBipolar = compiledPreview && previewPrepared_ && outputOnlyPreviewAllowed_ && !twoCores;
    if (!tryBipolar) {
        leftEngine_->releaseBipolarPreview();
        if (rightEngine_) rightEngine_->releaseBipolarPreview();
    }
    const bool keepBlockAnswer = compiledPreview && !isNonRealtime() && previewKeepsBlockAnswer_.load(std::memory_order_relaxed);
    leftEngine_->useLoadedJunctionPreview(compiledPreview);
    leftEngine_->useTriodePreview(compiledPreview && !isNonRealtime());
    leftEngine_->keepBlockAnswer(keepBlockAnswer);
    if (secondEngine) {
        rightEngine_->useLoadedJunctionPreview(compiledPreview);
        rightEngine_->useTriodePreview(compiledPreview && !isNonRealtime());
        rightEngine_->keepBlockAnswer(keepBlockAnswer);
    }
    leftEngine_->useMixedAffineReduction(fastLive);
    leftEngine_->useCircuitPlan(fastLive);
    leftEngine_->useSlowLag(fastLive);
    leftEngine_->usePreviewPentodes(compiledPreview);
    if (secondEngine) {
        rightEngine_->useMixedAffineReduction(fastLive);
        rightEngine_->useCircuitPlan(fastLive);
        rightEngine_->useSlowLag(fastLive);
        rightEngine_->usePreviewPentodes(compiledPreview);
    }

    std::copy(baseSourceValues_.begin(), baseSourceValues_.end(), svLeft_.begin());
    if (secondEngine)
        std::copy(baseSourceValues_.begin(), baseSourceValues_.end(), svRight_.begin());
    const double targetPad = compiledPreview && *apvts_.getRawParameterValue("preview_pad") > 0.5f
        ? previewPadScale_ : 1.;
    const auto retryPaddedPreview = [&] {
        leftEngine_->retryBipolarPreviewAfterAttenuation();
        if (rightEngine_) rightEngine_->retryBipolarPreviewAfterAttenuation();
    };
    if (isNonRealtime() || !compiledPreview) {
        if (targetPad < previewPadGain_.getCurrentValue())
            retryPaddedPreview();
        previewPadGain_.setCurrentAndTargetValue(targetPad);
    } else {
        previewPadGain_.setTargetValue(targetPad);
    }
    double paddedInputScale = inputScale * previewPadGain_.getCurrentValue();
    double makeup = 1. / previewPadGain_.getCurrentValue();
    bool padSettled = false;
    const auto advanceControls = [&](bool applyToEngines) {
        const bool potsDue = advanceLivePots();
        if (potsDue && applyToEngines)
            applyLiveControlsToEngines(true, false);
        padSettled = false;
        if (previewPadGain_.isSmoothing()) {
            const double gain = previewPadGain_.getNextValue();
            paddedInputScale = inputScale * gain;
            makeup = 1. / gain;
            padSettled = !previewPadGain_.isSmoothing() && targetPad < 1.;
            if (padSettled && applyToEngines)
                retryPaddedPreview();
        }
        return potsDue;
    };
    // Only amplify the signal above the circuit's quiescent output voltage.
    const auto compensate = [&](double volts, double gainMakeup) {
        return gainMakeup == 1. ? volts : previewPadOutputBias_ + (volts - previewPadOutputBias_) * gainMakeup;
    };
    const int inIdx = inSourceIndex_;
    const int outIdx = outNodeIndex_;
    struct OutputSample {
        double volts;
        const std::vector<double>* nodes;
    };
    const auto solve = [&](wdf::Engine& engine, std::vector<double>& sources, double sample, double scale) -> OutputSample {
        const double input = sample * scale;
        if (inIdx >= 0)
            sources[static_cast<size_t>(inIdx)] = input;
        double volts;
        if (tryBipolar && engine.stepBipolarPreview(input, volts))
            return {volts, nullptr};
        const auto& nodes = engine.step(sources, maxIterations, solveTolerance);
        return {nodes[static_cast<size_t>(outIdx)], &nodes};
    };
    OutputSample lastLeft{};
    float maxAbsOut = 0.0f;

    juce::dsp::AudioBlock<float> processed = block;
    if (!useHalfBand_ && activeOversampler)
        processed = activeOversampler->processSamplesUp(block);
    float* leftData = processed.getChannelPointer(0);
    float* rightData = (numChannels > 1) ? processed.getChannelPointer(1) : nullptr;
    const int oversampling = !useHalfBand_ && activeOversampler ? currentOversamplingFactor_ : 1;

    // One channel over host samples [from, to). `controls` runs before each host sample.
    const auto runChannel = [&](wdf::Engine& engine, std::vector<double>& sources, wdf::HalfBandOversampler& halfBand,
                                float* data, int from, int to, long long& acSample, OutputSample* last, auto&& controls,
                                auto&& solveStep) {
        float peak = 0.0f;
        double scale = paddedInputScale, gainMakeup = makeup;
        const auto setAcSources = [&] {
            for (const auto& ac : acSources_) {
                if (ac.index < 0 || ac.index >= static_cast<int>(sources.size()))
                    continue;
                sources[static_cast<size_t>(ac.index)] = wdf::sourceValueAt(
                    baseSourceValues_[static_cast<size_t>(ac.index)], ac.ac, ac.freq, acSampleRate_, acSample);
            }
            ++acSample;
        };
        if (useHalfBand_) {
            for (int s = from; s < to; ++s) {
                controls(s, scale, gainMakeup);
                const double out = halfBand.process(static_cast<double>(data[s]), [&](double osIn) -> double {
                    std::copy(baseSourceValues_.begin(), baseSourceValues_.end(), sources.begin());
                    if (!acSources_.empty())
                        setAcSources();
                    const auto result = solveStep(engine, sources, osIn, scale);
                    if (last) *last = result;
                    return compensate(result.volts, gainMakeup) * outputScale;
                });
                data[s] = static_cast<float>(out);
                peak = std::max(peak, std::abs(data[s]));
            }
            return peak;
        }
        for (size_t s = static_cast<size_t>(from * oversampling); s < static_cast<size_t>(to * oversampling); ++s) {
            if (s % static_cast<size_t>(oversampling) == 0)
                controls(static_cast<int>(s) / oversampling, scale, gainMakeup);
            if (!acSources_.empty())
                setAcSources();
            const auto result = solveStep(engine, sources, data[s], scale);
            if (last) *last = result;
            data[s] = static_cast<float>(compensate(result.volts, gainMakeup)) * outputScale;
            peak = std::max(peak, std::abs(data[s]));
        }
        return peak;
    };

    bool pipelined = false;
    // A moving control runs on one core: stage 1's copied blocks hold the settled transfer.
    // The split comes back once the settled rebuild is installed and stage 1 is re-copied from stage 2.
    const bool split = twoCores && !potsMoving_ && !leftEngine_->hasAdjustedLiveTransfer();
    if (split && !(leftEngine_->pipelinePrepared() && rightEngine_->pipelinePrepared()) &&
        pipeTried_ != leftEngine_->linearRebuilds()) {
        pipeTried_ = leftEngine_->linearRebuilds();
        if (!(leftEngine_->preparePipelineStage(2) && rightEngine_->preparePipelineStage(1) &&
              leftEngine_->pipelineOutputSize() <= kPipePorts && rightEngine_->pipelineOutputSize() <= kPipePorts))
            leftEngine_->stopPipeline();
    }
    if (split && leftEngine_->pipelinePrepared() && rightEngine_->pipelinePrepared()) {
        // First block, or the first after a late one: stage 1 restarts from stage 2's state.
        if (leftEngine_->pipelinePaused() || rightEngine_->pipelinePaused()) {
            rightEngine_->copyStateFrom(*leftEngine_);
            std::copy(svLeft_.begin(), svLeft_.end(), svRight_.begin());
            leftEngine_->resumePipeline();
            rightEngine_->resumePipeline();
            pipeStart_ = pipeStep_;
            pipeEarlyDone_.store(pipeStep_, std::memory_order_relaxed);
            pipeLateDone_.store(pipeStep_, std::memory_order_relaxed);
            pipeAbort_.store(false, std::memory_order_relaxed);
        }
        pipelined = true;
    } else {
        leftEngine_->pausePipeline();
        if (rightEngine_) rightEngine_->pausePipeline();
        if (!twoCores) pipeTried_ = -1;
    }
    if (!processStereo && !pipelined) {
        maxAbsOut = runChannel(*leftEngine_, svLeft_, halfBandOversamplerL_, leftData, 0, numSamples, acSampleCounter_,
                               &lastLeft, [&](int, double& scale, double& gainMakeup) {
            advanceControls(true);
            scale = paddedInputScale;
            gainMakeup = makeup;
        }, solve);
        if (rightData)
            std::copy(leftData, leftData + processed.getNumSamples(), rightData);
    }
    for (int from = 0; secondEngine && pipelined != processStereo && from < numSamples; from += kControlChunk) {
        const int to = std::min(numSamples, from + kControlChunk);
        int potRows = 0;
        for (int s = from; s < to; ++s) {
            const int row = advanceControls(false) ? potRows++ : -1;
            for (int g = 0; row >= 0 && g < potControlCount_; ++g)
                potLog_[static_cast<size_t>(row)][static_cast<size_t>(g)] =
                    potControls_[static_cast<size_t>(g)].position.getCurrentValue();
            controlLog_[static_cast<size_t>(s - from)] = {paddedInputScale, makeup, padSettled, row};
        }
        const auto replay = [&](wdf::Engine& engine, std::vector<std::pair<int, double>>& pots,
                                std::vector<std::pair<int, double>>& varRes, int s, double& scale, double& gainMakeup) {
            const auto& step = controlLog_[static_cast<size_t>(s - from)];
            if (step.potRow >= 0) {
                pots.clear();
                varRes.clear();
                for (const auto& t : potTargets_)
                    (t.varRes ? varRes : pots).emplace_back(t.index, potLog_[static_cast<size_t>(step.potRow)][static_cast<size_t>(t.group)]);
                if (!engine.setLiveControls(pots, varRes))
                    liveControlUpdateFailed_.store(true, std::memory_order_relaxed);
            }
            if (step.retry)
                engine.retryBipolarPreviewAfterAttenuation();
            scale = step.scale;
            gainMakeup = step.makeup;
        };
        long long rightAcSample = acSampleCounter_;
        float leftPeak = 0.0f, rightPeak = 0.0f;
        const long long firstStep = pipeStep_;
        long long earlyStep = firstStep, lateStep = firstStep;
        // The audio thread waits a few sample periods at most, then solves the rest of the block alone.
        const auto spinLimit = std::chrono::nanoseconds(static_cast<long long>(4.0e9 / sampleRate_));
        const auto arrived = [&](const std::atomic<long long>& done, long long step) {
            const auto deadline = std::chrono::steady_clock::now() + spinLimit;
            while (done.load(std::memory_order_acquire) < step)
                if (std::chrono::steady_clock::now() > deadline)
                    return false;
                else
                    cpuRelax();
            return true;
        };
        bool late = false;
        // Stage 1 reads stage 2's currents two steps back, so the two run one step apart.
        const auto earlyStage = [&](wdf::Engine& engine, std::vector<double>& sources, double sample, double scale) {
            const long long n = earlyStep++;
            if (pipeAbort_.load(std::memory_order_relaxed))
                return OutputSample{0.0, nullptr};
            if (n - 2 >= pipeStart_) {
                while (pipeLateDone_.load(std::memory_order_acquire) < n - 1)
                    if (pipeAbort_.load(std::memory_order_relaxed))
                        return OutputSample{0.0, nullptr};
                    else
                        cpuRelax();
                const auto& late = pipeLate_[static_cast<size_t>(n - 2) & 3];
                std::copy_n(late.begin(), engine.pipelineInputSize(), engine.pipelineInput());
            }
            const auto result = solve(engine, sources, sample, scale);
            engine.pipelineOutput(pipeEarly_[static_cast<size_t>(n) & 3].data());
            pipeEarlyDone_.store(n + 1, std::memory_order_release);
            return result;
        };
        const auto lateStage = [&](wdf::Engine& engine, std::vector<double>& sources, double sample, double scale) {
            const long long n = lateStep++;
            if (!late && !arrived(pipeEarlyDone_, n + 1)) {
                late = true;
                ++pipeTimeouts_;
                pipeAbort_.store(true, std::memory_order_relaxed);
                leftEngine_->pausePipeline();
            }
            if (late)
                return solve(engine, sources, sample, scale);
            const auto& early = pipeEarly_[static_cast<size_t>(n) & 3];
            std::copy_n(early.begin(), engine.pipelineInputSize(), engine.pipelineInput());
            const auto result = solve(engine, sources, sample, scale);
            engine.pipelineOutput(pipeLate_[static_cast<size_t>(n) & 3].data());
            pipeLateDone_.store(n + 1, std::memory_order_release);
            return result;
        };
        const auto runLeft = [&](int a, int b) {
            const auto step = [&](wdf::Engine& e, std::vector<double>& src, double x, double sc) {
                return pipelined ? lateStage(e, src, x, sc) : solve(e, src, x, sc);
            };
            leftPeak = std::max(leftPeak, runChannel(*leftEngine_, svLeft_, halfBandOversamplerL_, leftData, a, b, acSampleCounter_, &lastLeft,
                                  [&](int s, double& scale, double& gainMakeup) { replay(*leftEngine_, potMoves_, varResMoves_, s, scale, gainMakeup); }, step));
        };
        const auto runRight = [&](int a, int b) {
            const auto step = [&](wdf::Engine& e, std::vector<double>& src, double x, double sc) {
                return pipelined ? earlyStage(e, src, x, sc) : solve(e, src, x, sc);
            };
            rightPeak = std::max(rightPeak, runChannel(*rightEngine_, svRight_, halfBandOversamplerR_, rightData, a, b, rightAcSample, nullptr,
                                   [&](int s, double& scale, double& gainMakeup) { replay(*rightEngine_, rightPotMoves_, rightVarResMoves_, s, scale, gainMakeup); }, step));
        };
        auto runRightBlock = [&] { runRight(from, to); };
        // Spin only against a realtime worker that takes the block at once; otherwise solve the block on one core.
        const auto pipePosted = [&](auto& job) {
            if (!rightWorker_.post(job))
                return false;
            const auto deadline = std::chrono::steady_clock::now() + spinLimit;
            while (rightWorker_.realtime() && !rightWorker_.claimed() && std::chrono::steady_clock::now() < deadline)
                cpuRelax();
            if (!rightWorker_.takeBack())
                return true;
            ++pipeUnclaimed_;
            rightWorker_.join();
            return false;
        };
        if (!pipelined) {
            rightWorker_.runBeside([&] { runLeft(from, to); }, runRightBlock);
        } else if (pipeAbort_.load(std::memory_order_relaxed)) {
            runLeft(from, to);
        } else if (pipePosted(runRightBlock)) {
            runLeft(from, to);
            rightWorker_.join();
        } else {
            late = true;
            leftEngine_->pausePipeline();
            runLeft(from, to);
        }
        pipeStep_ = lateStep;
        maxAbsOut = std::max({maxAbsOut, leftPeak, pipelined ? 0.0f : rightPeak});
    }
    if (pipelined && rightData)
        std::copy(leftData, leftData + processed.getNumSamples(), rightData);

    // Read live node probe voltages for UI from the last processed sample.
    // **Do not step again here.** An extra step advances the engine's state and
    // makes the probe readout a different circuit from the one just heard.
    {
        std::unique_lock<std::mutex> pLock(probeMutex_, std::try_to_lock);
        if (pLock.owns_lock()) {
            for (size_t i = 0; i < probeNodes_.size(); ++i) {
                const auto node = static_cast<size_t>(probeNodes_[i].second);
                if (lastLeft.nodes != nullptr && node < lastLeft.nodes->size())
                    probeVoltages_[i] = (*lastLeft.nodes)[node];
                else if (probeNodes_[i].second == outIdx)
                    probeVoltages_[i] = lastLeft.volts;
            }
        }
    }

    signalActivity_.store(std::min(1.0f, maxAbsOut * 0.5f), std::memory_order_relaxed);
    if (compiledPreview && !previewPrepared_ && std::chrono::steady_clock::now() - solveStart >
            std::chrono::duration<double>(numSamples / sampleRate_))
        ++previewLoadOverruns_;

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

    processOutputEffects(block);
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
