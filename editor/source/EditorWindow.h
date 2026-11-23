#pragma once

// The editor window: the frame that holds the canvas, the panels, and the scope,
// and the one place that knows how they talk to each other.
//
// Why one window class owns the wiring: the panels are deliberately dumb -- the
// palette emits a type, the inspector emits "changed", the canvas emits "changed"
// and "selection changed". *Something* has to turn those into "recompile and
// refresh the tables", and that something is here. Spreading that logic across
// the panels would mean each panel needing an engine, which is the coupling this
// design exists to avoid.
//
// The test signal driving the scope is generated here, not pushed in by a host,
// because the editor has no host: it is a drawing tool, and the scope exists to
// show the drawn circuit doing something, not to be a DAW meter.

#include "CircuitSession.h"
#include "InspectorPanel.h"
#include "MessageConsole.h"
#include "NodeTablePanel.h"
#include "PresetLibraryPanel.h"
#include "format/PartLibrary.h"
#include "PartPalettePanel.h"
#include "Ruler.h"
#include "SchematicEditor.h"
#include "SchematicHistory.h"
#include "TabBar.h"
#include "gui/ScopeView.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <deque>
#include <functional>
#include <future>
#include <memory>

namespace livewdf::editor {

class EditorWindow : public juce::DocumentWindow {
public:
    EditorWindow();
    ~EditorWindow() override;

    void closeButtonPressed() override;

    /// Open a `.livewdf` file. Returns false (and shows an alert) on failure.
    bool openFile(const juce::File& file);
    /// Save the current document to `file`.
    bool saveFile(const juce::File& file);

    // ---------------------------------------------------------------------------
    // The self-test
    //
    // These drive the window the way a user does -- put a part on the sheet,
    // click it, change its picker -- and report whether it survived. The
    // paths they cover are the ones that mix the model, the document and the
    // widgets, which no unit test can reach: a picker that fires a change, a
    // host that rebuilds the panel in answer, a canvas that replaces the
    // document underneath. A crash in one of those took the editor down.
    // ---------------------------------------------------------------------------

    /// Put a part of `type` on the sheet and select it.
    bool selfTestPlacePart(const juce::String& type);

    /// Select the part of `type` that is on the sheet.
    bool selfTestSelectPart(const juce::String& type);

    /// Change the part of `type`'s picker to a different choice.
    bool selfTestChangeFirstChoice(const juce::String& type);

    /// Recompile the engine from the current document -- the Rebuild button.
    bool selfTestRebuild();

    /// Arm a part by pressing its keyboard shortcut, and check that the part
    /// that got armed is the one the shortcut names.
    ///
    /// **Why the self-test and not a unit test.** The shortcut is matched in the
    /// canvas, and the canvas is where the tool letters and the command
    /// shortcuts are claimed first -- so the interesting question is not "does
    /// the matcher work" (a unit test has that) but "does the *right* part get
    /// armed when the key is pressed, with the other letters still doing what
    /// they did". That is a question about the canvas as a whole.
    bool selfTestArmByShortcut(char letter, bool withCommand, const juce::String& expectedType);

    /// Click on a wire and check that the *wire* is what got selected.
    ///
    /// **The complaint this checks.** "Selecting some items selects unwanted
    /// ones, and wires are especially hard to select." A part's hit box used to
    /// be a quarter of the space around it, so a wire that reached a part could
    /// not be picked at all near the part -- the part won every time.
    bool selfTestSelectWire();

    /// Click just *beside* a part -- where its symbol is not -- and check that
    /// nothing was selected.
    ///
    /// The other half of the same complaint: a part used to claim a whole
    /// grid square of space around itself, so clicking near one grabbed it.
    bool selfTestMissPart();

    /// Drag a part and check that the sheet says it has changed.
    ///
    /// **A drag that moved a part and said nothing left two things wrong:** the
    /// Rebuild button stayed locked, because nothing told the host the circuit
    /// had changed; and the document was not regenerated, so pressing Rebuild
    /// would have compiled the circuit with the part *where it used to be* --
    /// the move silently lost.
    bool selfTestDragPart();

    /// What the last self-test step found, when it found the wrong thing.
    ///
    /// A self-test that only says "failed" makes the reader guess; this says
    /// which part of the step went wrong.
    juce::String selfTestNote() const { return selfTestNote_; }

    /// How many times the inspector has rebuilt.
    ///
    /// The self-test checks this, because the crash was a *loop*: a picker sets
    /// its own opening position, JUCE posts a change for that, the panel
    /// rebuilt itself in answer, and the editor spun building and destroying
    /// widgets as fast as the message loop would go. It took a widget down with
    /// it, on exactly the parts with a picker.
    int inspectorRebuildCount() const { return content_.inspector().rebuildCount(); }

    /// Whether the sheet has changed since the last build -- which is what the
    /// Rebuild button's colour says.
    ///
    /// The self-test checks it after a *drag*, because a drag that moved a part
    /// and said nothing left the button locked: the host was never told the
    /// circuit had changed, and the document was not regenerated -- so pressing
    /// Rebuild would have compiled the circuit with the part where it used to
    /// be.
    bool isRebuildPending() const { return content_.isRebuildPending(); }

    /// Open a new, empty tab -- the "New" button's action, and the self-test's
    /// way of having a second circuit to switch between.
    void newTab() { content_.newTab(); }

    /// The tabs' titles, in order. The current one is the one the title bar
    /// names.
    std::vector<juce::String> tabTitles() const { return content_.tabTitles(); }

    /// The title bar's text: the *current* tab's name and its modified mark.
    ///
    /// **It follows the tab.** It used to be the window's file -- so a second
    /// tab's title said the first tab's name, and the answer to "which circuit
    /// am I in" was the wrong circuit.
    juce::String titleText() const { return content_.titleText(); }

    /// Open and close the preset library through the same button a user
    /// presses, and check that the overlay is on screen and laid out.
    ///
    /// This catches the failure a screenshot cannot: a panel that is technically
    /// visible with a zero width, or a button that was never wired to it.
    bool selfTestPresetLibrary();

    /// Toggle the message drawer through its status-bar button and check that
    /// it takes height from the sheet and gives it back.
    bool selfTestMessageConsole();

    /// Put one unconnected part on a fresh sheet, rebuild, and check that the
    /// drawer opened itself with the complaint in it.
    bool selfTestMessageConsoleAutoOpen();

    /// Drag a part, and check the undo that follows the drag: Undo is dark
    /// *during* it and lights when it lands; one press takes the whole drag
    /// back to where it started; and redo puts it where it ended.
    ///
    /// **Why a self-test and not a unit test**: `SchematicHistory` has its own
    /// assertions, and they pass with the window wired up wrong. What is not
    /// pinned there is the window's half -- that a gesture in progress is not a
    /// step, and that a drag is one step rather than one per grid square.
    bool selfTestUndoRedo();

    /// Aim the inspector at a selected resistor and type a new value into it --
    /// an edit the panel posts asynchronously, so this step only posts it.
    bool selfTestInspectorValueEdit();

    /// On a later tick, when the message loop has delivered that edit: check
    /// Undo is reachable, and that undoing puts the old number back and redo
    /// puts the new one on again.
    bool selfTestInspectorUndo();

private:
    // -----------------------------------------------------------------------
    // The content component
    // -----------------------------------------------------------------------
    class Content : public juce::Component, private juce::Timer {
    public:
        Content();
        ~Content() override;

        void paint(juce::Graphics& g) override;
        void resized() override;

        bool openFile(const juce::File& file, bool replaceTab = false);
        bool saveFile(const juce::File& file);

        /// Open a new, empty tab.
        void newTab();

        /// The tabs' titles, in order.
        std::vector<juce::String> tabTitles() const;

        /// The title bar's text: the current tab's name and its modified mark.
        juce::String titleText() const { return titleLabel_.getText(); }

        wdf::LiveWDFDocument& doc() noexcept { return editor_.getDocument(); }

        /// Recompile the engine from the current document, now.
        ///
        /// The Rebuild button's action. It is public because the self-test has
        /// to press it: changing a part's model changes the *engine*, and the
        /// engine is what the crash was in -- the part that has to be
        /// recompiled after a model change, and the click that came after.
        void rebuild() { rebuildNow(); }

        /// Whether the sheet has changed since the last build.
        bool isRebuildPending() const { return pendingRebuild_; }

        /// The sheet and the inspector, for the self-test.
        ///
        /// Public because the self-test has to drive the window the way a user
        /// does -- put a part on the sheet, click it, change its picker -- and
        /// that means reaching the canvas and the panel. It is a *reader* of the
        /// window, and it is how the editor checks itself.
        SchematicEditor& editor() noexcept { return editor_; }
        InspectorPanel& inspector() noexcept { return inspector_; }
        const InspectorPanel& inspector() const noexcept { return inspector_; }

    private:
        /// A transparent sheet-wide catcher for "click outside the preset
        /// library". It is a separate component because the library is an
        /// overlay: the editor behind it must see one click, and that click must
        /// close the panel rather than edit a part by accident.
        class Dismisser : public juce::Component {
        public:
            std::function<void()> onClick;

            void mouseDown(const juce::MouseEvent&) override {
                if (onClick) onClick();
            }
        };

        void timerCallback() override;
        void startSavedCircuitPreparation();

        std::deque<juce::File> pendingPreparations_;
        std::future<void> savedCircuitPreparation_;

        // ---------------------------------------------------------------------
        // Tabs
        //
        // **One open circuit per tab.** A schematic editor is a place where more
        // than one circuit is open at a time -- a filter and the amplifier it
        // goes in, the original and the copy being edited -- and one canvas
        // and one "Open" button made every one of them a round trip through the
        // file system.
        //
        // **What a tab owns.** The document, the undo history, the file it came
        // from, the selection and the view. The document and the history are
        // *the tab's* rather than the window's, because undo must not reach
        // across from one circuit into another: undoing a step in the filter
        // must not undo a step in the amplifier.
        // ---------------------------------------------------------------------
        struct Tab {
            wdf::LiveWDFDocument document;
            SchematicHistory history;
            juce::File file;

            /// Whether this tab's sheet differs from its file on disk. It is the
            /// tab's own mark, because one tab's save must not clear another's.
            bool dirty = false;

            /// The selection and the view, so a tab comes back as it was left.
            std::vector<std::string> selectedParts;
            std::vector<int> selectedWires;
            SchematicEditor::ViewState view;
        };

        /// Put the canvas's current state into the current tab, so it can be
        /// put back when the tab is chosen again.
        void saveCurrentTab();

        /// Put a tab's state onto the canvas.
        void loadTab(int index);

        /// Choose a tab: the current one is saved, the new one is loaded.
        void selectTab(int index);

        /// Close a tab. The last tab cannot be closed: an editor with no circuit
        /// open has nothing to edit.
        void closeTab(int index);

        /// The file the current tab is from, or an empty file when it has never
        /// been saved. **The tab's file is the only one there is** -- a window
        /// with two tabs has two files, and the second tab's Save must not
        /// write over the first tab's.
        juce::File currentFile() const;

        /// Save the current tab. A tab that has never been saved asks where to
        /// put it, rather than writing over some *other* tab's file.
        void saveCurrentTabOrAsk();

        /// Save the current tab under a name the user chooses, starting in
        /// `folder`.
        void saveAsToFolder(const juce::File& folder);

        /// Save the current tab under a name the user chooses.
        void saveAs();

        /// Open a document, optionally reusing the active tab for preset steps.
        /// Modified tabs always keep their document.
        void openDocumentTab(wdf::LiveWDFDocument document, const juce::File& file,
                             bool replaceTab = false);

        /// Open the built-in RC lowpass that the canvas shows on first launch.
        void openStarterTab();

        /// The names of the tabs, for the strip.
        std::vector<std::string> tabNames() const;

        /// The current tab's name, for the title.
        juce::String currentTabName() const;
        /// Recompile the engine from the current document and refresh every
        /// reader. Only ever called from the Rebuild seam (`rebuildNow`), because
        /// compiling is the expensive step and drawing must not pay for it.
        void rebuildEngine();

        /// Push the engine's DC results into the table and the canvas probes.
        void refreshOperatingPoint();

        /// Replace the console's headline and rows, and open it when the result
        /// contains a problem. The console is a drawer, not a permanent column:
        /// it takes height only while it has something worth reading.
        void presentMessages(juce::String headline, MessageConsole::Severity severity,
                             std::vector<MessageConsole::Message> messages);

        /// Toggle the console drawer from the status bar.
        void toggleConsole();

        /// Keep the status-bar button and the drawer's visibility in step with
        /// the last build's problems and the user's last choice.
        void refreshConsole();

        /// Add a one-line notice to the console without throwing away the build
        /// result. Notices are not problems, so they do not force the drawer
        /// open; they just appear if it is already open.
        void addConsoleNotice(const juce::String& text);

        /// Show or hide the preset library dropdown.
        void showPresetLibrary();
        void closePresetLibrary();
        void togglePresetLibrary();

        /// Put the open dropdown under its button, clipped to the window.
        void placePresetLibrary();

        /// Choose the folder the library lists.
        void choosePresetFolder();

        /// Save the current tab into the preset folder, asking for a name when
        /// it has never been saved.
        void saveCurrentToPresetFolder();

        /// The compact Edit menu in the tab strip: the canvas commands that
        /// otherwise only exist as keyboard shortcuts.
        void showEditMenu();

        /// Set the title bar text from the file name and the *modified* flag.
        void refreshTitle();

        /// Compile now and clear the pending flag. This is the Rebuild button's
        /// action and the only caller of `rebuildEngine`.
        void rebuildNow();

        /// The sheet changed. Two independent flags, because they mean different
        /// things: `pendingRebuild_` is what you can *hear* falling behind, while
        /// the canvas's dirty flag is what differs from the file on disk. Editing
        /// sets both; only a compile clears the first and only a save clears the
        /// second.
        void markContentChanged();
        /// Colour the Rebuild button from `pendingRebuild_`. Gathered in one
        /// function so a theme change agrees with every other place that reads it,
        /// instead of three setters slowly drifting apart.
        void updateRebuildButton();

        /// Step a short block of a test signal and fill the scope buffers.
        void pumpScope();

        /// Give the scope the sheet's scope probes: one buffer and one colour
        /// each, so every probe's signal is drawn.
        void syncScopeProbes();

        /// Drop the scope's history. Called when the *document* changes (open,
        /// new tab, tab switch), not on every rebuild: a probe's trace is worth
        /// keeping across an edit, but a signal from the previous circuit is
        /// not a signal from this one.
        void clearScopeHistory();

        /// Record the sheet's new state in the history (see `SchematicHistory`).
        /// Called from `editor_.onChanged`, so the state before an edit is
        /// whatever was last recorded and no pre-mutation hook is needed. A drag
        /// is one step: while the canvas reports `isMidGesture()`, everything
        /// amends the same record, which `onGestureEnd` closes.
        ///
        /// **The inspector's own edits call this too** -- the panel edits the
        /// document and never passes through the canvas's notify, so it would
        /// otherwise be the one visible edit Undo could not take back.
        void recordUndoState();

        /// Restore a state the history handed back, without rebuilding the engine
        /// from scratch and without rewinding the knobs -- see the note in the
        /// implementation. Refused while a gesture is running: a drag that has
        /// not landed is not a step yet.
        void undo();
        void redo();
        void updateUndoButtons();

        SchematicEditor editor_;

        Ruler topRuler_{Ruler::Axis::Horizontal, editor_};
        Ruler leftRuler_{Ruler::Axis::Vertical, editor_};
        PartPalettePanel palette_;
        InspectorPanel inspector_;
        NodeTablePanel nodeTable_;
        ScopeView scope_;
        TabBar tabBar_;

        /// The dropdown preset library and its click-away catcher.
        PresetLibraryPanel presetLibrary_;
        Dismisser presetDismisser_;

        /// The bottom message drawer. It sits above the status bar and is laid
        /// out only while open, so closing it gives the sheet its height back.
        MessageConsole console_;

        CircuitSession session_;

        /// The open circuits, and which one is showing.
        std::vector<Tab> tabs_;
        int currentTab_ = 0;

        /// The drawing's undo/redo stack. Owned here, not by the canvas -- the
        /// stack is a property of the document, not of drawing.
        SchematicHistory history_;
        /// True once a mid-gesture change has been recorded, so the rest of the
        /// gesture amends rather than pushing one step per grid square.
        bool gestureRecorded_ = false;
        /// True while a restored state is being put back on the sheet, so the
        /// change it announces is not mistaken for a fresh edit.
        bool applyingHistory_ = false;
        /// What you can *hear* is behind: an edit has landed but not been compiled.
        /// Independent of the canvas's dirty flag, which is about the file on disk.
        bool pendingRebuild_ = false;

        // Scope plumbing.
        ScopeAudioBuffer scopeIn_;
        ScopeAudioBuffer scopeOut_;
        double scopePhase_ = 0.0;

        /// One buffer per scope probe on the sheet, and the probe's name beside
        /// it. The scope draws one trace per probe, each on its own colour.
        ///
        /// The buffers are held by pointer because a scope buffer's write
        /// position is an atomic -- it cannot be copied or moved, so it cannot
        /// live in a vector that reallocates.
        std::vector<std::pair<juce::String, std::unique_ptr<ScopeAudioBuffer>>> scopeProbes_;

        juce::TextButton newBtn_{"New"};
        juce::TextButton openBtn_{"Open"};
        juce::TextButton saveBtn_{"Save"};
        juce::TextButton saveAsBtn_{"Save As"};
        juce::TextButton rebuildBtn_{"Rebuild"};
        juce::TextButton frameBtn_{"Fit"};
        juce::TextButton selectBtn_{"Select"};
        juce::TextButton wireBtn_{"Wire"};
        juce::TextButton deleteBtn_{"Delete"};
        juce::TextButton undoBtn_{"Undo"};
        juce::TextButton redoBtn_{"Redo"};
        juce::TextButton layoutBtn_{"Layout"};
        juce::TextButton aboutBtn_{"About"};
        juce::TextButton keysBtn_{"Keys"};
        juce::TextButton presetsBtn_{"Presets"};
        juce::TextButton editMenuBtn_{"Edit"};
        juce::TextButton messagesBtn_{"Messages"};
        juce::Label statusLabel_;
        juce::Label titleLabel_;
        FactoryPresetStepper presetStepper_{*this, titleLabel_};

        std::unique_ptr<juce::FileChooser> chooser_;
        std::unique_ptr<juce::FileChooser> folderChooser_;

        // The console's retained content, so a notice can be added without
        // discarding the build result that preceded it.
        juce::String consoleHeadline_;
        MessageConsole::Severity consoleHeadlineSeverity_ = MessageConsole::Severity::Info;
        std::vector<MessageConsole::Message> consoleMessages_;
        bool consoleOpen_ = false;
        bool consoleAutoOpened_ = false;

        /// The starter sheet, captured before the first edit. Opening it from
        /// the preset library is the same circuit the editor first showed, not
        /// a second, drifting copy.
        wdf::LiveWDFDocument starterDocument_;

        /// Where the library looks. The default is a folder in Documents; the
        /// user can point it anywhere for the session.
        juce::File presetFolder_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Content)
    };

    Content content_;

    /// What the last self-test step found, when it found the wrong thing.
    juce::String selfTestNote_;

    /// Which column the next self-test part goes in, so the parts do not stack.
    int selfTestColumn_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorWindow)
};

}  // namespace livewdf::editor
