#include "EditorWindow.h"
#include "EditorWindowInternal.h"
#include "DiagLog.h"
#include "EditorLayout.h"
#include "EditorLookAndFeel.h"
#include "gui/Fonts.h"

#include <cmath>

namespace livewdf::editor {
// ============================================================================
// Shell glue
// ============================================================================

void EditorWindow::Content::recordUndoState() {
    // A restore is not an edit. The state came *out* of the history, so putting
    // it back on the sheet must not record it as a new step -- that would clear
    // the redo stack and make undo/redo a one-way door.
    if (applyingHistory_)
        return;

    const auto state = editor_.schematic();

    // Called *after* the edit, so the state before it is whatever was last
    // recorded. A drag is one step: while the canvas reports it is still
    // mid-gesture, the run of notifications collapses into one record, which
    // `onGestureEnd` closes.
    if (editor_.isMidGesture()) {
        if (gestureRecorded_)
            history_.amend(state);
        else
            history_.record(state);

        gestureRecorded_ = true;
    } else {
        history_.record(state);
        gestureRecorded_ = false;
    }

    updateUndoButtons();
}

void EditorWindow::Content::undo() {
    // **Not while a gesture is running.** A drag that has not landed has no step
    // to take back: the history is mid-*amend* on it, and handing the pre-drag
    // sheet back now would put that state under a pointer that is still moving --
    // the part snaps to where it was and is then dragged on from there, which is
    // a jump nobody meant. The gesture has to land first; `onGestureEnd` lights
    // Undo at the moment the step exists.
    if (editor_.isMidGesture())
        return;

    if (!history_.canUndo())
        return;

    applyingHistory_ = true;
    editor_.adoptRestoredState(history_.undo());
    applyingHistory_ = false;

    // Undo restores the *drawing*, not what is being heard: the model is back but
    // nothing recompiles until Rebuild is pressed. That is the seam U2.9 makes
    // explicit -- `markContentChanged` lights Rebuild like any other edit, rather
    // than silently recompiling and interrupting whatever is playing.
    markContentChanged();
    updateUndoButtons();
}

void EditorWindow::Content::redo() {
    // **Same seam as undo**: a redo while the pointer is down would move the
    // sheet under a gesture that is still deciding where to put the part.
    if (editor_.isMidGesture())
        return;

    if (!history_.canRedo())
        return;

    applyingHistory_ = true;
    editor_.adoptRestoredState(history_.redo());
    applyingHistory_ = false;

    markContentChanged();
    updateUndoButtons();
}

void EditorWindow::Content::updateUndoButtons() {
    // **A gesture in progress is not a step.** Both buttons stay dark while the
    // canvas is mid-drag, including over a step the drag has not taken yet: the
    // one thing Undo can honestly answer during a drag is "nothing yet", so the
    // label says the same as the refusal above. `onGestureEnd` closes the
    // gesture, and that is what lights Undo -- one press per drag rather than
    // one per grid square.
    if (editor_.isMidGesture()) {
        undoBtn_.setEnabled(false);
        redoBtn_.setEnabled(false);
        return;
    }

    undoBtn_.setEnabled(history_.canUndo());
    redoBtn_.setEnabled(history_.canRedo());
}
} // namespace livewdf::editor