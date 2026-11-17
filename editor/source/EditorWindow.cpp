#include "EditorWindow.h"
#include "EditorWindowInternal.h"
#include "DiagLog.h"
#include "EditorLayout.h"
#include "EditorLookAndFeel.h"
#include "gui/Fonts.h"
#include "gui/AboutDialog.h"
#include "gui/PluginLayoutDialog.h"

#include <cmath>

namespace livewdf::editor {
// ============================================================================
// Window
// ============================================================================

EditorWindow::EditorWindow()
    : DocumentWindow("LiveWDF Editor", glow::getColourBackground(),
                     DocumentWindow::allButtons) {
    setUsingNativeTitleBar(true);
    setContentOwned(&content_, false);
    setResizable(true, true);
    setResizeLimits(900, 600, 5000, 4000);
    centreWithSize(1360, 860);
    setVisible(true);
}

EditorWindow::~EditorWindow() = default;

void EditorWindow::closeButtonPressed() {
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

bool EditorWindow::openFile(const juce::File& file) { return content_.openFile(file); }
bool EditorWindow::saveFile(const juce::File& file) { return content_.saveFile(file); }

// ============================================================================
// Content
// ============================================================================

EditorWindow::Content::Content() {
    // The demo sheet is constructed by the canvas. Keep a copy now, before any
    // edit can change it, so the preset library's "Starter" is the same circuit
    // the editor opened with rather than a second, slowly diverging version.
    starterDocument_ = editor_.getDocument();
    starterDocument_.name = "RC Lowpass";

    presetFolder_ = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                        .getChildFile("LiveWDF");
    presetLibrary_.setFolder(presetFolder_);

    // --- Canvas callbacks -------------------------------------------------
    editor_.onChanged = [this] {
        // Record the sheet's new state *after* the edit. This is what lets the
        // canvas mutate freely and push no snapshot itself: the "before" is
        // always whatever was last recorded here.
        recordUndoState();
        markContentChanged();
    };
    editor_.onNotice = [this](const juce::String& text) {
        // **Said in amber, not red.** A notice is not an error: a wire that
        // ended in mid-air is a thing to know about, and the build may well
        // work. The red is kept for the refusals.
        statusLabel_.setText(text, juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, glow::getColourAmber());
        addConsoleNotice(text);
    };
    editor_.onSelectionChanged = [this] {
        // **Diagnostic.** The crash being chased was "clicking a part with a
        // picker takes the editor down", and the log is what turns that into a
        // story: the selection, and what the inspector did about it.
        diagLog("selection: '" + editor_.selectedPartId() + "'");

        // The inspector edits whatever the canvas has selected. The canvas
        // reports the selection; this reads it back, so the panel stays dumb and
        // never has to know how selection works.
        //
        // **Wires are a second selection, and they need their own target.** Parts
        // and wires are selected separately (Celine's split, which we keep), so
        // showing the part target whenever a wire is picked left the inspector
        // blank while the canvas clearly had something selected -- the "I clicked
        // a wire and it doesn't say wire" bug. A wire has no fields to edit, but
        // it does have facts worth reading: which way it runs and which nets its
        // ends landed on.
        InspectorPanel::WireInfo wire;
        if (editor_.selectedPartId().empty() && editor_.selectedWireIds().size() == 1) {
            const int id = editor_.selectedWireIds().front();
            if (const auto* w = editor_.selectedWire(id)) {
                wire.id = w->id;
                wire.ax = w->ax; wire.ay = w->ay;
                wire.bx = w->bx; wire.by = w->by;

                // Net names come from the model's own extraction, so the inspector
                // cannot disagree with what the solver sees. `netOfWire` is
                // parallel to `getWires()`, so the index is found by id.
                const auto nets = editor_.schematic().extractNets();
                const auto& wires = editor_.schematic().getWires();
                for (size_t i = 0; i < wires.size() && i < nets.netOfWire.size(); ++i) {
                    if (wires[i].id != id)
                        continue;
                    const int n = nets.netOfWire[i];
                    if (n >= 0 && n < static_cast<int>(nets.netNames.size())) {
                        wire.netA = juce::String(nets.netNames[static_cast<size_t>(n)]);
                        wire.netB = wire.netA;   // a wire is one net end to end
                    }
                    break;
                }
            }
        }

        if (wire.valid()) {
            inspector_.setWireTarget(wire);
        } else {
            inspector_.setTarget(&editor_.getDocument(), editor_.selectedPartId());
        }

        // **The scope follows the selection.** A scope probe stamps nothing into
        // the circuit -- it only names a net -- so the only way to say "watch
        // *this* one" is to select it. Selecting a probe moves the scope's eye
        // there; selecting anything else puts it back on the first probe (or on
        // the output, when the sheet has none).
        {
            const auto& doc = editor_.getDocument();
            const std::string selected = editor_.selectedPartId();
            const std::string net = doc.probeNetFor(selected);
            if (!net.empty())
                session_.watchProbe(selected, net);
            else
                session_.watchProbe("", "");
        }
    };
    editor_.onToolChanged = [this] {
        // Show what is actually armed, including the canvas dropping back to
        // Select after a placement.
        const auto tool = editor_.getTool();
        selectBtn_.setToggleState(tool == SchematicEditor::Tool::Select, juce::dontSendNotification);
        wireBtn_.setToggleState(tool == SchematicEditor::Tool::Wire, juce::dontSendNotification);
        deleteBtn_.setToggleState(tool == SchematicEditor::Tool::Delete, juce::dontSendNotification);

        // The palette's Wire row is the same fact seen from the other panel, so
        // it lights from here rather than keeping a second idea of "active".
        //
        // The part highlight follows the canvas too. Escape drops the canvas
        // back to Select, and this is what clears the row it was armed with;
        // the palette is never allowed to keep a highlight the tool has lost.
        palette_.setArmed(tool == SchematicEditor::Tool::Place
                              ? editor_.pendingPartSpec()
                              : nullptr);
        palette_.setWireArmed(tool == SchematicEditor::Tool::Wire);
    };

    // The end of a drag, so the next one starts a fresh undo step rather than
    // amending the same record -- and this is the moment Undo becomes reachable:
    // while the pointer is down the gesture has no step to take back, and the
    // button says the same as `undo()`'s refusal (see `updateUndoButtons`).
    editor_.onGestureEnd = [this] {
        gestureRecorded_ = false;
        updateUndoButtons();
    };

    // The canvas asks; the history lives here.
    editor_.onUndoRequested = [this] { undo(); };
    editor_.onRedoRequested = [this] { redo(); };

    // --- Palette -> canvas ------------------------------------------------
    palette_.onPartChosen = [this](const PartSpec& spec) {
        editor_.armPlacement(spec);
    };

    // The Wire row arms the Wire tool. Ported from Celine's `onWireChosen`: the
    // wire is a tool, not a part, so it does not go through `armPlacement`.
    palette_.onWireChosen = [this] {
        editor_.setTool(SchematicEditor::Tool::Wire);
    };

    // --- Inspector -> canvas ---------------------------------------------
    // The inspector edits the *document*, but the canvas owns the model that the
    // document is generated from. So an inspector edit has to be pushed into the
    // model and read back out as a fresh document, or the next canvas edit would
    // overwrite it from the model. The canvas is told to reload and the engine
    // recompiles.
    inspector_.onChanged = [this] {
        editor_.reloadFromDocument();

        // **The inspector's edits are undoable too.** The canvas says `onChanged`
        // when it mutates the model, so the history is written there; the
        // inspector mutates the *document* and never passes through the canvas's
        // notify, so it has to do the bookkeeping itself. Without the record,
        // typing 40 over a selected 47K resistor is the one visible edit Undo
        // cannot take back.
        recordUndoState();
        markContentChanged();
    };

    // **Type a digit over one selected part to edit its value.** The canvas
    // collects the number (it is the thing under the eye) and hands it here on
    // Enter; the panel writes it through the same parse its fields use, and the
    // `inspector_.onChanged` path above then does exactly what a click-and-type
    // into a field does: reload the model, light the Rebuild amber.
    editor_.onQuickValueEdit = [this](const juce::String& text) {
        inspector_.quickValueEdit(text);
    };

    // --- Toolbar ----------------------------------------------------------
    newBtn_.onClick = [this] { newTab(); };
    openBtn_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Open .livewdf circuit",
                                                       juce::File::getSpecialLocation(
                                                           juce::File::userDocumentsDirectory),
                                                       kFileWildcard);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                                  juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  const auto f = fc.getResult();
                                  if (f != juce::File()) openFile(f);
                              });
    };
    saveBtn_.onClick = [this] { saveCurrentTabOrAsk(); };
    saveAsBtn_.onClick = [this] { saveAs(); };
    frameBtn_.onClick = [this] { editor_.frameAll(); };
    rebuildBtn_.onClick = [this] { rebuildNow(); };

    // --- The tab strip ------------------------------------------------------
    tabBar_.onTabSelected = [this](int index) { selectTab(index); };
    tabBar_.onTabClosed = [this](int index) { closeTab(index); };

    // The two compact actions at the right of the strip. They live beside the
    // tabs because they act on the open sheets, not on the drawing tools in the
    // toolbar.
    presetsBtn_.onClick = [this] { togglePresetLibrary(); };
    editMenuBtn_.onClick = [this] { showEditMenu(); };

    presetLibrary_.onNewSheet = [this] {
        closePresetLibrary();
        newTab();
    };
    presetLibrary_.onStarterChosen = [this] {
        closePresetLibrary();
        openStarterTab();
    };
    presetLibrary_.onPresetChosen = [this](const juce::File& file) {
        closePresetLibrary();
        openFile(file);
    };
    presetLibrary_.onChooseFolder = [this] { choosePresetFolder(); };
    presetLibrary_.onSaveCurrent = [this] {
        closePresetLibrary();
        saveCurrentToPresetFolder();
    };
    presetLibrary_.onDismiss = [this] { closePresetLibrary(); };
    presetDismisser_.onClick = [this] { closePresetLibrary(); };

    // --- The message console ------------------------------------------------
    messagesBtn_.onClick = [this] { toggleConsole(); };
    console_.onMessageClicked = [this](const juce::String& partId) {
        // **The row is a pointer, not a second inspector.** Clicking it selects
        // the part on the sheet; the existing selection path then updates the
        // inspector and the scope. The console stays open so the next row is one
        // click away.
        if (partId.isEmpty())
            return;
        editor_.selectPart(partId.toStdString());
        editor_.grabKeyboardFocus();
    };

    // The rulers follow the pointer. Driven by the canvas's move rather than by
    // polling on the timer: a ruler redraws on every move, and only a move should
    // make it. `(-1,-1)` on exit blanks the mark instead of leaving it stranded.
    editor_.onPointerMoved = [this](juce::Point<float> p) {
        topRuler_.setCursor(p);
        leftRuler_.setCursor(p);
    };
    selectBtn_.onClick = [this] { editor_.setTool(SchematicEditor::Tool::Select); };
    wireBtn_.onClick = [this] { editor_.setTool(SchematicEditor::Tool::Wire); };
    deleteBtn_.onClick = [this] { editor_.setTool(SchematicEditor::Tool::Delete); };
    undoBtn_.onClick = [this] { undo(); };
    redoBtn_.onClick = [this] { redo(); };

    layoutBtn_.onClick = [this] {
        editor_.syncDocument();
        auto* dlg = new PluginLayoutDialog(editor_.getDocument());
        dlg->onApply = [this](std::vector<std::string> pOrder, std::vector<std::string> sOrder) {
            editor_.schematic().setPotOrder(pOrder);
            editor_.schematic().setSwitchOrder(sOrder);
            editor_.syncDocument();
            editor_.getDocument().potOrder = pOrder;
            editor_.getDocument().switchOrder = sOrder;
            markContentChanged();
        };
        dlg->setBounds(getLocalBounds());
        addAndMakeVisible(dlg);
    };

    aboutBtn_.onClick = [this] {
        AboutDialog::show(this, AboutDialog::Mode::Editor);
    };

    // **The keyboard card.** The shortcuts were a complaint of being *there*
    // nowhereto be found: the fix is a surface to read them from, and the About
    // card is already that surface -- a button, a scrim, Esc closes. Reusing it
    // means "how do I close this" cannot disagree between the two cards.
    keysBtn_.onClick = [this] {
        AboutDialog::show(this, AboutDialog::Mode::Keys);
    };

    // The three tool buttons are a radio group: the canvas can change the tool
    // itself (a bare letter, or dropping back to Select after a placement), so
    // the buttons must be showable rather than only settable.
    for (auto* b : {&selectBtn_, &wireBtn_, &deleteBtn_}) {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(9001, juce::dontSendNotification);
        addAndMakeVisible(*b);
    }
    selectBtn_.setToggleState(true, juce::dontSendNotification);

    for (auto* b : {&newBtn_, &openBtn_, &saveBtn_, &saveAsBtn_, &rebuildBtn_, &frameBtn_, &undoBtn_, &redoBtn_, &layoutBtn_, &aboutBtn_, &keysBtn_})
        addAndMakeVisible(*b);

    // Undo steps a *gesture*, not a grid square, and the tooltip is where that
    // answer gets said to the user before they press it.
    undoBtn_.setTooltip("Undo the last gesture");
    redoBtn_.setTooltip("Put that gesture back");

    // The strip actions are quiet by default; they should read as part of the
    // tab strip, not compete with the toolbar.
    for (auto* b : {&presetsBtn_, &editMenuBtn_}) {
        b->setColour(juce::TextButton::buttonColourId, glow::getColourPanel());
        b->setColour(juce::TextButton::buttonOnColourId, glow::getColourElevated());
        b->setColour(juce::TextButton::textColourOffId, glow::getColourTextSecondary());
        b->setColour(juce::TextButton::textColourOnId, glow::getColourTextPrimary());
        addAndMakeVisible(*b);
    }
    presetsBtn_.setTooltip("Open the preset library");
    editMenuBtn_.setTooltip("Edit the selected parts");

    messagesBtn_.setColour(juce::TextButton::buttonColourId, glow::getColourPanel());
    messagesBtn_.setColour(juce::TextButton::buttonOnColourId, glow::getColourElevated());
    messagesBtn_.setColour(juce::TextButton::textColourOffId, glow::getColourTextSecondary());
    messagesBtn_.setColour(juce::TextButton::textColourOnId, glow::getColourTextPrimary());
    messagesBtn_.setTooltip("Show or hide the build messages");
    addAndMakeVisible(messagesBtn_);

    titleLabel_.setJustificationType(juce::Justification::centredRight);
    titleLabel_.setColour(juce::Label::textColourId, glow::getColourHeaderText());
    titleLabel_.setFont(Fonts::bold(15.5f));
    addAndMakeVisible(titleLabel_);

    statusLabel_.setJustificationType(juce::Justification::centredLeft);
    statusLabel_.setColour(juce::Label::textColourId, glow::getColourTextSecondary());
    statusLabel_.setFont(Fonts::mono(11.5f));
    addAndMakeVisible(statusLabel_);

    addAndMakeVisible(topRuler_);
    addAndMakeVisible(leftRuler_);
    addAndMakeVisible(editor_);
    addAndMakeVisible(palette_);
    addAndMakeVisible(inspector_);
    addAndMakeVisible(nodeTable_);
    addAndMakeVisible(scope_);
    addAndMakeVisible(tabBar_);

    addAndMakeVisible(console_);
    console_.setVisible(false);
    console_.setComponentID("messageConsole");

    // Overlay order: the dismisser catches clicks anywhere behind the library,
    // then the library itself is on top of the dismisser.
    addChildComponent(presetDismisser_);
    addChildComponent(presetLibrary_);
    presetLibrary_.setComponentID("presetLibrary");

    scope_.setBuffers(&scopeIn_, &scopeOut_);
    scope_.setSampleRate(48000.0);

    // The canvas opens on its own small demo sheet (see `SchematicEditor`'s
    // constructor), so the editor is never a blank page with nothing to try. The
    // document is whatever that demo is, read back out of the canvas.
    pendingRebuild_ = true;
    rebuildNow();
    updateRebuildButton();

    // **The first tab**, holding the demo sheet the canvas opened on.
    Tab first;
    first.document = editor_.getDocument();
    first.file = juce::File();
    first.view = editor_.viewState();
    tabs_.push_back(std::move(first));
    currentTab_ = 0;
    tabBar_.setTabs(tabNames(), currentTab_);
    refreshTitle();

    startTimerHz(30);
}

EditorWindow::Content::~Content() { stopTimer(); }

} // namespace livewdf::editor