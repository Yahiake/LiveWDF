#include "EditorLookAndFeel.h"
#include "ElementAccess.h"
#include "SchematicModel.h"
#include "SymbolArtwork.h"
#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace livewdf::editor {

// ============================================================================
// Pin geometry
// ============================================================================

juce::Point<float> pinOffsetWorld(const PartSpec& spec, int index, int quarterTurns) {
    if (index < 0 || index >= static_cast<int>(spec.pinSides.size()))
        return {};

    // Body half-extents. These match the symbol drawing below, because a pin
    // that sat off the symbol body would read as a mistake. Kept in step with
    // `Schematic::pinOffsets`, which is the authority; the same ±3/±3.5 grid
    // squares, so a pin lands on the body edge at any rotation.
    constexpr float hx = 3.0f;
    constexpr float hy = 3.5f;

    const int side = rotateSide(spec.pinSides[static_cast<size_t>(index)], quarterTurns);

    // How many pins share this side, so they can be spread along it and not
    // land on top of each other (a triode's cathode and grid are both "down").
    int countOnSide = 0, ordinal = 0;
    for (int i = 0; i < static_cast<int>(spec.pinSides.size()); ++i) {
        if (rotateSide(spec.pinSides[static_cast<size_t>(i)], quarterTurns) == side) {
            if (i < index) ++ordinal;
            ++countOnSide;
        }
    }
    const float t = (countOnSide <= 1)
                        ? 0.0f
                        : (static_cast<float>(ordinal) / static_cast<float>(countOnSide - 1)) * 2.0f - 1.0f;

    switch (side) {
    case 0: return {-hx, t * hy * 0.8f};   // left
    case 1: return { hx, t * hy * 0.8f};   // right
    case 2: return { t * hx * 0.8f, -hy};  // up
    default: return { t * hx * 0.8f, hy};  // down
    }
}

// ============================================================================
// Symbols
// ============================================================================

namespace {

/// The sheet's line weight, in pixels: Céline's own rule, `gridSize * 0.12` with
/// a one-pixel floor, so a symbol stays legible when the view is zoomed out.
float sheetStroke(const SheetView& view) {
    return std::max(1.0f, view.gridSize * 0.12f);
}

/// Rotates a point about the origin by a part's orientation, in grid units, so
/// symbol geometry can be written once facing one way.
///
/// Ported from Céline's `rotate`. The same turn as `Schematic::pinPosition`
/// uses, so a symbol-local point and the pin it belongs to stay together.
juce::Point<float> rotateGrid(float x, float y, int orientation) {
    for (int turn = 0; turn < (orientation & 3); ++turn) {
        const float rotatedX = -y;
        y = x;
        x = rotatedX;
    }
    return {x, y};
}

/// A little drawing context: knows where the part is and which way round, and
/// turns symbol-local grid coordinates into pixels.
///
/// Ported from Céline's `LocalSpace`. It exists for the things a symbol draws
/// in *code* rather than loads -- the potentiometer's wiper, the rheostat's
/// diagonal arrow, the terminals' IN/OUT lettering -- because those are a
/// *reading of the part's state* and no file could hold them.
struct LocalSpace {
    const SheetView& view;
    juce::Point<int> centre;
    int orientation;
    bool mirrored;

    juce::Point<float> operator()(float x, float y) const {
        // Mirror then rotate, matching `Schematic::pinPosition` -- if the two
        // disagreed, the pins would stop landing on the symbol.
        const auto rotated = rotateGrid(mirrored ? -x : x, y, orientation);
        return view.toPixel(static_cast<float>(centre.x) + rotated.x,
                            static_cast<float>(centre.y) + rotated.y);
    }
};

/// A filled arrowhead at `tip`, pointing back along the line from `from`.
///
/// Ported from Céline's `drawArrowHead`.
void drawArrowHead(juce::Graphics& g, juce::Point<float> tip,
                   juce::Point<float> from, float size) {
    const auto direction = tip - from;
    const auto length = direction.getDistanceFromOrigin();

    if (length < 0.0001f)
        return;

    const auto unit = direction / length;
    const juce::Point<float> normal(-unit.y, unit.x);

    juce::Path head;
    head.startNewSubPath(tip);
    head.lineTo(tip - unit * size + normal * size * 0.45f);
    head.lineTo(tip - unit * size - normal * size * 0.45f);
    head.closeSubPath();

    g.fillPath(head);
}

/// 0..1 control position from the part's `pos` value, 0.5 when it has none.
float controlPosition(const wdf::SchematicComponent& part) {
    const auto it = part.values.find("pos");
    return it != part.values.end()
               ? juce::jlimit(0.0f, 1.0f, static_cast<float>(it->second))
               : 0.5f;
}

/// The part's own frame, for the marks that are drawn in code.
LocalSpace localSpaceFor(const wdf::SchematicComponent& part, const SheetView& view) {
    return {view,
            {static_cast<int>(std::lround(part.visual.x)),
             static_cast<int>(std::lround(part.visual.y))},
            (part.visual.rotation / 90) & 3,
            part.visual.flipped};
}

/// The potentiometer's wiper: the arrow that says where the knob is.
///
/// Ported from Céline's `drawWiper`. It is drawn in code rather than loaded,
/// because it is a reading of the part's *state*: the wiper slides up and down
/// the body as the value changes, and a fixed file could not follow it. Without
/// it a potentiometer is just a rectangle with three pins -- a shape, not a part.
///
/// The position is `values["pos"]`, 0 at the low end and 1 at the high end. In
/// the part's own frame the low end is the bottom, so the tap runs from `+1`
/// (low) up to `-1` (high); the placement transform turns that into the
/// direction the pot is actually drawn in.
void drawPotWiper(juce::Graphics& g, const wdf::SchematicComponent& part,
                  const SheetView& view) {
    const LocalSpace s = localSpaceFor(part, view);
    const float tap = 1.0f - 2.0f * controlPosition(part);
    const auto tip = s(0.85f, tap);
    const auto tail = s(2.6f, 0.0f);

    g.setColour(EditorLookAndFeel::partColour());
    g.drawLine(juce::Line<float>(tail, tip), sheetStroke(view));
    drawArrowHead(g, tip, tail, view.gridSize * 0.55f);
}

/// The rheostat's diagonal arrow: a resistor body with an arrow striking
/// through it, the IEC mark for a two-terminal variable resistor.
///
/// **Not a wiper.** A rheostat has two terminals, so it must not borrow the
/// potentiometer's third-terminal arrow. The diagonal line is the whole mark;
/// its height follows the live position only so the plugin's schematic shows
/// the knob moving, exactly as the pot's wiper does. The line still crosses the
/// body at every position.
void drawRheostatArrow(juce::Graphics& g, const LocalSpace& s, float strokePixels,
                       float arrowSize, float position) {
    const float tap = 1.0f - 2.0f * position;
    const float shift = tap * 0.45f;

    const auto tail = s(-1.35f, 0.95f + shift);
    const auto tip = s(1.35f, -0.95f + shift);

    g.drawLine(juce::Line<float>(tail, tip), strokePixels);
    drawArrowHead(g, tip, tail, arrowSize);
}

/// The "IN" and "OUT" lettering beside the terminal flags.
///
/// Ported from Céline's `drawTerminalLettering`. It is drawn in *code* and not
/// loaded, for the reason every code-drawn thing is: it is text, and text does
/// not belong in a picture of a shape. It also has to stay upright when the
/// part is turned, which an outline baked into the artwork could not do.
///
/// The lettering sits on the far side of the flag from the wire -- the input's
/// on the left, the output's on the right -- so it does not land on the flag it
/// labels.
void drawTerminalLettering(juce::Graphics& g, const wdf::SchematicComponent& part,
                           const SheetView& view, bool isInput) {
    const LocalSpace s = localSpaceFor(part, view);

    const float centreX = isInput ? -1.2f : 1.2f;

    g.setColour(EditorLookAndFeel::partColour());
    g.setFont(Fonts::semiBold(juce::jmax(9.0f, view.gridSize * 0.6f)));
    g.drawText(isInput ? juce::String("IN") : juce::String("OUT"),
               juce::Rectangle<float>(view.gridSize * 1.4f, view.gridSize * 1.4f)
                   .withCentre(s(centreX, 0.0f)),
               juce::Justification::centred, false);
}

/// The transistor terminal letters (B/C/E, G/D/S).
///
/// The SVG files carry these as `<text>`, but the artwork loader deliberately
/// extracts paths only -- text is not a shape, and a letter baked into a path
/// could not stay upright when the part is rotated or mirrored. So the letters
/// are drawn in code, from the same pin frame the artwork is authored against,
/// and they follow the part without ever being upside down.
void drawTransistorTerminalLetters(juce::Graphics& g,
                                   const wdf::SchematicComponent& part,
                                   const SheetView& view) {
    const auto offsets = Schematic::pinOffsets(part);
    if (offsets.empty())
        return;

    const char* letters = nullptr;
    if (part.type == "bjt")
        letters = "CBE";   // collector, base, emitter -- the palette's pin order
    else if (part.type == "jfet" || part.type == "mosfet")
        letters = "DGS";   // drain, gate, source
    else
        return;

    const LocalSpace s = localSpaceFor(part, view);
    const float box = view.gridSize * 1.0f;

    g.setColour(EditorLookAndFeel::partColour().withAlpha(0.8f));
    g.setFont(Fonts::semiBold(juce::jmax(9.0f, view.gridSize * 0.6f)));

    for (size_t i = 0; i < std::min<size_t>(offsets.size(), 3); ++i) {
        const float pinX = static_cast<float>(offsets[i].x);
        const float pinY = static_cast<float>(offsets[i].y);

        // Put the letter just outside the lead end, on the side the pin itself
        // points. A pin on the vertical axis has no horizontal direction of its
        // own, so its label goes to the right -- the same side the files use.
        const float dx = pinX < 0.0f ? -0.60f : 0.60f;
        const float dy = pinY < 0.0f ? -0.40f : (pinY > 0.0f ? 0.40f : 0.0f);

        const auto centre = s(pinX + dx, pinY + dy);
        g.drawText(juce::String::charToString(letters[i]),
                   juce::Rectangle<float>(box, box).withCentre(centre),
                   juce::Justification::centred, false);
    }
}

/// The op-amp's polarity marks: `-` and `+` at the two inputs, `V+`/`V-` on the
/// supply leads.
///
/// The five-pin symbol is symmetric about its centre line, so without these
/// marks nothing on the sheet says which supply pin is the positive rail or
/// which input is inverting. They are drawn in code for the same reason as the
/// transistor letters: text has to stay upright when the part is turned or
/// mirrored.
void drawOpAmpPolarityLetters(juce::Graphics& g,
                              const wdf::SchematicComponent& part,
                              const SheetView& view) {
    const auto offsets = Schematic::pinOffsets(part);
    if (offsets.size() < 3)
        return;

    const LocalSpace s = localSpaceFor(part, view);
    const float box = view.gridSize;

    g.setColour(EditorLookAndFeel::partColour().withAlpha(0.85f));
    g.setFont(Fonts::semiBold(juce::jmax(9.0f, view.gridSize * 0.56f)));

    // Pin order is the document contract (`out`, `neg`, `pos`, `vcc`, `vee`).
    // The marks sit just inside the triangle's input edge.
    const auto mark = [&](float x, float y, const char* text, float width) {
        g.drawText(text, juce::Rectangle<float>(width, box)
                             .withCentre(s(x, y)),
                   juce::Justification::centred, false);
    };
    mark(static_cast<float>(offsets[1].x) + 0.9f,
         static_cast<float>(offsets[1].y), "-", box);
    mark(static_cast<float>(offsets[2].x) + 0.9f,
         static_cast<float>(offsets[2].y), "+", box);

    if (offsets.size() >= 5) {
        // `vcc` is the top lead and `vee` the bottom one; label just past each
        // lead end, clear of the pin dot.
        const auto rail = [&](const auto& pin, const char* text) {
            const float dy = pin.y < 0.0 ? -0.62f : 0.62f;
            g.drawText(text, juce::Rectangle<float>(box * 1.7f, box)
                                 .withCentre(s(static_cast<float>(pin.x),
                                               static_cast<float>(pin.y) + dy)),
                       juce::Justification::centred, false);
        };
        rail(offsets[3], "V+");
        rail(offsets[4], "V-");
    }
}

/// The potentiometer's hidden terminal names, drawn as `H` and `L`.
///
/// **Why these are on the sheet.** A pot's `high`/`low` ends are not told apart
/// by the artwork: its body is a symmetric rectangle, so a rotated or mirrored
/// pot -- even though the default now places `high` on top -- can hold either
/// end at the visual top while still looking like an ordinary pot. Wiring the
/// visible top to the signal then silently wires it to whichever end is there,
/// and the engine reads the knob backwards: position 0 (the low end) full
/// instead of off. The letters make the pin frame a reading of the drawing
/// rather than a secret in the file. They stay upright when the part turns,
/// like the transistor terminal letters.
void drawPotTerminalLetters(juce::Graphics& g, const wdf::SchematicComponent& part,
                            const SheetView& view) {
    const auto offsets = Schematic::pinOffsets(part);
    if (offsets.size() < 3)
        return;

    const LocalSpace s = localSpaceFor(part, view);
    const float box = view.gridSize * 0.9f;
    const float dx = -0.85f;

    g.setColour(EditorLookAndFeel::partColour().withAlpha(0.8f));
    g.setFont(Fonts::light(juce::jmax(9.0f, view.gridSize * 0.58f)));

    // Palette order is high, wiper, low. The wiper already has its arrow, so
    // only the two ends need names. Each letter sits beside its lead and just
    // outside the body, toward the centre -- low enough not to collide with the
    // caption, high enough to stay clear of the body at every rotation.
    const auto drawPin = [&](const PinOffset& pin, const char* letter) {
        const float dy = pin.y < 0 ? 0.35f : -0.35f;
        const auto centre = s(static_cast<float>(pin.x) + dx,
                              static_cast<float>(pin.y) + dy);
        g.drawText(juce::String(letter),
                   juce::Rectangle<float>(box, box).withCentre(centre),
                   juce::Justification::centred, false);
    };

    drawPin(offsets[0], "H");
    drawPin(offsets[2], "L");
}

/// The shared body of the two `drawArtwork` overloads below -- declared first so
/// each can call it. See the first overload for what `strokePixels` means.
void drawArtworkAt(juce::Graphics& g, const SymbolArtwork& artwork,
                   const juce::AffineTransform& placement, float strokePixels);

/// Strokes (or fills) one artwork, centred on `centre` (the part's own pixel
/// position) at `gridSize` pixels per grid square.
///
/// **The artwork is the whole symbol, leads included**, which is why this takes a
/// centre and a grid size rather than a body rectangle. The file's own convention
/// -- (0,0) is the part's centre, 10 SVG units = 1 grid square -- means a pin drawn
/// in the file at grid (0,-3.5) lands on the pin the model computes, one to one,
/// with no scale step. (Celine works exactly this way: artwork carries the leads,
/// and the canvas paints pins over the top.)
///
/// `strokePixels` is the line weight **in pixels**, not in artwork units: the
/// caller measures it (the sheet uses `gridSize * 0.12` with a floor, the palette
/// swatch a heavier floor of its own) and this only multiplies it by each shape's
/// authored `strokeScale`. Stating it in pixels is Céline's arrangement and the
/// right one here -- the file's 1.2 is a *ratio* baseline, so a symbol zoomed out
/// to a thumbnail keeps its line weight instead of fading to nothing.
void drawArtwork(juce::Graphics& g, const SymbolArtwork& artwork,
                 juce::Point<float> centre, float gridSize, float strokePixels) {
    if (artwork.isEmpty())
        return;

    drawArtworkAt(g, artwork,
                  juce::AffineTransform::scale(gridSize, gridSize)
                      .translated(centre.x, centre.y),
                  strokePixels);
}

/// The same, with the part's own mirror/rotation applied first. Kept separate so
/// the palette preview -- which has no placed part to read a transform from --
/// can call the plain form.
void drawArtwork(juce::Graphics& g, const SymbolArtwork& artwork,
                 const wdf::SchematicComponent& part,
                 juce::Point<float> centre, float gridSize, float strokePixels) {
    if (artwork.isEmpty())
        return;

    // The one placement function, shared with the hit test. Mirror, then rotate,
    // then move is the order `pinPosition` uses, so the drawn symbol, the pins
    // the model computes and the area the pointer can hit all turn together.
    drawArtworkAt(g, artwork, artworkTransformFor(part, centre, gridSize), strokePixels);
}

/// The placement and the stroke, in Céline's arrangement, verbatim.
///
/// **Two things here are easy to get wrong and both of them are invisible in the
/// measured geometry** -- the bounds, the shape count and the parity test all
/// stay correct while the symbol draws as a speck or as a solid block:
///
///  * **The scale is `gridSize`, not `gridSize / 10`.** `SymbolArtwork`'s loader
///    already divides the file's SVG units by ten (10 SVG units = 1 grid
///    square), so a path coordinate of 0.7 is *seven tenths of a grid square*.
///    Dividing again draws every symbol ten times too small: a resistor becomes
///    a speck, a triode a dot. The artwork is in grid units; the sheet's scale
///    is pixels per grid unit.
///  * **The transform is passed to `strokePath` as an argument, not installed
///    with `addTransform`.** JUCE 9 scales the stroke width by the graphics
///    transform, so a 3px line through a 26x transform is stroked 78px wide and
///    the symbol fills in as a solid block. The three-argument `strokePath` takes
///    the transform as a placement and leaves the width in device pixels, which
///    is what "a line weight in pixels" means.
///
/// The placement comes from `artworkTransformFor` for a placed part, so the
/// painter and the hit test use one transform rather than two expressions that
/// have to be kept in step by hand.
void drawArtworkAt(juce::Graphics& g, const SymbolArtwork& artwork,
                   const juce::AffineTransform& placement, float strokePixels) {
    for (const auto& shape : artwork.shapes) {
        if (shape.filled)
            g.fillPath(shape.path, placement);
        else
            g.strokePath(shape.path,
                         juce::PathStrokeType(strokePixels * shape.strokeScale),
                         placement);
    }
}

}  // namespace

void EditorLookAndFeel::drawSymbol(juce::Graphics& g, const PartSpec& spec,
                                   const wdf::SchematicComponent& comp,
                                   juce::Rectangle<float> b,
                                   bool hovered, bool selected, float sw) {
    const bool hl = hovered || selected;

    // Rotation is applied about the body centre, so a rotated part keeps its
    // pins where `pinOffsetWorld` says they are.
    juce::Graphics::ScopedSaveState save(g);
    g.addTransform(juce::AffineTransform::rotation(
                       juce::MathConstants<float>::halfPi * (comp.visual.rotation / 90.0f),
                       b.getCentreX(), b.getCentreY()));

    g.setColour(hl ? hover() : wireColour());

    const std::string& t = spec.type;
    if      (t == "res")         drawResistor(g, b, hl, sw);
    else if (t == "cap")         drawCapacitor(g, b, hl, sw);
    else if (t == "ind")         drawInductor(g, b, hl, sw);
    else if (t == "diode")       drawDiode(g, b, hl, sw);
    else if (t == "triode")      drawTriode(g, b, hl, sw);
    else if (t == "pentode")     drawPentode(g, b, hl, sw);
    else if (t == "jfet")        drawJfet(g, b, hl, sw);
    else if (t == "pot")         drawPot(g, b, hl, true, sw);
    else if (t == "varres")      drawPot(g, b, hl, false, sw);
    else if (t == "src")         drawSource(g, b, hl, false, sw);
    else if (t == "isrc")        drawSource(g, b, hl, true, sw);
    else if (t == "transformer") drawTransformer(g, b, hl, sw);
    else if (t == "vcvs")        drawGainBlock(g, b, hl, sw);
    else if (t == "ctrlres")     drawResistor(g, b, hl, sw);
    else                         g.drawRect(b, sw);
}

void EditorLookAndFeel::drawResistor(juce::Graphics& g, juce::Rectangle<float> b, bool, float sw) {
    const float cy = b.getCentreY();
    const float x0 = b.getX(), x1 = b.getRight();
    const float bodyW = b.getWidth() * 0.55f;
    const float bx = b.getCentreX() - bodyW * 0.5f;

    g.drawLine(x0, cy, bx, cy, sw);
    g.drawLine(bx + bodyW, cy, x1, cy, sw);

    // Zig-zag, the US convention, because that is what the reference uses.
    juce::Path z;
    z.startNewSubPath(bx, cy);
    const int segs = 6;
    const float step = bodyW / segs;
    for (int i = 0; i < segs; ++i) {
        const float px = bx + step * (i + 1);
        const float py = cy + ((i % 2 == 0) ? -7.0f : 7.0f);
        z.lineTo(px - step * 0.5f, py);
        z.lineTo(px, cy);
    }
    g.strokePath(z, juce::PathStrokeType(sw));
}

void EditorLookAndFeel::drawCapacitor(juce::Graphics& g, juce::Rectangle<float> b, bool, float sw) {
    const float cy = b.getCentreY();
    const float cx = b.getCentreX();
    const float gap = 5.0f;
    const float plate = b.getHeight() * 0.32f;
    g.drawLine(b.getX(), cy, cx - gap, cy, sw);
    g.drawLine(cx + gap, cy, b.getRight(), cy, sw);
    g.drawLine(cx - gap, cy - plate, cx - gap, cy + plate, sw * 1.25f);
    g.drawLine(cx + gap, cy - plate, cx + gap, cy + plate, sw * 1.25f);
}

void EditorLookAndFeel::drawInductor(juce::Graphics& g, juce::Rectangle<float> b, bool, float sw) {
    const float cy = b.getCentreY();
    const float x0 = b.getX(), x1 = b.getRight();
    const float bodyW = b.getWidth() * 0.6f;
    const float bx = b.getCentreX() - bodyW * 0.5f;
    g.drawLine(x0, cy, bx, cy, sw);
    g.drawLine(bx + bodyW, cy, x1, cy, sw);

    juce::Path coil;
    const int arcs = 4;
    const float r = bodyW / (2.0f * arcs);
    coil.startNewSubPath(bx, cy);
    for (int i = 0; i < arcs; ++i)
        coil.addArc(bx + r * (2 * i + 1), cy - r, 2 * r, 2 * r,
                    juce::MathConstants<float>::pi, juce::MathConstants<float>::twoPi, true);
    g.strokePath(coil, juce::PathStrokeType(sw));
}

void EditorLookAndFeel::drawDiode(juce::Graphics& g, juce::Rectangle<float> b, bool, float sw) {
    const float cy = b.getCentreY();
    const float cx = b.getCentreX();
    const float s = b.getHeight() * 0.3f;
    g.drawLine(b.getX(), cy, cx - s, cy, sw);
    g.drawLine(cx + s * 0.4f, cy, b.getRight(), cy, sw);

    juce::Path tri;
    tri.addTriangle(cx - s, cy - s, cx - s, cy + s, cx + s * 0.4f, cy);
    g.fillPath(tri);
    g.drawLine(cx + s * 0.4f, cy - s, cx + s * 0.4f, cy + s, sw * 1.25f);
}

void EditorLookAndFeel::drawTriode(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw) {
    // Glass envelope
    g.setColour(hl ? hover().withAlpha(0.10f) : glow::getColourNormal().withAlpha(0.03f));
    g.fillEllipse(b.reduced(b.getWidth() * 0.12f, b.getWidth() * 0.06f));
    g.setColour(hl ? hover() : wireColour());
    g.drawEllipse(b.reduced(b.getWidth() * 0.12f, b.getWidth() * 0.06f), sw);

    const float cx = b.getCentreX();
    const float top = b.getY() + b.getHeight() * 0.22f;
    const float bot = b.getBottom() - b.getHeight() * 0.22f;
    // Anode plate
    g.drawLine(cx - 7.0f, top, cx - 7.0f, bot, sw * 1.4f);
    // Grid (dashed)
    juce::Path grid;
    grid.startNewSubPath(cx, top);
    grid.lineTo(cx, bot);
    const float dashes[] = {4.0f, 4.0f};
    juce::PathStrokeType(sw).createDashedStroke(grid, grid, dashes, 2);
    g.strokePath(grid, juce::PathStrokeType(sw));
    // Cathode bracket
    g.drawLine(cx + 7.0f, top, cx + 7.0f, bot, sw * 1.4f);
    juce::Path k;
    k.startNewSubPath(cx + 2.0f, bot);
    k.lineTo(cx + 12.0f, bot);
    g.strokePath(k, juce::PathStrokeType(sw * 1.2f));
}

void EditorLookAndFeel::drawPentode(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw) {
    drawTriode(g, b, hl, sw);
    // Screen grid: a second dashed line, which is what makes it read as a
    // pentode rather than a triode.
    const float cx = b.getCentreX() + 4.0f;
    const float top = b.getY() + b.getHeight() * 0.22f;
    const float bot = b.getBottom() - b.getHeight() * 0.22f;
    juce::Path sg;
    sg.startNewSubPath(cx, top);
    sg.lineTo(cx, bot);
    const float dashes[] = {3.0f, 5.0f};
    juce::PathStrokeType(sw * 0.85f).createDashedStroke(sg, sg, dashes, 2);
    g.strokePath(sg, juce::PathStrokeType(sw * 0.85f));
}

void EditorLookAndFeel::drawJfet(juce::Graphics& g, juce::Rectangle<float> b, bool, float sw) {
    const float cx = b.getCentreX();
    const float cy = b.getCentreY();
    const float h = b.getHeight() * 0.30f;
    const float gateX = b.getX() + b.getWidth() * 0.28f;

    g.drawLine(b.getX(), cy, gateX, cy, sw);
    g.drawLine(gateX, cy - h, gateX, cy + h, sw * 1.3f);
    g.drawLine(cx, b.getY(), cx, b.getBottom(), sw * 1.5f);
    g.drawLine(cx, cy - h, b.getRight(), cy - h, sw);
    g.drawLine(cx, cy + h, b.getRight(), cy + h, sw);

    // Arrow on the source end
    juce::Path arrow;
    arrow.startNewSubPath(cx + 2.0f, cy + h);
    arrow.lineTo(cx + 8.0f, cy + h - 4.0f);
    arrow.startNewSubPath(cx + 2.0f, cy + h);
    arrow.lineTo(cx + 8.0f, cy + h + 4.0f);
    g.strokePath(arrow, juce::PathStrokeType(sw));
}

void EditorLookAndFeel::drawPot(juce::Graphics& g, juce::Rectangle<float> b, bool hl, bool wiper, float sw) {
    drawResistor(g, b, hl, sw);
    if (!wiper) return;
    // Wiper arrow, pointing at the body from above.
    const float cx = b.getCentreX();
    const float top = b.getY() + 2.0f;
    juce::Path arrow;
    arrow.startNewSubPath(cx, b.getCentreY() - 6.0f);
    arrow.lineTo(cx, top);
    arrow.lineTo(cx - 4.0f, top + 6.0f);
    arrow.startNewSubPath(cx, top);
    arrow.lineTo(cx + 4.0f, top + 6.0f);
    g.strokePath(arrow, juce::PathStrokeType(sw));
}

void EditorLookAndFeel::drawSource(juce::Graphics& g, juce::Rectangle<float> b, bool, bool current, float sw) {
    const float cy = b.getCentreY();
    const float cx = b.getCentreX();
    const float r = b.getHeight() * 0.3f;
    g.drawEllipse(cx - r, cy - r, 2 * r, 2 * r, sw);
    if (current) {
        // A current source is drawn with a bar through it.
        g.drawLine(cx - r * 0.7f, cy, cx + r * 0.7f, cy, sw * 1.2f);
    } else {
        juce::Path s;
        const float a = r * 0.55f;
        s.startNewSubPath(cx - a, cy);
        s.lineTo(cx - a, cy - a);
        s.lineTo(cx + a, cy + a);
        s.lineTo(cx + a, cy);
        g.strokePath(s, juce::PathStrokeType(sw));
    }
}

void EditorLookAndFeel::drawTransformer(juce::Graphics& g, juce::Rectangle<float> b, bool, float sw) {
    const float cx = b.getCentreX();
    const float cy = b.getCentreY();
    const int arcs = 3;
    const float r = b.getHeight() * 0.09f;
    for (int side = 0; side < 2; ++side) {
        const float x = cx + (side == 0 ? -r * 2.0f : r * 2.0f);
        for (int i = 0; i < arcs; ++i) {
            const float y = cy - r * arcs + i * 2 * r;
            g.drawEllipse(x - r, y, 2 * r, 2 * r, sw);
        }
    }
    // Core lines
    g.drawLine(cx - r * 0.4f, cy - r * arcs, cx - r * 0.4f, cy + r * arcs, sw);
    g.drawLine(cx + r * 0.4f, cy - r * arcs, cx + r * 0.4f, cy + r * arcs, sw);
}

void EditorLookAndFeel::drawGainBlock(juce::Graphics& g, juce::Rectangle<float> b, bool, float sw) {
    juce::Path tri;
    tri.addTriangle(b.getX() + 6.0f, b.getY() + 6.0f,
                    b.getX() + 6.0f, b.getBottom() - 6.0f,
                    b.getRight() - 6.0f, b.getCentreY());
    g.strokePath(tri, juce::PathStrokeType(sw));
}

// ============================================================================
// Probe badge
// ============================================================================

void EditorLookAndFeel::drawProbeBadge(juce::Graphics& g, juce::Point<float> centre,
                                       const std::string& node, double volts) {
    // Colour by magnitude, so a reader can see roughly what is hot without
    // reading the number.
    const float mag = static_cast<float>(std::min(1.0, std::abs(volts) / 350.0));
    const juce::Colour c = accent().interpolatedWith(amber(), mag);

    juce::String text = juce::String(node) + "  " +
                        juce::String(volts, std::abs(volts) < 10.0 ? 3 : 1) + " V";

    const juce::Font font = Fonts::mono(11.5f);
    g.setFont(font);
    const float textW = juce::GlyphArrangement::getStringWidth(font, text);
    const float w = textW + 18.0f;
    const float h = 20.0f;
    juce::Rectangle<float> box(centre.x - w * 0.5f, centre.y - 26.0f - 10.0f, w, h);

    // Glow halo behind the pill.
    juce::Path frame;
    frame.addRoundedRectangle(box, 5.0f);
    glow::drawGlowPath(g, frame, c, 0.45f);

    // Translucent dark fill with accent border — engineering readout.
    g.setColour(glow::getColourCanvasBackground().withAlpha(0.82f));
    g.fillRoundedRectangle(box, 5.0f);
    g.setColour(c.withAlpha(0.7f));
    g.drawRoundedRectangle(box, 5.0f, 1.0f);
    g.setColour(c.brighter(0.3f));
    g.drawFittedText(text, box.toNearestInt(), juce::Justification::centred, 1);

    // Dashed leader line from the badge down to the wire it measures.
    juce::Path leader;
    leader.startNewSubPath(box.getCentreX(), box.getBottom());
    leader.lineTo(centre.x, centre.y);
    const float dashes[] = {3.0f, 2.5f};
    juce::Path dashedLeader;
    juce::PathStrokeType(1.0f).createDashedStroke(dashedLeader, leader, dashes, 2);
    g.setColour(c.withAlpha(0.45f));
    g.strokePath(dashedLeader, juce::PathStrokeType(1.0f));
}

// ============================================================================
// Placed parts
// ============================================================================
//
// The three functions below are what the canvas actually calls. They are here,
// not in the canvas, for the same reason `pinOffsetWorld` is: they are the
// *look* of a part, and the canvas is about what a click does. Everything they
// place -- body, pins, label -- comes through the same `SheetView` transform the
// hit test uses, so what is drawn and what is clickable cannot drift apart.

namespace {

/// The symbol body: the grid rectangle from which each pin leads out. Built from
/// the part's own pins rather than a fixed size, so a rotated or mirrored part
/// gets a body that still sits between its leads.
juce::Rectangle<float> bodyRect(const wdf::SchematicComponent& part, const SheetView& view) {
    const auto offsets = Schematic::pinOffsets(part);

    if (offsets.empty())
        return {};

    // Half-extents in grid units, matching `Schematic::pinOffsets`' own body and
    // the artwork's reach so the label baseline sits below the drawn symbol.
    constexpr float hx = 3.0f, hy = 3.5f;

    const int quarterTurns = (part.visual.rotation / 90) & 3;
    const bool mirrored = part.visual.flipped;

    // The body is a rectangle in the part's own frame; rotating it can swap its
    // aspect, so compute the transformed corner extents directly.
    int ex = static_cast<int>(std::lround(hx));
    int ey = static_cast<int>(std::lround(hy));

    for (int turn = 0; turn < quarterTurns; ++turn)
        std::swap(ex, ey);

    const auto centre = view.toPixel(static_cast<int>(std::lround(part.visual.x)),
                                     static_cast<int>(std::lround(part.visual.y)));
    juce::ignoreUnused(mirrored);

    const float w = static_cast<float>(ex) * 2.0f * view.gridSize;
    const float h = static_cast<float>(ey) * 2.0f * view.gridSize;

    return {centre.x - w * 0.5f, centre.y - h * 0.5f, w, h};
}

/// Where each pin sits in pixels, in the part's pin order.
std::vector<juce::Point<float>> pinPixels(const wdf::SchematicComponent& part,
                                          const SheetView& view) {
    std::vector<juce::Point<float>> points;
    const auto offsets = Schematic::pinOffsets(part);

    for (int i = 0; i < static_cast<int>(offsets.size()); ++i) {
        const auto p = Schematic::pinPosition(part, i);
        points.push_back(view.toPixel(p.x, p.y));
    }
    return points;
}

/// How far, in grid units, the part's drawing reaches *below* its centre.
///
/// Ported from Céline's `bodyDepthBelowCentre`: measured from the artwork's own
/// bounds, so a valve's bottle and a resistor's rectangle each get the clearance
/// they need without a per-type table. Falls back to the pin frame for a type
/// whose artwork is not loaded, which is the same fallback the drawing uses.
float lowestReachBelowCentre(const wdf::SchematicComponent& part) {
    float reach = 0.0f;

    for (const auto& p : Schematic::pinOffsets(part))
        reach = std::max(reach, static_cast<float>(p.y));

    if (const auto* art = artworkForPart(part)) {
        // `bounds` is in grid units with (0,0) at the part centre, so its bottom
        // edge is how far the body reaches down.
        reach = std::max(reach, art->bounds.getBottom());
    }

    return reach;
}

/// The part's caption: name and value, in Céline's three colours, spaced clear of
/// whatever the symbol actually reaches.
///
/// Why this is its own function rather than inline in `drawComponent`: the ghost
/// and the palette swatch want the same placement rule, and Céline keeps the
/// caption out of `drawSymbolOnly` for exactly that reason -- a preview that
/// calls the symbol-only path gets the symbol and no caption. Stating it once
/// means the two can never drift about where a name belongs.
void drawPartCaption(juce::Graphics& g, const wdf::SchematicComponent& part,
                     const SheetView& view) {
    const auto centre = view.toPixel(static_cast<int>(std::lround(part.visual.x)),
                                     static_cast<int>(std::lround(part.visual.y)));

    // Up to two runs, in the order a caption reads: what it is called, then what
    // it is set to. A name is a name; a value that is still zero reads red,
    // because that is a part nobody has filled in, not a part set to nothing.
    struct Run {
        std::string text;
        juce::Colour colour;
    };
    std::vector<Run> runs;

    runs.push_back({partLabelText(part), EditorLookAndFeel::captionName()});

    const double value = valueOf(part);
    const std::string valueStr = valueText(part);
    const bool unset = (value == 0.0) && !hasModelChoice(part);
    if (!valueStr.empty())
        runs.push_back({valueStr, unset ? EditorLookAndFeel::captionUnset()
                                        : EditorLookAndFeel::captionValue()});

    const float captionHeight = view.gridSize * 1.2f;
    const float space = view.gridSize * 0.55f;

    // The name runs in the UI face, the value in mono — the distinction a
    // data-sheet makes between what a part is called and what it is set to.
    const juce::Font nameFont = Fonts::semiBold(view.gridSize * 0.85f);
    const juce::Font valueFont = Fonts::mono(view.gridSize * 0.78f);

    float total = 0.0f;
    std::vector<float> widths;
    widths.reserve(runs.size());
    for (size_t ri = 0; ri < runs.size(); ++ri) {
        const auto& f = (ri == 0) ? nameFont : valueFont;
        const float w = juce::GlyphArrangement::getStringWidth(f, juce::String(runs[ri].text));
        widths.push_back(w);
        total += w + space;
    }
    total -= space;  // no trailing gap

    // One square of air below whatever the symbol reaches -- measured, not a
    // constant, which is the whole point of `lowestReachBelowCentre`.
    const float top = centre.y + view.gridSize * (lowestReachBelowCentre(part) + 1.0f)
                      - captionHeight * 0.5f;
    float x = centre.x - total * 0.5f;

    for (size_t i = 0; i < runs.size(); ++i) {
        g.setFont((i == 0) ? nameFont : valueFont);
        g.setColour(runs[i].colour);
        g.drawText(juce::String(runs[i].text),
                   juce::Rectangle<float>(x, top, widths[i], captionHeight),
                   juce::Justification::centredLeft, false);
        x += widths[i] + space;
    }
}

}  // namespace

void drawComponentSymbolOnly(juce::Graphics& g, const wdf::SchematicComponent& part,
                              const SheetView& view) {
    const PartSpec* spec = findPart(part.type);
    if (spec == nullptr)
        return;

    const auto body = bodyRect(part, view);
    const auto pins = pinPixels(part, view);
    const auto centre = view.toPixel(static_cast<int>(std::lround(part.visual.x)),
                                     static_cast<int>(std::lround(part.visual.y)));

    // The order is Céline's: the artwork first, then the things drawn in *code*
    // (a reading of the part's state, which no file could hold), and the pin
    // dots **last**, on top of everything.
    //
    // Where there is an artwork file, **it is the whole symbol** -- leads and body
    // together -- so nothing else is drawn for the body. This is Celine's own
    // division and the reason the two never fight: artwork carries the leads, the
    // canvas paints the pins over the top.
    if (part.type == "switch_res" && wdf::switchResThrowCount(part) > 2) {
        g.setColour(EditorLookAndFeel::partColour());
        const auto selected = static_cast<size_t>(wdf::switchResPosition(part) + 1);
        if (selected < pins.size())
            g.drawLine(pins.front().x, pins.front().y, pins[selected].x, pins[selected].y,
                       sheetStroke(view));
    } else if (const auto* art = artworkForPart(part)) {
        g.setColour(EditorLookAndFeel::partColour());
        drawArtwork(g, *art, part, centre, view.gridSize, sheetStroke(view));
    } else {
        // The fallback for types with no file yet: code-drawn leads from each pin
        // to the body edge, then the body itself from the symbol table.
        g.setColour(EditorLookAndFeel::wireColour());
        const float stroke = std::max(1.0f, view.gridSize * 0.10f);
        for (const auto& p : pins) {
            const auto bodyCentre = juce::Point<float>(body.getCentreX(), body.getCentreY());
            const auto dir = bodyCentre - p;
            const float len = dir.getDistanceFromOrigin();
            if (len < 1.0f)
                continue;

            const auto unit = dir / len;
            float t = len;
            const float hw = body.getWidth() * 0.5f;
            const float hh = body.getHeight() * 0.5f;
            if (std::abs(unit.x) > 1e-6f) t = std::min(t, std::abs(hw / unit.x));
            if (std::abs(unit.y) > 1e-6f) t = std::min(t, std::abs(hh / unit.y));

            const auto edge = p + unit * t;
            g.drawLine(p.x, p.y, edge.x, edge.y, stroke);
        }

        EditorLookAndFeel::drawSymbol(g, *spec, part, body, false, false, sheetStroke(view));
    }

    // The live marks, drawn in code because they are readings of the part's
    // state rather than marks a file could hold. A pot has a wiper; a rheostat
    // has two terminals and must wear the diagonal arrow instead.
    if (spec->type == "pot") {
        drawPotWiper(g, part, view);
        drawPotTerminalLetters(g, part, view);
    } else if (spec->type == "varres") {
        g.setColour(EditorLookAndFeel::partColour());
        drawRheostatArrow(g, localSpaceFor(part, view), sheetStroke(view),
                          view.gridSize * 0.55f, controlPosition(part));
    }

    // The terminals' lettering, also drawn in code, for the same reason: it is
    // text, and it has to stay upright when the part is turned.
    if (spec->type == "in")
        drawTerminalLettering(g, part, view, true);
    else if (spec->type == "out")
        drawTerminalLettering(g, part, view, false);
    else if (spec->type == "bjt" || spec->type == "jfet" || spec->type == "mosfet")
        drawTransistorTerminalLetters(g, part, view);
    else if (spec->type == "opamp" || spec->type == "idealopamp")
        drawOpAmpPolarityLetters(g, part, view);

    // The pin dots, on top of everything, so a wire has somewhere obvious to
    // land.
    //
    // Céline's numbers: the dot is `max(1.5, gridSize * 0.18)` in radius, and it
    // wears the *part's* colour at 90% -- not the wire colour. A pin dot is a
    // mark on the part, so it reads as part of the part; making it the wire's
    // violet made every part look as though it were made of wire.
    g.setColour(EditorLookAndFeel::partColour().withAlpha(0.8f));
    const float pinR = std::max(1.2f, view.gridSize * 0.14f);
    for (const auto& p : pins)
        g.fillEllipse(p.x - pinR, p.y - pinR, pinR * 2.0f, pinR * 2.0f);
}

void drawSymbolArtwork(juce::Graphics& g, const std::string& type,
                       juce::Point<float> centre, float gridSize, float strokePixels) {
    const auto* art = artworkForType(type);
    if (art == nullptr)
        return;

    g.setColour(EditorLookAndFeel::partColour());
    drawArtwork(g, *art, centre, gridSize, strokePixels);
}

void drawComponent(juce::Graphics& g, const wdf::SchematicComponent& part,
                   const SheetView& view) {
    // The symbol and its pins first, then the caption under them. Split so the
    // symbol can be drawn on its own -- Céline's `drawSymbolOnly`, which the
    // palette swatch wants and which the comparison harness uses to look at the
    // drawing without the caption in the way.
    drawComponentSymbolOnly(g, part, view);
    // The caption: name and value under the symbol, in Céline's three colours.
    //
    // Two things here are ported from Céline's `SymbolPainter::draw` and were
    // missing before: the run colours (a *name* is yellow, a *filled-in* value is
    // green, a value still at zero is red), and the clearance, which is measured
    // from what the symbol actually reaches rather than from a fixed offset. A
    // valve reaches three grid squares below its centre where a resistor reaches
    // two, so one fixed offset either buries the caption inside the bottle or
    // floats it away from the small parts.
    //
    // Skipped at a coarse zoom, where a caption is bigger than the part it names
    // (Céline's own threshold, `gridSize <= 6`). Ours used to be 10, which hid
    // every caption on a zoomed-out sheet.
    if (view.gridSize > 6.0f) {
        drawPartCaption(g, part, view);
    }
}

void drawComponentGhost(juce::Graphics& g, const wdf::SchematicComponent& part,
                        const SheetView& view) {
    const PartSpec* spec = findPart(part.type);
    if (spec == nullptr)
        return;

    const auto body = bodyRect(part, view);
    const auto centre = view.toPixel(static_cast<int>(std::lround(part.visual.x)),
                                     static_cast<int>(std::lround(part.visual.y)));

    juce::Graphics::ScopedSaveState save(g);
    g.setOpacity(0.45f);

    // A fill behind the faint outline, so the ghost reads as "this will go here"
    // rather than as a second, paler copy of a real part.
    g.setColour(EditorLookAndFeel::selection().withAlpha(0.10f));
    g.fillRoundedRectangle(body, EditorLookAndFeel::smallRadius);

    g.setColour(EditorLookAndFeel::selection());

    if (const auto* art = artworkForPart(part)) {
        drawArtwork(g, *art, part, centre, view.gridSize, sheetStroke(view) * 1.6f);

        if (part.type == "varres")
            drawRheostatArrow(g, localSpaceFor(part, view), sheetStroke(view) * 1.6f,
                              view.gridSize * 0.55f, controlPosition(part));
    } else {
        EditorLookAndFeel::drawSymbol(g, *spec, part, body, true, true, sheetStroke(view) * 1.6f);
    }

    // Pins on the ghost too: the whole point is to see where the connections
    // will land before committing.
    const float pinR = std::max(1.5f, view.gridSize * 0.12f);
    for (int i = 0; i < static_cast<int>(Schematic::pinOffsets(part).size()); ++i) {
        const auto p = Schematic::pinPosition(part, i);
        const auto px = view.toPixel(p.x, p.y);
        g.setColour(EditorLookAndFeel::selection());
        g.drawEllipse(px.x - pinR, px.y - pinR, pinR * 2.0f, pinR * 2.0f, 1.5f);
    }
}

void drawSelectionHalo(juce::Graphics& g, const wdf::SchematicComponent& part,
                       const SheetView& view) {
    // **The symbol's own measured bounds**, not the generic six-by-seven pin
    // frame. The halo is feedback for what is selected, so it should sit round
    // the thing the user can see and hit -- a resistor's narrow body, not a large
    // empty rectangle shared by every part.
    const auto bounds = Schematic::partBounds(part);
    const auto pixels = juce::Rectangle<float>(
        view.toPixel(bounds.getX(), bounds.getY()),
        view.toPixel(bounds.getRight(), bounds.getBottom()));
    const auto body = pixels.expanded(view.gridSize * 0.4f);
    const float cr = std::max(3.0f, view.gridSize * 0.25f);

    g.setColour(EditorLookAndFeel::selection().withAlpha(0.14f));
    g.fillRoundedRectangle(body, cr);

    g.setColour(EditorLookAndFeel::selection());
    g.drawRoundedRectangle(body, cr, 1.6f);
}

void drawProblemHalo(juce::Graphics& g, const wdf::SchematicComponent& part,
                     const SheetView& view) {
    // **The part the build failed on, marked on the drawing itself.**
    //
    // The message in the status line says what is wrong; this says *where*.
    // A red ring round the part is a thing the eye finds without being told
    // to look for it, and it costs the user nothing: the selection is not
    // touched, so whatever they were working on is still selected.
    const auto body = bodyRect(part, view).expanded(view.gridSize * 0.55f);

    g.setColour(glow::getColourHotRed().withAlpha(0.16f));
    g.fillRoundedRectangle(body, 4.0f);

    g.setColour(glow::getColourHotRed());
    g.drawRoundedRectangle(body, 4.0f, 1.8f);
}

void drawPreviewSymbol(juce::Graphics& g, const std::string& type,
                       juce::Rectangle<float> area, juce::Colour colour) {
    const PartSpec* spec = findPart(type);

    // What the symbol covers, in grid units and relative to the part's centre:
    // the artwork's own bounds, unioned with the pins so a terminal stays inside
    // the swatch. This is Céline's `symbolExtent`, and it is measured rather than
    // a constant because a transformer and a resistor differ by something like
    // three to one -- a single scale either clips the big ones or shrinks the
    // small ones to motes.
    juce::Rectangle<float> extent;

    if (const auto* art = artworkForType(type))
        extent = art->bounds;
    for (const auto& p : Schematic::pinOffsets(type)) {
        const juce::Rectangle<float> pinRect(static_cast<float>(p.x), static_cast<float>(p.y), 0.0f, 0.0f);
        extent = extent.isEmpty() ? pinRect : extent.getUnion(pinRect);
    }
    if (extent.isEmpty())
        return;

    // Fit the symbol's own reach into the swatch, less a little for the line
    // weight, and centre on what is actually *drawn* -- a valve's envelope sits
    // above its origin and a gain block's triangle to the right of it, so
    // centring the origin instead would push the symbol off to one side.
    constexpr float previewSquares = 6.0f;   // the room a swatch offers, in squares
    const float swatch = std::min(area.getWidth(), area.getHeight()) - 5.0f;
    const float reach = std::max(previewSquares, std::max(extent.getWidth(), extent.getHeight()));
    const float gridSize = std::max(0.5f, swatch / reach);

    SheetView view;
    view.gridSize = gridSize;
    view.origin = area.getCentre()
                  - juce::Point<float>(extent.getCentreX(), extent.getCentreY()) * gridSize;

    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(area.toNearestInt());

    juce::Graphics::ScopedSaveState ink(g);
    g.setColour(colour);

    if (const auto* art = artworkForType(type)) {
        // Draw the artwork directly rather than through `drawComponent`, because a
        // preview wants the symbol *only*: no pin dots, no caption, and a stroke
        // floor of its own. A half-pixel stroke at swatch scale renders as grey
        // rather than ink, which is what made an earlier palette read as pale.
        const auto centre = view.toPixel(0.0f, 0.0f);
        drawArtwork(g, *art, centre, gridSize, 1.5f);

        // The rheostat's arrow is a live mark on the sheet, so it is not in the
        // artwork file. The palette still has to show it, or the row offers a
        // resistor under the name "Rheostat".
        if (type == "varres") {
            const LocalSpace s{view, {0, 0}, 0, false};
            drawRheostatArrow(g, s, 1.5f, gridSize * 0.55f, 0.5f);
        }
    } else if (spec != nullptr) {
        // A type with no file falls back to the code-drawn symbol, fitted to the
        // extent's box -- so a palette row never shows a blank where a part is.
        const auto box = juce::Rectangle<float>(view.toPixel(extent.getX(), extent.getY()),
                                                view.toPixel(extent.getRight(), extent.getBottom()));
        EditorLookAndFeel::drawSymbol(g, *spec, wdf::SchematicComponent{}, box, false, false);
    }
}

}  // namespace livewdf::editor
