// Holds our symbol geometry to Céline's, numerically (U3.2).
//
// This is the test the user asked for: a way to know whether a symbol looks like
// Céline's without a person looking at it. It does not compare pixels -- it
// compares the data the pixels are drawn from, against Céline's own measured
// values (`celine_reference_geometry.h`), which were produced by rendering Céline
// through `tools/render`.
//
// **What it can and cannot hold.** It can hold: pin offsets (where the terminals
// are), the number of pins, the symbol's overall size in grid units, its aspect,
// and the shape count. It cannot hold: colour, line weight in pixels, caption
// placement or font -- those are the painter's and the theme's, and are checked by
// looking at the rendered PNG, not here. Saying that plainly is the point: a green
// run of this test is not "it looks right", it is "it is the right shape".
//
// **The orientation rule.** Céline draws a two-pin part vertically by default
// (pins `(0,-2)`/`(0,2)`); we draw it horizontally (`(-3,0)`/`(3,0)`). That is a
// genuine difference in how the two editors default a placed part, not a drawing
// bug, and forcing our default to match Céline's is a separate decision. So the
// comparisons here are written to be **orientation-agnostic**:
//
//   * pin offsets are compared as the *sorted multiset of distances from the
//     centre*, so a vertical Céline pin set and a horizontal ours compare equal
//     when they are the same shape turned;
//   * bounds are compared as {long side, short side}, for the same reason.
//
// Everything else that could be wrong -- a pin at the wrong radius, a body twice
// the size, a missing pin -- still fails, because those change the numbers even
// after the normalisation.

#include "celine_reference_geometry.h"

#include "format/PartLibrary.h"
#include "SchematicModel.h"
#include "SymbolArtwork.h"

#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace livewdf::editor;

namespace {

/// The sorted radii of a pin set (distance from the centre, in grid units,
/// rounded to whole units), so two sets that are the same shape turned compare
/// equal.
template <typename Pin>
std::vector<int> sortedRadii(const std::vector<Pin>& pins) {
    std::vector<int> out;
    out.reserve(pins.size());
    for (const auto& p : pins)
        out.push_back(static_cast<int>(
            std::lround(std::hypot(static_cast<double>(p.x), static_cast<double>(p.y)))));
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("Every part we place that Céline draws is held to Céline's geometry") {
    // The failure this exists for: adding a part to the palette and never giving
    // it reference geometry, so nothing holds it to Céline's shape. A type Céline
    // genuinely does not draw is allowed through, but must be *named* here -- the
    // list is the record of what we knowingly do not compare.
    const std::vector<std::string> celineDoesNotDraw = {
        "vcvs",    // Céline's "Gain Block" is not in its element table
        "isrc",    // Céline has no independent current-source symbol
        "varres",  // our rheostat shares Céline's potentiometer artwork
        "ctrlres", // our own addition
        "jfet",    // generic three-terminal symbol; Celine has no JFET artwork
        "mosfet",  // generic three-terminal symbol; Celine has no MOSFET artwork
        "bjt",     // generic three-terminal symbol; Celine has no BJT artwork
        "opamp",   // our addition: Celine has no op-amp symbol
        "idealopamp", // its own three-pin frame, for the same reason
        "scope",   // our own addition
        "net_label", // our own one-pin terminal
        "rail",    // our own addition
        "in",      // circular port, not Céline's flag
        "out",     // circular port, not Céline's flag
        // LiveWDF's own dynamics, optoelectronic and delay device symbols.
        "photores", "dbx209", "rmsdet", "bbd", "vca", "dbx200", "that2180", "ca3080", "ne570",
    };

    std::set<std::string> covered;
    for (const auto& ref : refgeom::table())
        if (!ref.ourType.empty())
            covered.insert(ref.ourType);

    for (const auto& spec : partPalette()) {
        INFO("type: " << spec.type);

        const bool known = covered.count(spec.type) > 0;
        const bool excused =
            std::find(celineDoesNotDraw.begin(), celineDoesNotDraw.end(), spec.type) !=
            celineDoesNotDraw.end();

        CHECK((known || excused));
    }
}

TEST_CASE("Our pin geometry matches Céline's, orientation aside") {
    // The contract wires and the net extractor both lean on: a part's terminals
    // are where Céline puts them. Compared as distances from the centre, so the
    // vertical/horizontal default does not matter; a pin count or a radius that
    // differs does.
    for (const auto& ref : refgeom::table()) {
        if (ref.ourType.empty())
            continue;  // a symbol we draw but do not offer as a placeable part

        INFO("Céline " << ref.celineName << " / ours " << ref.ourType);

        REQUIRE(findPart(ref.ourType) != nullptr);

        const auto ours = Schematic::pinOffsets(ref.ourType);

        CHECK(static_cast<int>(ours.size()) == static_cast<int>(ref.pins.size()));
        CHECK(sortedRadii(ours) == sortedRadii(ref.pins));
    }
}

TEST_CASE("Our symbol size and aspect match Céline's") {
    // The proportions are what the eye reads first. Bounds are compared as
    // {long side, short side} so a symbol that is Céline's shape turned still
    // matches, while one that is twice the size -- or wider than tall where
    // Céline is taller than wide -- fails.
    // Types we deliberately redesigned (in, out) are excused from this comparison.
    // The pentode joins them for a different reason: Céline feeds both of its
    // filament leads from the left, so its box is 5.22 squares across against
    // 6.12 tall. Ours puts the screen terminal opposite the control grid -- one
    // lead each side, so a wire bound for the screen never crosses the strap
    // inside the envelope -- which makes the box a square. Its pins (compared as
    // distances from the centre, below) and its shape count are still compared.
    const std::vector<std::string> excusedFromBounds = {"in", "out", "pentode"};

    for (const auto& ref : refgeom::table()) {
        INFO("Céline " << ref.celineName << " / artwork " << ref.artworkKey);

        if (std::find(excusedFromBounds.begin(), excusedFromBounds.end(), ref.artworkKey) != excusedFromBounds.end())
            continue;

        const auto* art = artworkForType(ref.artworkKey);
        REQUIRE(art != nullptr);
        INFO("our artwork bounds: " << art->bounds.toString());

        const auto& b = art->bounds;
        const float ourLong  = (b.getWidth() > b.getHeight()) ? b.getWidth() : b.getHeight();
        const float ourShort = (b.getWidth() < b.getHeight()) ? b.getWidth() : b.getHeight();

        const float refLong  = (ref.boundsW > ref.boundsH) ? ref.boundsW : ref.boundsH;
        const float refShort = (ref.boundsW < ref.boundsH) ? ref.boundsW : ref.boundsH;

        // Half a grid square of tolerance. The artwork's bounds include the
        // stroke, and how much a round cap adds at a corner is a sub-square
        // difference that has no meaning on the sheet -- but twice the size, or a
        // swapped aspect, is far outside it.
        CHECK(std::abs(ourLong - refLong) < 0.5f);
        CHECK(std::abs(ourShort - refShort) < 0.5f);
    }
}

TEST_CASE("Our artwork is drawn in Céline's frame -- one grid square, no scale") {
    // Céline's convention, which the whole comparison assumes: 10 SVG units per
    // grid square, (0,0) at the centre, and the file's frame *is* the sheet's, so
    // no scale step is applied. If this is 1 for every type, a pin drawn in a file
    // lands on the pin the model computes; anything else means the artwork is
    // being stretched and the "matches Céline" claim above is comparison of the
    // wrong thing.
    for (const auto& ref : refgeom::table()) {
        INFO("artwork: " << ref.artworkKey);

        const auto* art = artworkForType(ref.artworkKey);
        REQUIRE(art != nullptr);

        CHECK(artworkScaleFor(*art) == 1.0f);
    }
}

TEST_CASE("A symbol's shape count matches Céline's") {
    // The cheapest signal that a file was swapped, truncated, or drawn in a
    // different style: the number of paths it parses into. Not exact -- a
    // re-drawing may split a path in two -- so it is a band, wide enough for a
    // legitimate redraw and narrow enough to catch "this is a different symbol".
    for (const auto& ref : refgeom::table()) {
        INFO("Céline " << ref.celineName << " / artwork " << ref.artworkKey
                       << " -- Céline has " << ref.shapeCount << " shapes");

        const auto* art = artworkForType(ref.artworkKey);
        REQUIRE(art != nullptr);

        const int ours = static_cast<int>(art->shapes.size());
        CHECK(ours >= ref.shapeCount - 1);
        CHECK(ours <= ref.shapeCount + 2);
    }
}
