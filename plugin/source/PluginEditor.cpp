#include "PluginEditor.h"
#include "FactorySchematics.h"
#include "Fonts.h"
#include "gui/AboutDialog.h"

namespace livewdf {

namespace {

/// **The loader's own words, on the line that says what is playing.** Reading
/// a geometry-only sheet can be successful and still worth saying something:
/// a part type the library does not have, or a sheet with no `in`/`out` part
/// so its terminals come from the stale names in the io block. Those answers
/// live on the document, which is what makes one helper cover every load.
juce::String loadNoteSuffix(const wdf::LiveWDFDocument& doc) {
    juce::String suffix;
    constexpr const size_t kMaxNotesShown = 3;
    for (size_t i = 0; i < doc.loadWarnings.size() && i < kMaxNotesShown; ++i)
        suffix += " -- " + juce::String(doc.loadWarnings[i]);
    if (doc.loadWarnings.size() > kMaxNotesShown)
        suffix += " ...";
    return suffix;
}

} // anonymous

LiveWDFPluginEditor::LiveWDFPluginEditor(LiveWDFPluginProcessor& p)
    : AudioProcessorEditor(&p), processor_(p) {
    setLookAndFeel(&lookAndFeel_);

    // 1. Title Label
    titleLabel_.setText("LIVEWDF", juce::dontSendNotification);
    titleLabel_.setFont(editor::Fonts::bold(16.0f));
    titleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFFFFFFFF));
    addAndMakeVisible(titleLabel_);

    // The plugin editor is often opened before the standalone editor, so it is
    // also a valid first run. Install the factory set here; the marker makes
    // this a cheap no-op after the first binary has done it.
    if (const auto factory = FactorySchematics::ensureInstalled(); factory.failed())
        juce::Logger::writeToLog("Factory schematics: " + factory.getErrorMessage());
    presetStepper_.setFactoryFolder(FactorySchematics::folder());
    presetStepper_.setCurrentFile(processor_.getCurrentCircuitFile());
    presetStepper_.onPresetChosen = [this](const juce::File& file) { loadCircuitFile(file); };

    // 2. Factory and Load File Buttons
    factoryBtn_.onClick = [this] { showFactoryBrowser(); };
    factoryBtn_.setTooltip("Load a circuit shipped with LiveWDF");
    addAndMakeVisible(factoryBtn_);

    loadFileBtn_.onClick = [this] { openCircuitFileChooser(); };
    addAndMakeVisible(loadFileBtn_);

    // 3. Reload Button
    reloadBtn_.onClick = [this] {
        if (processor_.reloadCircuit()) {
            schematicCanvas_.setDocument(processor_.getCurrentDocument());
            syncSchematicLiveState();
            rebuildDynamicControls();
            presetStepper_.setName(currentCircuitDescription() + " · Reloaded");
        } else {
            // **A reload that went dead says why**, because the usual reason
            // now is a file the format refuses outright, and the fix is in the
            // reason's words.
            const juce::String reason = processor_.lastError();
            presetStepper_.setName(reason.isEmpty() ? juce::String("Reload failed")
                                                    : "Reload failed -- " + reason);
        }
    };
    addAndMakeVisible(reloadBtn_);

    // About Button
    aboutBtn_.onClick = [this] {
        editor::AboutDialog::show(this, editor::AboutDialog::Mode::Plugin);
    };
    addAndMakeVisible(aboutBtn_);

    // 4. Oversampling ComboBox
    oversamplingCombo_.addItem("1x Direct", 1);
    oversamplingCombo_.addItem("2x Polyphase", 2);
    oversamplingCombo_.addItem("4x Polyphase", 3);
    oversamplingCombo_.addItem("8x Polyphase", 4);
    oversamplingCombo_.addItem("2x Halfband IIR", 5);
    oversamplingCombo_.addItem("4x Halfband IIR", 6);
    addAndMakeVisible(oversamplingCombo_);
    oversamplingAttach_ = std::make_unique<ComboBoxAttachment>(processor_.getAPVTS(), "oversampling", oversamplingCombo_);

    oversampleExportBtn_.setTooltip("Use 4× Polyphase when the host renders offline. "
                                    "Turn off to use the playback oversampling setting.");
    addAndMakeVisible(oversampleExportBtn_);
    oversampleExportAttach_ = std::make_unique<ButtonAttachment>(
        processor_.getAPVTS(), "export_4x", oversampleExportBtn_);

    solveQualityCombo_.addItem("Accurate", 1);
    solveQualityCombo_.addItem("Fast Live", 2);
    solveQualityCombo_.setTooltip("Fast Live uses a looser solver tolerance.");
    addAndMakeVisible(solveQualityCombo_);
    solveQualityAttach_ = std::make_unique<ComboBoxAttachment>(
        processor_.getAPVTS(), "solve_quality", solveQualityCombo_);

    accurateExportBtn_.setTooltip("Use Accurate when the host renders offline. "
                                  "Turn off to render with the selected Solve Quality.");
    addAndMakeVisible(accurateExportBtn_);
    accurateExportAttach_ = std::make_unique<ButtonAttachment>(
        processor_.getAPVTS(), "accurate_export", accurateExportBtn_);

    // 4b. Mono / stereo. Off (mono) is the default; the processor runs one
    // engine and folds the input to mono. Turning it on runs the right-hand
    // engine as well, at roughly twice the CPU.
    stereoBtn_.setClickingTogglesState(true);
    stereoBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF1A212D));
    stereoBtn_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF103A3A));
    stereoBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFE2E8F0));
    stereoBtn_.setColour(juce::TextButton::textColourOnId, juce::Colour(0xFF00E5FF));
    stereoBtn_.setTooltip("Mono uses one engine; Stereo runs a second engine for the right channel");
    stereoBtn_.onClick = [this] {
        stereoBtn_.setButtonText(stereoBtn_.getToggleState() ? "STEREO" : "MONO");
    };
    addAndMakeVisible(stereoBtn_);
    stereoAttach_ = std::make_unique<ButtonAttachment>(processor_.getAPVTS(), "stereo", stereoBtn_);

    // 5. Shared editor canvas, read-only. Mouse and keyboard gestures are
    //    disabled because the plugin is a player: editing belongs in the
    //    standalone editor, and a drag here must not silently diverge from the
    //    engine's document.
    schematicCanvas_.setInterceptsMouseClicks(false, false);
    schematicCanvas_.setWantsKeyboardFocus(false);
    schematicCanvas_.setMouseClickGrabsKeyboardFocus(false);
    schematicCanvas_.setMouseCursor(juce::MouseCursor::NormalCursor);
    schematicCanvas_.setDocument(processor_.getCurrentDocument());
    syncSchematicLiveState();
    addAndMakeVisible(schematicCanvas_);

    // 6. Dual-Trace Oscilloscope
    oscilloscope_.setBuffers(&processor_.getInputScopeBuffer(), &processor_.getOutputScopeBuffer());
    oscilloscope_.setSampleRate(processor_.getHostSampleRate());
    addAndMakeVisible(oscilloscope_);

    // 7. Input Trim & Output Knobs
    inTrimKnob_.setAccentColor(juce::Colour(0xFF00E5A0)); // Teal
    inTrimKnob_.slider().setDoubleClickReturnValue(true, 0.0f);
    addAndMakeVisible(inTrimKnob_);
    inTrimAttach_ = std::make_unique<SliderAttachment>(processor_.getAPVTS(), "input_trim", inTrimKnob_.slider());

    outMasterKnob_.setAccentColor(juce::Colour(0xFFFFB800)); // Amber
    outMasterKnob_.slider().setDoubleClickReturnValue(true, 0.0f);
    addAndMakeVisible(outMasterKnob_);
    outMasterAttach_ = std::make_unique<SliderAttachment>(processor_.getAPVTS(), "output_master", outMasterKnob_.slider());

    mixKnob_.setAccentColor(juce::Colour(0xFFD8DEE9)); // Platinum
    mixKnob_.slider().setDoubleClickReturnValue(true, 100.0f);
    addAndMakeVisible(mixKnob_);
    mixAttach_ = std::make_unique<SliderAttachment>(processor_.getAPVTS(), "mix", mixKnob_.slider());

    // 8. Rebuild dynamic circuit potentiometers
    rebuildDynamicControls();

    // 9. Window configuration (spacious, wide layout)
    setSize(1080, 750);
    setResizable(true, true);
    setResizeLimits(880, 620, 1800, 1300);

    // 10. Start UI update timer at 30 Hz
    startTimerHz(30);

    // The status line says what is playing, so the plugin is never a mystery.
    statusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFF94A3B8));
    statusLabel_.setFont(editor::Fonts::monoSemiBold(12.0f));
    presetStepper_.setName(currentCircuitDescription());
    addAndMakeVisible(statusLabel_);
}

juce::String LiveWDFPluginEditor::currentCircuitDescription() const {
    const auto file = processor_.getCurrentCircuitFile();
    const auto& doc = processor_.getCurrentDocument();

    juce::String base;
    if (file.existsAsFile())
        base = file.getFileName();
    else if (doc.components.empty())
        base = "No circuit loaded";
    else
        base = doc.name.empty() ? juce::String("Restored circuit") : juce::String(doc.name);

    // **The status line answers "which circuit am I hearing, and did anything
    // about it not read cleanly".** A sheet that loads but carries a part the
    // library does not have, or no terminal part, is worth hearing about here
    // rather than nowhere.
    return base + loadNoteSuffix(doc);
}

LiveWDFPluginEditor::~LiveWDFPluginEditor() {
    if (factoryBrowser_ != nullptr)
        factoryBrowser_->dismiss();
    setLookAndFeel(nullptr);
    stopTimer();
}

void LiveWDFPluginEditor::rebuildDynamicControls() {
    // Attachments first: they hold a reference to the knob's slider, so the
    // slider must outlive the attachment, not the other way round.
    dynamicPotAttachments_.clear();
    dynamicPotKnobs_.clear();
    dynamicSwitchButtons_.clear();
    dynamicSwitchNames_.clear();

    // **One knob per pot group.** The processor grouped pots and rheostats by
    // their label; the group order is the parameter order (`pot_0`, ...), so a
    // ganged pair is one knob and one parameter driving both members.
    const auto potGroups = processor_.getPotGroups();
    for (size_t i = 0; i < potGroups.size() && i < static_cast<size_t>(kMaxDynamicPots); ++i) {
        const juce::String potLabel = juce::String(potGroups[i].name).toUpperCase();

        auto knob = std::make_unique<VintageKnob>(potLabel, "");
        knob->setAccentColor(juce::Colour(0xFFFFB800)); // Warm Amber
        knob->slider().setDoubleClickReturnValue(true, 0.5f);

        const juce::String paramId = "pot_" + juce::String(static_cast<int>(i));
        auto attach = std::make_unique<SliderAttachment>(processor_.getAPVTS(), paramId,
                                                         knob->slider());

        addAndMakeVisible(*knob);
        dynamicPotKnobs_.push_back(std::move(knob));
        dynamicPotAttachments_.push_back(std::move(attach));
    }

    // **One button per switch group.** Clicking any ganged switch throws all of
    // them: the processor owns the state and writes every member's controlled
    // resistors, so the drawing, the document and the sound agree.
    const auto switchGroups = processor_.getSwitchGroups();
    for (size_t i = 0; i < switchGroups.size() && i < static_cast<size_t>(kMaxDynamicSwitches); ++i) {
        const int groupIndex = static_cast<int>(i);
        dynamicSwitchNames_.push_back(juce::String(switchGroups[i].name).toUpperCase());

        auto button = std::make_unique<juce::TextButton>();
        button->setClickingTogglesState(true);
        button->setColour(juce::TextButton::buttonColourId,
                          juce::Colour(0xFF1A212D));
        button->setColour(juce::TextButton::buttonOnColourId,
                          juce::Colour(0xFF3A2A12));
        button->setColour(juce::TextButton::textColourOffId,
                          juce::Colour(0xFFE2E8F0));
        button->setColour(juce::TextButton::textColourOnId,
                          juce::Colour(0xFFFFAB00));
        button->setTooltip("Throw every switch named " +
                           juce::String(switchGroups[i].name));

        auto* raw = button.get();
        button->onClick = [this, groupIndex, raw] {
            processor_.setSwitchGroupB(groupIndex, raw->getToggleState());
            updateSwitchButton(groupIndex, *raw);
            // The canvas would catch up on the next timer tick; repainting it
            // now makes the throw land visually with the click.
            syncSchematicLiveState();
        };

        addAndMakeVisible(*button);
        dynamicSwitchButtons_.push_back(std::move(button));
        updateSwitchButton(groupIndex, *dynamicSwitchButtons_.back());
    }

    resized();
}

void LiveWDFPluginEditor::updateSwitchButton(int index, juce::TextButton& button) {
    if (index < 0 || index >= static_cast<int>(dynamicSwitchNames_.size()))
        return;

    const bool throwB = processor_.getSwitchGroupB(index);
    button.setButtonText(dynamicSwitchNames_[static_cast<size_t>(index)]);
    button.setToggleState(throwB, juce::dontSendNotification);
}

void LiveWDFPluginEditor::syncSchematicLiveState() {
    schematicCanvas_.setLiveControlState(processor_.getLivePotPositions(),
                                         processor_.getLiveSwitchStates());
}

void LiveWDFPluginEditor::openCircuitFileChooser() {
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Select .livewdf Circuit File",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.livewdf;*.json");

    const int chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    fileChooser_->launchAsync(chooserFlags, [this](const juce::FileChooser& fc) {
        const auto result = fc.getResult();
        if (result.existsAsFile())
            loadCircuitFile(result);
    });
}

bool LiveWDFPluginEditor::loadCircuitFile(const juce::File& file) {
    if (!processor_.loadCircuitFromFile(file)) {
        // **Say why.** A file that does not load used to fail silently, which is
        // the worst way to fail: the user picked a file and nothing happened.
        // The reason is in the user's words -- which part is joined to nothing,
        // or which legacy sheet the format refuses outright -- and not the
        // engine's.
        const juce::String reason = processor_.lastError();
        presetStepper_.setName(reason.isEmpty() ? juce::String("Could not open circuit")
                                                : "Could not open circuit -- " + reason);
        return false;
    }

    schematicCanvas_.setDocument(processor_.getCurrentDocument());
    syncSchematicLiveState();
    rebuildDynamicControls();
    presetStepper_.setName(currentCircuitDescription());
    presetStepper_.setCurrentFile(processor_.getCurrentCircuitFile());
    return true;
}

void LiveWDFPluginEditor::showFactoryBrowser() {
    if (factoryBrowser_ != nullptr)
        return;

    if (const auto factory = FactorySchematics::ensureInstalled(); factory.failed()) {
        presetStepper_.setName("Factory circuits unavailable -- " + factory.getErrorMessage());
        return;
    }

    using Browser = editor::PresetLibraryPanel;
    auto browser = std::make_unique<Browser>(Browser::Mode::Factory);
    browser->setSize(500, juce::jmax(320, juce::jmin(480, getHeight() - 100)));
    browser->setFactoryFolder(FactorySchematics::folder());
    browser->setCurrentFile(processor_.getCurrentCircuitFile());
    browser->setView(factoryView_);

    const juce::Component::SafePointer<Browser> panel(browser.get());
    auto& callout = juce::CallOutBox::launchAsynchronously(std::move(browser),
                                                           factoryBtn_.getBounds(), this);
    callout.setDismissalMouseClicksAreAlwaysConsumed(true);
    factoryBrowser_ = &callout;
    panel->onDismiss = [popup = factoryBrowser_] {
        if (popup != nullptr)
            popup->dismiss();
    };
    panel->onPresetChosen = [safe = juce::Component::SafePointer<LiveWDFPluginEditor>(this),
                             popup = factoryBrowser_, panel](const juce::File& file) {
        if (safe != nullptr && panel != nullptr && safe->loadCircuitFile(file)) {
            safe->factoryView_ = panel->view();
            safe->presetStepper_.setBrowseOrder(panel->visibleFactoryOrder());
        }
        if (popup != nullptr)
            popup->dismiss();
    };
    juce::MessageManager::callAsync([panel] {
        if (panel != nullptr)
            panel->focusSearch();
    });
}

// -----------------------------------------------------------------------------
// File Drag and Drop
// -----------------------------------------------------------------------------
bool LiveWDFPluginEditor::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& file : files) {
        if (file.endsWithIgnoreCase(".livewdf") || file.endsWithIgnoreCase(".json"))
            return true;
    }
    return false;
}

void LiveWDFPluginEditor::fileDragEnter(const juce::StringArray& files, int x, int y) {
    juce::ignoreUnused(files, x, y);
    isDraggingFile_ = true;
    repaint();
}

void LiveWDFPluginEditor::fileDragExit(const juce::StringArray& files) {
    juce::ignoreUnused(files);
    isDraggingFile_ = false;
    repaint();
}

void LiveWDFPluginEditor::filesDropped(const juce::StringArray& files, int x, int y) {
    juce::ignoreUnused(x, y);
    isDraggingFile_ = false;
    for (const auto& file : files) {
        if (file.endsWithIgnoreCase(".livewdf") || file.endsWithIgnoreCase(".json")) {
            if (loadCircuitFile(juce::File(file)))
                break;
        }
    }
    repaint();
}

// -----------------------------------------------------------------------------
// Timer Callback (Periodic UI Refresh)
// -----------------------------------------------------------------------------
void LiveWDFPluginEditor::timerCallback() {
    // Follow the host's rate. The editor can open before prepareToPlay and a
    // host can change the device rate while it stays open, so reading this once
    // in the constructor is not enough: the scope's timebase is in samples.
    oscilloscope_.setSampleRate(processor_.getHostSampleRate());

    // The parameter can move from automation or host state restore, not only
    // from the button, so the label follows the parameter rather than the click.
    if (const auto* stereo = processor_.getAPVTS().getRawParameterValue("stereo")) {
        const bool on = stereo->load() > 0.5f;
        if (stereoBtn_.getToggleState() != on)
            stereoBtn_.setToggleState(on, juce::dontSendNotification);
        stereoBtn_.setButtonText(on ? "STEREO" : "MONO");
    }

    // 1. Pass live probe voltages to the shared editor canvas
    schematicCanvas_.setNodeVoltages(processor_.getLiveProbeVoltages());

    // 2. Pass live audio signal activity for glowing signal flow pulses
    schematicCanvas_.setSignalActivity(processor_.getLiveSignalActivity());

    // 3. The knobs and switches are APVTS parameters, not edits to the loaded
    //    document. Overlay them on the canvas so the drawing follows the sound.
    syncSchematicLiveState();

    // 4. The Reload button follows whether there is a file to reload. A circuit
    //    restored from host state has none, so the button is dead -- and says
    //    why in its tooltip, rather than being a grey rectangle with no
    //    explanation.
    const bool canReload = processor_.canReload();
    presetStepper_.setCurrentFile(processor_.getCurrentCircuitFile());
    if (reloadBtn_.isEnabled() != canReload)
        reloadBtn_.setEnabled(canReload);

    reloadBtn_.setTooltip(canReload
                              ? "Re-read " + processor_.getCurrentCircuitFile().getFileName() +
                                    " from disk -- use this after saving in the editor"
                              : "No file loaded: load a .livewdf circuit to begin");
}

// -----------------------------------------------------------------------------
// Layout & Painting
// -----------------------------------------------------------------------------
void LiveWDFPluginEditor::paint(juce::Graphics& g) {
    // Technical dark chassis background for outer frame and knob panels
    g.setColour(juce::Colour(0xFF0F1216));
    g.fillAll();

    // Toolbar background
    auto bounds = getLocalBounds();
    g.setColour(juce::Colour(0xFF12151A));
    g.fillRect(bounds.removeFromTop(52));

    // Divider line below top bar
    g.setColour(juce::Colour(0xFF262E3B));
    g.drawHorizontalLine(52, 0.0f, static_cast<float>(getWidth()));

    // Divider lines around control strip
    const int knobStripY = getHeight() - 268;
    g.setColour(juce::Colour(0xFF14171C));
    g.fillRect(0, knobStripY, getWidth(), 118);
    g.setColour(juce::Colour(0xFF262E3B));
    g.drawHorizontalLine(knobStripY, 0.0f, static_cast<float>(getWidth()));
    g.drawHorizontalLine(knobStripY + 118, 0.0f, static_cast<float>(getWidth()));

    // Switch strip sits above the knobs, and only exists when the circuit has
    // an SPDT to throw.
    if (!dynamicSwitchButtons_.empty()) {
        constexpr int switchStripHeight = 36;
        const int switchStripY = knobStripY - switchStripHeight;
        g.setColour(juce::Colour(0xFF101318));
        g.fillRect(0, switchStripY, getWidth(), switchStripHeight);
        g.setColour(juce::Colour(0xFF262E3B));
        g.drawHorizontalLine(switchStripY, 0.0f, static_cast<float>(getWidth()));
    }

    // File Drag & Drop visual indicator
    if (isDraggingFile_) {
        g.setColour(juce::Colour(0xD00F1216));
        g.fillAll();
        g.setColour(juce::Colour(0xFFFFB800));
        g.drawRect(getLocalBounds(), 3);

        g.setFont(editor::Fonts::bold(20.0f));
        g.drawText("DROP .LIVEWDF CIRCUIT FILE TO LOAD", getLocalBounds(), juce::Justification::centred);
    }
}

void LiveWDFPluginEditor::resized() {
    auto bounds = getLocalBounds();

    // 1. Top Bar (height 52px)
    auto topBar = bounds.removeFromTop(52);
    aboutBtn_.setBounds(topBar.removeFromRight(70).reduced(6, 10));
    auto oversamplingControls = topBar.removeFromRight(120).reduced(6, 3);
    oversamplingCombo_.setBounds(oversamplingControls.removeFromTop(27));
    oversampleExportBtn_.setBounds(oversamplingControls);
    auto qualityControls = topBar.removeFromRight(128).reduced(6, 3);
    solveQualityCombo_.setBounds(qualityControls.removeFromTop(27));
    accurateExportBtn_.setBounds(qualityControls);
    stereoBtn_.setBounds(topBar.removeFromRight(88).reduced(6, 10));
    reloadBtn_.setBounds(topBar.removeFromRight(78).reduced(6, 10));
    loadFileBtn_.setBounds(topBar.removeFromRight(128).reduced(6, 10));
    factoryBtn_.setBounds(topBar.removeFromRight(92).reduced(6, 10));
    const int nameWidth = juce::jmax(0, topBar.getWidth() - 22);
    titleLabel_.setBounds(16, 7, juce::jmin(240, nameWidth), 20);
    presetStepper_.setBounds({16, 27, nameWidth, 18});

    // 2. Bottom Oscilloscope (height 150px)
    auto bottomScope = bounds.removeFromBottom(150);
    oscilloscope_.setBounds(bottomScope.reduced(4));

    // 3. Middle Knob Strip (height 118px)
    auto knobStrip = bounds.removeFromBottom(118).reduced(12, 4);

    // Left: Input Trim
    inTrimKnob_.setBounds(knobStrip.removeFromLeft(92));

    // Right: Mix & Output Master
    outMasterKnob_.setBounds(knobStrip.removeFromRight(92));
    mixKnob_.setBounds(knobStrip.removeFromRight(92));

    // Center: Dynamic Circuit Potentiometers. One row while every knob keeps a
    // readable width; once it cannot, the strip wraps onto a second row. An
    // amp with a graphical equalizer has twelve groups (seven on the body,
    // five on the EQ), and squeezing them into one row makes them unusable.
    if (!dynamicPotKnobs_.empty()) {
        const int count = static_cast<int>(dynamicPotKnobs_.size());
        const int available = knobStrip.getWidth();
        const int perRow = juce::jlimit(1, count, available / kMinDynamicPotKnobWidth);
        const int rows = (count + perRow - 1) / perRow;
        const int rowHeight = knobStrip.getHeight() / rows;
        int next = 0;
        for (int row = 0; row < rows && next < count; ++row) {
            const int inRow = std::min(perRow, count - next);
            const int potWidth = juce::jmin(92, available / inRow);
            auto slot = knobStrip.removeFromTop(rowHeight);
            if (rows > 1)
                slot.reduce(0, 1);
            for (int k = 0; k < inRow; ++k)
                dynamicPotKnobs_[static_cast<size_t>(next + k)]->setBounds(
                    slot.removeFromLeft(potWidth).reduced(2, 0));
            next += inRow;
        }
    }

    // 4. Dynamic Switch Strip, directly above the knobs.
    if (!dynamicSwitchButtons_.empty()) {
        auto switchStrip = bounds.removeFromBottom(36).reduced(12, 2);
        const int count = static_cast<int>(dynamicSwitchButtons_.size());
        const int buttonWidth = juce::jmin(160, switchStrip.getWidth() / count);
        int x = switchStrip.getX() + (switchStrip.getWidth() - buttonWidth * count) / 2;
        for (auto& button : dynamicSwitchButtons_) {
            button->setBounds(x, switchStrip.getY(), buttonWidth, switchStrip.getHeight());
            x += buttonWidth;
        }
    }

    // 5. Central schematic (shared editor canvas, remaining space)
    schematicCanvas_.setBounds(bounds.reduced(4));

    // The canvas is handed its document before it has a size, so the fit from
    // setDocument ran against a zero-sized component. Re-fit now that the
    // bounds are real, and again on every resize -- this is a read-only view,
    // so there is no user pan/zoom to preserve.
    schematicCanvas_.frameAll();
}

} // namespace livewdf
