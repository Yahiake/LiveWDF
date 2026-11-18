#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "wdf/Engine.h"
#include "dsp/HalfBandOversampler.h"
#include "format/LiveWDFFile.h"
#include "gui/DualOscilloscope.h"

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
    static constexpr int kMaxPotKnobs = 16;

    LiveWDFPluginProcessor();
    ~LiveWDFPluginProcessor() override;

    // -------------------------------------------------------------------------
    // AudioProcessor Overrides
    // -------------------------------------------------------------------------
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "LiveWDF"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // The plugin no longer carries factory circuits: a saved host session is the
    // one program, and everything else is a `.livewdf` file the user loads.
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Custom Circuit"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // -------------------------------------------------------------------------
    // LiveWDF Circuit Management
    // -------------------------------------------------------------------------
    bool loadCircuitDocument(const wdf::LiveWDFDocument& doc);
    bool loadCircuitFromFile(const juce::File& file);

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
    juce::File getCurrentCircuitFile() const { return currentFile_; }

    /// True when there is a file to reload -- what the Reload button's enabled
    /// state follows.
    bool canReload() const { return currentFile_.existsAsFile(); }

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

    /// The rate the host handed to `prepareToPlay`. This is the rate the
    /// engine is compiled at (times the oversampling factor), which is the one
    /// the editor's scope must use for its timebase.
    double getHostSampleRate() const noexcept { return sampleRate_; }

    const ScopeAudioBuffer& getInputScopeBuffer() const noexcept { return inputScopeBuffer_; }
    const ScopeAudioBuffer& getOutputScopeBuffer() const noexcept { return outputScopeBuffer_; }

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts_; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    juce::AudioProcessorValueTreeState apvts_;

    // Oversampling (1x bypass, 2x, 4x, 8x polyphase IIR, 2x/4x Halfband Butterworth IIR)
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler2x_;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler4x_;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler8x_;
    int attemptedOversamplingChoice_ = 0;
    int currentOversamplingFactor_ = 1;
    bool useHalfBand_ = false;
    wdf::HalfBandOversampler halfBandOversamplerL_{wdf::OversamplingFactor::X1};
    wdf::HalfBandOversampler halfBandOversamplerR_{wdf::OversamplingFactor::X1};

    // Engine instances (stereo pair)
    // Serialises loads from the timer, a bounce, prepareToPlay and the host state; recursive so a caller can hold it across its load.
    std::recursive_mutex loadMutex_;
    mutable std::mutex engineMutex_;
    std::unique_ptr<wdf::Engine> leftEngine_;
    std::unique_ptr<wdf::Engine> rightEngine_;

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
    double outputFullScaleVolts_ = 1.0;

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
    struct PotTarget { std::atomic<float>* knob; int index; bool varRes; };
    struct SwitchTarget { size_t group; int resA; int resB; };
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
    void syncPotParametersFromDocument(const wdf::LiveWDFDocument& doc);
    void timerCallback() override;
    void applyLiveControlsToEngines();
    void buildEngineFromDoc(const wdf::LiveWDFDocument& doc, double fs,
                            std::unique_ptr<wdf::Engine>& left,
                            std::unique_ptr<wdf::Engine>& right,
                            int& inSrcIdx, int& outIdx,
                            std::vector<double>& baseSources,
                            std::vector<AcSource>& acSourcesOut);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiveWDFPluginProcessor)
};

} // namespace livewdf
