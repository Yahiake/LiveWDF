// Checks on the symbol artwork layer (U3.1).
//
// The layer's whole job is to turn an SVG file into shapes the canvas can stroke,
// and the failure it has to make impossible is **silent**: a part whose file is
// missing or misnamed draws as nothing but its pins, which reads as a bug in the
// drawing rather than as a missing asset. A component joins the checker before the
// product uses it, so this walks every type the canvas can ask for.

#include "format/PartLibrary.h"
#include "SchematicModel.h"
#include "SymbolArtwork.h"

#include <catch2.hpp>

#include <algorithm>

using namespace livewdf::editor;

TEST_CASE("Every palette type that has artwork resolves to a non-empty file") {
    // The property that matters: for each type we claim to have artwork for, the
    // file is actually there and actually parsed into shapes. A missing or
    // renamed file fails this rather than quietly blanking the part on the sheet.
    const auto types = artworkTypes();

    // A floor, so deleting every asset cannot make this pass vacuously.
    REQUIRE(types.size() >= 8);

    for (const auto& type : types) {
        INFO("type: " << type);

        REQUIRE(hasArtwork(type));

        const auto* art = artworkForType(type);
        REQUIRE(art != nullptr);
        CHECK_FALSE(art->isEmpty());

        // At minimum a lead and a body -- except the parts whose Céline artwork is
        // a single shape and whose other marks are drawn by the painter rather
        // than baked into the file: the potentiometer (body only; the wiper is a
        // live reading of the knob) and the two terminals (the pennant only; the
        // "IN" and "OUT" lettering is drawn in code). One shape is correct there,
        // so the floor is "not empty" and these exceptions are named.
        const bool singleShape = (type == "pot" || type == "in" || type == "out");

        if (singleShape)
            CHECK(art->shapes.size() >= 1);
        else
            CHECK(art->shapes.size() >= 2);
    }
}

TEST_CASE("Artwork bounds are measured, finite and centred near the origin") {
    // The bounds are what the canvas uses to place and scale a symbol, so a
    // nonsense box is a misplaced part. Measured, not hand-written: this asserts
    // the measurement produced something a caller can act on.
    for (const auto& type : artworkTypes()) {
        INFO("type: " << type);

        const auto* art = artworkForType(type);
        REQUIRE(art != nullptr);

        const auto& b = art->bounds;

        CHECK(std::isfinite(b.getX()));
        CHECK(std::isfinite(b.getY()));
        CHECK(std::isfinite(b.getWidth()));
        CHECK(std::isfinite(b.getHeight()));
        CHECK(b.getWidth() > 0.0f);
        CHECK(b.getHeight() > 0.0f);

        // The convention is that (0,0) is the part's centre, so the measured box
        // straddles the origin -- a file authored in the wrong frame would sit
        // wholly in one quadrant and fail this.
        CHECK(b.getX() < 0.0f);
        CHECK(b.getY() < 0.0f);
        CHECK(b.getRight() > 0.0f);
        CHECK(b.getBottom() > 0.0f);
    }
}

TEST_CASE("A type with no artwork says so rather than returning an empty one") {
    // The contract the canvas relies on: `artworkForType` returns nullptr for a
    // type with no file so it falls back to the code-drawn symbol, rather than
    // handing back an empty artwork that would draw nothing. The valves used to
    // be the example here; now that their pin frame is Céline's and their
    // artwork has been ported, the example is our own additions.

    // And a type that does not exist at all is the same case, not a crash.
    CHECK_FALSE(hasArtwork("not-a-real-part"));
    CHECK(artworkForType("not-a-real-part") == nullptr);
}

TEST_CASE("Every drawing in the palette has a file") {
    // These are the types the artwork layer was authored for. Listing them here
    // rather than trusting `artworkTypes` means a type dropped from the map is
    // caught as a *missing* one, not merely as a shorter list.
    for (const auto* type : {"res", "cap", "ind", "diode", "ground", "gnd",
                             "pot", "varres", "src", "isrc", "vcvs",
                             "transformer", "transformer_ct", "triode", "pentode", "ctrlres",
                             "switch_res", "bjt", "jfet", "mosfet", "in", "out",
                             "scope", "opamp", "idealopamp"}) {
        INFO("expected artwork for: " << type);
        CHECK(hasArtwork(type));
    }
}

TEST_CASE("A part's artwork reaches its pins", "[artwork]") {
    // A pin that sits off the end of its own symbol leaves a gap the eye reads
    // as a mistake -- and it is invisible in the geometry, which is how it
    // happens: the artwork and the pin table are two separate pieces of data,
    // and nothing checks that they agree.
    //
    // The current source was exactly this: its symbol was drawn for pins at
    // grid (0,±2), its pin table said (0,±3.5), and the wire stopped in
    // mid-air a grid and a half short of the symbol's end.
    //
    // The exception is named: the potentiometer's leads and its wiper are
    // drawn *in code* -- the wiper is a live reading of the knob, not a mark
    // baked into the file -- so its artwork is the body alone.
    constexpr float kTolerance = 0.25f;   // grid squares; the stroke adds a little

    for (const auto& spec : partPalette()) {
        const auto* art = artworkForType(spec.type);
        if (art == nullptr)
            continue;   // no file: the part is drawn in code, pins and all

        INFO("part: " << spec.type);

        if (spec.type == "pot")
            continue;   // the body only; the leads and wiper are drawn in code

        const auto pins = Schematic::pinOffsets(spec.type);
        REQUIRE(pins.size() == spec.pins.size());

        for (size_t i = 0; i < pins.size(); ++i) {
            INFO("pin: " << i);

            CHECK(art->bounds.expanded(kTolerance)
                      .contains(static_cast<float>(pins[i].x),
                                static_cast<float>(pins[i].y)));
        }
    }
}

TEST_CASE("Artwork draws one-to-one in grid units") {
    // The pins are ±3/±3.5 grid squares and the artwork is authored at 10 SVG
    // units = 1 grid square, so a file's own frame *is* the sheet's frame: the
    // scale is exactly 1 and a pin drawn in the file lands on the pin the model
    // computes. This pins that -- a future change that reintroduces a scale step
    // (or changes the pin frame without changing this) fails here rather than
    // making every symbol silently the wrong size.
    for (const auto& type : artworkTypes()) {
        INFO("type: " << type);

        const auto* art = artworkForType(type);
        REQUIRE(art != nullptr);

        CHECK(artworkScaleFor(*art) == 1.0f);
    }
}

TEST_CASE("Artwork is small enough to sit between our pins") {
    // The failure this guards against is the one that made the sheet unreadable
    // before: artwork (or pins) sized so that a part's body dwarfs the spacing
    // parts are placed at, so everything overlaps. Our body is ±3/±3.5 grid
    // squares, so nothing should measure much past that.
    for (const auto& type : artworkTypes()) {
        INFO("type: " << type);

        const auto* art = artworkForType(type);
        REQUIRE(art != nullptr);

        // Half-extents, in grid squares. The leads reach the pins at ±3.x, so a
        // file is allowed a little over; a file that measured 20+ squares across
        // would mean it was authored in the old oversized frame.
        CHECK(art->bounds.getWidth() * 0.5f < 8.0f);
        CHECK(art->bounds.getHeight() * 0.5f < 8.0f);
    }
}

TEST_CASE("Transistor artwork follows the effective polarity", "[artwork][transistor]") {
    // Six symbols exist: NPN/PNP, N/P JFET, N/P enhancement MOSFET. The palette
    // preview shows the N variant, but a placed part must switch to the P file
    // when its card (or a custom part's explicit polarity) says P.
    auto npn = makeComponent("bjt", {}, 0.0f, 0.0f);
    CHECK(artworkForPart(npn) == artworkForType("bjt"));
    auto pnp = npn;
    pnp.properties["model"] = "2N3906";
    CHECK(artworkForPart(pnp) != artworkForPart(npn));
    auto npnAgain = pnp;
    npnAgain.properties["model"] = "2N3904";
    CHECK(artworkForPart(npnAgain) == artworkForPart(npn));

    auto nJfet = makeComponent("jfet", {}, 0.0f, 0.0f);
    const auto* nJfetArt = artworkForPart(nJfet);
    auto pJfet = nJfet;
    pJfet.properties["model"] = "2N5460";
    CHECK(artworkForPart(pJfet) != nJfetArt);
    auto customPJfet = pJfet;
    customPJfet.properties["model"] = "Custom";
    customPJfet.properties["polarity"] = "P-channel";
    CHECK(artworkForPart(customPJfet) != nJfetArt);
    customPJfet.properties["polarity"] = "N-channel";
    CHECK(artworkForPart(customPJfet) == nJfetArt);

    auto nMos = makeComponent("mosfet", {}, 0.0f, 0.0f);
    const auto* nMosArt = artworkForPart(nMos);
    auto pMos = nMos;
    pMos.properties["preset"] = "IRF9540N";
    CHECK(artworkForPart(pMos) != nMosArt);
    auto customPMos = pMos;
    customPMos.properties["preset"] = "Custom";
    customPMos.properties["polarity"] = "P-channel";
    CHECK(artworkForPart(customPMos) != nMosArt);
    customPMos.properties["polarity"] = "N-channel";
    CHECK(artworkForPart(customPMos) == nMosArt);
    // A hand-edited part with no preset is Custom, so its explicit polarity
    // still chooses the symbol (the converter reads it the same way).
    customPMos.properties.erase("preset");
    customPMos.properties["polarity"] = "P-channel";
    CHECK(artworkForPart(customPMos) != nMosArt);

    // Each P file is a real, parsed drawing, and it uses the same pin frame as
    // its N counterpart. A polarity switch must not move the terminals.
    for (const auto* part : {&pnp, &pJfet, &pMos}) {
        const auto* art = artworkForPart(*part);
        REQUIRE(art != nullptr);
        CHECK_FALSE(art->isEmpty());

        const auto pins = Schematic::pinOffsets(part->type);
        for (const auto& pin : pins)
            CHECK(art->bounds.expanded(0.25f)
                      .contains(static_cast<float>(pin.x),
                                static_cast<float>(pin.y)));
    }
}
