#include "PluginProcessor.h"
#include "spice/MatchedPentodes.h"
#include "PluginEditor.h"
#include "FactorySchematics.h"
#include "FastLiveCache.h"

#include "format/AcSource.h"
#include "format/MainsSupply.h"

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

void nameLegacyAmp(wdf::LiveWDFDocument& doc, const juce::File& file) {
    if (!doc.name.empty() && doc.name != "Untitled Circuit")
        return;
    const auto title = file.getFileNameWithoutExtension().toStdString();
    if (OutputEffects::suggestedCabinet(title).isNotEmpty())
        doc.name = title;
}

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
        juce::NormalisableRange<float>(-48.0f, 18.0f, 0.1f), 3.0f,
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
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"two_cores", 1}, "Two cores", false));

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
        juce::ParameterID{"compiled_preview", 1}, "Compiled Preview", true));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"chain_input", 1}, "Input from physical chain", false,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"match_output", 1}, "Match output level", true,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));

    for (size_t i = 0; i < numPorts; ++i)
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{portParameterIds[i], 1}, i == 0 ? "Source impedance" : "Output load",
            juce::NormalisableRange<float>(0.f, 1.f, static_cast<float>(wdf::PortImpedanceRange::step)), 0.f));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"preview_pad", 1}, "Preview pad", false));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"cab_enabled", 1}, "Cabinet IR", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"echo_mix", 1}, "Echo Level",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.5f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverb_mix", 1}, "Reverb Level",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.5f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"power_amp", 2}, "Power Amp", true));

    const auto addOutputControl = [&](const char* id, const char* label, float min, float max,
                                      float step, float start, const char* units) {
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id, 3}, label, juce::NormalisableRange<float>(min, max, step), start,
            juce::AudioParameterFloatAttributes().withLabel(units)));
    };
    addOutputControl("echo_delay", "Echo Delay", 10.0f, OutputEffects::maxEchoDelayMs, 1.0f, 250.0f, "ms");
    addOutputControl("echo_feedback", "Echo Feedback", 0.0f, 95.0f, 0.5f, 42.0f, "%");
    addOutputControl("reverb_size", "Reverb Size", 50.0f, 150.0f, 1.0f, 100.0f, "%");
    addOutputControl("reverb_predelay", "Reverb Pre-delay", 0.0f, OutputEffects::maxPreDelayMs, 1.0f, 0.0f, "ms");

    return { params.begin(), params.end() };
}

// ==============================================================================
// Processor Construction & Audio Callbacks
// ==============================================================================
LiveWDFPluginProcessor::LiveWDFPluginProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "Parameters", createParameterLayout()) {
    for (size_t i = 0; i < numPorts; ++i)
        ports_[i].parameter = apvts_.getRawParameterValue(portParameterIds[i]);
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
    outputEffects_.prepare({sampleRate, static_cast<juce::uint32>(std::max(1, samplesPerBlock)),
                            static_cast<juce::uint32>(std::max(1, getMainBusNumOutputChannels()))},
                           reverbSize_->load(std::memory_order_relaxed) * 0.01f);

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
    outputEffects_.release();
    std::lock_guard<std::mutex> lock(engineMutex_);
    oversampler2x_->reset();
    oversampler4x_->reset();
    oversampler8x_->reset();
    halfBandOversamplerL_.reset();
    halfBandOversamplerR_.reset();
    halfBandPadding_.reset();
    dryDelay_.reset();
}

double LiveWDFPluginProcessor::getTailLengthSeconds() const {
    return outputEffects_.tailSeconds(outputEffectSettings());
}

bool LiveWDFPluginProcessor::loadOutputImpulse(bool reverb, const juce::File& file) {
    const std::lock_guard lock(loadMutex_);
    if (!outputEffects_.loadImpulse(reverb, file))
        return false;
    if (!reverb)
        cabinetFollowsSuggestion_ = false;
    return true;
}

bool LiveWDFPluginProcessor::selectFactoryOutputImpulse(bool reverb, juce::StringRef id) {
    const std::lock_guard lock(loadMutex_);
    if (!outputEffects_.selectFactoryImpulse(reverb, id))
        return false;
    if (!reverb)
        cabinetFollowsSuggestion_ = false;
    return true;
}

void LiveWDFPluginProcessor::resetOutputImpulse(bool reverb) {
    const std::lock_guard lock(loadMutex_);
    outputEffects_.resetImpulse(reverb);
    if (!reverb)
        cabinetFollowsSuggestion_ = false;
}

bool LiveWDFPluginProcessor::selectSuggestedCabinet() {
    const std::lock_guard lock(loadMutex_);
    const auto id = OutputEffects::suggestedCabinet(currentDoc_.name);
    if (id.isEmpty() || (id != outputEffects_.impulseId(false) && !outputEffects_.selectFactoryImpulse(false, id)))
        return false;
    cabinetFollowsSuggestion_ = true;
    return true;
}

bool LiveWDFPluginProcessor::cabinetFollowsSuggestion() const {
    const std::lock_guard lock(loadMutex_);
    return cabinetFollowsSuggestion_;
}

juce::String LiveWDFPluginProcessor::suggestedCabinetId() const {
    const std::lock_guard lock(loadMutex_);
    return OutputEffects::suggestedCabinet(currentDoc_.name);
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

bool LiveWDFPluginProcessor::compiledPreviewRequested() const {
    return !shouldForceAccurate() && *apvts_.getRawParameterValue("solve_quality") > 0.5f
           && *apvts_.getRawParameterValue("compiled_preview") > 0.5f;
}

// The drawn mains supply with its ripple only in Accurate. Fast uses its sagging DC equivalent; Preview holds every
// supply node at its idle voltage, because the filter chain otherwise couples the stages and costs more than fixed rails.
LiveWDFPluginProcessor::MainsSupply LiveWDFPluginProcessor::mainsSupplyRequested() const {
    if (shouldForceAccurate() || *apvts_.getRawParameterValue("solve_quality") <= 0.5f)
        return MainsSupply::drawn;
    return compiledPreviewRequested() ? MainsSupply::stiff : MainsSupply::sagging;
}

int LiveWDFPluginProcessor::effectiveOversamplingChoice() const {
    constexpr int polyphase4x = 2;
    return isNonRealtime() && *apvts_.getRawParameterValue("export_4x") > 0.5f
        ? polyphase4x : static_cast<int>(*apvts_.getRawParameterValue("oversampling"));
}

// Changes to grid current or sample rate rebuild the engines.
void LiveWDFPluginProcessor::timerCallback() {
    outputEffects_.reclaim();
    if (*apvts_.getRawParameterValue("stereo") > 0.5f || *apvts_.getRawParameterValue("two_cores") > 0.5f)
        rightWorker_.start();
    if (!isNonRealtime())
        outputEffects_.applyReverbSize(reverbSize_->load(std::memory_order_relaxed) * 0.01f, false);
    const std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    if (liveControlUpdateFailed_.exchange(false, std::memory_order_relaxed))
        lastError_ = "Could not apply a live control setting";
    applyPendingCircuitOptions();
    if (!isNonRealtime() && !currentDoc_.components.empty()) {
        const auto audioRevision = audioTransferRevision_.exchange(noAudioSinceTick, std::memory_order_relaxed);
        prepareSettledLiveTransfer(audioRevision != noAudioSinceTick);
        if (!settledTransferJob_.valid() && !settledTransferPublished_ &&
            captureState_.load(std::memory_order_acquire) == CaptureState::idle &&
            audioRevision != adjustedAudioTransfer)
            prepareFastLiveAffine(audioRevision);
    }
    updateReportedLatency();
}

void LiveWDFPluginProcessor::applyPendingCircuitOptions() {
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    if (currentDoc_.components.empty())
        return;

    std::array<float, numPorts> requestedPorts{};
    bool portTopologyChanged = false;
    for (size_t i = 0; i < numPorts; ++i) {
        const auto& port = ports_[i];
        requestedPorts[i] = port.range ? port.parameter->load(std::memory_order_relaxed) : 0.f;
        portTopologyChanged |= port.enabled != (port.range && port.range->needsFixture(requestedPorts[i], port.enabled));
    }
    if (!portTopologyChanged) rejectedPortRequest_.reset();
    auto* powerAmp = apvts_.getParameter("power_amp");
    const bool requestedPowerAmp = powerAmp->getValue() > 0.5f;
    const bool powerAmpChanged = powerAmpEnabled_ && requestedPowerAmp != *powerAmpEnabled_;
    const bool mainsHumChanged = mainsSupply_ && mainsSupplyRequested() != *mainsSupply_;
    if ((portTopologyChanged && rejectedPortRequest_ != requestedPorts) ||
        effectiveOversamplingChoice() != attemptedOversamplingChoice_ ||
        (*apvts_.getRawParameterValue("grid_current") > 0.5f) != attemptedGridCurrent_ ||
        powerAmpChanged || mainsHumChanged || gatedSwitchChanged_.exchange(false)) {
        const juce::File circuitFile = currentFile_;
        if (!loadCircuitDocument(currentDoc_, false)) {
            if (portTopologyChanged)
                rejectedPortRequest_ = requestedPorts;
            if (powerAmpChanged && (powerAmp->getValue() > 0.5f) == requestedPowerAmp)
                powerAmp->setValueNotifyingHost(*powerAmpEnabled_ ? 1.f : 0.f);
        }
        currentFile_ = circuitFile;
    }
    // A headless export can start before the UI timer has prepared a live plan.
    // Only offline processing may perform setup here.
    if (isNonRealtime() && compiledPreviewRequested()) {
        const std::lock_guard lock(engineMutex_);
        if (leftEngine_ && rightEngine_ && inSourceIndex_ >= 0 && outNodeIndex_ >= 0 &&
            !potsMoving_ && !leftEngine_->hasAdjustedLiveTransfer() && (!previewPrepared_ ||
                previewLinearRevision_ != leftEngine_->linearRebuilds() ||
                previewControlRevision_ != leftEngine_->liveControlRevision())) {
            const double peak = currentDoc_.previewPeakVolts();
            for (auto* engine : {leftEngine_.get(), rightEngine_.get()})
                engine->prepareCompiledPreview(static_cast<size_t>(inSourceIndex_), outNodeIndex_,
                                               outputOnlyPreviewAllowed_, peak, currentDoc_.allowPreviewInputPad);
            previewPrepared_ = true;
            previewLinearRevision_ = leftEngine_->linearRebuilds();
            previewControlRevision_ = leftEngine_->liveControlRevision();
        }
    }
}

void LiveWDFPluginProcessor::prepareSettledLiveTransfer(bool audioActive) {
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
    // The timer can inspect an idle engine immediately. While playing a heavy
    // circuit, service the same request at a block boundary under the audio
    // thread's existing lock. The control thread owns all snapshot storage.
    if (captureState_.load(std::memory_order_acquire) == CaptureState::idle) {
        if (affineWorkerBusy.test(std::memory_order_relaxed))
            return;
        capture_.fastLive = *apvts_.getRawParameterValue("solve_quality") > 0.5f;
        capture_.wantPreview = compiledPreviewRequested();
        capture_.revision.reset();
        captureState_.store(CaptureState::requested, std::memory_order_release);
    }
    if (captureState_.load(std::memory_order_acquire) == CaptureState::needsStorage) {
        capture_.work.emplace(engineGeneration_, settledControlCircuit_);
        captureState_.store(CaptureState::requested, std::memory_order_release);
    }
    if (!audioActive && captureState_.load(std::memory_order_acquire) == CaptureState::requested) {
        std::unique_lock lock(engineMutex_, std::try_to_lock);
        if (lock.owns_lock())
            captureSettledTransferIfRequested();
    }
    if (captureState_.load(std::memory_order_acquire) != CaptureState::done)
        return;
    if (!capture_.revision) {
        capture_.work.reset();
        captureState_.store(CaptureState::idle, std::memory_order_release);
        return;
    }
    if (affineWorkerBusy.test_and_set(std::memory_order_acquire))
        return;
    const auto revision = *capture_.revision;
    const auto fastLive = capture_.fastLive;
    const auto input = static_cast<size_t>(inSourceIndex_);
    const auto output = outNodeIndex_;
    const bool allowOutputOnly = outputOnlyPreviewAllowed_, allowPad = currentDoc_.allowPreviewInputPad;
    const auto peak = currentDoc_.previewPeakVolts();
    auto job = std::move(*capture_.work);
    capture_.work.reset();
    captureState_.store(CaptureState::idle, std::memory_order_release);
    try {
        settledTransferJob_ = std::async(std::launch::async,
            [work = std::move(job), input, output, fastLive, lag = fastLive && !capture_.wantPreview,
             allowOutputOnly, peak, allowPad]() mutable {
                const AffineWorkerSlot slot;
                work.left.build(input, work.affine, lag);
                work.right.build(input, work.affine, lag);
                if (fastLive)
                    work.left.screenCircuitPlan(work.right, input, output);
                if (work.compilePreview)
                    work.left.prepareCompiledPreviewPair(work.right, input, output, allowOutputOnly, peak, allowPad);
                return std::move(work);
            });
        preparedTransferRevision_ = revision;
        initialTransferPending_ = false;
    } catch (const std::exception&) {
        affineWorkerBusy.clear(std::memory_order_release);
    }
}

void LiveWDFPluginProcessor::captureSettledTransferIfRequested() noexcept {
    if (captureState_.load(std::memory_order_acquire) != CaptureState::requested)
        return;

    if (leftEngine_ && rightEngine_ && inSourceIndex_ >= 0) {
        const bool planOnly = !leftEngine_->hasAdjustedLiveTransfer();
        const bool needsPreview = capture_.wantPreview && (!previewPrepared_ ||
            previewLinearRevision_ != leftEngine_->linearRebuilds() ||
            previewControlRevision_ != leftEngine_->liveControlRevision());
        const bool needsPlan = capture_.fastLive &&
            !(leftEngine_->circuitPlanPrepared() && rightEngine_->circuitPlanPrepared());
        if (potsMoving_ || leftEngine_->hasAdjustedLiveTransfer() != rightEngine_->hasAdjustedLiveTransfer() ||
            (planOnly && !needsPreview && !needsPlan)) {
            observedTransferRevision_.reset();
        } else {
            const TransferRevision revision{leftEngine_->linearRebuilds(), leftEngine_->liveControlRevision(),
                leftEngine_->mixedAffineRevision(), rightEngine_->mixedAffineRevision(), needsPreview};
            if ((initialTransferPending_ || observedTransferRevision_ == revision) &&
                preparedTransferRevision_ != revision) {
                if (!capture_.work) {
                    captureState_.store(CaptureState::needsStorage, std::memory_order_release);
                    return;
                }
                auto& job = *capture_.work;
                if (leftEngine_->captureLiveTransferSetup(job.left, planOnly) &&
                    rightEngine_->captureLiveTransferSetup(job.right, planOnly)) {
                    job.affine = capture_.fastLive && !planOnly;
                    job.compilePreview = needsPreview;
                    capture_.revision = revision;
                } else {
                    previewPrepared_ = true; // No worker can prepare it, so Preview stops waiting.
                }
            }
            observedTransferRevision_ = revision;
        }
    }
    captureState_.store(CaptureState::done, std::memory_order_release);
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
        if (ready->compilePreview) {
            previewPrepared_ = true;
            previewLinearRevision_ = leftEngine_->linearRebuilds();
            previewControlRevision_ = leftEngine_->liveControlRevision();
        }
        ready->installedRevision = leftEngine_->linearRebuilds();
        ready->stereo = stereo;
    }
    // Publish the old transfer for destruction by the timer. The timer
    // cannot submit another job before taking this result back.
    retiredSettledTransfer_.store(ready, std::memory_order_release);
}

void LiveWDFPluginProcessor::prepareFastLiveAffine(long long audioRevision) {
    if (affineJob_.valid()) {
        if (affineJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try {
            readyAffine_ = affineJob_.get();
        } catch (const std::exception&) {
            return; // The ordinary Fast Live solver is already running.
        }
    }

    const bool stereo = *apvts_.getRawParameterValue("stereo") > 0.5f;
    if (!readyAffine_ && audioRevision >= 0 &&
        requestedAffineKey_ == AffineKey{engineGeneration_, audioRevision, stereo})
        return;

    std::optional<AffineResult> completed;
    std::optional<wdf::Engine::MixedAffineSetup> leftSetup, rightSetup;
    AffineKey key;
    {
        std::unique_lock<std::mutex> lock(engineMutex_, std::try_to_lock);
        if (!lock.owns_lock() || !leftEngine_ || !rightEngine_ || inSourceIndex_ < 0)
            return;
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
                                               const std::array<double, numPorts>& portPositions,
                                               std::array<std::string, numPorts>& fixtureNames,
                                               std::unique_ptr<wdf::Engine>& left,
                                               std::unique_ptr<wdf::Engine>& right,
                                               int& inSrcIdx, int& outIdx, double& outputBias,
                                               std::vector<double>& baseSources,
                                               std::vector<AcSource>& acSourcesOut,
                                               wdf::Circuit& settledControlCircuit,
                                               std::optional<bool>& previewPentodePairs) {
    // **A document can be rejected before there is an engine.** `toCircuit`
    // throws for a rail wired to ground, for example, and that is a drawing
    // error the user can fix -- not a reason to take the plugin down. The
    // engine construction below already reports its own failures; this catch
    // covers the conversion that happens first.
    wdf::Circuit c;
    try {
        c = wdf::circuitWithPortImpedances(doc, &portPositions, &fixtureNames);
        if (const auto supply = mainsSupplyRequested(); supply != MainsSupply::drawn)
            wdf::replaceMainsSupplies(c, doc, fs, supply == MainsSupply::stiff);
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
    for (const auto& source : c.isrc)
        baseSources.push_back(source.dc);

    // The AC sources: those with an amplitude *and* a frequency. The plugin
    // generates their sine itself, because a host's audio is not a sine wave
    // and the engine drives sources with values the caller supplies.
    //
    // The settings come from the *document*, in the order `toCircuit` creates
    // the sources: the frequency is the signal's, not the network's, and the
    // compiled circuit stays a topology description (see `format/AcSource.h`).
    acSourcesOut.clear();
    {
        const auto settings = doc.sourceAcSettings(c);
        for (size_t i = 0; i < settings.size(); ++i) {
            if (settings[i].ac == 0.0 || settings[i].freq == 0.0)
                continue;
            acSourcesOut.push_back({static_cast<int>(i), settings[i].ac, settings[i].freq});
        }
    }

    const bool hasAcSupply = doc.hasAcPowerSupply();
    const auto build = [&](wdf::Circuit& circuit) {
        left = std::make_unique<wdf::Engine>(circuit, fs, attemptedGridCurrent_);
        right = std::make_unique<wdf::Engine>(circuit, fs, attemptedGridCurrent_);

        // Precharge a modeled mains supply off the audio thread, ending at the first playback phase.
        outIdx = left->nodeIndex(doc.outputNet());
        outputBias = 0.;
        auto startupSources = baseSources;
        const int warmup = hasAcSupply ? static_cast<int>(fs / 4.) : 64;
        for (int p = -warmup; p < 0; ++p) {
            if (hasAcSupply) {
                for (const auto& source : acSourcesOut) {
                    const auto index = static_cast<size_t>(source.index);
                    startupSources[index] = wdf::sourceValueAt(baseSources[index], source.ac, source.freq, fs, p);
                }
            }
            const auto& nodes = left->step(startupSources);
            right->step(startupSources);
            if (outIdx >= 0)
                outputBias = nodes[static_cast<size_t>(outIdx)];
        }
        settledControlCircuit = std::move(circuit);
    };
    if (c.pent.size() >= 2) {
        std::vector<std::size_t> varyingSources;
        if (inSrcIdx >= 0)
            varyingSources.push_back(static_cast<std::size_t>(inSrcIdx));
        for (const auto& source : acSourcesOut)
            varyingSources.push_back(static_cast<std::size_t>(source.index));
        try {
            auto reduced = c;
            if (wdf::collapseMatchedPentodes(reduced, varyingSources) != 0) {
                build(reduced);
                if (outIdx >= 0) {
                    previewPentodePairs = true;
                    return;
                }
                previewPentodePairs.reset();
            }
        } catch (...) {
            previewPentodePairs.reset();
        }
    }

    try {
        build(c);
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
    const std::lock_guard lock(loadMutex_);
    if (!loadCircuitDocument(doc, true))
        return false;
    if (cabinetFollowsSuggestion_)
        selectSuggestedCabinet();
    updateReportedLatency();
    return true;
}

bool LiveWDFPluginProcessor::loadCircuitDocument(const wdf::LiveWDFDocument& doc,
                                                 bool initializePotParameters) {
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    lastError_.clear();
    liveControlUpdateFailed_.store(false, std::memory_order_relaxed);

    const bool hasPowerAmp = doc.preampTap() != nullptr;
    const bool enablePowerAmp = !hasPowerAmp || *apvts_.getRawParameterValue("power_amp") > 0.5f;
    wdf::LiveWDFDocument preamp;
    if (!enablePowerAmp && !doc.makePreampDocument(preamp, lastError_))
        return false;
    const auto& playbackDoc = enablePowerAmp ? doc : preamp;

    attemptedOversamplingChoice_ = effectiveOversamplingChoice();
    attemptedGridCurrent_ = *apvts_.getRawParameterValue("grid_current") > 0.5f;
    constexpr int kFactors[] = {1, 2, 4, 8, 2, 4};
    const int factor = kFactors[std::clamp(attemptedOversamplingChoice_, 0, 5)];
    const bool halfBand = attemptedOversamplingChoice_ >= 4;
    const double effectiveFs = sampleRate_ * static_cast<double>(factor);

    std::unique_ptr<wdf::Engine> newLeft, newRight;
    int inSrcIdx = -1;
    int outIdx = -1;
    double outputBias = 0.;
    std::vector<double> baseSources;
    std::vector<AcSource> acSources;

    std::array<double, numPorts> portPositions{};
    std::array<std::string, numPorts> fixtureNames;
    std::array<std::optional<wdf::PortImpedanceRange>, numPorts> portRanges;
    for (size_t i = 0; i < numPorts; ++i) {
        portRanges[i] = wdf::portImpedanceRange(playbackDoc, i == 0);
        portPositions[i] = initializePotParameters
            ? (portRanges[i] ? portRanges[i]->defaultPosition : 0.)
            : ports_[i].parameter->load(std::memory_order_relaxed);
    }

    wdf::Circuit newSettledControlCircuit;
    std::optional<bool> newPreviewPentodePairs;
    buildEngineFromDoc(playbackDoc, effectiveFs, portPositions, fixtureNames, newLeft, newRight, inSrcIdx, outIdx, outputBias, baseSources,
                       acSources, newSettledControlCircuit, newPreviewPentodePairs);

    if (!newLeft || !newRight) {
        return false;
    }
    if (!enablePowerAmp && outIdx < 0) {
        lastError_ = "The preamp output is not connected";
        return false;
    }
    // A document loaded from anywhere but a file has no file to reload. The
    // callers that *do* have a file set `currentFile_` again afterwards, so
    // this clearing is what makes the Reload button go dead for a circuit
    // restored from host state.
    currentFile_ = juce::File();

    powerAmpEnabled_ = hasPowerAmp ? std::optional(enablePowerAmp) : std::nullopt;
    previewPentodePairs_ = newPreviewPentodePairs;
    mainsSupply_ = playbackDoc.hasAcPowerSupply() ? std::optional(mainsSupplyRequested()) : std::nullopt;
    rejectedPortRequest_.reset();
    auto cacheKey = wdf::playbackCacheKey(playbackDoc, fixtureNames);
    if (previewPentodePairs_.value_or(false))
        cacheKey += "\npreview-pentode-pairs";
    if (mainsSupply_.value_or(MainsSupply::drawn) != MainsSupply::drawn)
        cacheKey += mainsSupply_ == MainsSupply::stiff ? "\nmains-stiff" : "\nmains-dc-equivalent";
    cacheDocument_ = std::make_shared<const std::string>(std::move(cacheKey));
    settledControlCircuit_ = std::move(newSettledControlCircuit);
    // **A meter is a thing on the sheet**: `allProbes()` asks the drawing for
    // its scope parts, keyed by the net each one watches, which is how the
    // schematic canvas wants its voltages.
    std::vector<std::pair<std::string, int>> probeNodes;
    for (const auto& probe : playbackDoc.allProbes()) {
        const int idx = newLeft->nodeIndex(probe.second);
        if (idx >= 0)
            probeNodes.emplace_back(probe.second, idx);
    }

    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        captureState_.store(CaptureState::idle, std::memory_order_release);
        capture_.work.reset();
        observedTransferRevision_.reset();
        preparedTransferRevision_.reset();
        initialTransferPending_ = true;
        leftEngine_.swap(newLeft);
        rightEngine_.swap(newRight);
        previewPrepared_ = false;
        previewLoadOverruns_ = 0;
        outputOnlyPreviewAllowed_ = acSources.empty() &&
            std::all_of(probeNodes.begin(), probeNodes.end(), [&](const auto& probe) { return probe.second == outIdx; });
        ++engineGeneration_;
        currentDoc_ = doc;
        inSourceIndex_ = inSrcIdx;
        outNodeIndex_ = outIdx;
        outputFullScaleVolts_.store(playbackDoc.outputFullScaleVolts(), std::memory_order_relaxed);
        outputFastTrim_.store(playbackDoc.outputTrim(false), std::memory_order_relaxed);
        outputPreviewTrim_.store(playbackDoc.outputTrim(true), std::memory_order_relaxed);
        outputAtSpeakerLevel_.store(hasPowerAmp && enablePowerAmp, std::memory_order_relaxed);
        previewPadScale_ = doc.validPreviewPadDb(doc.previewPadDb)
            ? juce::Decibels::decibelsToGain(-doc.previewPadDb) : 1.;
        previewPadOutputBias_ = outputBias;
        auto* pad = apvts_.getParameter("preview_pad");
        if ((initializePotParameters || previewPadScale_ == 1.) && pad->getValue() > 0.5f)
            pad->setValueNotifyingHost(0.f);
        previewPadGain_.reset(sampleRate_, .015);
        previewPadGain_.setCurrentAndTargetValue(compiledPreviewRequested() && pad->getValue() > 0.5f
                                                 ? previewPadScale_ : 1.);
        inputVoltsPerFullScale_ = doc.inputVoltsPerFullScale();
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
            potControls_[static_cast<size_t>(i)].impedance = nullptr;
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
        for (size_t i = 0; i < numPorts; ++i) {
            auto& port = ports_[i];
            port.range = std::move(portRanges[i]);
            port.enabled = !fixtureNames[i].empty();
            const float position = static_cast<float>(portPositions[i]);
            if (initializePotParameters && port.parameter->load() != position)
                apvts_.getParameter(portParameterIds[i])->setValueNotifyingHost(position);
            if (!port.enabled) continue;
            const int index = leftEngine_->varResIndexByName(fixtureNames[i]);
            jassert(port.range && index >= 0);
            auto& control = potControls_[static_cast<size_t>(potControlCount_)];
            control.knob = port.parameter;
            control.impedance = &*port.range;
            control.position.setCurrentAndTargetValue(port.range->ohmsAt(portPositions[i]) / port.range->maxOhms);
            potTargets_.push_back({potControlCount_++, index, true});
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
        if (initializePotParameters)
            syncPotParametersFromDocument(doc);

        // Start at the saved knob positions. Measure ramp progress in host
        // samples so oversampling does not change the feel of the controls.
        samplesUntilPotUpdate_ = 0;
        potsMoving_ = false;
        for (int i = 0; i < potControlCount_; ++i) {
            auto& control = potControls_[static_cast<size_t>(i)];
            const double value = control.targetPosition();
            control.position.setCurrentAndTargetValue(std::isfinite(value) ? value : 0.5);
        }
        applyLiveControlsToEngines(true, true);
        leftEngine_->prepareLiveControls();
        rightEngine_->prepareLiveControls();
    }

    // A fresh document already has settled controls. Start its background
    // screen before a heavy circuit can saturate the playback callbacks.
    if (!isNonRealtime() && *apvts_.getRawParameterValue("solve_quality") > 0.5f) {
        if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
            manager != nullptr && manager->isThisTheMessageThread())
            prepareSettledLiveTransfer();
    }
    return true;
}

void LiveWDFPluginProcessor::syncPotParametersFromDocument(const wdf::LiveWDFDocument& doc) {
    const int potLimit = std::min(kMaxPotKnobs, static_cast<int>(potGroupsInCircuit_.size()));
    for (int i = 0; i < potLimit; ++i) {
        double pos = 0.5;
        for (const auto& member : potGroupsInCircuit_[static_cast<size_t>(i)].members) {
            const auto comp = std::find_if(doc.components.begin(), doc.components.end(),
                                           [&](const wdf::SchematicComponent& c) {
                                               return c.id == member.id;
                                           });
            if (comp == doc.components.end())
                continue;

            const auto it = comp->values.find("pos");
            if (it != comp->values.end()) {
                pos = std::clamp(it->second, 0.0, 1.0);
                break;
            }
        }

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
        nameLegacyAmp(doc, file);
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

    nameLegacyAmp(doc, file);
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
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    std::lock_guard<std::mutex> lock(engineMutex_);

    if (index < 0 || index >= static_cast<int>(switchGroupsInCircuit_.size())
        || index >= static_cast<int>(switchGroupStates_.size()))
        return;

    // The editor reads the throws through `controlMutex_`; the audio thread
    // reads them under the engine lock, which we already hold. Lock order is
    // engine -> control everywhere, so the two cannot deadlock.
    const bool changed = switchGroupStates_[static_cast<size_t>(index)] != throwB;
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
    for (const auto& member : switchGroupsInCircuit_[static_cast<size_t>(index)].members) {
        if (changed && std::ranges::any_of(currentDoc_.components, [&](const auto& comp) {
                const auto gate = comp.properties.find("enabledBy");
                return gate != comp.properties.end() && (gate->second == member.id || gate->second == "!" + member.id);
            }))
            gatedSwitchChanged_.store(true, std::memory_order_relaxed);
    }

    applyLiveControlsToEngines(false, true);
}

double LiveWDFPluginProcessor::PotControl::targetPosition() const {
    const double value = knob->load(std::memory_order_relaxed);
    if (impedance == nullptr) return value;
    // Removing a fixture changes topology. Keep the previous physical value
    // until the control thread installs the as-wired circuit.
    if (value <= 0.) return position.getTargetValue();
    return impedance->ohmsAt(value) / impedance->maxOhms;
}

void LiveWDFPluginProcessor::readLivePotTargets(int rampSamples) {
    for (int i = 0; i < potControlCount_; ++i) {
        auto& control = potControls_[static_cast<size_t>(i)];
        const double value = control.targetPosition();
        if (std::isfinite(value) && !juce::approximatelyEqual(value, control.position.getTargetValue())) {
            // JUCE resets to the old target. Keep the physical wiper continuous
            // when automation changes direction before its ramp is complete.
            const double current = control.position.getCurrentValue();
            control.position.reset(rampSamples);
            control.position.setCurrentAndTargetValue(current);
            control.position.setTargetValue(value);
        }
        potsMoving_ |= control.position.isSmoothing();
    }
}

bool LiveWDFPluginProcessor::advanceLivePots() {
    if (!potsMoving_)
        return false;

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
        return false;
    }

    samplesUntilPotUpdate_ = interval - 1;
    if (!potsMoving_)
        samplesUntilPotUpdate_ = 0;
    return true;
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
        if (switches) {
            engine.releaseBipolarPreview();
            engine.setParametersByIndex(potMoves_, varResMoves_, resistorMoves_);
        } else if (!engine.setLiveControls(potMoves_, varResMoves_)) {
            liveControlUpdateFailed_.store(true, std::memory_order_relaxed);
        }
    };
    apply(*leftEngine_);
    if (rightEngine_)
        apply(*rightEngine_);
}

// ==============================================================================
// State Persistence
// ==============================================================================
void LiveWDFPluginProcessor::getStateInformation(juce::MemoryBlock& destData) {
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    auto state = apvts_.copyState();
    outputEffects_.save(state);
    if (cabinetFollowsSuggestion_)
        state.setProperty("cabinet_follow_suggested", true, nullptr);
    std::unique_ptr<juce::XmlElement> xml(state.createXml());

    // Save active LiveWDF JSON document
    xml->setAttribute("livewdf_json", juce::String(currentDoc_.toJson()));

    copyXmlToBinary(*xml, destData);
}

void LiveWDFPluginProcessor::setStateInformation(const void* data, int sizeInBytes) {
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName(apvts_.state.getType())) {
        std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
        // Earlier session files contain no Preview parameter. Reloading them
        // into an existing instance must restore the original playback mode.
        auto state = juce::ValueTree::fromXml(*xmlState);
        const auto restoreMissing = [&](const char* id, float value) {
            if (!state.getChildWithProperty("id", id).isValid())
                state.appendChild(juce::ValueTree{"PARAM", {{"id", id}, {"value", value}}}, nullptr);
        };
        restoreMissing("compiled_preview", 0.0f);
        restoreMissing("preview_pad", 0.0f);
        restoreMissing("chain_input", 0.0f);
        restoreMissing("match_output", 1.0f);
        restoreMissing("output_master", 0.0f);
        restoreMissing("power_amp", 1.0f);
        restoreMissing("cab_enabled", 0.0f);
        restoreMissing("echo_mix", 0.0f);
        restoreMissing("echo_delay", 250.0f);
        restoreMissing("echo_feedback", 42.0f);
        restoreMissing("reverb_mix", 0.0f);
        restoreMissing("reverb_size", 100.0f);
        restoreMissing("reverb_predelay", 0.0f);
        for (const auto id : portParameterIds) restoreMissing(id, 0.f);
        // A retired factory cabinet id follows the circuit's suggestion instead.
        const auto storedCabinet = state.getProperty("cabinet_ir_factory").toString();
        const auto cabinets = OutputEffects::factoryImpulses(false);
        const bool retiredCabinet = storedCabinet.isNotEmpty() && std::none_of(cabinets.begin(), cabinets.end(),
            [&](const auto& cabinet) { return storedCabinet == cabinet.id; });
        cabinetFollowsSuggestion_ = retiredCabinet || static_cast<bool>(state.getProperty("cabinet_follow_suggested", false));
        state.removeProperty("cabinet_follow_suggested", nullptr);
        outputEffects_.restore(state);
        apvts_.replaceState(state);

        // Restore custom LiveWDF JSON document
        if (xmlState->hasAttribute("livewdf_json")) {
            const std::string jsonStr = xmlState->getStringAttribute("livewdf_json").toStdString();
            wdf::LiveWDFDocument doc;
            std::string err;
            if (wdf::LiveWDFDocument::fromJson(jsonStr, doc, err)) {
                // The APVTS state was restored above; that is the user's knob
                // state. Do not overwrite it with the document's saved `pos`.
                const bool documentChanged = currentDoc_.toJson() != jsonStr;
                if (loadCircuitDocument(doc, false) && documentChanged)
                    restoredDocumentRevision_.fetch_add(1, std::memory_order_release);
            }
        }
        if (retiredCabinet)
            selectSuggestedCabinet();
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
