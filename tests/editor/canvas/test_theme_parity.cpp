// Holds our editor's theme to Céline's, numerically (U3.4).
//
// This is Step 1 of the port plan made checkable: the editor wears Céline's
// palette, not the warm one it started on, and this test is what stops that from
// drifting back. It compares role *values* -- our accessor against Céline's own
// number (`celine_reference_theme.h`) -- and the window metrics that decide the
// shape of the window.
//
// **What it can and cannot hold.** It can hold: every colour we name that Céline
// also names, and the widths and radii. It cannot hold: whether the window *looks*
// right -- fonts, spacing, the way a pill sits under the mouse. Those are the
// painter's, and the rendered PNG is what makes that judgement possible. A green
// run here is "it is the right palette and the right proportions", not "it looks
// like Céline".

#include "celine_reference_theme.h"

#include "EditorLayout.h"
#include "EditorLookAndFeel.h"
#include "gui/glow/SchematicConstants.h"

#include <catch2.hpp>

#include <array>
#include <string>

using namespace livewdf::editor;

namespace {

/// Our role's colour, looked up by the name the reference table uses. A name with
/// no case here is a mistake in the table, not a silent skip: the test fails so
/// the table gets fixed rather than quietly measuring nothing.
juce::Colour ourColour(const std::string& role) {
    if (role == "canvasBackground")         return EditorLookAndFeel::canvasBackground();
    if (role == "wire / pin / cursorMark")  return EditorLookAndFeel::wireColour();
    if (role == "element / ground")         return EditorLookAndFeel::partColour();
    if (role == "selected / hover")         return EditorLookAndFeel::selection();
    if (role == "gridLine")                 return livewdf::glow::getColourGridLine();
    if (role == "captionName")              return EditorLookAndFeel::captionName();
    if (role == "captionValue")             return EditorLookAndFeel::captionValue();
    if (role == "captionUnset / danger")    return EditorLookAndFeel::captionUnset();
    if (role == "boxEnclose")               return EditorLookAndFeel::boxEnclose();
    if (role == "boxCrossing")              return EditorLookAndFeel::boxCrossing();
    if (role == "panel")                    return EditorLookAndFeel::panel();
    if (role == "textOnPanel")              return EditorLookAndFeel::textOnPanel();
    if (role == "pill")                     return EditorLookAndFeel::pill();
    if (role == "headerBackground")         return livewdf::glow::getColourHeaderBackground();

    return juce::Colours::transparentBlack;  // a table name with no case: see below
}

std::uint32_t argb(const juce::Colour& c) {
    return (static_cast<std::uint32_t>(c.getAlpha()) << 24)
         | (static_cast<std::uint32_t>(c.getRed())   << 16)
         | (static_cast<std::uint32_t>(c.getGreen()) << 8)
         |  static_cast<std::uint32_t>(c.getBlue());
}

}  // namespace

TEST_CASE("Every colour matches LiveWDF Design System semantic tokens") {
    // The failure this exists for: the palette silently reverting to a warm value,
    // or a role being added to the port and pointed at the wrong Céline colour.
    for (const auto& ref : reftheme::colourTable()) {
        INFO("role: " << ref.ourRole << "  (Céline '" << ref.celineRole << "')");

        const auto ours = ourColour(ref.ourRole);

        // A table entry with no case above comes back transparent; report it as
        // an uncovered role rather than a colour mismatch.
        REQUIRE_FALSE(ours.isTransparent());

        CHECK(argb(ours) == ref.argb);
    }
}

TEST_CASE("The window's proportions are Céline's") {
    // The shape of the window is read before any one colour: a palette the wrong
    // width, or a toolbar the wrong height, makes the window look like a different
    // program even if every colour matches. The metrics are our layout constants,
    // so this is the test that fails when one of them is nudged.
    const auto m = reftheme::metrics();

    CHECK(layout::kPaletteWidth     == m.paletteWidth);
    CHECK(layout::kInspectorWidth   == m.inspectorWidth);
    CHECK(layout::kToolbarHeight    == m.toolbarHeight);
    CHECK(layout::kPaletteRowHeight == m.paletteRowHeight);
    CHECK(EditorLookAndFeel::cornerRadius == m.cornerRadius);
}
