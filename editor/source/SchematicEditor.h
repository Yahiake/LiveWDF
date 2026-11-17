#pragma once

// The drawing sheet.
//
// This is the *editing* canvas, and the plugin embeds the same component
// read-only: it is handed a finished document and paints it with the same
// artwork, fit and live-value overlays. In the editor it owns an editable
// `Schematic`, decides what a click does, and reports every change so the
// engine can be recompiled.
//
// The interaction model is Celine's, ported wholesale rather than reinvented,
// because it is the working reference for this exact job and the first editor
// here -- which invented its own -- was unusable. The gestures themselves are
// documented at their definitions below; what matters at the top is the shape:
//
//   * **Grid units, not world pixels** -- see `SchematicModel.h` for the one
//     coordinate note. `toGrid`/`toPixel` are the only two conversions.
//   * **Box selection**, drawn dashed or solid by direction: left-to-right
//     encloses, right-to-left crosses. One gesture, and the drawing says which.
//   * **Junction dots** where the connection is not obvious from the drawing, so
//     a T does not read as a crossing.
//   * **Wire-end handles** on a selected wire, so a wire is a *thing* rather
//     than a side effect of two pins sharing a string.
//   * **Ghost preview** under the pointer when a part is armed.
//   * **Command-key actions**: Cmd/Ctrl-Z undo, Shift-Z redo, Cmd-D duplicate,
//     Cmd-R rotate, Cmd-F flip. Bare letters name parts and tools, so the two
//     sets never collide.
//
// The canvas owns *geometry and gestures only*. It does not know about the
// engine, about DC, or about scope traces -- those listen to `onChanged` and
// read the model back.

#include "Clipboard.h"
#include "EditorLookAndFeel.h"
#include "format/PartLibrary.h"
#include "SchematicModel.h"

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <string>
#include <vector>

namespace livewdf::editor {

class SchematicEditor : public juce::Component,
                        private juce::Timer {
public:
    /// What a click does.
    enum class Tool { Select, Wire, Delete, Place };

    SchematicEditor();
    ~SchematicEditor() override;

    // ---------------------------------------------------------------------
    // Model
    // ---------------------------------------------------------------------
    void setDocument(const wdf::LiveWDFDocument& doc);
    const wdf::LiveWDFDocument& getDocument() const noexcept { return doc_; }
    wdf::LiveWDFDocument& getDocument() noexcept { return doc_; }

    /// Re-read the model from `getDocument()`. For a panel that edited the
    /// document in place (the inspector), which cannot reach the model directly:
    /// the canvas is the only thing that owns one, and the document is its
    /// serialised form, so an in-place document edit must round-trip through
    /// here or the next canvas edit would overwrite it.
    void reloadFromDocument();

    /// The editable model, for the panels that need connectivity (the DC table
    /// and the probe badges read net names from here).
    const Schematic& schematic() const noexcept { return schematic_; }
    Schematic& schematic() noexcept { return schematic_; }

    bool isDirty() const noexcept { return dirty_; }
    void markClean() { dirty_ = false; }
    /// The sheet differs from the file on disk. Set by the *editor window* when
    /// content changes, not by the canvas's own mutations: the canvas mutates on
    /// every drag, and marking dirty per mutation would light the dot mid-drag.
    void markDirty() { dirty_ = true; }

    // ---------------------------------------------------------------------
    // Live values
    // ---------------------------------------------------------------------
    /// DC node voltages keyed by node name. Drawn as probe badges and read by
    /// the scope.
    void setNodeVoltages(const std::map<std::string, double>& v);
    /// 0..1 live output level, driving the glowing signal-flow beads.
    void setSignalActivity(float level);

    /// Live control positions, keyed by component id.
    ///
    /// **This is an overlay, not an edit.** The plugin's knobs and switches are
    /// APVTS parameters, while the canvas holds the document that was loaded
    /// from a file. Painting only that snapshot is why the plugin's pots stayed
    /// parked and its SPDTs never changed throw. A live position is drawn from
    /// here, on top of the unchanged model, so a read-only viewer cannot
    /// pretend a parameter move was a document edit.
    ///
    /// `potPositions` carries 0..1 for both pots and rheostats; a missing id
    /// keeps the document's saved position. `switchThrowsB` is true for throw B.
    void setLiveControlState(const std::map<std::string, double>& potPositions,
                             const std::map<std::string, bool>& switchThrowsB);

    /// The part the last build failed on, marked with a red ring. "" clears it.
    ///
    /// **Why a ring and not a selection.** The message in the status line says
    /// what is wrong; the ring says *where*, on the drawing, without taking
    /// the selection away -- so whatever the user was working on is still
    /// selected and still theirs.
    void setProblemPart(const std::string& id);

    /// Frame the whole drawing.
    void frameAll();

    // ---------------------------------------------------------------------
    // Tools
    // ---------------------------------------------------------------------
    void setTool(Tool t);
    Tool getTool() const noexcept { return tool_; }

    /// Arm the Place tool with a fresh part of `type`.
    void armPlacement(const std::string& partType);

    /// Arm the Place tool with one specific palette entry. Model-bearing parts
    /// have one entry per model, and this is what carries the chosen device
    /// (e.g. 2N5457 rather than the family-default 2N5952) into placement.
    void armPlacement(const PartSpec& spec);

    // ---------------------------------------------------------------------
    // The clipboard
    // ---------------------------------------------------------------------
    /// Copy the selected parts, and the wires among them, to the clipboard.
    ///
    /// **It goes to the *system* clipboard**, so it can be pasted into another
    /// editor process -- which is the case that matters when the answer to
    /// "how do I get this sub-circuit from that window" is "copy it".
    void copySelection();

    /// Copy the selection and delete it.
    void cutSelection();

    /// Paste the clipboard's group into the sheet.
    ///
    /// It lands under the pointer when the pointer is over the sheet, and where
    /// it was copied from when it is not. The pasted parts are selected, so they
    /// can be dragged into place straight away.
    void pasteClipboard();

    /// What the clipboard holds, for the host (and for a test).
    const ClipboardGroup& clipboard() const noexcept { return clipboard_; }

    /// How many of a wire's two ends land on a selected part. Used to decide
    /// whether a wire belongs to the copied group.
    int countEndsOnSelectedParts(const Wire& wire) const;

    /// Arm the Place tool with a *copy* of an existing part -- its value, model,
    /// label and settings, not just its type. Middle-click does this, so wanting
    /// five more of the resistor you already set up is one click to pick it up
    /// and five to put them down.
    void clonePart(const std::string& partId);

    std::string pendingPlacement() const noexcept { return pendingType_; }

    /// The palette row the Place tool is armed with, or nullptr. The palette
    /// panel follows this so its highlight and the canvas cannot disagree.
    const PartSpec* pendingPartSpec() const noexcept { return pendingSpec_; }

    /// Rotates a quarter turn: the part about to be placed when Place is armed,
    /// otherwise the selection. The angle starts at the part's default when it
    /// is armed and is sticky across placements, so a run of horizontal
    /// resistors is set up once.
    void rotate();

    /// Mirrors about the part's own vertical axis. Rotation cannot do this:
    /// turning a triode twice puts its plate at the bottom, where flipping it
    /// moves the grid to the other side and leaves the plate up. The mirror
    /// starts at the part's default when it is armed.
    void flip();

    /// Duplicates everything selected, offset so the copies are visibly separate
    /// and immediately draggable, and selects the copies.
    void duplicateSelection();

    /// Delete whatever is selected. True if anything was removed.
    bool deleteSelection();

    /// Menu/panel-facing ids. `selectedPartId()` is "" when nothing is selected.
    std::string selectedPartId() const;
    bool hasSelection() const noexcept;

    /// **Select** a part, by id -- the public half of what a click on it does.
    ///
    /// It exists because a host (and the editor's own self-test) needs to put a
    /// part into the selection without synthesising a mouse event, which in
    /// JUCE 9 means constructing a platform mouse source. The selection is what
    /// the inspector reacts to, so this is the same path a click takes from the
    /// panel's point of view.
    void selectPart(const std::string& id);

    /// **Place** the armed part at a grid position, and select it.
    ///
    /// The public half of what a click with a part armed does. Returns false
    /// when nothing was armed, or the position was not on the sheet.
    bool placePartAt(int gridX, int gridY);

    /// The selected wires, in the order they joined the selection. A separate
    /// list from the parts because almost everything that acts on a selection --
    /// rotate, flip, the inspector -- acts on parts only, which is exactly why
    /// Celine keeps them apart too.
    ///
    /// The two lists may both be non-empty: Shift/Ctrl-click adds either kind
    /// without dropping the other. Actions that can move or delete both -- drag,
    /// Delete, copy -- read both lists. Showing the part inspector while a wire
    /// is also selected is deliberate; parts remain the editable target.
    const std::vector<int>& selectedWireIds() const noexcept { return selectedWireIds_; }

    /// One wire, for a panel that has to describe it. `nullptr` if no wire has
    /// that id -- a selected wire can be deleted underneath the panel.
    const Wire* selectedWire(int id) const;

    /// The scope probes in the document, as (partId, node, reference).
    struct ProbeInfo { std::string partId; std::string node; std::string reference; };
    std::vector<ProbeInfo> probes() const;

    // ---------------------------------------------------------------------
    // Undo
    // ---------------------------------------------------------------------
    // The canvas does **not** own history -- see `SchematicHistory`. It reports
    // the two facts a history needs and nothing else: whether a drag is still
    // running, and that one has ended. The Command-key shortcuts ask the owner
    // to undo through these callbacks rather than doing it here, because the
    // owner is the one holding the stack.
    bool isMidGesture() const noexcept;

    /// Puts a state the history handed back onto the sheet, clearing the
    /// selection (a restored sheet may not still contain what was selected) and
    /// announcing the change. The history decided *what*; this decides *how*.
    void adoptRestoredState(const Schematic& state);

    // ---------------------------------------------------------------------
    // Callbacks
    // ---------------------------------------------------------------------
    /// Fired after any edit that would change the circuit.
    std::function<void()> onChanged;
    /// Fired when the selection changes.
    std::function<void()> onSelectionChanged;
    /// Fired when the tool changes (including the canvas dropping back to Select
    /// after a placement), so a toolbar can show what is actually active.
    std::function<void()> onToolChanged;
    /// Fired when a mouse gesture finishes, whether or not it changed anything.
    /// The boundary between "still dragging" and "done", which the change
    /// notification alone cannot express -- whoever records undo steps needs it
    /// or the next drag folds into the last one's step.
    std::function<void()> onGestureEnd;

    /// Fired with a short message for the user, when the canvas has something
    /// worth saying that is not an error: a wire that ended in mid-air, say.
    ///
    /// **Said at the moment it happens.** The user is looking at the wire they
    /// have just drawn, and a message then is worth ten messages later.
    std::function<void(const juce::String&)> onNotice;
    /// Command-Z / Shift-Command-Z, forwarded to whoever owns the history.
    std::function<void()> onUndoRequested;
    std::function<void()> onRedoRequested;

    /// A number the user typed *over one selected part* -- digits on the
    /// sheet, committed by Enter (see `keyPressed`). The canvas owns the
    /// gesture because the part is the thing under the eye, but values are
    /// not its job: whoever edits them answers this, and writes it through
    /// its own parse so "2k" means exactly what it means in the field.
    std::function<void(const juce::String& text)> onQuickValueEdit;

    /// Where the pointer is, in this component's pixels, or (-1,-1) when it
    /// leaves. The rulers follow this rather than polling: they are siblings of
    /// the canvas, not children, and a ruler redraws on every move -- so only a
    /// move should drive it.
    std::function<void(juce::Point<float>)> onPointerMoved;

    // ---------------------------------------------------------------------
    // Component overrides
    // ---------------------------------------------------------------------
    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void rebuildGeometry();
    void syncDocument();

    float gridSize() const noexcept { return view_.gridSize; }
    juce::Point<float> gridOriginF() const noexcept { return view_.origin; }

    // ---------------------------------------------------------------------
    // Hit testing
    //
    // Public because "what is under this point" is a question a host has a
    // legitimate reason to ask -- the editor's own self-test asks it, to check
    // that a click lands where the user aimed.
    // ---------------------------------------------------------------------

    /// Which part is under a *pixel* position, or "". Takes pixels, not grid
    /// squares: rounding to the intersection first was most of what made the
    /// old hit test feel vague. The answer follows the drawn symbol -- body,
    /// lines and pin dots -- rather than its bounding box.
    std::string partAtPixel(juce::Point<float> pixel) const;
    /// Which wire is under a pixel position, or -1. Parts win: a wire's end
    /// usually sits on a pin, and the part is what you meant.
    int wireAtPixel(juce::Point<float> pixel) const;

    /// The pixel position of a part's centre, in the canvas's own coordinates.
    ///
    /// The counterpart of `partAtPixel`: one asks what is under a point, the
    /// other says where a part is. The self-test needs both -- it clicks where
    /// the part is and checks that the click found it -- and a host that wants
    /// to point at a part needs the same pair.
    juce::Point<float> partCentrePixel(const wdf::SchematicComponent& part) const;

    /// The pixel position of a grid point.
    ///
    /// The same pair of questions as `partCentrePixel`, for a point that is not
    /// a part's centre: where a wire runs, and where a click that is meant to
    /// miss everything would land.
    juce::Point<float> gridToPixel(int gx, int gy) const;

    /// The box the *drawn* wires occupy, in pixels.
    ///
    /// **The wires' paths are built in pixels, not in grid units**, so a change
    /// of view leaves them where they were until they are built again. This is
    /// the way to see whether that has happened: the wires' drawn box has to
    /// follow the parts' when the sheet is fitted.
    juce::Rectangle<float> drawnWiresBounds() const;

    /// The wire's end, snapped to a pin under the pointer.
    ///
    /// **The preview is the point.** The end of the wire being drawn snaps to
    /// the pin it will land on -- the same snap the commit does -- so the
    /// drawing shows where the wire will *land* rather than where the pointer
    /// happens to be. A wire that stops a square short is the commonest wiring
    /// mistake there is, and with the preview the user can see which they are
    /// about to make.
    void updateWireEnd(juce::Point<float> position);

    /// The view -- the zoom and the pan -- so a host can save it and put it
    /// back. A tab remembers where its sheet was looking, because a tab that
    /// comes back zoomed into nothing is a tab that looks broken.
    struct ViewState {
        float gridSize = 16.0f;
        float originX = 24.0f;
        float originY = 24.0f;
    };

    ViewState viewState() const {
        return {view_.gridSize, view_.origin.x, view_.origin.y};
    }

    void setViewState(const ViewState& state) {
        view_.gridSize = state.gridSize;
        view_.origin = {state.originX, state.originY};
        rebuildGeometry();
        repaint();
    }

    /// The selected parts, for a host that is keeping a selection of its own
    /// (a tab, say).
    const std::vector<std::string>& selectedPartIds() const noexcept { return selectedIds_; }

    /// ("", -1).
    std::pair<std::string, int> pinAtPixel(juce::Point<float> pixel) const;

private:
    // ---------------------------------------------------------------------
    // Timer: the signal-flow animation
    // ---------------------------------------------------------------------
    void timerCallback() override;
    // ---------------------------------------------------------------------
    // View
    // ---------------------------------------------------------------------
    /// Line weight for a wire at the current zoom.
    float wireThickness() const noexcept;

    // ---------------------------------------------------------------------
    // Drawing
    // ---------------------------------------------------------------------
    void paintGrid(juce::Graphics& g) const;
    /// The model's own drawing: wires and junctions, then parts, back to front.
    /// The transient bits (dragged wire, selection box, ghost) are `paintInFlight`.
    void paintDrawing(juce::Graphics& g) const;
    /// A copy of `part` with the live pot position or switch throw applied, for
    /// the painter. The model itself is never changed.
    wdf::SchematicComponent partWithLiveState(const wdf::SchematicComponent& part) const;
    /// Halos on the selection and the handles it earns.
    void paintSelection(juce::Graphics& g) const;
    /// The wire being dragged, the selection box, the part about to be placed.
    void paintInFlight(juce::Graphics& g) const;
    /// A ring on a terminal's pin when the pointer is on it, and on the pin a
    /// wire drag started from -- see the note at its definition.
    void paintTerminalHighlight(juce::Graphics& g) const;
    /// Live voltage badges, one per probe.
    void paintProbes(juce::Graphics& g) const;
    void paintSignalFlow(juce::Graphics& g) const;

    /// Rebuild the cached wire paths and signal-path beads from the model. This
    /// is the only thing that must happen after a structural edit before the
    /// drawing is correct; the nets themselves are extracted on demand.
    // moved to public

    // ---------------------------------------------------------------------
    // Selection
    //
    // Public because a host has a legitimate reason to select things: the
    // inspector follows the selection, and a "select all" or a "select the
    // part I just pasted" is a thing a host may want to do.
    // ---------------------------------------------------------------------
public:
    /// Replace the selection with `partId`, or clear it when empty. Wires are
    /// cleared too: a plain click means "this one thing".
    void setSelection(const std::string& partId);

    /// Replace the selection with these parts. Wires are cleared.
    void setSelection(std::vector<std::string> ids);

    /// Replace the selection with these wires. Parts are cleared.
    void setSelectedWires(std::vector<int> ids);

    /// Replace the selection with both lists at once.
    ///
    /// Shift/Ctrl-click and a box selection can hold parts and wires together,
    /// and restoring a tab must put both back. Calling the two setters above in
    /// sequence would not: each clears the other half.
    void setSelection(std::vector<std::string> partIds, std::vector<int> wireIds);

    /// Add or remove one part, leaving the selected wires and the other parts
    /// alone. This is what a Shift/Ctrl-click on a part does.
    void togglePartInSelection(const std::string& partId);

    /// Add or remove one wire, leaving the selected parts and the other wires
    /// alone. This is what a Shift/Ctrl-click on a wire does.
    void toggleWireInSelection(int wireId);

private:
    bool isSelected(const std::string& partId) const;
    bool isWireSelected(int wireId) const noexcept;
    /// The wire whose ends wear handles, or -1: one wire selected on its own.
    /// Asked by the painter and the mouse alike, so what is drawn and what is
    /// grabbable cannot disagree.
    int wireWithHandles() const noexcept;
    float wireHandleReach() const noexcept;
    /// Which end of `wire` this pixel is on: 0 for a, 1 for b, -1 for neither.
    int wireHandleAt(const Wire& wire, juce::Point<float> pixel) const noexcept;

    /// Merge wires that became collinear neighbours, then drop any absorbed wire
    /// id from the selection. Called after a wire draw, a wire-end resize or a
    /// whole-wire drag -- and **never mid-drag**, because a merge swaps the wire
    /// under the pointer for a longer one while the gesture is still naming it.
    /// `preferredId` is the survivor when one is obvious (the wire you were
    /// editing), or -1 to let the merge pick by its own rule.
    void mergeWiresAfterEdit(int preferredId);

    // ---------------------------------------------------------------------
    // Gestures
    // ---------------------------------------------------------------------
    void beginSelectGesture(const juce::MouseEvent& e, juce::Point<int> grid);
    /// Starts a wire drag, snapped onto a pin when the press landed on one.
    void beginWireGesture(const juce::MouseEvent& e, juce::Point<int> grid);
    void beginDrag(juce::Point<int> grid);
    /// Starts a corner/wire-end drag if the click landed on one. True if it did.
    bool grabResizeHandle(const juce::MouseEvent& e, juce::Point<int> grid);
    void placePendingPart(juce::Point<int> grid);
    bool deleteAtPixel(juce::Point<float> pixel);
    void applyBoxSelection(bool additive);
    juce::Rectangle<float> getSelectionBox() const noexcept;
    bool isCrossingBox() const noexcept;

    /// The terminal (``in``/``out`` part) the pointer is on, or "". Asked the
    /// way the painter draws it -- body first, then pin -- so the repaint
    /// trigger and the ring cannot disagree about what is hovered.
    std::string hoveredTerminal() const;

    /// The Command-modified shortcuts: undo, redo, duplicate, rotate, flip.
    bool commandKeyPressed(const juce::KeyPress& key);

    /// Called whenever the drawing changed in a way that would change the
    /// circuit: repaints and announces, in one place.
    void notifyChanged();

    /// Say something to the user, through `onNotice`.
    void notify(const juce::String& text);
    /// Rebuilds the `.livewdf` document from the model and announces it.
    // syncDocument moved to public (see above)

    // ---------------------------------------------------------------------
    // State
    // ---------------------------------------------------------------------
    Schematic schematic_;
    /// The `.livewdf` document, kept in step with `schematic_` so the panels and
    /// the save path see the format they already speak.
    wdf::LiveWDFDocument doc_;

    SheetView view_;

    // Cached drawing data, rebuilt from the model.
    struct WirePath { int wireId = 0; juce::Path path; };
    std::vector<WirePath> wirePaths_;

    struct FlowPath { juce::Path path; float length = 0.0f; float phase = 0.0f; };
    std::vector<FlowPath> flowPaths_;
    float flowPhase_ = 0.0f;

    std::map<std::string, double> voltages_;
    float signalActivity_ = 0.0f;

    /// Live control overlay, keyed by component id. Empty in the editor app,
    /// where moving a control edits the document itself.
    std::map<std::string, double> livePotPositions_;
    std::map<std::string, bool> liveSwitchThrowsB_;

    Tool tool_ = Tool::Select;
    std::string pendingType_;
    /// The exact palette row being placed. Points into the static palette, so
    /// it outlives the arming; null for a clone or after Escape.
    const PartSpec* pendingSpec_ = nullptr;
    int pendingOrientation_ = 0;
    bool pendingMirrored_ = false;
    std::optional<wdf::SchematicComponent> pendingClone_;

    std::vector<std::string> selectedIds_;
    std::vector<int> selectedWireIds_;

    /// Digits typed over exactly one selected part, waiting for Enter: the
    /// "type a number over the thing you are looking at" edit of its value.
    /// Cleared on commit, on Escape, and whenever the key lands on anything
    /// else -- an abandoned number is abandoned, not half-typed later.
    std::string pendingValue_;

    // Drag state.
    bool draggingPart_ = false;
    bool boxSelectionStarted_ = false;
    /// The end being dragged (0 or 1) while a selected wire's handle is grabbed,
    /// or -1. The wire itself is `resizingWireId_`.
    int resizingWireEnd_ = -1;
    int resizingWireId_ = -1;
    juce::Point<int> dragLastGrid_;
    bool dragMovedSomething_ = false;

    bool boxSelecting_ = false;
    juce::Point<float> boxAnchor_, boxCurrent_;

    bool draggingWire_ = false;
    juce::Point<int> wireStart_, wireEnd_;

    /// The pin the wire being drawn *started* from, as (partId, pinIndex), or
    /// ("", -1). The end already wears a ring (``wireEndPin_``); the start wants
    /// one too while the drag runs, so a terminal that already holds one end of
    /// a wire reads as connected from both sides.
    std::pair<std::string, int> wireStartPin_;
    std::pair<std::string, int> wireEndPin_;

    bool panning_ = false;
    juce::Point<float> panOrigin_;

    juce::Point<int> hoverGrid_;
    std::string hoverPart_;
    /// The pin under the pointer, as (partId, pinIndex) -- the terminal ring is
    /// drawn at a *pin*, so the hover needs the pin as well as the part.
    std::pair<std::string, int> hoverPin_;
    /// The hovered terminal's id, or "". The repaint follows a change of this
    /// rather than every pointer move -- a move that changed nothing else
    /// should not pay for a full redraw.
    std::string hoverTerminal_;

    /// The part the last build failed on, or "".
    std::string problemPart_;

    /// What the clipboard holds.
    ClipboardGroup clipboard_;

    bool dirty_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SchematicEditor)
};

}  // namespace livewdf::editor
