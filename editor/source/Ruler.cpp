#include "Ruler.h"

#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

namespace livewdf::editor {

namespace {

// The thickness of each strip. Celine's numbers: the top ruler is taller because
// a horizontal number reads at full height, where a vertical one is rotated into
// a narrower column.
constexpr int kTopThickness = 33;
constexpr int kLeftThickness = 29;

/// The smallest step from a human ladder that keeps numbers from colliding.
///
/// Ported from Celine's `{1,2,5,10,20,50,100}` scan: at 40 px per square every
/// square can be numbered, at 5 px per square only every tenth can. A "20"
/// between the fives is far easier to read off than a jump straight from 10 to
/// 100 with nothing between.
int niceStep(float gridSize) {
    int step = 1;

    for (const int candidate : {1, 2, 5, 10, 20, 50, 100}) {
        step = candidate;

        if (gridSize * static_cast<float>(step) >= 44.0f)
            break;
    }

    return step;
}

}  // namespace

Ruler::Ruler(Axis axis, SchematicEditor& editor)
    : axis_(axis), editor_(editor) {
    // Opaque so repainting the ruler never drags the sheet underneath it along;
    // click-through so the canvas still receives the mouse in the ruler's pixels.
    // (Celine's `Ruler` constructor, ported: a ruler repaints on every mouse move,
    // so a translucent one would repaint the whole canvas with it.)
    setOpaque(true);
    setInterceptsMouseClicks(false, false);
    setWantsKeyboardFocus(false);
}

int Ruler::thickness() const noexcept {
    return axis_ == Axis::Horizontal ? kTopThickness : kLeftThickness;
}

void Ruler::setCursor(juce::Point<float> p) {
    if (cursor_ == p)
        return;

    cursor_ = p;
    repaint();
}

void Ruler::paint(juce::Graphics& g) {
    const bool horiz = axis_ == Axis::Horizontal;

    const float thickness = static_cast<float>(Ruler::thickness());

    // Where this strip's numbering may begin: after *the other* ruler, which owns
    // the corner they share. Crossed on purpose -- the top ruler is stopped by the
    // left one's thickness and vice versa -- and it matters because the two are
    // not the same size.
    const float crossThickness =
        static_cast<float>(horiz ? kLeftThickness : kTopThickness);

    // The frame, not the sheet: the same editorial ground as the toolbar and the
    // inspector, so the sheet reads as something set into the chrome.
    g.fillAll(glow::getColourBackground());

    // A definite edge, which is what separates the frame from the sheet where the
    // two colours nearly meet. The two edges meet at the sheet's top-left and run
    // outwards, leaving the block where the rulers overlap as plain chrome --
    // drawing a line through it would put a stray stub across the corner.
    g.setColour(glow::getColourNormal());

    if (horiz) {
        g.fillRect(0.0f, thickness - 2.0f, static_cast<float>(getWidth()), 2.0f);
    } else {
        g.fillRect(thickness - 2.0f, crossThickness - 2.0f, 2.0f,
                   static_cast<float>(getHeight()) - crossThickness + 2.0f);
    }

    const float gridSize = editor_.gridSize();
    const auto origin = editor_.gridOriginF();
    const int step = niceStep(gridSize);

    // Grid -> pixel, the same map `SheetView::toPixel` builds, reconstructed here
    // because a ruler is a sibling of the canvas rather than its child and is not
    // handed the view.
    const auto toPixelAlong = [&](int n) {
        return (horiz ? origin.x : origin.y) + static_cast<float>(n) * gridSize;
    };

    // The strip covers the whole edge, but the canvas it labels starts after the
    // other ruler, so the first number is whichever grid line falls just past it.
    const float span = horiz ? static_cast<float>(getWidth())
                             : static_cast<float>(getHeight());
    const float start = horiz ? origin.x : origin.y;

    const int from = static_cast<int>((crossThickness - start) / gridSize) - step;
    const int to = static_cast<int>((span - start) / gridSize) + step;

    const auto tickColour = glow::getColourNormal();
    const auto labelColour = glow::getColourNormal();

    g.setFont(Fonts::light(13.0f));

    for (int n = (from / step) * step; n <= to; n += step) {
        const float at = toPixelAlong(n);

        if (at < crossThickness || at > span)
            continue;

        g.setColour(tickColour);

        if (horiz)
            g.drawLine(at, thickness - 8.0f, at, thickness - 2.0f);
        else
            g.drawLine(thickness - 8.0f, at, thickness - 2.0f, at);

        g.setColour(labelColour);

        if (horiz)
            g.drawText(juce::String(n),
                       juce::Rectangle<float>(at - 20.0f, 3.0f, 40.0f, 13.0f),
                       juce::Justification::centred, false);
        else
            g.drawText(juce::String(n),
                       juce::Rectangle<float>(1.0f, at - 7.0f, thickness - 10.0f, 14.0f),
                       juce::Justification::centredRight, false);
    }

    // The cursor. Drawn at the pointer's actual pixel rather than snapped to the
    // grid, so it tracks the mouse instead of hopping a square at a time.
    const float mark = horiz ? cursor_.x : cursor_.y;

    if (mark >= crossThickness) {
        // The ruler cursor wears Céline's `cursorMark` role, which is the accent
        // (`#9761dc`) -- the same colour as a wire, not the selection orange. It
        // was on `highlight()`, which read as "this is selected".
        g.setColour(glow::getColourCursorMark());

        if (horiz)
            g.fillRect(mark - 1.0f, 0.0f, 2.0f, thickness);
        else
            g.fillRect(0.0f, mark - 1.0f, thickness, 2.0f);
    }
}

}  // namespace livewdf::editor
