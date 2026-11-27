#include "SchematicEditor.h"
#include "SchematicEditorInternal.h"
#include "EditorLookAndFeel.h"
#include "format/PartLibrary.h"
#include "Shortcut.h"
#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace livewdf::editor {
// ============================================================================
// Mouse
// ============================================================================

void SchematicEditor::mouseDown(const juce::MouseEvent& e) {
    grabKeyboardFocus();

    const auto grid = view_.toGrid(e.position);

    // Middle button clones: pick up a copy of whatever is under the pointer and
    // arm it for placing, which is the fast way to lay out a run of one part.
    if (e.mods.isMiddleButtonDown()) {
        const auto hit = partAtPixel(e.position);
        if (!hit.empty())
            clonePart(hit);
        return;
    }

    // While placing, right-click is the cancel gesture: it disarms the tool and
    // returns to Select. Everywhere else the right button pans, so a sheet
    // larger than the window can be moved without a scroll bar.
    if (e.mods.isPopupMenu()) {
        if (tool_ == Tool::Place) {
            setTool(Tool::Select);
            repaint();
            return;
        }
        panning_ = true;
        panOrigin_ = e.position - view_.origin;
        return;
    }

    switch (tool_) {
    case Tool::Place:
        placePendingPart(grid);
        break;

    case Tool::Delete:
        deleteAtPixel(e.position);
        break;

    case Tool::Wire:
        beginWireGesture(e, grid);
        break;

    case Tool::Select:
        beginSelectGesture(e, grid);
        break;
    }

    repaint();
}

void SchematicEditor::mouseDoubleClick(const juce::MouseEvent& e) {
    // Only the Select tool: with the wire tool both clicks already mean
    // something.
    if (tool_ != Tool::Select)
        return;

    const auto hit = partAtPixel(e.position);
    if (hit.empty())
        return;

    if (auto* part = schematic_.findPart(hit)) {
        part->properties["label"] = part->properties.count("label")
                                        ? part->properties["label"]
                                        : part->id;
        notifyChanged();
    }
}

void SchematicEditor::mouseDrag(const juce::MouseEvent& e) {
    const auto grid = view_.toGrid(e.position);

    if (panning_) {
        view_.origin = e.position - panOrigin_;
        rebuildGeometry();
        repaint();
        return;
    }

    if (tool_ == Tool::Wire && draggingWire_) {
        updateWireEnd(e.position);
        repaint();
        return;
    }

    // A part drag: the whole selection moves together, so a block picked up by a
    // box lands where it was dropped.
    if (draggingPart_) {
        const int dx = grid.x - dragLastGrid_.x;
        const int dy = grid.y - dragLastGrid_.y;

        if (dx != 0 || dy != 0) {
            for (const auto& id : selectedIds_)
                if (auto* part = schematic_.findPart(id)) {
                    part->visual.x += static_cast<float>(dx);
                    part->visual.y += static_cast<float>(dy);
                }
            for (const int id : selectedWireIds_)
                schematic_.moveWire(id, dx, dy);

            dragLastGrid_ = grid;
            dragMovedSomething_ = true;

            // **The change has to be announced.**
            //
            // A drag that moved a part and said nothing left two things wrong:
            // the Rebuild button stayed locked, because nothing told the host
            // the circuit had changed; and the document was not regenerated, so
            // pressing Rebuild would have compiled the circuit with the part
            // *where it used to be* -- the move silently lost.
            //
            // The undo history collapses the run of these into one step (see
            // `recordUndoState`), so announcing every frame costs nothing.
            notifyChanged();
        }
        return;
    }

    if (boxSelectionStarted_) {
        // A rectangle selection box, dragged from a corner. There is no resize
        // handle on a two-pin symbol that means anything, so this is the box.
        boxCurrent_ = e.position;
        boxSelecting_ = true;
        repaint();
        return;
    }

    if (resizingWireEnd_ >= 0) {
        if (selectedWireIds_.size() == 1
            && schematic_.resizeWireEnd(resizingWireId_, resizingWireEnd_, grid.x, grid.y)) {
            // Not merged yet: joining mid-drag would swap the wire under the
            // pointer for a longer one. That happens once, on mouseUp.
            dragMovedSomething_ = true;

            // **Announced, like the part drag** -- the circuit has changed, and
            // the Rebuild button has to say so.
            notifyChanged();
        }
        return;
    }

    if (boxSelecting_) {
        boxCurrent_ = e.position;
        repaint();
    }
}

void SchematicEditor::mouseUp(const juce::MouseEvent& e) {
    const auto grid = view_.toGrid(e.position);

    if (panning_) {
        panning_ = false;
        return;
    }

    // Ending a wire drag: commit it, then merge any collinear pieces the drag
    // created so two segments drawn end to end become the one wire they look
    // like.
    if (tool_ == Tool::Wire && draggingWire_) {
        draggingWire_ = false;

        const auto pinStart = pinAtPixel(view_.toPixel(wireStart_.x, wireStart_.y));
        const auto pinEnd = pinAtPixel(view_.toPixel(grid.x, grid.y));

        // Snap the ends onto pins when they landed near one, so a wire that
        // looks attached is attached.
        int ax = wireStart_.x, ay = wireStart_.y;
        int bx = grid.x, by = grid.y;

        if (!pinStart.first.empty())
            if (const auto* p = schematic_.findPart(pinStart.first)) {
                const auto pos = Schematic::pinPosition(*p, pinStart.second);
                ax = pos.x;
                ay = pos.y;
            }
        if (!pinEnd.first.empty())
            if (const auto* p = schematic_.findPart(pinEnd.first)) {
                const auto pos = Schematic::pinPosition(*p, pinEnd.second);
                bx = pos.x;
                by = pos.y;
            }

        const auto added = schematic_.addWire(ax, ay, bx, by);
        const int preferred = added.empty() ? -1 : added.back();
        mergeWiresAfterEdit(preferred);

        rebuildGeometry();
        notifyChanged();
        repaint();

        // **A wire that ends in mid-air is a wire that connects nothing.**
        //
        // This is said *now*, at the moment the wire is drawn, rather than
        // later as a build failure: the user is looking at the wire they just
        // drew, and a message at that moment is worth ten messages later.
        //
        // It is the commonest wiring mistake there is -- a wire that stops a
        // square short of the pin it was aimed at -- and it is invisible on the
        // drawing, because a wire that stops in the middle of the sheet looks
        // exactly like a wire that goes somewhere.
        if (!added.empty() && (pinStart.first.empty() || pinEnd.first.empty())) {
            const bool looseStart = pinStart.first.empty();
            const bool looseEnd = pinEnd.first.empty();

            if (looseStart && looseEnd)
                notify("the wire touches nothing -- it is not connected to anything");
            else if (looseStart)
                notify("the wire's start is not on a pin -- that end connects nothing");
            else
                notify("the wire's end is not on a pin -- that end connects nothing");
        }
    }

    // The end of a resize, and the end of a whole-wire drag: either can leave a
    // wire touching a collinear neighbour that it now reads as one line with.
    // (Celine's second `mergeWiresAfterEdit` site; ours only merged after a wire
    // *draw*, so a dragged wire could sit end-to-end with its neighbour forever.)
    //
    // **The notification is for anything that moved**, not only for a wire: a
    // part that has been dragged is a different circuit, and the Rebuild button
    // has to say so. It used to be announced only when a wire was involved, so
    // dragging a component left the button locked and the document stale.
    if (dragMovedSomething_) {
        if (resizingWireEnd_ >= 0 || (draggingPart_ && !selectedWireIds_.empty())) {
            const int preferred =
                selectedWireIds_.size() == 1 ? selectedWireIds_.front() : -1;
            mergeWiresAfterEdit(preferred);
        }

        rebuildGeometry();
        notifyChanged();
    }

    // Ending a box selection: a click rather than a drag never opened the box,
    // and the selection was already cleared on the way down.
    if (boxSelecting_) {
        boxSelecting_ = false;
        if (getSelectionBox().getWidth() >= 2.0f || getSelectionBox().getHeight() >= 2.0f)
            applyBoxSelection(e.mods.isShiftDown() || e.mods.isCommandDown());
    }

    // Clicking a member of a group without dragging narrows to that one thing --
    // the way out of a group.
    if (draggingPart_ && !dragMovedSomething_ && !e.mods.isShiftDown()
        && selectedIds_.size() + selectedWireIds_.size() > 1) {
        if (const auto hit = partAtPixel(e.position); !hit.empty())
            setSelection(hit);
        else if (const int wire = wireAtPixel(e.position); wire >= 0)
            setSelectedWires({wire});
    }

    // A drag that moved nothing but was over a wire selects that instead of
    // leaving a stale part selection.
    if ((draggingPart_ || boxSelectionStarted_) && !dragMovedSomething_ && selectedIds_.empty()
        && selectedWireIds_.empty()) {
        if (const int wire = wireAtPixel(e.position); wire >= 0)
            setSelectedWires({wire});
    }

    draggingPart_ = false;
    boxSelectionStarted_ = false;
    resizingWireEnd_ = -1;
    resizingWireId_ = -1;
    dragMovedSomething_ = false;
    wireStartPin_ = {};

    if (onGestureEnd)
        onGestureEnd();

    repaint();
}

void SchematicEditor::mouseMove(const juce::MouseEvent& e) {
    hoverPart_ = partAtPixel(e.position);
    hoverGrid_ = view_.toGrid(e.position);
    hoverPin_ = pinAtPixel(e.position);

    if (tool_ == Tool::Place && !pendingType_.empty())
        repaint();   // move the ghost

    // **The terminal highlight follows the hover, but only a change to it
    // repaints.** The ghost above is the exception that proves the point: a
    // move that changed nothing else must not pay for a full redraw, so the
    // question asked is not "did the pointer move" but "did the terminal under
    // it change".
    if (const auto id = hoveredTerminal(); id != hoverTerminal_)
    {
        hoverTerminal_ = id;
        repaint();
    }

    if (onPointerMoved)
        onPointerMoved(e.position);
}

std::string SchematicEditor::hoveredTerminal() const {
    // Which ``in``/``out`` the pointer is on: body first, then pin. A terminal
    // is one small flag, so the hover that says "grab here" belongs to the flag
    // as much as to its dot; the pin answer covers a pointer parked exactly on
    // a dot the body test somehow missed.
    const auto isTerminal = [this](const std::string& id) {
        if (id.empty())
            return false;
        if (const auto* part = schematic_.findPart(id))
            return part->type == "in" || part->type == "out";
        return false;
    };

    if (isTerminal(hoverPart_))
        return hoverPart_;
    if (isTerminal(hoverPin_.first))
        return hoverPin_.first;
    return {};
}

void SchematicEditor::mouseExit(const juce::MouseEvent&) {
    hoverPart_ = -1;
    hoverPin_ = {};
    hoverTerminal_.clear();

    if (onPointerMoved)
        onPointerMoved({-1.0f, -1.0f});

    repaint();
}

void SchematicEditor::mouseWheelMove(const juce::MouseEvent& e,
                                     const juce::MouseWheelDetails& wheel) {
    // A press hard enough to start a pan can nudge the wheel, so a pan would zoom
    // as it went. While panning, panning wins. (Celine's guard, ported.)
    if (panning_ || e.mods.isPopupMenu())
        return;

    // Zoom about the cursor. The *unrounded* position: `toGrid` snaps, and
    // anchoring on a snapped point drifts the sheet up to half a square a wheel
    // step. (Celine's `toGridExact` anchor.)
    const auto before = view_.toGridExact(e.position);

    const float factor = wheel.deltaY > 0.0f ? kZoomStep : (1.0f / kZoomStep);
    view_.gridSize = std::clamp(view_.gridSize * factor, kMinGridSize, kMaxGridSize);

    view_.origin = e.position
                   - juce::Point<float>(before.x * view_.gridSize, before.y * view_.gridSize);

    rebuildGeometry();
    repaint();
}

// ============================================================================
// Gestures
// ============================================================================

bool SchematicEditor::grabResizeHandle(const juce::MouseEvent& e, juce::Point<int> grid) {
    juce::ignoreUnused(grid);

    const int id = wireWithHandles();
    if (id < 0)
        return false;

    const auto* wire = schematic_.findWire(id);
    if (wire == nullptr)
        return false;

    const int end = wireHandleAt(*wire, e.position);
    if (end < 0)
        return false;

    // The *end* (0 or 1), not the wire id. The wire is the single selected one
    // (`wireWithHandles`), which is the same condition the diamonds are drawn
    // under -- so there is exactly one wire these two numbers can disagree about,
    // and they cannot. (Celine's `resizingWireEnd`, ported: the id used to be
    // stored here and read back as an end index, which is two meanings for one
    // field.)
    resizingWireEnd_ = end;
    resizingWireId_ = id;
    dragMovedSomething_ = false;
    return true;
}

void SchematicEditor::beginWireGesture(const juce::MouseEvent& e, juce::Point<int> grid) {
    const auto pin = pinAtPixel(e.position);

    // A wire starts at a pin when there is one under the pointer, and at the
    // bare grid otherwise -- so a wire can be routed between two points before
    // either part exists.
    if (pin.first.empty())
        { /* bare grid */ }
    else if (const auto* p = schematic_.findPart(pin.first)) {
        const auto pos = Schematic::pinPosition(*p, pin.second);
        grid = {pos.x, pos.y};
    }

    draggingWire_ = true;
    wireStart_ = grid;
    wireEnd_ = grid;
    // The pin the drag *started* from, kept for the highlight: the preview
    // rings the end it will land on already, and a terminal holding the
    // other end should wear a ring too or it reads as connected on one side
    // only. A bare-grid start has no pin and the ring simply does not draw.
    wireStartPin_ = pin;
}

void SchematicEditor::updateWireEnd(juce::Point<float> position) {
    const auto grid = view_.toGrid(position);
    wireEnd_ = grid;
    wireEndPin_ = {};

    // **The end snaps to a pin under the pointer**, exactly as it will when the
    // wire is committed -- so the drawing shows where the wire will *land*,
    // not where the pointer happens to be.
    //
    // This is the difference between a wire that lands and a wire that stops a
    // square short: with the preview, the user can see which it is going to
    // be before letting go of the mouse.
    const auto pin = pinAtPixel(position);
    if (pin.first.empty())
        return;

    const auto* p = schematic_.findPart(pin.first);
    if (p == nullptr)
        return;

    const auto pos = Schematic::pinPosition(*p, pin.second);
    wireEnd_ = {pos.x, pos.y};
    wireEndPin_ = pin;
}

void SchematicEditor::beginSelectGesture(const juce::MouseEvent& e, juce::Point<int> grid) {
    if (grabResizeHandle(e, grid))
        return;

    const auto hit = partAtPixel(e.position);

    // **Shift *or* Command adds to the selection.** The two are the same
    // gesture to the user -- "and this one too" -- and the platform's
    // Command is Ctrl on Windows and Linux and Cmd on a Mac, so
    // `isCommandDown` covers both.
    const bool additive = e.mods.isShiftDown() || e.mods.isCommandDown();

    if (!hit.empty()) {
        // Shift-click adds or removes and starts no drag: building a selection up
        // part by part should not move anything.
        if (additive) {
            togglePartInSelection(hit);
            draggingPart_ = false;
            return;
        }

        if (!isSelected(hit))
            setSelection(hit);

        beginDrag(grid);
        return;
    }

    // Not a part: a wire, then a box.
    if (const int wire = wireAtPixel(e.position); wire >= 0) {
        if (additive) {
            toggleWireInSelection(wire);
            draggingPart_ = false;
            return;
        }

        if (!isWireSelected(wire))
            setSelectedWires({wire});

        beginDrag(grid);
        return;
    }

    // Empty sheet: clear (unless adding) and start a box.
    if (!additive)
        setSelection(std::string());

    boxSelectionStarted_ = true;
    boxCurrent_ = e.position;
    boxAnchor_ = e.position;
    boxSelecting_ = false;
}

void SchematicEditor::beginDrag(juce::Point<int> grid) {
    draggingPart_ = true;
    dragMovedSomething_ = false;
    dragLastGrid_ = grid;
}

void SchematicEditor::placePendingPart(juce::Point<int> grid) {
    std::string newId;

    if (pendingClone_) {
        wdf::SchematicComponent copy = *pendingClone_;
        copy.id.clear();
        copy.visual.x = static_cast<float>(grid.x);
        copy.visual.y = static_cast<float>(grid.y);
        copy.visual.rotation = pendingOrientation_;
        copy.visual.flipped = pendingMirrored_;

        newId = schematic_.addPart(std::move(copy));

        // The copy starts on its own nets: it must not silently share the node
        // names of the part it was cloned from.
        if (auto* placed = schematic_.findPart(newId)) {
            for (const auto& pin : partPins(*placed))
                placed->pins[pin] = danglingNet(placed->id, pin);
        }
    } else {
        // Built through the palette entry, not just the type, so a model row
        // places that exact device: 2N5457 is a 2N5457 from the first click.
        wdf::SchematicComponent part = makeComponent(
            pendingType_, schematic_.getParts(), static_cast<float>(grid.x),
            static_cast<float>(grid.y),
            pendingSpec_ ? pendingSpec_->modelProperty : std::string(),
            pendingSpec_ ? pendingSpec_->model : std::string());
        newId = schematic_.addPart(std::move(part));

        if (auto* placed = schematic_.findPart(newId)) {
            placed->visual.rotation = pendingOrientation_;
            placed->visual.flipped = pendingMirrored_;
        }
    }

    if (!newId.empty())
        setSelection(newId);

    rebuildGeometry();
    notifyChanged();

    // Placing a fresh part keeps the tool armed -- laying out a row of resistors
    // is one gesture, not one gesture and one letter press per part. Placing a
    // *clone* is a one-off: it was picked up by a middle-click and is put down
    // once, like dragging a part out of a bin.
    if (pendingClone_) {
        pendingClone_.reset();
        pendingType_.clear();
        setTool(Tool::Select);
    }
}

void SchematicEditor::selectPart(const std::string& id) {
    setSelection(id);
}

bool SchematicEditor::placePartAt(int gridX, int gridY) {
    if (pendingType_.empty() && !pendingClone_)
        return false;

    placePendingPart({gridX, gridY});
    return hasSelection();
}

bool SchematicEditor::deleteAtPixel(juce::Point<float> pixel) {
    const auto part = partAtPixel(pixel);
    if (!part.empty()) {
        schematic_.removePart(part);
        selectedIds_.clear();
        selectedWireIds_.clear();
        rebuildGeometry();
        notifyChanged();
        if (onSelectionChanged)
            onSelectionChanged();
        return true;
    }

    const int wire = wireAtPixel(pixel);
    if (wire >= 0) {
        schematic_.removeWire(wire);
        selectedWireIds_.clear();
        rebuildGeometry();
        notifyChanged();
        if (onSelectionChanged)
            onSelectionChanged();
        return true;
    }

    return false;
}

juce::Rectangle<float> SchematicEditor::getSelectionBox() const noexcept {
    return juce::Rectangle<float>::leftTopRightBottom(
        std::min(boxAnchor_.x, boxCurrent_.x), std::min(boxAnchor_.y, boxCurrent_.y),
        std::max(boxAnchor_.x, boxCurrent_.x), std::max(boxAnchor_.y, boxCurrent_.y));
}

bool SchematicEditor::isCrossingBox() const noexcept {
    // Drawing the box right-to-left asks "anything I touched"; left-to-right
    // asks "everything I surrounded". The painter reads the same answer to pick
    // the dashed crossing colour versus the solid enclosing one, so the gesture
    // and its feedback cannot disagree.
    return boxCurrent_.x < boxAnchor_.x;
}

void SchematicEditor::applyBoxSelection(bool additive) {
    const auto box = getSelectionBox();
    const bool crossing = isCrossingBox();

    // **The box stays in pixels.** It is a pointer gesture, and rounding it to
    // the grid first would make a box drawn just inside a part's edge catch it
    // when zoomed out. The crossing test converts the box to grid units only so
    // the model can compare it with the artwork's own coordinates.
    const juce::Rectangle<float> pixelBox = box;
    const auto gridTopLeft = view_.toGridExact({pixelBox.getX(), pixelBox.getY()});
    const auto gridBottomRight = view_.toGridExact({pixelBox.getRight(), pixelBox.getBottom()});
    const juce::Rectangle<float> gridBox(
        gridTopLeft.x, gridTopLeft.y,
        gridBottomRight.x - gridTopLeft.x, gridBottomRight.y - gridTopLeft.y);

    std::vector<std::string> parts = additive ? selectedIds_ : std::vector<std::string>{};
    std::vector<int> wires = additive ? selectedWireIds_ : std::vector<int>{};

    for (const auto& part : schematic_.getParts()) {
        // Left-to-right encloses: the part's whole bounds must be inside.
        // Right-to-left crosses: only drawn ink counts, so a box in the empty
        // space inside a valve's bounding box does not pick the valve.
        bool hit = false;
        if (crossing) {
            hit = schematic_.partTouchesBox(part, gridBox);
        } else {
            const auto bounds = Schematic::partBounds(part);
            const juce::Rectangle<float> pixels(
                view_.toPixel(bounds.getX(), bounds.getY()),
                view_.toPixel(bounds.getRight(), bounds.getBottom()));
            hit = pixelBox.contains(pixels);
        }

        if (hit && std::find(parts.begin(), parts.end(), part.id) == parts.end())
            parts.push_back(part.id);
    }

    for (const auto& wire : schematic_.getWires()) {
        const juce::Line<float> line(view_.toPixel(wire.ax, wire.ay),
                                     view_.toPixel(wire.bx, wire.by));

        // Same rule as the parts: enclosing needs the whole wire inside,
        // crossing takes anything the box touches.
        //
        // **Crossing tests the *line*, not its bounding rectangle.** Every wire
        // is axis-aligned, so that rectangle has zero width or height, and
        // `Rectangle::intersects` ends with "&& other.w > 0 && other.h > 0" --
        // false for every wire on the sheet. Ours tested the rectangle, so a
        // crossing box never caught a single wire.
        const bool hit = crossing
                             ? pixelBox.intersects(line)
                             : pixelBox.contains(juce::Rectangle<float>(line.getStart(),
                                                                       line.getEnd()));

        if (hit && std::find(wires.begin(), wires.end(), wire.id) == wires.end())
            wires.push_back(wire.id);
    }

    // One combined commit: a box may enclose parts and wires together, and an
    // additive box must keep both halves of the previous selection too.
    setSelection(std::move(parts), std::move(wires));
}

// ============================================================================
// Keyboard
// ============================================================================

} // namespace livewdf::editor
