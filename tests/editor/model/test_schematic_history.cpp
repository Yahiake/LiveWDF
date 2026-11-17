// Tests for `SchematicHistory`, the editor's undo/redo stack.
//
// Why this exists: undo is easy to get *almost* right. The bugs that matter are
// not "the stack is empty" -- they are "the redo path survived an edit", "a drag
// pushed sixty-four steps", "a load left an undo into the previous document".
// None of those are visible by pressing the button once, and all of them are
// cheap to pin with assertions. The class exists at all because the canvas
// owning history was the source of the first editor's `mergeWiresAfterEdit` /
// `isMidGesture` tangle, so the contract it must keep is worth stating: recording
// is "state after the edit", amending folds a gesture into one step, and an
// undo/redo pair is reversible.

#include <catch2.hpp>

#include "SchematicHistory.h"

#include <string>

using namespace livewdf::editor;

namespace {

/// A sheet with `count` resistors on it, named by their generated ids. Distinct
/// counts stand in for distinct edit states, so "which state is this" is one
/// comparison.
Schematic sheetWith(int count) {
    Schematic s;
    for (int i = 0; i < count; ++i)
        s.addPart("res", 10 + i * 4, 10);
    return s;
}

}  // namespace

TEST_CASE("Nothing to undo until something is recorded", "[history]") {
    SchematicHistory h;
    CHECK_FALSE(h.canUndo());
    CHECK_FALSE(h.canRedo());

    h.record(sheetWith(1));
    // The first record establishes a baseline; there is no earlier state to
    // return to, so it is not itself undoable.
    CHECK_FALSE(h.canUndo());
    CHECK_FALSE(h.canRedo());
}

TEST_CASE("Recording a second state makes the first undoable", "[history]") {
    SchematicHistory h;
    h.record(sheetWith(1));   // baseline
    h.record(sheetWith(2));   // an edit

    CHECK(h.canUndo());
    CHECK_FALSE(h.canRedo());

    // undo() returns the *previous* state -- one part.
    CHECK(h.undo().getParts().size() == 1);
    CHECK_FALSE(h.canUndo());
    CHECK(h.canRedo());

    // redo() returns to the state we left.
    CHECK(h.redo().getParts().size() == 2);
    CHECK(h.canUndo());
    CHECK_FALSE(h.canRedo());
}

TEST_CASE("A new edit after an undo burns the redo path", "[history]") {
    SchematicHistory h;
    h.record(sheetWith(1));
    h.record(sheetWith(2));
    h.undo();

    CHECK(h.canRedo());

    // Branching: the redo would put back a change from a history that no longer
    // exists.
    h.record(sheetWith(3));
    CHECK_FALSE(h.canRedo());

    // We undid to state 1, then edited to state 3. Undo now returns state 1 --
    // the state we were sitting on when the branch was made, not the state 2
    // that the abandoned redo held.
    CHECK(h.undo().getParts().size() == 1);
}

TEST_CASE("Amending folds a gesture into one step", "[history]") {
    SchematicHistory h;
    h.record(sheetWith(1));   // baseline
    h.record(sheetWith(5));   // the edit so far (a drag's first notification)

    // The rest of the drag amends rather than recording: same step.
    h.amend(sheetWith(6));
    h.amend(sheetWith(7));

    // One undo returns to the baseline, not to an intermediate grid square.
    CHECK(h.undo().getParts().size() == 1);
    CHECK_FALSE(h.canUndo());
}

TEST_CASE("Amend before any record takes the state as the baseline", "[history]") {
    SchematicHistory h;
    h.amend(sheetWith(4));

    // The amended state is "now", and there is nothing behind it yet.
    CHECK_FALSE(h.canUndo());
    CHECK_FALSE(h.canRedo());

    h.record(sheetWith(5));
    CHECK(h.canUndo());
    CHECK(h.undo().getParts().size() == 4);
}

TEST_CASE("Reset forgets both directions", "[history]") {
    SchematicHistory h;
    h.record(sheetWith(1));
    h.record(sheetWith(2));
    h.undo();
    CHECK(h.canRedo());

    h.reset(sheetWith(9));
    // A load or a New: undoing across it would put back another document's
    // drawing.
    CHECK_FALSE(h.canUndo());
    CHECK_FALSE(h.canRedo());

    // After a reset the next record is a fresh baseline.
    h.record(sheetWith(10));
    CHECK(h.canUndo());
    CHECK(h.undo().getParts().size() == 9);
}

TEST_CASE("The stack is bounded at maxDepth", "[history]") {
    SchematicHistory h;
    h.record(sheetWith(0));   // baseline

    // Push well past the bound. Each record is an edit from the last.
    for (size_t i = 1; i <= SchematicHistory::maxDepth + 10; ++i)
        h.record(sheetWith(static_cast<int>(i)));

    // The oldest states are dropped rather than the newest: undo walks the most
    // recent `maxDepth` edits and then stops, instead of reaching the baseline.
    size_t undos = 0;
    while (h.canUndo()) {
        h.undo();
        ++undos;
        CHECK(undos <= SchematicHistory::maxDepth);
    }
    CHECK(undos == SchematicHistory::maxDepth);
}

TEST_CASE("An empty undo returns nothing to apply", "[history]") {
    SchematicHistory h;
    // A caller should test `canUndo` first; if it forgets, it gets an empty
    // sheet rather than a crash or a stale state.
    CHECK(h.undo().getParts().empty());
    CHECK(h.redo().getParts().empty());
}
