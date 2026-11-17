#pragma once

// The drawn shape of every part, loaded from SVG rather than written in code.
//
// **Ported from Celine** (`Celine/source/ui/SymbolArtwork.{h,cpp}`), because the
// two things it gets right are exactly the things an artwork layer is for:
//
//   * geometry lives in **grid units**, extracted once when the file is parsed,
//     so painting allocates nothing and the canvas strokes at a floor that
//     follows the zoom instead of letting a transform scale strokes to nothing;
//   * the box a part occupies is **measured from the artwork**, not written out
//     by hand in a table -- redraw a file and the bounds follow, which a
//     hand-kept box would not.
//
// **Where this copy differs from Celine's, and why:**
//
//   1. **Our parts are bigger than Celine's in grid terms.** Our pins sit at
//      ±22/±26 grid squares (see `PartLibrary`'s body note); Celine's sit at ±2.
//      So artwork is authored in Celine's convention (10 SVG units = 1 grid
//      square, `(0,0)` at the part's centre) and **scaled to our body** at draw
//      time (`scaleFor`), rather than making every file carry our pin frame.
//   2. **The type lookup returns a default; `artworkForPart` returns the part's
//      actual variant.** The SPDT's blade is on throw A or throw B, and the file
//      changes with the document's `select` property. Model variants for diodes
//      (zener, schottky, LED) are still deferred because our part's model is a
//      runtime property rather than an artwork table.
//   3. `hasArtwork` says which types have a file; the canvas falls back to its
//      old code-drawn symbol for the rest, so nothing disappears.

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <vector>

namespace wdf {
struct SchematicComponent;
}

namespace livewdf::editor {

/// One part's shape, extracted from its SVG file.
struct SymbolArtwork {
    /// One path from the file, and how it is meant to be painted.
    struct Shape {
        juce::Path path;

        /// Filled shapes are the solid ones -- a diode's triangle, an arrowhead,
        /// a wiper's head. Everything else is stroked.
        bool filled = false;

        /// Authored stroke width over the 1.2 baseline, so a 1.56 line comes back
        /// out as 1.3x. Multiplied into the canvas's own line weight rather than
        /// scaled by the view transform, so lines stay visible when zoomed out.
        float strokeScale = 1.0f;

        /// True when every sub-path closes, so the path encloses an area. An
        /// outline such as a resistor's body or a valve's envelope is still a
        /// region for picking: the user sees a body, not four line segments.
        /// Measured once when the file is parsed, not on every mouse move.
        bool closed = false;
    };

    std::vector<Shape> shapes;

    /// What the shapes actually cover, in grid units, including the width of the
    /// strokes. Measured when the file is parsed -- the one (and only) moment it
    /// can go stale.
    juce::Rectangle<float> bounds;

    /// True when the file was missing or empty. This must not be silent: a part
    /// with no artwork draws as nothing at all, which reads as a bug in the
    /// drawing rather than as a missing asset.
    bool isEmpty() const noexcept { return shapes.empty(); }
};

/// The artwork for a part type, or `nullptr` if that type has no file. This is
/// the type's default variant -- the switch's throw A -- for callers that have
/// no part state (the palette preview).
const SymbolArtwork* artworkForType(const std::string& type);

/// The artwork for a placed part in its current state. For an SPDT this is the
/// file matching the selected throw; every other type uses `artworkForType`.
const SymbolArtwork* artworkForPart(const wdf::SchematicComponent& part);

/// Where the artwork's own paths sit in the sheet's grid coordinates.
///
/// Mirror, then rotate, then move to the part's rounded centre -- the same
/// transform the painter uses. `scale` is pixels per grid unit when drawing and
/// 1 when examining the symbol in grid units, so a hit test and the picture
/// cannot disagree about where a shape is.
juce::AffineTransform artworkTransformFor(const wdf::SchematicComponent& part,
                                          juce::Point<float> centre, float scale);

/// True when `(px, py)` is within `reach` grid units of the part's drawn symbol.
///
/// **The symbol's shape, not its bounding box.** A path that encloses an area
/// (a resistor's body, a valve's envelope, a filled diode triangle) is picked
/// throughout that area; a path that is only a line (a capacitor plate, a
/// transformer winding) is picked along its ink. That is what stops a click
/// inside the empty corner of a bounding box from grabbing a part the pointer
/// is visibly not on.
bool artworkHitTest(const SymbolArtwork& artwork, const wdf::SchematicComponent& part,
                    float px, float py, float reach);

/// True when the part's drawn symbol meets `box` in any way.
///
/// Used by a *crossing* marquee: one edge through a line, a box inside a filled
/// body, or a box wholly containing the symbol all count. The empty space inside
/// a bounding box does not.
bool artworkIntersectsBox(const SymbolArtwork& artwork, const wdf::SchematicComponent& part,
                          juce::Rectangle<float> box);

/// True when `type` has an artwork file, so the canvas can fall back cleanly.
bool hasArtwork(const std::string& type);

/// Every type that has a file. The test that walks these is what stops a
/// renamed or missing file from silently blanking a part.
std::vector<std::string> artworkTypes();

/// How much to scale the artwork to cover one part's body.
///
/// Our pins sit at ±22/±26 grid squares where Celine's artwork is authored for
/// ±2, so the file has to grow to meet our pins. Taken from the artwork's
/// measured `bounds` rather than a constant, so redrawing a file changes its
/// scale on its own -- the same "measure, do not write it down" rule the bounds
/// themselves follow.
float artworkScaleFor(const SymbolArtwork& artwork);

}  // namespace livewdf::editor
