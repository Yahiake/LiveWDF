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
// Hit testing
// ============================================================================

juce::Point<float> SchematicEditor::partCentrePixel(const wdf::SchematicComponent& part) const {
    return view_.origin
           + juce::Point<float>(view_.gridSize * static_cast<float>(part.visual.x),
                                view_.gridSize * static_cast<float>(part.visual.y));
}

juce::Point<float> SchematicEditor::gridToPixel(int gx, int gy) const {
    return view_.origin
           + juce::Point<float>(view_.gridSize * static_cast<float>(gx),
                                view_.gridSize * static_cast<float>(gy));
}

juce::Rectangle<float> SchematicEditor::drawnWiresBounds() const {
    // **Min and max, not `getUnion`.** A wire's path is a straight line, so its
    // bounds have zero width or zero height -- and JUCE's `getUnion` answers
    // with the *other* rectangle whenever either side is empty. A union of
    // those is the union of nothing.
    if (wirePaths_.empty())
        return {};

    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    bool first = true;

    for (const auto& wp : wirePaths_) {
        const auto b = wp.path.getBounds();
        if (first) {
            minX = b.getX(); minY = b.getY();
            maxX = b.getRight(); maxY = b.getBottom();
            first = false;
            continue;
        }

        minX = juce::jmin(minX, b.getX());
        minY = juce::jmin(minY, b.getY());
        maxX = juce::jmax(maxX, b.getRight());
        maxY = juce::jmax(maxY, b.getBottom());
    }

    return juce::Rectangle<float>(minX, minY, maxX - minX, maxY - minY);
}

std::string SchematicEditor::partAtPixel(juce::Point<float> pixel) const {
    const auto grid = view_.toGridExact(pixel);

    // **The shape, not the box.** `Schematic::partAt` walks the artwork itself:
    // a closed symbol is picked across its body, a line along its ink, and the
    // empty corners of the old bounding box are not the part. The reach is in
    // pixels so the target stays the same size as the user zooms; converting it
    // here is the only place the view has to meet the model's grid units.
    const float reach = kPartPickPixels / std::max(1.0f, view_.gridSize);

    return schematic_.partAt(grid.x, grid.y, reach);
}

int SchematicEditor::wireAtPixel(juce::Point<float> pixel) const {
    const auto grid = view_.toGridExact(pixel);

    // The reach is in pixels, like the pin's, because a hit target is something
    // you aim a pointer at and the pointer does not zoom. It is at least the
    // wire's own width, so a thick wire is as easy to catch as a thin one.
    const float wireWidth = std::max(1.0f, view_.gridSize * 0.13f);
    const float tolerance = std::max(kWirePickPixels, wireWidth) / std::max(1.0f, view_.gridSize);

    return schematic_.wireNear(grid.x, grid.y, tolerance);
}

std::pair<std::string, int> SchematicEditor::pinAtPixel(juce::Point<float> pixel) const {
    const auto grid = view_.toGridExact(pixel);
    const float reachSquares = kPinPickPixels / view_.gridSize;

    std::string bestPart;
    int bestPin = -1;
    float bestDistance = reachSquares;

    const auto& parts = schematic_.getParts();
    for (const auto& part : parts) {
        const auto offsets = Schematic::pinOffsets(part.type);
        for (int pin = 0; pin < static_cast<int>(offsets.size()); ++pin) {
            const auto p = Schematic::pinPosition(part, pin);
            const float dx = grid.x - static_cast<float>(p.x);
            const float dy = grid.y - static_cast<float>(p.y);
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d <= bestDistance) {
                bestDistance = d;
                bestPart = part.id;
                bestPin = pin;
            }
        }
    }

    return {bestPart, bestPin};
}

int SchematicEditor::wireWithHandles() const noexcept {
    // Handles on a single selected wire only. Two wires selected at once would
    // put four diamonds on the sheet and no way to say which end you grabbed.
    if (selectedWireIds_.size() != 1)
        return -1;
    return selectedWireIds_.front();
}

float SchematicEditor::wireHandleReach() const noexcept {
    // Celine's `wireHandleReach`: a *pixel* reach that grows with zoom, so the
    // diamond is equally easy to grab however far in you are, and a floor so it
    // stays grabbable when zoomed out.
    return std::max(3.0f, view_.gridSize * 0.34f);
}

int SchematicEditor::wireHandleAt(const Wire& wire, juce::Point<float> pixel) const noexcept {
    // The diamond that is drawn and not a pixel more: in pixels against the
    // handle, not grid squares against the snapped point. Manhattan distance,
    // which is the diamond exactly. (Celine's `wireHandleAt`, ported -- the
    // Euclidean circle this used to be reached into the corners of a shape that
    // is not drawn there.)
    const float reach = wireHandleReach();

    const auto near = [this, pixel, reach](juce::Point<int> end) {
        const auto centre = view_.toPixel(end);
        return std::abs(pixel.x - centre.x) + std::abs(pixel.y - centre.y) <= reach;
    };

    // `a` first: on a wire two squares or shorter the grab bands overlap, and
    // without a fixed answer the end you caught would depend on which way the
    // wire happened to be stored.
    if (near({wire.ax, wire.ay}))
        return 0;

    return near({wire.bx, wire.by}) ? 1 : -1;
}

// ============================================================================
// Selection
// ============================================================================

void SchematicEditor::rebuildGeometry() {
    wirePaths_.clear();
    flowPaths_.clear();

    for (const auto& wire : schematic_.getWires()) {
        WirePath wp;
        wp.wireId = wire.id;
        wp.path.startNewSubPath(view_.toPixel(wire.ax, wire.ay));
        wp.path.lineTo(view_.toPixel(wire.bx, wire.by));
        wirePaths_.push_back(std::move(wp));

        FlowPath fp;
        fp.path = wirePaths_.back().path;
        fp.length = static_cast<float>(std::abs(wire.bx - wire.ax) + std::abs(wire.by - wire.ay))
                    * view_.gridSize;
        flowPaths_.push_back(std::move(fp));
    }
}

} // namespace livewdf::editor