#pragma once

#include "SchematicEditor.h"

namespace livewdf::editor {

/// A numbered strip along one edge of the sheet.
///
/// **Ported from Celine** (`SchematicCanvas::Ruler`,
/// `Celine/source/ui/SchematicCanvas.cpp`), because the two things it gets right
/// are not obvious:
///
///   * it is **opaque and click-through**. Opaque so repainting it never drags the
///     whole canvas underneath along -- a ruler repaints on every mouse move, and
///     a translucent one would repaint the sheet with it. Click-through so the
///     sheet still receives the mouse in the strip's own pixels, which is how a
///     drag that leaves the canvas still ends cleanly.
///   * the label step is adapted to the zoom ({1,2,5,10,20,50,100} grid squares,
///     the smallest that keeps ~44 px between numbers), so the numbers never
///     collide however far in or out you are.
///
/// It is a **child component** rather than something the canvas paints, so the
/// canvas's own repaint does not redraw the rulers and vice versa -- which is the
/// whole reason they are components at all.
///
/// Cathodyne-styled: the frame uses the warm chrome ground and the ticks and
/// numbers use the muted warm ink (`getColourBackground` / `getColourNormal`),
/// rather than the grey/yellow placeholders an earlier stub carried, so the
/// chrome matches the rest of the sheet.
class Ruler : public juce::Component {
public:
    enum class Axis { Horizontal, Vertical };

    Ruler(Axis axis, SchematicEditor& editor);

    void paint(juce::Graphics& g) override;

    /// The pointer position, in the *canvas's* pixels, or a negative x/y when the
    /// pointer is outside. Drawn at the pointer's actual pixel rather than snapped
    /// to the grid, so it tracks the mouse instead of hopping a square at a time.
    void setCursor(juce::Point<float> p);

    /// How thick this strip is, in pixels. Read by the canvas when it lays the
    /// sheet out, and by the other ruler when it decides where its numbering may
    /// begin (the corner they share belongs to the *other* ruler).
    int thickness() const noexcept;

private:
    Axis axis_;
    SchematicEditor& editor_;
    juce::Point<float> cursor_{-1.0f, -1.0f};
};

}  // namespace livewdf::editor