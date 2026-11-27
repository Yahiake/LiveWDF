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

namespace {
void clearCopiedPreampTap(wdf::SchematicComponent& part) {
    if (part.properties.erase("preampTap") != 0)
        part.values.erase("preampFullScaleVolts");
}
}  // namespace

void SchematicEditor::copySelection() {
    ClipboardGroup group;

    // **The selected parts**, with their values and their settings but *not*
    // their nets: a pasted part gets fresh ones, so it cannot join the circuit
    // it is pasted into by accident.
    for (const auto& id : selectedIds_) {
        const auto* part = schematic_.findPart(id);
        if (part == nullptr)
            continue;

        ClipboardPart entry;
        entry.type = part->type;
        entry.x = part->visual.x;
        entry.y = part->visual.y;
        entry.rotation = part->visual.rotation;
        entry.flipped = part->visual.flipped;
        entry.values = part->values;
        entry.properties = part->properties;

        entry.pins = partPins(*part);

        group.parts.push_back(std::move(entry));
    }

    // **The wires among them**: the ones the user selected, and the ones whose
    // two ends both land on a selected part -- a group's wiring is part of the
    // group.
    const auto selectedPart = [this](const std::string& id) {
        return std::find(selectedIds_.begin(), selectedIds_.end(), id) != selectedIds_.end();
    };

    for (const auto& wire : schematic_.getWires()) {
        const bool chosen = isWireSelected(wire.id);

        // Both ends on a selected part?
        bool bothEnds = false;
        bool anyEnd = false;
        for (const auto& part : schematic_.getParts()) {
            if (!selectedPart(part.id))
                continue;
            for (int pin = 0; pin < static_cast<int>(Schematic::pinOffsets(part).size()); ++pin) {
                const auto p = Schematic::pinPosition(part, pin);
                if ((p.x == wire.ax && p.y == wire.ay) || (p.x == wire.bx && p.y == wire.by))
                    anyEnd = true;
            }
        }
        bothEnds = anyEnd && countEndsOnSelectedParts(wire) == 2;

        if (!chosen && !bothEnds)
            continue;

        group.wires.push_back({wire.ax, wire.ay, wire.bx, wire.by});
    }

    clipboard_ = std::move(group);

    // **And onto the system clipboard**, so it can be pasted into another
    // editor process -- which is the case that matters when the answer to
    // "how do I get this sub-circuit from that window" is "copy it".
    //
    // The system clipboard carries text, and the payload starts with a line
    // that says it is ours, so a paste can tell the difference between a group
    // and whatever else was on the clipboard.
    juce::SystemClipboard::copyTextToClipboard(juce::String(clipboard_.toText()));
}

int SchematicEditor::countEndsOnSelectedParts(const Wire& wire) const {
    int ends = 0;

    for (const auto& part : schematic_.getParts()) {
        if (std::find(selectedIds_.begin(), selectedIds_.end(), part.id) == selectedIds_.end())
            continue;

        for (int pin = 0; pin < static_cast<int>(Schematic::pinOffsets(part).size()); ++pin) {
            const auto p = Schematic::pinPosition(part, pin);
            if ((p.x == wire.ax && p.y == wire.ay) || (p.x == wire.bx && p.y == wire.by))
                ++ends;
        }
    }

    return ends;
}

void SchematicEditor::cutSelection() {
    copySelection();

    // **And take it off the sheet.** The parts and the wires among them, so
    // the sheet is left as it was before the copy.
    const auto parts = selectedIds_;
    for (const auto& id : parts)
        schematic_.removePart(id);

    for (const auto id : selectedWireIds_)
        schematic_.removeWire(id);

    setSelection(std::string());
    setSelectedWires({});

    rebuildGeometry();
    notifyChanged();
}

void SchematicEditor::pasteClipboard() {
    // **The system clipboard first**, so a group copied in another process
    // pastes here; the editor's own copy is the fallback when there is no
    // system clipboard to ask (a headless test, say).
    {
        const auto text = juce::SystemClipboard::getTextFromClipboard();
        if (text.isNotEmpty()) {
            const auto fromSystem = ClipboardGroup::fromText(text.toStdString());
            if (!fromSystem.isEmpty())
                clipboard_ = fromSystem;
        }
    }

    if (clipboard_.isEmpty())
        return;

    // **Where it lands.** Under the pointer when the pointer is over the
    // sheet; where it was copied from when it is not.
    int offsetX = 0, offsetY = 0;

    if (hoverGrid_.x >= 0 && hoverGrid_.y >= 0) {
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        clipboard_.bounds(minX, minY, maxX, maxY);

        const int centreX = static_cast<int>(std::lround((minX + maxX) * 0.5f));
        const int centreY = static_cast<int>(std::lround((minY + maxY) * 0.5f));
        offsetX = hoverGrid_.x - centreX;
        offsetY = hoverGrid_.y - centreY;
    }

    // **The parts**, with fresh ids and fresh nets -- the same as a newly
    // placed part -- and their values and settings carried over.
    std::vector<std::string> pasted;
    bool hasPreampTap = std::ranges::any_of(doc_.components, [](const auto& part) {
        return part.properties.contains("preampTap");
    });

    for (const auto& entry : clipboard_.parts) {
        const auto id = schematic_.addPart(entry.type,
                                           static_cast<int>(std::lround(entry.x)) + offsetX,
                                           static_cast<int>(std::lround(entry.y)) + offsetY);
        if (id.empty())
            continue;

        if (auto* part = schematic_.findPart(id)) {
            part->visual.rotation = entry.rotation;
            part->visual.flipped = entry.flipped;
            part->values = entry.values;
            part->properties = entry.properties;
            if (hasPreampTap)
                clearCopiedPreampTap(*part);
            else
                hasPreampTap = part->properties.contains("preampTap");
        }

        pasted.push_back(id);
    }

    // **The wires**, offset with them. The group's own wiring is preserved by
    // the geometry: the net extractor joins the pins by position, so the parts
    // and the wires arrive joined.
    for (const auto& wire : clipboard_.wires)
        schematic_.addWire(wire.ax + offsetX, wire.ay + offsetY,
                           wire.bx + offsetX, wire.by + offsetY);

    // The pasted parts are selected, so they can be dragged into place
    // straight away -- which is what a paste is for.
    setSelection(pasted);

    rebuildGeometry();
    notifyChanged();
    repaint();
}

void SchematicEditor::armPlacement(const std::string& partType) {
    // Keep the type-only entry point for the keyboard shortcuts and tests: it
    // arms the family's default palette row (the first model).
    if (const PartSpec* spec = findPart(partType)) {
        armPlacement(*spec);
        return;
    }

    pendingClone_.reset();
    pendingType_ = keyForPartType(partType);
    pendingSpec_ = nullptr;
    pendingOrientation_ = 0;
    pendingMirrored_ = false;

    tool_ = Tool::Place;
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
    if (isShowing())
        grabKeyboardFocus();
    repaint();
    if (onToolChanged)
        onToolChanged();
}

void SchematicEditor::armPlacement(const PartSpec& spec) {
    pendingClone_.reset();
    pendingType_ = keyForPartType(spec.type);

    // **The chosen row, not just its type.** A model-bearing family has one
    // palette row per device; placing the row the user clicked is what makes
    // "2N5457" place a 2N5457 rather than the family default.
    pendingSpec_ = &spec;

    // A part's default placement is part of its palette entry. `makeComponent`
    // applies the row's values, but `placePendingPart` overwrites them with
    // the pending orientation, so arming has to seed that state from the same
    // row -- the swatch and the placed part make one promise. Rotating or
    // flipping while armed then edits this start value; it stays sticky across
    // placements until the part is armed again.
    pendingOrientation_ = spec.defaultRotation;
    pendingMirrored_ = spec.defaultFlipped;

    tool_ = Tool::Place;
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
    if (isShowing())
        grabKeyboardFocus();
    repaint();
    if (onToolChanged)
        onToolChanged();
}

void SchematicEditor::clonePart(const std::string& partId) {
    const auto* source = schematic_.findPart(partId);
    if (source == nullptr)
        return;

    // Copy the whole component, not just its type: wanting five more of the
    // resistor you already set up should not mean setting the value up four more
    // times. The copy is held, not placed -- the next click drops it.
    pendingClone_ = *source;
    clearCopiedPreampTap(*pendingClone_);
    pendingType_ = source->type;
    pendingSpec_ = nullptr;
    pendingOrientation_ = source->visual.rotation;
    pendingMirrored_ = source->visual.flipped;
    tool_ = Tool::Place;
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
    repaint();
    if (onToolChanged)
        onToolChanged();
}

void SchematicEditor::rotate() {
    // The armed part turns first: rotating what you are about to place is the
    // common case, and it must not need a placement first.
    if (tool_ == Tool::Place && !pendingType_.empty()) {
        // `pendingOrientation_` is in **degrees** -- the same unit as
        // `VisualPlacement::rotation` and `VisualPlacement::flipped` beside it,
        // and the same unit the clone path copies (`source->visual.rotation`).
        //
        // It was stepped as if it were a quarter-turn count, with a mask:
        // `(0 + 90) & 3` is 2, so the first press of R set the pending part's
        // rotation to *two degrees*, and the part came out unturned. The mask is
        // what made it look deliberate; it is simply the wrong unit.
        pendingOrientation_ = (pendingOrientation_ + 90) % 360;
        repaint();
        return;
    }

    bool changed = false;
    for (const auto& id : selectedIds_)
        if (auto* part = schematic_.findPart(id)) {
            part->visual.rotation = (part->visual.rotation + 90) % 360;
            changed = true;
        }

    if (changed) {
        rebuildGeometry();
        notifyChanged();
    }
}

void SchematicEditor::flip() {
    if (tool_ == Tool::Place && !pendingType_.empty()) {
        pendingMirrored_ = !pendingMirrored_;
        repaint();
        return;
    }

    bool changed = false;
    for (const auto& id : selectedIds_)
        if (auto* part = schematic_.findPart(id)) {
            part->visual.flipped = !part->visual.flipped;
            changed = true;
        }

    if (changed) {
        rebuildGeometry();
        notifyChanged();
    }
}

void SchematicEditor::duplicateSelection() {
    if (selectedIds_.empty())
        return;

    // Offset so the copies are visibly separate and immediately draggable rather
    // than hidden exactly behind what they came from. (Celine's constant.)
    constexpr int offset = 2;

    std::vector<std::string> copies;

    for (const auto& id : selectedIds_) {
        const auto* source = schematic_.findPart(id);
        if (source == nullptr)
            continue;

        // Read the source out before adding: adding can reallocate the part
        // vector and invalidate the pointer.
        wdf::SchematicComponent original = *source;
        clearCopiedPreampTap(original);

        original.id.clear();   // a fresh id is minted from the palette
        original.visual.x += offset;
        original.visual.y += offset;

        const std::string newId = schematic_.addPart(std::move(original));

        if (auto* placed = schematic_.findPart(newId)) {
            // Pins are renamed so the copy starts on its own nets rather than
            // silently being the same node as the original it sits beside.
            for (const auto& pin : partPins(*placed))
                placed->pins[pin] = danglingNet(placed->id, pin);
            copies.push_back(newId);
        }
    }

    // Select the copies, so a second Cmd-D fans them out and a drag moves what
    // you just made.
    selectedIds_ = std::move(copies);
    selectedWireIds_.clear();

    rebuildGeometry();
    notifyChanged();

    if (onSelectionChanged)
        onSelectionChanged();
}

bool SchematicEditor::deleteSelection() {
    if (selectedIds_.empty() && selectedWireIds_.empty())
        return false;

    for (const auto& id : selectedIds_)
        schematic_.removePart(id);
    for (const int id : selectedWireIds_)
        schematic_.removeWire(id);

    selectedIds_.clear();
    selectedWireIds_.clear();

    rebuildGeometry();
    notifyChanged();

    if (onSelectionChanged)
        onSelectionChanged();

    return true;
}


void SchematicEditor::mergeWiresAfterEdit(int preferredId) {
    if (schematic_.mergeCollinearWires(preferredId) == 0)
        return;

    // An absorbed wire no longer exists, and a selection holding its id would
    // draw nothing and delete nothing. (Celine's `mergeWiresAfterEdit`, ported:
    // the pruning is the half a naive merge forgets.)
    std::vector<int> surviving;

    for (const int id : selectedWireIds_)
        if (schematic_.findWire(id) != nullptr)
            surviving.push_back(id);

    if (surviving.size() != selectedWireIds_.size())
        setSelection(selectedIds_, std::move(surviving));
}

// ============================================================================
// View
// ============================================================================

void SchematicEditor::frameAll() {
    auto bounds = schematic_.contentBounds();

    if (bounds.isEmpty() || getWidth() < 20 || getHeight() < 20) {
        view_.gridSize = 16.0f;
        view_.origin = {24.0f, 24.0f};
        rebuildGeometry();
        repaint();
        return;
    }

    // Breathing room for captions and terminal labels, which are drawn outside the
    // pin geometry the bounds are computed from. (Celine's `expanded(5)`.)
    bounds = bounds.expanded(5);

    const float scaleX =
        static_cast<float>(getWidth()) / std::max(1.0f, bounds.getWidth());
    const float scaleY =
        static_cast<float>(getHeight()) / std::max(1.0f, bounds.getHeight());

    view_.gridSize = std::clamp(std::min(scaleX, scaleY), kMinGridSize, kMaxGridSize);

    // Centre the bounds' own centre in the viewport. (Celine's `zoomToFit`, so a
    // framed sheet is placed the same way as there.)
    view_.origin = {static_cast<float>(getWidth()) * 0.5f
                        - static_cast<float>(bounds.getCentreX()) * view_.gridSize,
                    static_cast<float>(getHeight()) * 0.5f
                        - static_cast<float>(bounds.getCentreY()) * view_.gridSize};

    // **The wires have to be laid out again.**
    //
    // The parts are drawn from the model through the *current* view, so they
    // move with it. The wires are not: their paths are built once, in pixels,
    // by `rebuildGeometry` -- so a change of view left them where they were.
    //
    // That is why "fit" framed the parts and left the wires in the corner, and
    // why a zoom put them right: the zoom rebuilt them. A fit has to do the
    // same thing, or it is not a fit.
    rebuildGeometry();
    repaint();
}


bool SchematicEditor::isSelected(const std::string& partId) const {
    return std::find(selectedIds_.begin(), selectedIds_.end(), partId) != selectedIds_.end();
}

bool SchematicEditor::isWireSelected(int wireId) const noexcept {
    return std::find(selectedWireIds_.begin(), selectedWireIds_.end(), wireId)
           != selectedWireIds_.end();
}

void SchematicEditor::setSelection(const std::string& partId) {
    setSelection(partId.empty() ? std::vector<std::string>{}
                                : std::vector<std::string>{partId});
}

void SchematicEditor::setSelection(std::vector<std::string> ids) {
    // A replacement selection is one kind of thing. Picking a part drops the
    // wires and the other way round, which is what makes a plain click mean
    // "this one thing". Additive gestures use the combined overload below.
    setSelection(std::move(ids), {});
}

void SchematicEditor::setSelectedWires(std::vector<int> ids) {
    setSelection({}, std::move(ids));
}

void SchematicEditor::setSelection(std::vector<std::string> partIds,
                                   std::vector<int> wireIds) {
    selectedIds_ = std::move(partIds);
    selectedWireIds_ = std::move(wireIds);
    // A replacement selection abandons a number that was being typed over the
    // old target -- the next Enter must not land on the part the field now
    // shows if the digits were meant for another one.
    pendingValue_.clear();
    repaint();
    if (onSelectionChanged)
        onSelectionChanged();
}

void SchematicEditor::togglePartInSelection(const std::string& partId) {
    if (partId.empty())
        return;

    auto next = selectedIds_;
    const auto it = std::find(next.begin(), next.end(), partId);
    if (it != next.end())
        next.erase(it);
    else
        next.push_back(partId);

    // Keep the wires: Ctrl/Shift-click builds one selection across both kinds,
    // so toggling a part must not silently drop the wires already chosen.
    setSelection(std::move(next), selectedWireIds_);
}

void SchematicEditor::toggleWireInSelection(int wireId) {
    if (wireId < 0)
        return;

    auto next = selectedWireIds_;
    const auto it = std::find(next.begin(), next.end(), wireId);
    if (it != next.end())
        next.erase(it);
    else
        next.push_back(wireId);

    // Keep the parts: see `togglePartInSelection`.
    setSelection(selectedIds_, std::move(next));
}

// ============================================================================
// Drawing
// ============================================================================



} // namespace livewdf::editor
