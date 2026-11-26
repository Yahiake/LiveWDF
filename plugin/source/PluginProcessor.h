#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "wdf/Engine.h"
#include "dsp/HalfBandOversampler.h"
#include "OutputEffects.h"
#include "format/LiveWDFFile.h"
#include "format/ControlGroups.h"
#include "format/PortImpedance.h"
#include "gui/DualOscilloscope.h"

#include <array>
#include <atomic>
#include <future>
#include <mutex>
#include <memory>
#include <map>
#include <string>

namespace livewdf {

class LiveWDFPluginProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    /// The APVTS layout is fixed when the processor is constructed, so this is
    /// the ceiling on how many document pot groups can get a knob. A document
    /// with more groups than this still loads and plays -- the extra groups
    /// keep their saved positions, they just have no parameter to drive them.
    static constexpr int kMaxPotKnobs = wdf::kLivePotSlots;

    LiveWDFPluginProcessor();
    ~LiveWDFPluginProcessor() override;

    // -------------------------------------------------------------------------
    // AudioProcessor Overrides
    // -------------------------------------------------------------------------
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "LiveWDF"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    // The plugin no longer carries factory circuits: a saved host session is the
    // one program, and everything else is a `.livewdf` file the user loads.
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Custom Circuit"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    bool loadOutputImpulse(bool reverb, const juce::File& file) { return outputEffects_.loadImpulse(reverb, file); }
    bool selectFactoryOutputImpulse(bool reverb, juce::StringRef id) { return outputEffects_.selectFactoryImpulse(reverb, id); }
    void resetOutputImpulse(bool reverb) { outputEffects_.resetImpulse(reverb); }
    juce::String outputImpulseName(bool reverb) const { return outputEffects_.impulseName(reverb); }
    juce::String outputImpulseId(bool reverb) const { return outputEffects_.impulseId(reverb); }
    std::span<const FactoryImpulse> factoryOutputImpulses(bool reverb) const noexcept {
        return OutputEffects::factoryImpulses(reverb);
    }
    juce::String suggestedCabinetId() const;
    juce::String factoryOutputImpulseCredits() const { return OutputEffects::factoryCredits(); }

    // -------------------------------------------------------------------------
    // LiveWDF Circuit Management
    // -------------------------------------------------------------------------
    bool loadCircuitDocument(const wdf::LiveWDFDocument& doc);
    bool loadCircuitFromFile(const juce::File& file);
    std::uint64_t getRestoredDocumentRevision() const noexcept { return restoredDocumentRevision_.load(); }

    /// Why the last load failed, in the user's words. Empty when it did not.
    ///
    /// **The engine's own message is not what is shown.** "Singular linear
    /// network" says the solver gave up and not why; this says which part is
    /// joined to nothing, which is a thing the user can go and fix.
    const std::string& lastError() const noexcept { return lastError_; }

    /// Re-read the file the circuit came from and rebuild the engine from it.
    ///
    /// **This is the "designing while listening" button.** The editor is where
    /// circuits are drawn and tested; the plugin is where one is played. When
    /// both are open on the same file, the plugin is holding a *copy* of the
    /// circuit from the moment it was loaded -- so an edit in the editor is
    /// invisible to the plugin until it is told to look again. This is that
    /// "look again".
    ///
    /// Returns false when there is no file to reload, or when the file has
    /// become unloadable -- a half-saved file, say. The current circuit is left
    /// playing either way: a failed reload must not take the sound away.
    bool reloadCircuit();

    /// The file the circuit was loaded from, or an empty file when it came from
    /// host state rather than a file.
    juce::File getCurrentCircuitFile() const {
        std::lock_guard<std::recursive_mutex> lock(loadMutex_);
        return currentFile_;
    }

    /// True when there is a file to reload -- what the Reload button's enabled
    /// state follows.
    bool canReload() const { return getCurrentCircuitFile().existsAsFile(); }

    wdf::LiveWDFDocument getDocumentSnapshot() const {
        std::lock_guard<std::recursive_mutex> lock(loadMutex_);
        return currentDoc_;
    }
    const wdf::LiveWDFDocument& getCurrentDocument() const { return currentDoc_; }

    /// The pots and rheostats grouped by their visible name. One entry is one
    /// knob; every member in `members` moves together. The order matches the
    /// `pot_0`, `pot_1`, ... parameters and survives a reload.
    std::vector<wdf::LiveControlGroup> getPotGroups() const;

    /// The SPDTs grouped by their visible name. One entry is one switch, and
    /// every member is thrown together.
    std::vector<wdf::LiveControlGroup> getSwitchGroups() const;

    /// The throw of switch group `index`: true for B, false for A.
    bool getSwitchGroupB(int index) const;

    /// Throw every SPDT in group `index`. The two resistor branches are written
    /// immediately; the document is updated too so the state survives a save.
    void setSwitchGroupB(int index, bool throwB);

    /// Live positions for the schematic overlay, keyed by component id.
    ///
    /// The document is the circuit as loaded; the knobs are APVTS values. The
    /// canvas needs the second to draw what is actually playing, and the two are
    /// deliberately not written into each other here -- `pos` in the document is
    /// what the file said, while the param is where the user put the knob.
    std::map<std::string, double> getLivePotPositions() const;
    std::map<std::string, bool> getLiveSwitchStates() const;

    std::map<std::string, double> getLiveProbeVoltages() const;
    float getLiveSignalActivity() const noexcept { return signalActivity_.load(std::memory_order_relaxed); }

    bool compiledPreviewRequested() const;

    /// The rate the host handed to `prepareToPlay`. This is the rate the
    /// engine is compiled at (times the oversampling factor), which is the one
    /// the editor's scope must use for its timebase.
    double getHostSampleRate() const noexcept { return sampleRate_; }
    double getCircuitOutputFullScaleVolts() const noexcept { return outputFullScaleVolts_.load(std::memory_order_relaxed); }

    const ScopeAudioBuffer& getInputScopeBuffer() const noexcept { return inputScopeBuffer_; }
    const ScopeAudioBuffer& getOutputScopeBuffer() const noexcept { return outputScopeBuffer_; }

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts_; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    friend bool waitForBenchmarkPlan(LiveWDFPluginProcessor&, juce::AudioBuffer<float>&,
                                     juce::MidiBuffer&, bool preview);
    friend bool checkLivePreviewCapture(const juce::File& file);
    friend bool checkPowerAmp(const juce::File& file);
    friend bool checkLivePreviewPotMotion(const juce::File& file);
    juce::AudioProcessorValueTreeState apvts_;
    OutputEffects outputEffects_;
    const std::atomic<float>* cabinetEnabled_ = apvts_.getRawParameterValue("cab_enabled");
    const std::atomic<float>* echoMix_ = apvts_.getRawParameterValue("echo_mix");
    const std::atomic<float>* reverbMix_ = apvts_.getRawParameterValue("reverb_mix");
    void processOutputEffects(juce::dsp::AudioBlock<float>) noexcept;

    // Oversampling (1x bypass, 2x, 4x, 8x polyphase IIR, 2x/4x Halfband Butterworth IIR)
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler2x_;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler4x_;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler8x_;
    int attemptedOversamplingChoice_ = 0;
    bool attemptedGridCurrent_ = true;
    std::optional<bool> powerAmpEnabled_;
    int currentOversamplingFactor_ = 1;
    bool useHalfBand_ = false;
    bool oversamplersPrepared_ = false;
    wdf::HalfBandOversampler halfBandOversamplerL_{wdf::OversamplingFactor::X1};
    wdf::HalfBandOversampler halfBandOversamplerR_{wdf::OversamplingFactor::X1};
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Thiran> halfBandPadding_{2};
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay_{32};
    std::atomic<int> processingLatencySamples_{0};
    int dryDelaySamples_ = 0;

    juce::dsp::Oversampling<float>* activePolyphaseOversampler() const noexcept;
    void applyPendingCircuitOptions();
    void updateReportedLatency();
    void processDryDelay(juce::dsp::AudioBlock<float> block, bool replace, int latency) noexcept;

    // Engine instances (stereo pair)
    // Serialises loads from the timer, a bounce, prepareToPlay and the host state; recursive so a caller can hold it across its load.
    mutable std::recursive_mutex loadMutex_;
    mutable std::mutex engineMutex_;
    std::unique_ptr<wdf::Engine> leftEngine_;
    std::unique_ptr<wdf::Engine> rightEngine_;
    unsigned long long engineGeneration_ = 0;

    struct AffineKey {
        unsigned long long generation = 0;
        long long revision = 0;
        bool stereo = false;
        bool operator==(const AffineKey&) const = default;
    };
    struct AffineResult {
        AffineKey key;
        wdf::Engine::MixedAffineSetup left;
        std::optional<wdf::Engine::MixedAffineSetup> right;
    };
    std::optional<AffineKey> observedAffineKey_, requestedAffineKey_;
    std::optional<AffineResult> readyAffine_;
    std::future<AffineResult> affineJob_;
    using TransferRevision = std::array<long long, 5>; // transfer, controls, channel affine plans, preview request
    struct SettledTransfer {
        SettledTransfer(unsigned long long currentGeneration, const wdf::Circuit& circuit)
            : generation(currentGeneration), left(circuit), right(left.makePeer()) {}
        unsigned long long generation = 0;
        wdf::Engine::LiveTransferSetup left, right;
        bool affine = false;
        long long installedRevision = 0;
        bool stereo = false;
        bool compilePreview = false;
    };
    std::future<SettledTransfer> settledTransferJob_;
    std::atomic<SettledTransfer*> pendingSettledTransfer_{nullptr}, retiredSettledTransfer_{nullptr};
    bool settledTransferPublished_ = false;
    bool initialTransferPending_ = false;
    bool previewPrepared_ = false; // eligibility was evaluated for these control/transfer revisions
    long long previewLinearRevision_ = -1, previewControlRevision_ = -1;
    bool outputOnlyPreviewAllowed_ = false;
    // The timer can service an idle engine directly. During playback the
    // callback publishes the revision and handles pending captures itself.
    static constexpr long long noAudioSinceTick = -2, adjustedAudioTransfer = -1;
    std::atomic<long long> audioTransferRevision_{noAudioSinceTick};
    enum class CaptureState { idle, requested, needsStorage, done };
    std::atomic<CaptureState> captureState_{CaptureState::idle};
    struct TransferCapture {
        std::optional<SettledTransfer> work;
        std::optional<TransferRevision> revision;
        bool fastLive = false, wantPreview = false;
    } capture_;
    std::optional<TransferRevision> observedTransferRevision_, preparedTransferRevision_;
    wdf::Circuit settledControlCircuit_;

    /// Guards the control-side copies: the pot/switch groups, the switch
    /// throws and the live positions the editor draws.
    ///
    /// **Deliberately not `engineMutex_`.** The audio thread holds
    /// `engineMutex_` for a whole block, and the editor polls this state at
    /// 30 Hz. When the polls took the engine lock, the audio thread's
    /// `try_lock` failed and the block was replaced by dry input -- the engine
    /// was fine, the plugin had stopped running it for that block. The audio
    /// thread never takes this mutex; writers take it while they already hold
    /// `engineMutex_`, so audio reads are serialised by the engine lock and UI
    /// copies by this one.
    mutable std::mutex controlMutex_;
    wdf::LiveWDFDocument currentDoc_;
    std::shared_ptr<const std::string> cacheDocument_;

    /// The file the circuit was loaded from, so it can be re-read. Empty when
    /// the circuit came from host state rather than a file.
    juce::File currentFile_;

    /// Why the last load failed, in the user's words.
    std::string lastError_;

    int inSourceIndex_ = -1;
    int outNodeIndex_ = -1;

    /// The voltage the circuit makes at its output when the input is at full
    /// scale, read off the Output terminal.
    ///
    /// The output is divided by it, so a preamp that makes 100 volts hands back
    /// a signal that peaks at 1.0 for a full-scale input. This is LiveSPICE's
    /// `Speaker.V0dBFS` -- "Voltage of the full signal level at this
    /// component" -- and it is the user's number, not ours: they write it on
    /// the Output terminal, and the circuit is not changed, only scaled.
    ///
    /// 1 when the document has not said, which scales nothing.
    std::atomic<double> outputFullScaleVolts_{1.0};
    std::atomic<double> inputVoltsPerFullScale_{1.0};
    double previewPadScale_ = 1.;
    double previewPadOutputBias_ = 0.;
    juce::SmoothedValue<double> previewPadGain_{1.};

    std::vector<double> baseSourceValues_;

    /// Per-block scratch, allocated when the circuit or block size changes.
    /// Keeping these here is what makes `processBlock` allocation-free in the
    /// normal case: the old code copied the source vector twice and copied the
    /// dry buffer on every block.
    std::vector<double> svLeft_;
    std::vector<double> svRight_;
    juce::AudioBuffer<float> dryBuffer_;

    /// Live controls, grouped by label. Built when a document is loaded and
    /// read while the engine lock is held.
    std::vector<wdf::LiveControlGroup> potGroupsInCircuit_;
    std::vector<wdf::LiveControlGroup> switchGroupsInCircuit_;
    std::vector<bool> switchGroupStates_;   // parallel to switchGroupsInCircuit_

    /// Engine indices of every grouped member, resolved at load, and the
    /// per-block batch built from them (reserved at load, so no allocation).
    struct PotControl {
        const std::atomic<float>* knob = nullptr;
        juce::SmoothedValue<double> position;
        const wdf::PortImpedanceRange* impedance = nullptr;
        double targetPosition() const;
    };
    struct PotTarget { int group; int index; bool varRes; };
    struct SwitchTarget { size_t group; int resA; int resB; };
    static constexpr size_t numPorts = 2;
    static constexpr std::array<const char*, numPorts> portParameterIds{"source_impedance", "load_impedance"};
    struct PortControl {
        std::optional<wdf::PortImpedanceRange> range;
        const std::atomic<float>* parameter = nullptr;
        bool enabled = false;
    };
    std::array<PortControl, numPorts> ports_;
    std::optional<std::array<float, numPorts>> rejectedPortRequest_;
    std::atomic<std::uint64_t> restoredDocumentRevision_{0};
    std::array<PotControl, kMaxPotKnobs + numPorts> potControls_;
    int potControlCount_ = 0;
    int samplesUntilPotUpdate_ = 0;
    bool potsMoving_ = false;
    std::atomic<bool> liveControlUpdateFailed_{false};
    std::vector<PotTarget> potTargets_;
    std::vector<SwitchTarget> switchTargets_;
    std::vector<std::pair<int, double>> potMoves_, varResMoves_, resistorMoves_;

    /// A source that is running an AC signal: which source, how big, how fast.
    ///
    /// The signal source is a DC voltage, or an AC one -- an amplitude and a
    /// frequency. The plugin has to generate that sine itself, because the
    /// engine drives sources with values the caller supplies and a host's audio
    /// is not a sine wave.
    struct AcSource {
        int index = -1;
        double ac = 0.0;
        double freq = 0.0;
    };
    std::vector<AcSource> acSources_;
    std::vector<AcSource> acCurrentSources_;

    /// The sample counter the AC sources' phase is counted from, and the rate
    /// they are counted at -- the *oversampled* rate, because that is the rate
    /// the engine is stepped at.
    long long acSampleCounter_ = 0;
    double acSampleRate_ = 48000.0;

    /// Probe nets and their engine node, resolved at load; the audio thread
    /// only writes `probeVoltages_`, and skips the write when the UI holds the lock.
    mutable std::mutex probeMutex_;
    std::vector<std::pair<std::string, int>> probeNodes_;
    std::vector<double> probeVoltages_;
    std::atomic<float> signalActivity_ { 0.0f };

    // Circular buffers for dual-trace oscilloscope
    ScopeAudioBuffer inputScopeBuffer_;
    ScopeAudioBuffer outputScopeBuffer_;

    double sampleRate_ = 48000.0;
    int samplesPerBlock_ = 512;

    bool loadCircuitDocument(const wdf::LiveWDFDocument& doc, bool initializePotParameters);
    bool shouldForceAccurate() const;
    int effectiveOversamplingChoice() const;
    void syncPotParametersFromDocument(const wdf::LiveWDFDocument& doc);
    void timerCallback() override;
    void prepareFastLiveAffine(long long audioRevision);
    void prepareSettledLiveTransfer(bool audioActive = false);
    void captureSettledTransferIfRequested() noexcept;
    void applySettledLiveTransfer(bool stereo) noexcept;
    void readLivePotTargets(int rampSamples);
    void advanceLivePots();
    void applyLiveControlsToEngines(bool pots, bool switches);
    void buildEngineFromDoc(const wdf::LiveWDFDocument& doc, double fs,
                            const std::array<double, numPorts>& portPositions,
                            std::array<std::string, numPorts>& fixtureNames,
                            std::unique_ptr<wdf::Engine>& left,
                            std::unique_ptr<wdf::Engine>& right,
                            int& inSrcIdx, int& outIdx, double& outputBias,
                            std::vector<double>& baseSources,
                            std::vector<AcSource>& acSourcesOut,
                            wdf::Circuit& settledControlCircuit);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiveWDFPluginProcessor)
};

} // namespace livewdf
