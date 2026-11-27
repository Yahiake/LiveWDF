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
void SchematicEditor::paintGrid(juce::Graphics& g) const {
    if (view_.gridSize < kGridDotThreshold)
        return;

    g.setColour(gridDot());

    const auto topLeft = view_.toGrid({0.0f, 0.0f});
    const auto bottomRight =
        view_.toGrid({static_cast<float>(getWidth()), static_cast<float>(getHeight())});

    const float dotR = std::max(0.5f, view_.gridSize * 0.035f);
    const float dotD = dotR * 2.0f;

    for (int y = topLeft.y; y <= bottomRight.y; ++y)
        for (int x = topLeft.x; x <= bottomRight.x; ++x) {
            const auto p = view_.toPixel(x, y);
            g.fillEllipse(p.x - dotR, p.y - dotR, dotD, dotD);
        }
}

void SchematicEditor::paintDrawing(juce::Graphics& g) const {
    // Wires under the parts, so a symbol sits on top of the line that reaches it.
    for (const auto& wp : wirePaths_) {
        g.setColour(wireColour());
        g.strokePath(wp.path, juce::PathStrokeType(wireThickness()));
    }

    // Junction dots: where the connection is not obvious from the drawing. A T
    // with no dot reads as a crossing, and a crossing is not a connection.
    //
    // The colour is set here rather than inherited from the wire loop above,
    // which leaves the last wire's behind. It is the *wire* colour and not the
    // selection's: a dot belongs to the connection, and there is no honest answer
    // for a junction where one of three wires is selected. (Celine's rule and its
    // radius, `jmax(2, gridSize * 0.22)` -- ours was 0.16, so a dot was slightly
    // smaller than the reference for no stated reason.)
    g.setColour(junctionColour());
    const float r = std::max(1.8f, view_.gridSize * 0.19f);
    for (const auto& [jx, jy] : schematic_.findJunctions()) {
        const auto p = view_.toPixel(jx, jy);
        g.fillEllipse(p.x - r, p.y - r, r * 2.0f, r * 2.0f);
    }

    // Parts, each drawn by the shared look-and-feel so the symbol, the pins and
    // the hit test come from one description. `partWithLiveState` applies the
    // plugin's control overlay to the copy the painter sees: the model keeps the
    // file's positions, while the drawing follows the knobs and switches.
    for (const auto& part : schematic_.getParts())
        drawComponent(g, partWithLiveState(part), view_);
}

wdf::SchematicComponent SchematicEditor::partWithLiveState(
    const wdf::SchematicComponent& part) const {
    auto live = part;

    if (part.type == "pot" || part.type == "varres") {
        if (const auto it = livePotPositions_.find(part.id); it != livePotPositions_.end())
            live.values["pos"] = it->second;
    } else if (part.type == "switch_res") {
        if (const auto it = liveSwitchPositions_.find(part.id); it != liveSwitchPositions_.end())
            live.properties["select"] = std::to_string(it->second);
    }

    return live;
}

void SchematicEditor::paintSelection(juce::Graphics& g) const {
    // **The part the build failed on**, marked in red. Drawn under the
    // selection's halo, so a selected part that also failed shows both.
    if (!problemPart_.empty())
        if (const auto* part = schematic_.findPart(problemPart_))
            drawProblemHalo(g, *part, view_);

    // Parts get a halo; wires get a thicker line in the selection colour plus
    // end handles, because a wire has no body to outline.
    for (const auto& id : selectedIds_)
        if (const auto* part = schematic_.findPart(id))
            drawSelectionHalo(g, *part, view_);

    for (const auto& wp : wirePaths_) {
        if (!isWireSelected(wp.wireId))
            continue;

        g.setColour(selectionColour());
        g.strokePath(wp.path, juce::PathStrokeType(wireThickness() * 1.6f));
    }

    if (const int id = wireWithHandles(); id >= 0) {
        if (const auto* wire = schematic_.findWire(id)) {
            const auto drawHandle = [&](int gx, int gy) {
                const auto p = view_.toPixel(gx, gy);
                juce::Path diamond;
                // The same reach the hit test uses, so the diamond that is drawn
                // is the diamond that is grabbable. (Celine draws and grabs off
                // one `wireHandleReach`; ours drew at 0.28 and grabbed at 0.34,
                // which is a handle you could click just outside of.)
                const float h = wireHandleReach();
                diamond.startNewSubPath(p.x, p.y - h);
                diamond.lineTo(p.x + h, p.y);
                diamond.lineTo(p.x, p.y + h);
                diamond.lineTo(p.x - h, p.y);
                diamond.closeSubPath();
                g.setColour(handleFill());
                g.fillPath(diamond);
                g.setColour(selectionColour());
                g.strokePath(diamond, juce::PathStrokeType(1.5f));
            };
            drawHandle(wire->ax, wire->ay);
            drawHandle(wire->bx, wire->by);
        }
    }
}

void SchematicEditor::paintInFlight(juce::Graphics& g) const {
    // The wire being dragged, snapped to the grid.
    if (draggingWire_) {
        g.setColour(selectionColour());
        const auto a = view_.toPixel(wireStart_.x, wireStart_.y);
        const auto b = view_.toPixel(wireEnd_.x, wireEnd_.y);
        g.drawLine(a.x, a.y, b.x, b.y, wireThickness() * 1.3f);

        // The other end previews as an L, exactly as it will be placed, so a
        // diagonal drag is not a surprise afterwards.
        if (wireEnd_.x != wireStart_.x && wireEnd_.y != wireStart_.y) {
            const auto corner = view_.toPixel(wireEnd_.x, wireStart_.y);
            g.drawLine(a.x, a.y, corner.x, corner.y, wireThickness() * 1.3f);
            g.drawLine(corner.x, corner.y, b.x, b.y, wireThickness() * 1.3f);
        }

        // **The pin the wire will land on, marked.** The end of the wire has
        // already snapped to it -- the drawing shows where the wire will
        // *land*, not where the pointer is -- and the ring says so.
        if (!wireEndPin_.first.empty())
            if (const auto* part = schematic_.findPart(wireEndPin_.first)) {
                const auto pos = Schematic::pinPosition(*part, wireEndPin_.second);
                const auto p = view_.toPixel(pos.x, pos.y);
                const float r = std::max(3.0f, view_.gridSize * 0.28f);

                g.setColour(selectionColour().withAlpha(0.9f));
                g.drawEllipse(p.x - r, p.y - r, r * 2.0f, r * 2.0f, 2.0f);
            }
    }

    // The selection box.
    if (boxSelecting_) {
        const auto box = getSelectionBox();
        const bool crossing = isCrossingBox();
        g.setColour(crossing ? boxCrossColour() : boxEncloseColour());

        if (crossing) {
            // Dashed for a crossing box: left-to-right encloses, right-to-left
            // crosses, and the drawing says which is in force.
            const float dashes[] = {5.0f, 4.0f};
            juce::Path p;
            p.addRectangle(box);
            juce::PathStrokeType(1.2f).createDashedStroke(p, p, dashes, 2);
            g.strokePath(p, juce::PathStrokeType(1.2f));
        } else {
            g.drawRect(box, 1.2f);
        }

        g.setColour((crossing ? boxCrossColour() : boxEncloseColour()).withAlpha(0.08f));
        g.fillRect(box);
    }

    // The part about to be placed, under the pointer at the place and
    // orientation it will land. Placing blind and then moving is two actions and
    // one mistake.
    if (tool_ == Tool::Place && !pendingType_.empty()) {
        wdf::SchematicComponent ghost;
        if (pendingClone_) {
            ghost = *pendingClone_;
            ghost.visual.x = static_cast<float>(hoverGrid_.x);
            ghost.visual.y = static_cast<float>(hoverGrid_.y);
            ghost.visual.rotation = pendingOrientation_;
            ghost.visual.flipped = pendingMirrored_;
        } else {
            ghost.type = pendingType_;
            ghost.visual.x = static_cast<float>(hoverGrid_.x);
            ghost.visual.y = static_cast<float>(hoverGrid_.y);
            ghost.visual.rotation = pendingOrientation_;
            ghost.visual.flipped = pendingMirrored_;
        }
        drawComponentGhost(g, ghost, view_);
    }
}

void SchematicEditor::paintTerminalHighlight(juce::Graphics& g) const {
    // **Terminals: the sheet's edges.** An ``in``/``out`` is one small flag with
    // one pin, and the pin dot is where a wire grabs it. The hover that says
    // "grab here" therefore belongs on the dot, in the same ring the wire
    // preview already wears on its landing pin -- and while a wire drag runs,
    // the *start* wears one too, so a terminal that already holds one end of a
    // wire reads as connected from both sides rather than one.
    const auto ringAt = [&](const std::pair<std::string, int>& pin, float alpha) {
        if (pin.first.empty() || pin.second < 0)
            return;
        const auto* part = schematic_.findPart(pin.first);
        if (part == nullptr || (part->type != "in" && part->type != "out"))
            return;

        const auto pos = Schematic::pinPosition(*part, pin.second);
        const auto p = view_.toPixel(pos.x, pos.y);
        const float r = std::max(3.0f, view_.gridSize * 0.28f);

        g.setColour(selectionColour().withAlpha(alpha));
        g.drawEllipse(p.x - r, p.y - r, r * 2.0f, r * 2.0f, 2.0f);
    };

    if (tool_ == Tool::Wire && draggingWire_)
    {
        // Dragging: the start the wire is being pulled out of. The end already
        // has its ring in `paintInFlight`, drawn after this pass.
        ringAt(wireStartPin_, 0.9f);
        return;
    }

    if (tool_ == Tool::Place)
        return;   // the ghost owns the pointer's place; a ring would fight it

    // Hovering a terminal: ring every pin it owns, following the pins rather
    // than assuming one, so a terminal that gains a pin still gets them all
    // ringed.
    if (!hoverTerminal_.empty())
    {
        if (const auto* part = schematic_.findPart(hoverTerminal_))
            if (part->type == "in" || part->type == "out")
                for (int i = 0; i < static_cast<int>(Schematic::pinOffsets(part->type).size()); ++i)
                    ringAt({hoverTerminal_, i}, 0.9f);
        return;
    }

    // Pointer on some other part's pin with the Wire tool in hand: the pin it
    // will start from next. Not terminal rings? `ringAt` answers that itself --
    // it rings terminals only.
    ringAt(hoverPin_, 0.9f);
}

void SchematicEditor::paintProbes(juce::Graphics& g) const {
    // A badge per probe, showing the node and its solved voltage. The node name
    // comes from the model's own connectivity, so a badge cannot disagree with
    // what the netlist will be.
    const auto readouts = probes();

    for (const auto& info : readouts) {
        const auto* part = schematic_.findPart(info.partId);
        if (part == nullptr)
            continue;

        const auto p = view_.toPixel(static_cast<int>(std::lround(part->visual.x)),
                                     static_cast<int>(std::lround(part->visual.y)));
        juce::String text = info.node.empty() ? juce::String("--") : juce::String(info.node);
        if (const auto it = voltages_.find(info.node); it != voltages_.end())
            text += "  " + juce::String(it->second, 3) + "V";

        const float w = 96.0f, h = 18.0f;
        const juce::Rectangle<float> badge(p.x - w * 0.5f, p.y - view_.gridSize - h,
                                           w, h);
        g.setColour(badgeBackground());
        g.fillRoundedRectangle(badge, 4.0f);
        g.setColour(badgeText());
        g.setFont(Fonts::bold(15.0f));
        g.drawText(text, badge.toNearestInt(), juce::Justification::centred);
    }
}

void SchematicEditor::paintSignalFlow(juce::Graphics& g) const {
    if (signalActivity_ <= 0.001f)
        return;

    // A bead per wire, moving along it. The glow helper is the same one the
    // plugin uses, so the editor's "something is happening here" reads the same
    // as the plugin's.
    const float beadRadius = std::max(2.0f, view_.gridSize * 0.14f * (0.6f + signalActivity_));

    for (const auto& fp : flowPaths_) {
        if (fp.length <= 1.0f)
            continue;

        const float t = std::fmod(flowPhase_ + fp.phase, 1.0f);
        const float along = t * fp.length;
        const auto point = fp.path.getPointAlongPath(along);

        g.setColour(glow::getColourSignal().withAlpha(0.35f + 0.5f * signalActivity_));
        g.fillEllipse(point.x - beadRadius, point.y - beadRadius,
                      beadRadius * 2.0f, beadRadius * 2.0f);
    }
}

void SchematicEditor::paint(juce::Graphics& g) {
    g.fillAll(sheetBackground());

    paintGrid(g);
    paintDrawing(g);
    paintSelection(g);
    paintTerminalHighlight(g);
    paintInFlight(g);
    paintSignalFlow(g);
    paintProbes(g);
}

} // namespace livewdf::editor
