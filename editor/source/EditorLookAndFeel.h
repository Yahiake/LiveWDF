#pragma once

// The editor's visual language: colours, schematic symbols, probe badges.
// Modern dark theme — charcoal surfaces, gold accent, teal signal flow.
//
// Why this is separate from the canvas: the *canvas* decides where things are
// and what a click does; this file decides what they look like. Keeping the two
// apart means the look can be tuned -- or ported to the plugin -- without
// touching hit-testing, and it is the one place a palette change has to be
// made.
//
// It also holds `pinOffsetWorld`, which the canvas uses for both drawing and
// picking a pin. Those two must agree exactly, or a wire would start from a dot
// the user did not click on, so they share one function rather than each
// computing the geometry themselves.

#include "format/PartLibrary.h"
#include "format/LiveWDFFile.h"
#include "gui/glow/Glow.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

namespace livewdf::editor {

/// Maps grid units to pixels. `gridSize` is pixels per grid square, so it *is*
/// the zoom; `origin` is where grid (0,0) lands on the component.
///
/// Ported from Celine's `SymbolPainter`, kept as a plain struct because every
/// coordinate question in the canvas goes through it and a class with state
/// would invite a second copy of the same fields.
///
/// It lives here, next to the drawing code, rather than in the canvas: the
/// symbol painter needs it to place a symbol and the canvas needs it to place a
/// hit test, and those two must be the same transform or a click misses the part
/// it visibly landed on.
struct SheetView {
    float gridSize = 16.0f;
    juce::Point<float> origin{24.0f, 24.0f};

    juce::Point<float> toPixel(float gx, float gy) const noexcept {
        return origin + juce::Point<float>(gx * gridSize, gy * gridSize);
    }

    juce::Point<float> toPixel(int gx, int gy) const noexcept {
        return toPixel(static_cast<float>(gx), static_cast<float>(gy));
    }

    juce::Point<float> toPixel(juce::Point<int> g) const noexcept { return toPixel(g.x, g.y); }

    /// Unrounded, for hit testing and zoom anchoring. `toGrid` snaps to the
    /// nearest intersection, which is right for placing a part and wrong for
    /// asking what is under the cursor.
    juce::Point<float> toGridExact(juce::Point<float> pixel) const noexcept {
        return {(pixel.x - origin.x) / gridSize, (pixel.y - origin.y) / gridSize};
    }

    juce::Point<int> toGrid(juce::Point<float> pixel) const noexcept {
        const auto exact = toGridExact(pixel);
        return {juce::roundToInt(exact.x), juce::roundToInt(exact.y)};
    }
};

/// World-space offset of pin `index` from a component's centre, for `spec`'s
/// body size, after `quarterTurns` of rotation.
juce::Point<float> pinOffsetWorld(const PartSpec& spec, int index, int quarterTurns);

/// Draw a placed part -- body, pins, id label and value -- through `view`.
/// The one entry point the canvas uses, so the symbol and its pins always come
/// from the same description.
void drawComponent(juce::Graphics& g, const wdf::SchematicComponent& part,
                   const SheetView& view);

/// The symbol and its pins, without the caption -- what a palette entry wants,
/// since the list already gives the name.
///
/// Ported from Céline's `SymbolPainter::drawSymbolOnly`. Kept separate because
/// it is genuinely a different thing: the sheet's part carries a caption, the
/// palette's swatch does not, and the comparison harness needs the drawing
/// without the text in the way.
void drawComponentSymbolOnly(juce::Graphics& g, const wdf::SchematicComponent& part,
                             const SheetView& view);

/// The artwork for `type`, drawn on its own: no pins, no caption, no part.
///
/// Public because it is the only way to measure what a symbol *looks* like
/// without anything else in the picture: the render test draws this into an
/// image and measures the ink, and the comparison harness does the same on
/// Céline's side. Both are after the same thing -- whether the drawing lands
/// where the model says it should -- and neither can see it any other way.
///
/// The artwork's own geometry is in `SymbolArtwork::bounds`, in grid units, so
/// the ink this leaves should be that box times `gridSize` pixels per square.
void drawSymbolArtwork(juce::Graphics& g, const std::string& type,
                       juce::Point<float> centre, float gridSize, float strokePixels);

/// The same, faint, for the part under the pointer before it is placed. The
/// geometry is identical, so the ghost is a truthful preview.
void drawComponentGhost(juce::Graphics& g, const wdf::SchematicComponent& part,
                        const SheetView& view);

/// A symbol centred in `area`, for the palette's preview swatches -- no pins, no
/// caption, scaled to fit.
///
/// Ported from Céline's `SymbolPainter::drawPreview`. Why it is distinct from
/// `drawComponent`: a palette row is offering a *type*, not a placed part, so the
/// things that matter on the sheet (where the pins are, what the value is) are
/// noise here, and the parts that are large on the sheet (a transformer, a valve)
/// have to be shrunk to fit a row. Céline's rule is the one kept: fit the
/// symbol's *own reach*, centre on what is actually drawn, and put a floor under
/// the line weight so a small swatch still reads as ink rather than as grey.
void drawPreviewSymbol(juce::Graphics& g, const std::string& type,
                       juce::Rectangle<float> area, juce::Colour colour);

/// The halo a selected part wears.
void drawSelectionHalo(juce::Graphics& g, const wdf::SchematicComponent& part,
                       const SheetView& view);

/// The red ring round the part a build failed on.
///
/// **Where the message points.** The status line says what is wrong; this says
/// where, on the drawing, without touching the selection -- so the part the
/// user was working on is still selected and still theirs.
void drawProblemHalo(juce::Graphics& g, const wdf::SchematicComponent& part,
                     const SheetView& view);

struct EditorLookAndFeel {

    // --- Canvas ------------------------------------------------------------
    static juce::Colour canvasBackground() { return glow::getColourCanvasBackground(); }
    static juce::Colour gridDotMinor()      { return glow::getColourGridMinor(); }
    static juce::Colour gridDotMajor()      { return glow::getColourGridMajor(); }
    static juce::Colour wireColour()        { return glow::getColourWire(); }
    static juce::Colour wireHotColour()     { return glow::getColourWireHot(); }
    static juce::Colour wireSignalColour()  { return glow::getColourSignal(); }
    static juce::Colour partBodyColour()    { return glow::getColourElement(); }
    static juce::Colour partStrokeColour()  { return glow::getColourElementStroke(); }
    static juce::Colour partColour()        { return glow::getColourElement(); }
    static juce::Colour pinColour()         { return glow::getColourPin(); }
    static juce::Colour groundColour()      { return glow::getColourPin(); }
    static juce::Colour selectionColour()   { return glow::getColourSelected(); }
    static juce::Colour selection()         { return glow::getColourSelected(); }
    static juce::Colour hoverColour()       { return glow::getColourAccent().withAlpha(0.15f); }
    static juce::Colour hover()             { return glow::getColourAccent().withAlpha(0.15f); }
    static juce::Colour errorColour()       { return glow::getColourError(); }

    // --- Panel / Palette / Inspector --------------------------------------
    static juce::Colour panel()             { return glow::getColourPanel(); }
    static juce::Colour panelElevated()     { return glow::getColourElevated(); }
    static juce::Colour panelBorder()       { return glow::getColourBorder(); }
    static juce::Colour textPrimary()       { return glow::getColourTextPrimary(); }
    static juce::Colour textSecondary()     { return glow::getColourTextSecondary(); }
    static juce::Colour textOnPanel()       { return glow::getColourTextOnPanel(); }
    static juce::Colour panelText()         { return glow::getColourTextOnPanel(); }
    static juce::Colour textOnAccent()      { return glow::getColourTextOnAccent(); }
    static juce::Colour accent()            { return glow::getColourAccent(); }
    static juce::Colour accentDim()         { return glow::getColourAccentDim(); }
    static juce::Colour amber()             { return glow::getColourAmber(); }
    static juce::Colour pill()              { return glow::getColourPill(); }
    static juce::Colour pillHover()         { return glow::getColourElevated(); }
    static juce::Colour pillArmed()         { return glow::getColourAccent(); }

    // --- Captions ---------------------------------------------------------
    // Name = gold, Value = teal, Unset = red
    static juce::Colour captionName()       { return glow::getColourAccent(); }
    static juce::Colour captionValue()      { return glow::getColourSignal(); }
    static juce::Colour captionUnset()      { return glow::getColourError(); }

    // --- Selection boxes ---------------------------------------------------
    static juce::Colour boxEnclose()        { return glow::getColourBoxEnclose(); }
    static juce::Colour boxCrossing()       { return glow::getColourBoxCrossing(); }

    // --- Geometry ----------------------------------------------------------
    static constexpr float cornerRadius = 8.0f;
    static constexpr float pillRadius = 6.0f;
    static constexpr float smallRadius = 4.0f;

    // --- Symbols ----------------------------------------------------------
    /// Draw the body of `comp` (type `spec.type`) fitted to `b`.
    /// `sw` is the line weight in pixels — zoom-aware, from `sheetStroke(view)`.
    static void drawSymbol(juce::Graphics& g, const PartSpec& spec,
                           const wdf::SchematicComponent& comp,
                           juce::Rectangle<float> b,
                           bool hovered, bool selected, float sw = 1.8f);

    /// A rounded voltage badge at `centre`, in the same style as the scope and
    /// the signal-flow dots.
    static void drawProbeBadge(juce::Graphics& g, juce::Point<float> centre,
                               const std::string& node, double volts);

private:
    static void drawResistor(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw);
    static void drawCapacitor(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw);
    static void drawInductor(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw);
    static void drawDiode(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw);
    static void drawTriode(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw);
    static void drawPentode(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw);
    static void drawJfet(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw);
    static void drawPot(juce::Graphics& g, juce::Rectangle<float> b, bool hl, bool wiper, float sw);
    static void drawSource(juce::Graphics& g, juce::Rectangle<float> b, bool hl, bool current, float sw);
    static void drawTransformer(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw);
    static void drawGainBlock(juce::Graphics& g, juce::Rectangle<float> b, bool hl, float sw);
};

}  // namespace livewdf::editor