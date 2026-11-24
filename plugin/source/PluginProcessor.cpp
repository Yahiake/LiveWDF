#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FactorySchematics.h"
#include "FastLiveCache.h"

#include "format/AcSource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace livewdf {

namespace {

// The low-frequency group delay is the first moment of the oversampling
// round trip. Measure after decimation so the phase of the decimator is included.
float halfBandLatency(wdf::OversamplingFactor factor) {
    wdf::HalfBandOversampler oversampler(factor);
    double gain = 0.0;
    double moment = 0.0;
    for (int i = 0; i < 256; ++i) {
        const double sample = oversampler.process(i == 0 ? 1.0 : 0.0, [](double x) { return x; });
        gain += sample;
        moment += i * sample;
    }
    return static_cast<float>(moment / gain);
}

std::atomic_flag affineWorkerBusy;

struct AffineWorkerSlot {
    ~AffineWorkerSlot() { affineWorkerBusy.clear(std::memory_order_release); }
};

/// The sentence the plugin shows when it cannot build the circuit.
///
/// **The engine's message is true and useless.** "rootTransfer failed
/// (singular linear network)" says the solver gave up and not why; the
/// drawing's own answer is "a part is joined to nothing", and that is a thing
/// the user can go and fix.
///
/// The same finding the editor uses, from the same place
/// (`spice/Connectivity.h`) -- so the plugin and the editor cannot disagree
/// about what is wrong with a circuit.
std::string describeRejection(const wdf::LiveWDFDocument& doc,
                               const std::string& engineMessage) {
    const auto unconnected = doc.unconnectedPins();
    if (!unconnected.empty())
        return unconnected.front().first + " is not connected to anything"
               " -- the circuit cannot be built";

    return "circuit rejected: " + engineMessage;
}

}  // namespace

// ==============================================================================
// Parameter Layout
// ==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout LiveWDFPluginProcessor::createParameterLayout() {
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"input_trim", 1}, "Input Trim",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"output_master", 1}, "Output Master",
        juce::NormalisableRange<float>(-48.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"mix", 1}, "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.5f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"oversampling", 1}, "Oversampling",
        juce::StringArray{ "1x (Direct)", "2x (Polyphase)", "4x (Polyphase)", "8x (Polyphase)",
                           "2x (Halfband IIR)", "4x (Halfband IIR)" }, 0));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"grid_current", 1}, "Grid Current", true));

    // Mono is the default because one engine is half the work of two and most
    // guitar/amp chains are mono until the very end. Stereo is opt-in, and the
    // editor exposes it as a button next to the oversampling choice.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"stereo", 1}, "Stereo", false));

    // Dynamic Potentiometers. The layout has to exist before the document is
    // known, so this is the ceiling: kMaxPotKnobs groups always get a
    // parameter, and a document with more keeps their saved positions.
    for (int i = 0; i < kMaxPotKnobs; ++i) {
        const juce::String potId = "pot_" + juce::String(i);
        const juce::String potName = "Pot " + juce::String(i + 1);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{potId, 1}, potName,
            juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));
    }

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"report_latency", 1}, "Report Latency", true,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"solve_quality", 1}, "Solve Quality",
        juce::StringArray{"Accurate", "Fast Live"}, 1));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"accurate_export", 1}, "Accurate export", true));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"export_4x", 1}, "4x export", true));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"model_preview", 1}, "Model Preview", false,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));

    return { params.begin(), params.end() };
}

// ==============================================================================
// Processor Construction & Audio Callbacks
// ==============================================================================
LiveWDFPluginProcessor::LiveWDFPluginProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "Parameters", createParameterLayout()) {
    startTimerHz(10);

    // Initialize oversamplers (2x = factor 1, 4x = factor 2, 8x = factor 3)
    oversampler2x_ = std::make_unique<juce::dsp::Oversampling<float>>(
        2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    oversampler4x_ = std::make_unique<juce::dsp::Oversampling<float>>(
        2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    oversampler8x_ = std::make_unique<juce::dsp::Oversampling<float>>(
        2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);

    // **No built-in circuit.** A factory preset is uneditable and cannot carry
    // the routed wires the editor draws, so the plugin opens empty and waits for
    // a `.livewdf` file (or a host session that restores one).
}

LiveWDFPluginProcessor::~LiveWDFPluginProcessor() {
    stopTimer();
    delete pendingSettledTransfer_.exchange(nullptr);
    delete retiredSettledTransfer_.exchange(nullptr);
}

bool LiveWDFPluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto& mainIn = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;

    if (mainIn != mainOut && !mainIn.isDisabled())
        return false;

    return true;
}

void LiveWDFPluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    std::unique_lock<std::mutex> engineLock(engineMutex_);
    sampleRate_ = sampleRate;
    samplesPerBlock_ = samplesPerBlock;

    oversampler2x_->initProcessing(static_cast<size_t>(samplesPerBlock));
    oversampler4x_->initProcessing(static_cast<size_t>(samplesPerBlock));
    oversampler8x_->initProcessing(static_cast<size_t>(samplesPerBlock));
    oversamplersPrepared_ = true;

    oversampler2x_->reset();
    oversampler4x_->reset();
    oversampler8x_->reset();
    halfBandOversamplerL_.reset();
    halfBandOversamplerR_.reset();
    const juce::dsp::ProcessSpec spec{sampleRate, static_cast<juce::uint32>(samplesPerBlock), 2};
    halfBandPadding_.prepare(spec);
    dryDelay_.prepare(spec);
    dryDelaySamples_ = 0;
    engineLock.unlock();

    // Scratch for the dry/wet copy. Pre-size larger than the announced host
    // block so an oversized callback never has to allocate on the audio thread.
    dryBuffer_.setSize(2, std::max(samplesPerBlock, 4096), false, false, true);

    // Rebuild engines with current sample rate and oversampling factor. This is
    // not a new document, so keep the user's pot positions. With no circuit
    // loaded yet there is nothing to rebuild.
    //
    // It is also still the same *file*: `loadCircuitDocument` clears the reload
    // path because a document can come from anywhere, so put it back. A host
    // changing the device rate or block size must not kill the Reload button.
    const juce::File circuitFile = currentFile_;
    if (!currentDoc_.components.empty())
        loadCircuitDocument(currentDoc_, false);
    currentFile_ = circuitFile;
    updateReportedLatency();
}

void LiveWDFPluginProcessor::releaseResources() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    oversampler2x_->reset();
    oversampler4x_->reset();
    oversampler8x_->reset();
    halfBandOversamplerL_.reset();
    halfBandOversamplerR_.reset();
    halfBandPadding_.reset();
    dryDelay_.reset();
}

juce::dsp::Oversampling<float>* LiveWDFPluginProcessor::activePolyphaseOversampler() const noexcept {
    if (!useHalfBand_) {
        switch (currentOversamplingFactor_) {
            case 2: return oversampler2x_.get();
            case 4: return oversampler4x_.get();
            case 8: return oversampler8x_.get();
            default: break;
        }
    }
    return nullptr;
}

void LiveWDFPluginProcessor::updateReportedLatency() {
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    const bool report = *apvts_.getRawParameterValue("report_latency") > 0.5f;
    setLatencySamples(report ? processingLatencySamples_.load(std::memory_order_relaxed) : 0);
}

bool LiveWDFPluginProcessor::shouldForceAccurate() const {
    return isNonRealtime() && *apvts_.getRawParameterValue("accurate_export") > 0.5f;
}

int LiveWDFPluginProcessor::effectiveOversamplingChoice() const {
    constexpr int polyphase4x = 2;
    return isNonRealtime() && *apvts_.getRawParameterValue("export_4x") > 0.5f
        ? polyphase4x : static_cast<int>(*apvts_.getRawParameterValue("oversampling"));
}

// Changes to grid current or sample rate rebuild the engines.
void LiveWDFPluginProcessor::timerCallback() {
    const std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    if (liveControlUpdateFailed_.exchange(false, std::memory_order_relaxed))
        lastError_ = "Could not apply a live control setting";
    applyPendingCircuitOptions();
    if (!isNonRealtime() && !currentDoc_.components.empty()) {
        preparePreview();
        if (previewStatus() != PreviewStatus::active && previewStatus() != PreviewStatus::experimental) {
            prepareSettledLiveTransfer();
            prepareFastLiveAffine();
        }
    }
    updateReportedLatency();
}

const char* LiveWDFPluginProcessor::previewLabel() const noexcept {
    switch (previewStatus()) {
        case PreviewStatus::loading: return "Loading fit";
        case PreviewStatus::unavailable: return "No fit";
        case PreviewStatus::armed: return "Preview ready";
        case PreviewStatus::active: return "Preview active";
        case PreviewStatus::experimental: return "Experimental";
        case PreviewStatus::controls: return "Full: controls";
        case PreviewStatus::range: return "Full: level";
        case PreviewStatus::options: return "Full: settings";
        case PreviewStatus::render: return "Full: export";
        case PreviewStatus::off: return "Preview";
    }
    return "Preview";
}

void LiveWDFPluginProcessor::preparePreview() {
    const bool enabled = *apvts_.getRawParameterValue("model_preview") > 0.5f;
    if (previewJob_.valid() && previewJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::unique_ptr<PreviewPlayback> result;
        try { result = previewJob_.get(); } catch (const std::exception&) {}
        if (enabled && previewRequest_ && previewJobGeneration_ == engineGeneration_) {
            if (!result) previewStatus_.store(PreviewStatus::unavailable);
            else {
                // Start both the model and its physical fallback from a fresh operating point.
                const auto file = currentFile_;
                if (!loadCircuitDocument(currentDoc_, false, std::move(result)))
                    previewStatus_.store(PreviewStatus::unavailable);
                currentFile_ = file;
            }
        }
    }
    if (!enabled) {
        previewRequest_ = 0;
        previewStatus_.store(PreviewStatus::off);
        return;
    }
    if (previewRequest_ == engineGeneration_ || previewJob_.valid()) return;
    previewRequest_ = previewJobGeneration_ = engineGeneration_;
    if (attemptedOversamplingChoice_ != 0 || !attemptedGridCurrent_) {
        previewStatus_.store(PreviewStatus::options);
        return;
    }
    previewStatus_.store(PreviewStatus::loading);
    try {
        previewJob_ = std::async(std::launch::async,
            [doc = currentDoc_, file = currentFile_, rate = sampleRate_] {
                return loadPreviewPlayback(doc.toJson(), file, rate);
            });
    } catch (const std::system_error&) {
        previewStatus_.store(PreviewStatus::unavailable);
    }
}

void LiveWDFPluginProcessor::applyPendingCircuitOptions() {
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    if (currentDoc_.components.empty())
        return;

    if (effectiveOversamplingChoice() != attemptedOversamplingChoice_ ||
        (*apvts_.getRawParameterValue("grid_current") > 0.5f) != attemptedGridCurrent_) {
        const juce::File circuitFile = currentFile_;
        loadCircuitDocument(currentDoc_, false);
        currentFile_ = circuitFile;
    }
}

void LiveWDFPluginProcessor::prepareSettledLiveTransfer() {
    if (settledTransferPublished_) {
        const std::unique_ptr<SettledTransfer> retired(
            retiredSettledTransfer_.exchange(nullptr, std::memory_order_acquire));
        if (!retired)
            return;
        settledTransferPublished_ = false;
        if (retired->installedRevision != 0 && retired->affine &&
            retired->generation == engineGeneration_)
            requestedAffineKey_ = {retired->generation, retired->installedRevision, retired->stereo};
    }
    if (settledTransferJob_.valid()) {
        if (settledTransferJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try {
            auto result = std::make_unique<SettledTransfer>(settledTransferJob_.get());
            if (result->generation == engineGeneration_) {
                pendingSettledTransfer_.store(result.release(), std::memory_order_release);
                settledTransferPublished_ = true;
            }
        } catch (const std::exception&) {
            // The adjusted transfer remains active.
        }
        return;
    }
    const auto revisions = [this]() -> TransferRevision {
        return {leftEngine_->linearRebuilds(), leftEngine_->liveControlRevision(),
                leftEngine_->mixedAffineRevision(), rightEngine_->mixedAffineRevision()};
    };
    TransferRevision revision;
    bool planOnly = false;
    const bool fastLive = *apvts_.getRawParameterValue("solve_quality") > 0.5f;
    {
        std::unique_lock<std::mutex> lock(engineMutex_, std::try_to_lock);
        if (!lock.owns_lock() || !leftEngine_ || !rightEngine_ || inSourceIndex_ < 0)
            return;
        planOnly = !leftEngine_->hasAdjustedLiveTransfer();
        if (potsMoving_ || leftEngine_->hasAdjustedLiveTransfer() != rightEngine_->hasAdjustedLiveTransfer() ||
            (planOnly && (!fastLive || (leftEngine_->circuitPlanPrepared() && rightEngine_->circuitPlanPrepared())))) {
            observedTransferRevision_.reset();
            return;
        }
        revision = revisions();
        if (observedTransferRevision_ != revision) {
            observedTransferRevision_ = revision;
            return;
        }
        if (preparedTransferRevision_ == revision || affineWorkerBusy.test(std::memory_order_relaxed))
            return;
    }
    SettledTransfer job{engineGeneration_,
                        wdf::Engine::LiveTransferSetup(settledControlCircuit_),
                        wdf::Engine::LiveTransferSetup(settledControlCircuit_),
                        fastLive && !planOnly};
    size_t input = 0;
    int output = -1;
    {
        std::unique_lock<std::mutex> lock(engineMutex_, std::try_to_lock);
        if (!lock.owns_lock() || potsMoving_ ||
            revisions() != revision ||
            !leftEngine_->captureLiveTransferSetup(job.left, planOnly) ||
            !rightEngine_->captureLiveTransferSetup(job.right, planOnly))
            return;
        input = static_cast<size_t>(inSourceIndex_);
        output = outNodeIndex_;
    }
    if (affineWorkerBusy.test_and_set(std::memory_order_acquire))
        return;
    try {
        settledTransferJob_ = std::async(std::launch::async,
            [work = std::move(job), input, output, fastLive]() mutable {
                const AffineWorkerSlot slot;
                work.left.build(input, work.affine);
                work.right.build(input, work.affine);
                if (fastLive)
                    work.left.screenCircuitPlan(work.right, input, output);
                return std::move(work);
            });
        preparedTransferRevision_ = revision;
    } catch (const std::exception&) {
        affineWorkerBusy.clear(std::memory_order_release);
    }
}

void LiveWDFPluginProcessor::applySettledLiveTransfer(bool stereo) noexcept {
    if (!pendingSettledTransfer_.load(std::memory_order_relaxed))
        return;
    auto* ready = pendingSettledTransfer_.exchange(nullptr, std::memory_order_acquire);
    if (!ready)
        return;
    if (!potsMoving_ && rightEngine_ && ready->generation == engineGeneration_ &&
        leftEngine_->canInstallLiveTransferSetup(ready->left) &&
        rightEngine_->canInstallLiveTransferSetup(ready->right)) {
        leftEngine_->installLiveTransferSetup(ready->left);
        rightEngine_->installLiveTransferSetup(ready->right);
        ready->installedRevision = leftEngine_->linearRebuilds();
        ready->stereo = stereo;
    }
    // Publish the old transfer for destruction by the timer. The timer
    // cannot submit another job before taking this result back.
    retiredSettledTransfer_.store(ready, std::memory_order_release);
}

void LiveWDFPluginProcessor::prepareFastLiveAffine() {
    if (affineJob_.valid()) {
        if (affineJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try {
            readyAffine_ = affineJob_.get();
        } catch (const std::exception&) {
            return; // The ordinary Fast Live solver is already running.
        }
    }

    std::optional<AffineResult> completed;
    std::optional<wdf::Engine::MixedAffineSetup> leftSetup, rightSetup;
    AffineKey key;
    {
        std::unique_lock<std::mutex> lock(engineMutex_, std::try_to_lock);
        if (!lock.owns_lock() || !leftEngine_ || !rightEngine_ || inSourceIndex_ < 0)
            return;
        const bool stereo = *apvts_.getRawParameterValue("stereo") > 0.5f;
        if (potsMoving_ || leftEngine_->hasAdjustedLiveTransfer() ||
            (stereo && rightEngine_->hasAdjustedLiveTransfer())) {
            if (readyAffine_) {
                completed.swap(readyAffine_);
                requestedAffineKey_.reset();
            }
            return;
        }
        key = {engineGeneration_, leftEngine_->linearRebuilds(), stereo};
        if (readyAffine_) {
            completed.swap(readyAffine_);
            if (completed->key == key) {
                leftEngine_->installMixedAffineSetup(completed->left);
                if (completed->right)
                    rightEngine_->installMixedAffineSetup(*completed->right);
            }
        }
        // Prepare a newly loaded circuit even before Fast Live is selected.
        // Only recalibrate after live control changes while Fast Live is in use.
        if (*apvts_.getRawParameterValue("solve_quality") <= 0.5f &&
            requestedAffineKey_ && requestedAffineKey_->generation == key.generation)
            return;
        // Wait for the same transfer on two timer ticks.
        if (observedAffineKey_ != key) {
            observedAffineKey_ = key;
            return;
        }
        if (requestedAffineKey_ == key)
            return;
        if (affineWorkerBusy.test(std::memory_order_relaxed))
            return;
        const auto input = static_cast<size_t>(inSourceIndex_);
        leftSetup = leftEngine_->captureMixedAffineSetup(input);
        if (leftSetup && stereo)
            rightSetup = rightEngine_->captureMixedAffineSetup(input);
    }
    if (!leftSetup) {
        requestedAffineKey_ = key;
        return;
    }
    // Sessions can load several instances together. Give audio priority by
    // allowing one calibration at a time across plugin instances.
    if (affineWorkerBusy.test_and_set(std::memory_order_acquire))
        return;
    // The worker owns only snapshots. The timer checks the engine revision
    // again before it publishes the resulting plans.
    try {
        affineJob_ = std::async(std::launch::async,
            [work = AffineResult{key, std::move(*leftSetup), std::move(rightSetup)},
             document = cacheDocument_, file = currentFile_, grid = attemptedGridCurrent_]() mutable {
                const AffineWorkerSlot slot;
                const auto cacheFile = file.isAChildOf(FactorySchematics::folder()) ? juce::File{} : file;
                buildCachedAffineSetup(work.left, work.right ? &*work.right : nullptr,
                                       *document, cacheFile, grid);
                return std::move(work);
            });
        requestedAffineKey_ = key;
    } catch (const std::exception&) {
        affineWorkerBusy.clear(std::memory_order_release);
        // Leave the current solver active if the worker cannot start.
    }
}

void LiveWDFPluginProcessor::buildEngineFromDoc(const wdf::LiveWDFDocument& doc, double fs,
                                               std::unique_ptr<wdf::Engine>& left,
                                               std::unique_ptr<wdf::Engine>& right,
                                               int& inSrcIdx, int& outIdx,
                                               std::vector<double>& baseSources,
                                               std::vector<AcSource>& acSourcesOut,
                                               wdf::Circuit& settledControlCircuit) {
    // **A document can be rejected before there is an engine.** `toCircuit`
    // throws for a rail wired to ground, for example, and that is a drawing
    // error the user can fix -- not a reason to take the plugin down. The
    // engine construction below already reports its own failures; this catch
    // covers the conversion that happens first.
    wdf::Circuit c;
    try {
        c = doc.toCircuit();
    } catch (const std::exception& e) {
        left.reset();
        right.reset();
        inSrcIdx = -1;
        outIdx = -1;
        baseSources.clear();
        acSourcesOut.clear();
        lastError_ = describeRejection(doc, e.what());
        return;
    } catch (...) {
        left.reset();
        right.reset();
        inSrcIdx = -1;
        outIdx = -1;
        baseSources.clear();
        acSourcesOut.clear();
        lastError_ = "circuit rejected (unknown error)";
        return;
    }

    // Where the audio enters.
    //
    // The Input terminal is a part, and it converts to a source *named after
    // itself* -- so this is a lookup, not a guess. The guess is still here, as
    // the fallback for a document with no Input terminal: a source called
    // "Vin", or one with a non-zero AC amplitude, or the first source there is.
    // A document that has an Input terminal never reaches it.
    inSrcIdx = -1;

    const std::string inputId = doc.inputId();
    if (!inputId.empty()) {
        for (size_t i = 0; i < c.src.size(); ++i) {
            if (c.src[i].name == inputId) {
                inSrcIdx = static_cast<int>(i);
                break;
            }
        }
    }

    if (inSrcIdx == -1) {
        for (size_t i = 0; i < c.src.size(); ++i) {
            if (c.src[i].name == "Vin" || c.src[i].name == "in" || c.src[i].ac != 0.0) {
                inSrcIdx = static_cast<int>(i);
                break;
            }
        }
    }
    if (inSrcIdx == -1 && !c.src.empty()) {
        inSrcIdx = 0; // Default to first source
    }

    baseSources.resize(c.src.size());
    for (size_t i = 0; i < c.src.size(); ++i) {
        baseSources[i] = c.src[i].dc;
    }

    // The AC sources: those with an amplitude *and* a frequency. The plugin
    // generates their sine itself, because a host's audio is not a sine wave
    // and the engine drives sources with values the caller supplies.
    //
    // The settings come from the *document*, in the order `toCircuit` creates
    // the sources: the frequency is the signal's, not the network's, and the
    // compiled circuit stays a topology description (see `format/AcSource.h`).
    acSourcesOut.clear();
    {
        const auto settings = doc.sourceAcSettings();
        for (size_t i = 0; i < settings.size(); ++i) {
            if (settings[i].ac == 0.0 || settings[i].freq == 0.0)
                continue;
            acSourcesOut.push_back({static_cast<int>(i), settings[i].ac, settings[i].freq});
        }
    }

    // Build twin engines for stereo
    try {
        // Construction already resets both engines at their DC operating point.
        left = std::make_unique<wdf::Engine>(c, fs, attemptedGridCurrent_);
        right = std::make_unique<wdf::Engine>(c, fs, attemptedGridCurrent_);

        // Silent pre-roll: settle dynamic states (op-amp GBW, reactive waves,
        // nonlinear predictors) at the DC operating point to eliminate
        // sample-0 transient clicks upon audio startup.
        for (int p = 0; p < 64; ++p) {
            left->step(baseSources);
            right->step(baseSources);
        }

        // Where the audio leaves: the Output terminal's pin, or the document's
        // named output node when the sheet has no Output terminal.
        outIdx = left->nodeIndex(doc.outputNet());
        settledControlCircuit = std::move(c);
    } catch (const std::exception& e) {
        left.reset();
        right.reset();
        outIdx = -1;

        // **Say what is wrong, in the user's words.** The engine's message is
        // true and useless ("singular linear network"); the drawing's own
        // answer is "a part is joined to nothing", and that is a thing the
        // user can go and fix.
        lastError_ = describeRejection(doc, e.what());
    } catch (...) {
        left.reset();
        right.reset();
        outIdx = -1;
        lastError_ = "circuit rejected (unknown error)";
    }
}

bool LiveWDFPluginProcessor::loadCircuitDocument(const wdf::LiveWDFDocument& doc) {
    const bool loaded = loadCircuitDocument(doc, true);
    if (loaded)
        updateReportedLatency();
    return loaded;
}

bool LiveWDFPluginProcessor::loadCircuitDocument(const wdf::LiveWDFDocument& doc,
                                                 bool initializePotParameters,
                                                 std::unique_ptr<PreviewPlayback> preview) {
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    lastError_.clear();
    liveControlUpdateFailed_.store(false, std::memory_order_relaxed);

    // A document loaded from anywhere but a file has no file to reload. The
    // callers that *do* have a file set `currentFile_` again afterwards, so
    // this clearing is what makes the Reload button go dead for a circuit
    // restored from host state.
    currentFile_ = juce::File();

    attemptedOversamplingChoice_ = effectiveOversamplingChoice();
    attemptedGridCurrent_ = *apvts_.getRawParameterValue("grid_current") > 0.5f;
    constexpr int kFactors[] = {1, 2, 4, 8, 2, 4};
    const int factor = kFactors[std::clamp(attemptedOversamplingChoice_, 0, 5)];
    const bool halfBand = attemptedOversamplingChoice_ >= 4;
    const double effectiveFs = sampleRate_ * static_cast<double>(factor);

    std::unique_ptr<wdf::Engine> newLeft, newRight;
    int inSrcIdx = -1;
    int outIdx = -1;
    std::vector<double> baseSources;
    std::vector<AcSource> acSources;

    wdf::Circuit newSettledControlCircuit;
    buildEngineFromDoc(doc, effectiveFs, newLeft, newRight, inSrcIdx, outIdx, baseSources,
                       acSources, newSettledControlCircuit);

    if (!newLeft || !newRight) {
        return false;
    }
    cacheDocument_ = std::make_shared<const std::string>(doc.toJson());
    settledControlCircuit_ = std::move(newSettledControlCircuit);
    observedTransferRevision_.reset();
    preparedTransferRevision_.reset();

    // **A meter is a thing on the sheet**: `allProbes()` asks the drawing for
    // its scope parts, keyed by the net each one watches, which is how the
    // schematic canvas wants its voltages.
    std::vector<std::pair<std::string, int>> probeNodes;
    for (const auto& probe : doc.allProbes()) {
        const int idx = newLeft->nodeIndex(probe.second);
        if (idx >= 0)
            probeNodes.emplace_back(probe.second, idx);
    }

    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        leftEngine_ = std::move(newLeft);
        rightEngine_ = std::move(newRight);
        ++engineGeneration_;
        preview_ = std::move(preview);
        previewRequest_ = preview_ ? engineGeneration_ : 0;
        previewPlaying_ = false;
        previewStereo_ = false;
        transitionRemaining_ = 0;
        transitionLength_ = std::max(1, int(sampleRate_ * 0.003));
        previewStatus_.store(preview_ ? PreviewStatus::armed : PreviewStatus::off);
        snapshotControlsAgree_ = true;
        currentDoc_ = doc;
        inSourceIndex_ = inSrcIdx;
        outNodeIndex_ = outIdx;
        outputFullScaleVolts_ = doc.outputFullScaleVolts();
        baseSourceValues_ = std::move(baseSources);
        svLeft_.assign(baseSourceValues_.size(), 0.0);
        svRight_.assign(baseSourceValues_.size(), 0.0);
        acSources_ = std::move(acSources);
        acSampleRate_ = effectiveFs;
        acSampleCounter_ = 0;
        {
            std::lock_guard<std::mutex> probeLock(probeMutex_);
            probeVoltages_.assign(probeNodes.size(), 0.0);
            probeNodes_ = std::move(probeNodes);
        }
        currentOversamplingFactor_ = factor;
        useHalfBand_ = halfBand;
        int latency = 0;
        if (halfBand) {
            const auto f = factor == 4 ? wdf::OversamplingFactor::X4 : wdf::OversamplingFactor::X2;
            halfBandOversamplerL_.setFactor(f);
            halfBandOversamplerR_.setFactor(f);
            halfBandOversamplerL_.reset();
            halfBandOversamplerR_.reset();
            if (oversamplersPrepared_) {
                static const float latency2x = halfBandLatency(wdf::OversamplingFactor::X2);
                static const float latency4x = halfBandLatency(wdf::OversamplingFactor::X4);
                const float nominal = factor == 4 ? latency4x : latency2x;
                latency = static_cast<int>(std::ceil(nominal));
                // Keep the first-order all-pass away from a near-zero delay.
                if (latency - nominal < 0.618f)
                    ++latency;
                halfBandPadding_.reset();
                halfBandPadding_.setDelay(latency - nominal);
            }
        } else if (auto* oversampler = activePolyphaseOversampler(); oversamplersPrepared_ && oversampler) {
            oversampler->reset();
            latency = juce::roundToInt(oversampler->getLatencyInSamples());
        }
        processingLatencySamples_.store(latency, std::memory_order_relaxed);

        // **One control per label.** The editor writes a grouped value to the
        // whole group; the plugin builds a group from the same document and
        // drives every member with the one knob or switch. An unlabelled part
        // is its own group, named by its id.
        // The UI reads these while the audio thread is running. Publish the new
        // control state under `controlMutex_`, which the audio thread never
        // takes; the engine lock above already serialises the audio thread
        // against this write.
        {
            std::lock_guard<std::mutex> controlLock(controlMutex_);

            potGroupsInCircuit_ = wdf::potControlGroups(doc);
            switchGroupsInCircuit_ = wdf::switchControlGroups(doc);

            // The switch's starting throw is the one in the file. If a
            // hand-edited file has members of one group disagreeing, the first
            // member is the group's state.
            switchGroupStates_.assign(switchGroupsInCircuit_.size(), false);
            for (size_t i = 0; i < switchGroupsInCircuit_.size(); ++i) {
                for (const auto& member : switchGroupsInCircuit_[i].members) {
                    const auto comp = std::find_if(doc.components.begin(), doc.components.end(),
                                                   [&](const wdf::SchematicComponent& c) {
                                                       return c.id == member.id;
                                                   });
                    if (comp != doc.components.end()) {
                        switchGroupStates_[i] = wdf::switchResSelectsB(*comp);
                        break;
                    }
                }
            }
        }

        potTargets_.clear();
        potControlCount_ = std::min(kMaxPotKnobs, static_cast<int>(potGroupsInCircuit_.size()));
        for (int i = 0; i < potControlCount_; ++i) {
            potControls_[static_cast<size_t>(i)].knob =
                apvts_.getRawParameterValue("pot_" + juce::String(i));
            for (const auto& member : potGroupsInCircuit_[static_cast<size_t>(i)].members) {
                const bool varRes = member.type == "varres";
                const int index = varRes ? leftEngine_->varResIndexByName(member.id)
                                         : leftEngine_->potIndexByName(member.id);
                if (index >= 0)
                    potTargets_.push_back({i, index, varRes});
            }
        }
        switchTargets_.clear();
        for (size_t i = 0; i < switchGroupsInCircuit_.size(); ++i) {
            for (const auto& member : switchGroupsInCircuit_[i].members) {
                const int resA = leftEngine_->resIndexByName(member.id + "_A");
                const int resB = leftEngine_->resIndexByName(member.id + "_B");
                if (resA >= 0 && resB >= 0)
                    switchTargets_.push_back({i, resA, resB});
            }
        }
        potMoves_.reserve(potTargets_.size());
        varResMoves_.reserve(potTargets_.size());
        resistorMoves_.reserve(2 * switchTargets_.size());

        // A new document brings its own pot positions. This is a document load,
        // not a sample-rate rebuild, so the saved `pos` becomes the knob value;
        // an oversampling change calls the private overload with `false` and
        // leaves the user's knob where it is.
        syncPotParametersFromDocument(doc, initializePotParameters);

        // Start at the saved knob positions. Measure ramp progress in host
        // samples so oversampling does not change the feel of the controls.
        samplesUntilPotUpdate_ = 0;
        potsMoving_ = false;
        for (int i = 0; i < potControlCount_; ++i) {
            auto& control = potControls_[static_cast<size_t>(i)];
            const float value = control.knob->load(std::memory_order_relaxed);
            control.position.reset(sampleRate_, .015);
            control.position.setCurrentAndTargetValue(std::isfinite(value) ? value : 0.5);
        }
        applyLiveControlsToEngines(true, true);
        leftEngine_->prepareLiveControls();
        rightEngine_->prepareLiveControls();
    }

    return true;
}

void LiveWDFPluginProcessor::syncPotParametersFromDocument(const wdf::LiveWDFDocument& doc, bool notify) {
    const int potLimit = std::min(kMaxPotKnobs, static_cast<int>(potGroupsInCircuit_.size()));
    for (int i = 0; i < potLimit; ++i) {
        double pos = 0.5;
        bool found = false;
        for (const auto& member : potGroupsInCircuit_[static_cast<size_t>(i)].members) {
            const auto comp = std::find_if(doc.components.begin(), doc.components.end(),
                                           [&](const wdf::SchematicComponent& c) {
                                               return c.id == member.id;
                                           });
            if (comp == doc.components.end())
                continue;

            const auto it = comp->values.find("pos");
            if (it != comp->values.end()) {
                const double value = std::clamp(it->second, 0.0, 1.0);
                if (found && pos != value) snapshotControlsAgree_ = false;
                if (!found) pos = value;
                found = true;
            }
        }

        previewPots_[static_cast<size_t>(i)] = pos;
        if (!notify) continue;
        const juce::String potId = "pot_" + juce::String(i);
        if (auto* param = apvts_.getParameter(potId))
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(pos)));
    }
}

bool LiveWDFPluginProcessor::loadCircuitFromFile(const juce::File& file) {
    const std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    if (!file.existsAsFile()) return false;

    wdf::LiveWDFDocument doc;
    std::string err;
    if (doc.loadFromFile(file.getFullPathName().toStdString(), err)) {
        if (loadCircuitDocument(doc)) {
            // Remember the file, so the plugin can be told to re-read it after
            // the editor has saved a new version. This is the whole point of
            // the reload button: the plugin is holding a copy from the moment
            // it was loaded, and the editor is where the circuit is drawn.
            currentFile_ = file;
            return true;
        }
        // `loadCircuitDocument` set its own reason; leave it standing.
    } else {
        // **The reader's own reason**, not the engine's. A file the format
        // refuses -- a legacy sheet that still carries net names beside pins
        // -- never reaches the engine at all, so without this the status line
        // would go blank and the plugin would be a mystery again.
        lastError_ = err.empty() ? std::string("the file could not be read") : err;
    }
    return false;
}

bool LiveWDFPluginProcessor::reloadCircuit() {
    const std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    if (!canReload())
        return false;

    // Keep the path: `loadCircuitDocument` clears it, because a document might
    // have come from anywhere, and this one came from *here*.
    const auto file = currentFile_;

    // Re-read the file from disk and rebuild the engine from what is in it now.
    //
    // A failed reload leaves the current circuit playing: taking the sound away
    // because the file was half-saved would be a worse failure than playing the
    // old version for a moment.
    wdf::LiveWDFDocument doc;
    std::string err;
    if (!doc.loadFromFile(file.getFullPathName().toStdString(), err)) {
        // **Say why the reload went dead.** The common reason now is a sheet
        // the format refuses outright, and the reason is in the reader's words
        // -- which is the one thing the user needs in order to fix the file.
        lastError_ = err.empty() ? std::string("the file could not be read") : err;
        return false;
    }

    if (!loadCircuitDocument(doc))
        return false;

    // `loadCircuitDocument` clears the file (a document might have come from
    // anywhere), so put it back: this *is* the file.
    currentFile_ = file;

    // The pot knobs are rebuilt by the editor, which owns them; the processor
    // only has to say the circuit changed.
    return true;
}

std::vector<wdf::LiveControlGroup> LiveWDFPluginProcessor::getPotGroups() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return potGroupsInCircuit_;
}

std::vector<wdf::LiveControlGroup> LiveWDFPluginProcessor::getSwitchGroups() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return switchGroupsInCircuit_;
}

bool LiveWDFPluginProcessor::getSwitchGroupB(int index) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return index >= 0 && index < static_cast<int>(switchGroupStates_.size())
           && switchGroupStates_[static_cast<size_t>(index)];
}

std::map<std::string, double> LiveWDFPluginProcessor::getLivePotPositions() const {
    std::lock_guard<std::mutex> lock(controlMutex_);

    std::map<std::string, double> positions;
    const int potLimit = std::min(kMaxPotKnobs, static_cast<int>(potGroupsInCircuit_.size()));

    for (int i = 0; i < potLimit; ++i) {
        double value = 0.5;
        if (const auto* raw = apvts_.getRawParameterValue("pot_" + juce::String(i)))
            value = static_cast<double>(raw->load());

        for (const auto& member : potGroupsInCircuit_[static_cast<size_t>(i)].members)
            positions[member.id] = value;
    }

    return positions;
}

std::map<std::string, bool> LiveWDFPluginProcessor::getLiveSwitchStates() const {
    std::lock_guard<std::mutex> lock(controlMutex_);

    std::map<std::string, bool> states;
    for (size_t i = 0; i < switchGroupsInCircuit_.size() && i < switchGroupStates_.size(); ++i) {
        for (const auto& member : switchGroupsInCircuit_[i].members)
            states[member.id] = switchGroupStates_[i];
    }

    return states;
}

void LiveWDFPluginProcessor::setSwitchGroupB(int index, bool throwB) {
    const std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    std::lock_guard<std::mutex> lock(engineMutex_);

    if (index < 0 || index >= static_cast<int>(switchGroupsInCircuit_.size())
        || index >= static_cast<int>(switchGroupStates_.size()))
        return;

    if (switchGroupStates_[static_cast<size_t>(index)] == throwB) return;
    // A switch changes the document digest, including while a worker is reading it.
    previewJobGeneration_ = 0;
    previewRequest_ = engineGeneration_;
    snapshotControlsAgree_ = false;
    if (*apvts_.getRawParameterValue("model_preview") > 0.5f) previewStatus_.store(PreviewStatus::controls);
    // The editor reads the throws through `controlMutex_`; the audio thread
    // reads them under the engine lock, which we already hold. Lock order is
    // engine -> control everywhere, so the two cannot deadlock.
    {
        std::lock_guard<std::mutex> controlLock(controlMutex_);
        switchGroupStates_[static_cast<size_t>(index)] = throwB;
    }

    // Keep the document in step, so the plugin's saved state carries the throw
    // the user is looking at. The group is the label, so every member moves.
    const std::string select = throwB ? "1" : "0";
    for (const auto& member : switchGroupsInCircuit_[static_cast<size_t>(index)].members) {
        for (auto& comp : currentDoc_.components) {
            if (comp.id == member.id)
                comp.properties["select"] = select;
        }
    }

    applyLiveControlsToEngines(false, true);
}

void LiveWDFPluginProcessor::readLivePotTargets() {
    for (int i = 0; i < potControlCount_; ++i) {
        auto& control = potControls_[static_cast<size_t>(i)];
        const double value = control.knob->load(std::memory_order_relaxed);
        if (std::isfinite(value))
            control.position.setTargetValue(value);
        potsMoving_ |= control.position.isSmoothing();
    }
}

void LiveWDFPluginProcessor::advanceLivePots() {
    if (!potsMoving_)
        return;

    // Use a host-sample interval so oversampling and block boundaries have
    // no effect on the control curve. Batch ganged and simultaneous changes.
    constexpr int interval = 8;
    potsMoving_ = false;
    for (int i = 0; i < potControlCount_; ++i) {
        auto& position = potControls_[static_cast<size_t>(i)].position;
        position.getNextValue();
        potsMoving_ |= position.isSmoothing();
    }
    if (potsMoving_ && samplesUntilPotUpdate_ > 0) {
        --samplesUntilPotUpdate_;
        return;
    }

    samplesUntilPotUpdate_ = interval - 1;
    applyLiveControlsToEngines(true, false);
    if (!potsMoving_)
        samplesUntilPotUpdate_ = 0;
}

void LiveWDFPluginProcessor::applyLiveControlsToEngines(bool pots, bool switches) {
    if (!leftEngine_) return;

    // Every pot and switch member goes to the engine in one batch, so a moved
    // ganged group re-derives the linear transfer once, not once per member.
    potMoves_.clear();
    varResMoves_.clear();
    resistorMoves_.clear();
    if (pots) {
        for (const auto& t : potTargets_) {
            const auto& control = potControls_[static_cast<size_t>(t.group)];
            (t.varRes ? varResMoves_ : potMoves_).emplace_back(t.index, control.position.getCurrentValue());
        }
    }
    if (switches) {
        for (const auto& t : switchTargets_) {
            const bool throwB = switchGroupStates_[t.group];
            resistorMoves_.emplace_back(t.resA, throwB ? wdf::kSpdtOpenOhms : wdf::kSpdtClosedOhms);
            resistorMoves_.emplace_back(t.resB, throwB ? wdf::kSpdtClosedOhms : wdf::kSpdtOpenOhms);
        }
    }

    const auto apply = [&](wdf::Engine& engine) {
        if (switches)
            engine.setParametersByIndex(potMoves_, varResMoves_, resistorMoves_);
        else if (!engine.setLiveControls(potMoves_, varResMoves_))
            liveControlUpdateFailed_.store(true, std::memory_order_relaxed);
    };
    apply(*leftEngine_);
    if (rightEngine_)
        apply(*rightEngine_);
}

// ==============================================================================
// State Persistence
// ==============================================================================
void LiveWDFPluginProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts_.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());

    // Save active LiveWDF JSON document
    xml->setAttribute("livewdf_json", juce::String(currentDoc_.toJson()));

    copyXmlToBinary(*xml, destData);
}

void LiveWDFPluginProcessor::setStateInformation(const void* data, int sizeInBytes) {
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName(apvts_.state.getType())) {
        std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
        apvts_.replaceState(juce::ValueTree::fromXml(*xmlState));

        // Restore custom LiveWDF JSON document
        if (xmlState->hasAttribute("livewdf_json")) {
            const std::string jsonStr = xmlState->getStringAttribute("livewdf_json").toStdString();
            wdf::LiveWDFDocument doc;
            std::string err;
            if (wdf::LiveWDFDocument::fromJson(jsonStr, doc, err)) {
                // The APVTS state was restored above; that is the user's knob
                // state. Do not overwrite it with the document's saved `pos`.
                loadCircuitDocument(doc, false);
            }
        }
        updateReportedLatency();
    }
}

juce::AudioProcessorEditor* LiveWDFPluginProcessor::createEditor() {
    return new LiveWDFPluginEditor(*this);
}

} // namespace livewdf

// ==============================================================================
// JUCE Plugin Entry Point
// ==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new livewdf::LiveWDFPluginProcessor();
}
