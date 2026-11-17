#pragma once

// Undo and redo for the drawing, as whole-sheet snapshots.
//
// **Why this is its own class, and why it lives here rather than in the canvas.**
// The first version of this editor kept undo inside the canvas: every mutating
// gesture called a `pushUndo()` on itself before it changed anything, and the
// canvas also grew a `mergeWiresAfterEdit` helper and an `isMidGesture` flag so
// that a drag of a wire would not push sixty-four snapshots -- one per grid
// square -- on the way past. That tangle is the fingerprint of the wrong owner.
// A snapshot stack is not a property of *drawing*; it is a property of the
// *document being edited*, and it wants a "something changed" signal rather than
// a hook before every mutation that memory says will be forgotten.
//
// So the canvas mutates and then says `onChanged`; the editor records the state
// *after* the change. `record` pushes whatever `current` held and takes the new
// state, so the "before" is always the last thing recorded -- no pre-mutation
// hook needed anywhere. A drag collapses to one step via `amend`: the canvas
// reports when the gesture is still running (`isMidGesture`) and when it ends
// (`onGestureEnd`), and everything in between amends the same record.
//
// This is Celine's `SchematicHistory` (`Celine/source/Schematic/
// SchematicHistory.h`) with our `Schematic` in place of its `ValueTree`. Same
// contract, same `maxDepth`, same three methods -- the class is small enough
// that inventing a different shape would be pure risk.
//
// **What it deliberately does not do:** it does not restore anything itself, and
// it does not know about the engine. It hands back a `Schematic` and the editor
// decides what that means. In particular a snapshot carries the drawing only --
// knob *positions* and the compiled circuit are the editor's to re-seed (see
// `EditorWindow::recordUndoState` and the note there about not rewinding knobs
// behind an automation lane).

#include "SchematicModel.h"

#include <vector>

namespace livewdf::editor {

class SchematicHistory {
public:
    /// Forgets everything and takes `state` as the new starting point. For a
    /// load or a New: undoing across one of those would put back a drawing from
    /// a different document.
    void reset(Schematic state);

    /// Records that the sheet has moved on from what it was.
    ///
    /// `now` is the state *after* the edit; the state before it is whatever was
    /// last handed in. That is what lets this sit on a plain "something changed"
    /// notification rather than needing a hook before every mutation -- which is
    /// the one thing the canvas cannot reliably provide, since a drag mutates
    /// continuously.
    void record(Schematic now);

    /// Replaces the newest recorded state without pushing a new one, so a
    /// continuous gesture is one undo step rather than one per grid square.
    void amend(Schematic now);

    bool canUndo() const noexcept { return !undoStack_.empty(); }
    bool canRedo() const noexcept { return !redoStack_.empty(); }

    /// The state to restore. Only call when `canUndo()`; otherwise returns an
    /// empty sheet, which a caller should test for nothing rather than apply.
    Schematic undo();

    /// The state to redo. Only call when `canRedo()`.
    Schematic redo();

    /// Bounded, because a long session's worth of a busy sheet is real memory and
    /// nobody undoes sixty-four steps. (Same depth as Celine.)
    static constexpr size_t maxDepth = 64;

private:
    std::vector<Schematic> undoStack_, redoStack_;
    /// Whether a state has ever been recorded. A `Schematic` has no "invalid"
    /// state to test, so this stands in for Celine's `current.isValid()`.
    bool hasCurrent_ = false;
    Schematic current_;
};

}  // namespace livewdf::editor
