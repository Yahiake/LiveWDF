#include "SchematicHistory.h"

namespace livewdf::editor {

void SchematicHistory::reset(Schematic state) {
    undoStack_.clear();
    redoStack_.clear();
    current_ = std::move(state);
    hasCurrent_ = true;
}

void SchematicHistory::record(Schematic now) {
    if (hasCurrent_)
        undoStack_.push_back(current_);

    current_ = std::move(now);
    hasCurrent_ = true;

    // A new edit is a new branch: whatever was undone is no longer reachable,
    // and offering redo after it would put back a change that belongs to a
    // history that no longer exists.
    redoStack_.clear();

    while (undoStack_.size() > maxDepth)
        undoStack_.erase(undoStack_.begin());
}

void SchematicHistory::amend(Schematic now) {
    current_ = std::move(now);
    hasCurrent_ = true;
}

Schematic SchematicHistory::undo() {
    if (!canUndo())
        return {};

    redoStack_.push_back(current_);
    current_ = undoStack_.back();
    undoStack_.pop_back();
    return current_;
}

Schematic SchematicHistory::redo() {
    if (!canRedo())
        return {};

    undoStack_.push_back(current_);
    current_ = redoStack_.back();
    redoStack_.pop_back();
    return current_;
}

}  // namespace livewdf::editor
