#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "format/AcSource.h"

#include <algorithm>

namespace livewdf {

namespace {

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
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.005f), 0.5f));
    }

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
        2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    oversampler4x_ = std::make_unique<juce::dsp::Oversampling<float>>(
        2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    oversampler8x_ = std::make_unique<juce::dsp::Oversampling<float>>(
        2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);

    // **No built-in circuit.** A factory preset is uneditable and cannot carry
    // the routed wires the editor draws, so the plugin opens empty and waits for
    // a `.livewdf` file (or a host session that restores one).
}

LiveWDFPluginProcessor::~LiveWDFPluginProcessor() { stopTimer(); }

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

    oversampler2x_->reset();
    oversampler4x_->reset();
    oversampler8x_->reset();
    halfBandOversamplerL_.reset();
    halfBandOversamplerR_.reset();
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
}

void LiveWDFPluginProcessor::releaseResources() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    oversampler2x_->reset();
    oversampler4x_->reset();
    oversampler8x_->reset();
    halfBandOversamplerL_.reset();
    halfBandOversamplerR_.reset();
}

// The oversampling and grid current choices rebuild the engines, so they are applied here on the
// message thread and never in the realtime audio callback.
void LiveWDFPluginProcessor::timerCallback() {
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    if (currentDoc_.components.empty()
        || (static_cast<int>(*apvts_.getRawParameterValue("oversampling")) == attemptedOversamplingChoice_
            && (*apvts_.getRawParameterValue("grid_current") > 0.5f) == attemptedGridCurrent_))
        return;

    // `loadCircuitDocument` clears the reload path; an oversampling change must
    // not make the Reload button go dead.
    const juce::File circuitFile = currentFile_;
    loadCircuitDocument(currentDoc_, false);
    currentFile_ = circuitFile;
}

void LiveWDFPluginProcessor::buildEngineFromDoc(const wdf::LiveWDFDocument& doc, double fs,
                                               std::unique_ptr<wdf::Engine>& left,
                                               std::unique_ptr<wdf::Engine>& right,
                                               int& inSrcIdx, int& outIdx,
                                               std::vector<double>& baseSources,
                                               std::vector<AcSource>& acSourcesOut) {
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
        left = std::make_unique<wdf::Engine>(c, fs, attemptedGridCurrent_);
        right = std::make_unique<wdf::Engine>(c, fs, attemptedGridCurrent_);

        std::vector<double> nL, rL, nR, rR;
        left->solveDc(nL, rL);
        left->reset();
        right->solveDc(nR, rR);
        right->reset();

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
    return loadCircuitDocument(doc, true);
}

bool LiveWDFPluginProcessor::loadCircuitDocument(const wdf::LiveWDFDocument& doc,
                                                 bool initializePotParameters) {
    std::lock_guard<std::recursive_mutex> loadLock(loadMutex_);
    lastError_.clear();

    // A document loaded from anywhere but a file has no file to reload. The
    // callers that *do* have a file set `currentFile_` again afterwards, so
    // this clearing is what makes the Reload button go dead for a circuit
    // restored from host state.
    currentFile_ = juce::File();

    attemptedOversamplingChoice_ = static_cast<int>(*apvts_.getRawParameterValue("oversampling"));
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

    buildEngineFromDoc(doc, effectiveFs, newLeft, newRight, inSrcIdx, outIdx, baseSources,
                        acSources);

    if (!newLeft || !newRight) {
        return false;
    }

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
        if (halfBand) {
            const auto f = factor == 4 ? wdf::OversamplingFactor::X4 : wdf::OversamplingFactor::X2;
            halfBandOversamplerL_.setFactor(f);
            halfBandOversamplerR_.setFactor(f);
            halfBandOversamplerL_.reset();
            halfBandOversamplerR_.reset();
        }

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
        const int potLimit = std::min(kMaxPotKnobs, static_cast<int>(potGroupsInCircuit_.size()));
        for (int i = 0; i < potLimit; ++i) {
            auto* knob = apvts_.getRawParameterValue("pot_" + juce::String(i));
            for (const auto& member : potGroupsInCircuit_[static_cast<size_t>(i)].members) {
                const bool varRes = member.type == "varres";
                const int index = varRes ? leftEngine_->varResIndexByName(member.id)
                                         : leftEngine_->potIndexByName(member.id);
                if (index >= 0)
                    potTargets_.push_back({knob, index, varRes});
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
        if (initializePotParameters)
            syncPotParametersFromDocument(doc);

        // Apply under the same lock: the new engine is already visible to the
        // audio thread, and touching its pots or switch resistors outside this
        // lock would race a block that is just starting.
        applyLiveControlsToEngines();
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
    std::lock_guard<std::mutex> lock(engineMutex_);

    if (index < 0 || index >= static_cast<int>(switchGroupsInCircuit_.size())
        || index >= static_cast<int>(switchGroupStates_.size()))
        return;

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

    applyLiveControlsToEngines();
}

void LiveWDFPluginProcessor::applyLiveControlsToEngines() {
    if (!leftEngine_) return;

    // Every pot and switch member goes to the engine in one batch, so a moved
    // ganged group re-derives the linear transfer once, not once per member.
    potMoves_.clear();
    varResMoves_.clear();
    resistorMoves_.clear();
    for (const auto& t : potTargets_)
        (t.varRes ? varResMoves_ : potMoves_).emplace_back(t.index, static_cast<double>(t.knob->load()));
    for (const auto& t : switchTargets_) {
        const bool throwB = switchGroupStates_[t.group];
        resistorMoves_.emplace_back(t.resA, throwB ? wdf::kSpdtOpenOhms : wdf::kSpdtClosedOhms);
        resistorMoves_.emplace_back(t.resB, throwB ? wdf::kSpdtClosedOhms : wdf::kSpdtOpenOhms);
    }

    leftEngine_->setParametersByIndex(potMoves_, varResMoves_, resistorMoves_);
    if (rightEngine_)
        rightEngine_->setParametersByIndex(potMoves_, varResMoves_, resistorMoves_);
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
