#pragma once

// Shared, file-local drawing constants and key matching for the canvas.
//
// The canvas is split across several translation units by job: model/editing,
// geometry, painting, gestures, and commands. These helpers are deliberately
// internal to that implementation, not part of `SchematicEditor`'s API.

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>

#include "format/PartLibrary.h"
#include "gui/glow/Glow.h"

namespace livewdf::editor {
namespace {
// ============================================================================
// Palette
//
// Every colour the sheet draws with, in one place. Nothing below sets a colour
// inline, so a theme change is an edit to this block and not a hunt through the
// file.
// ============================================================================

// The sheet's colours, all named roles off `glow`, whose values are Celine's own
// (the palette block in `SchematicConstants.h` carries the role names). The names
// here say the *job*, so a theme change is one line per role rather than a
// hex-literal hunt. Wire and part are deliberately different colours.
juce::Colour sheetBackground()  { return glow::getColourCanvasBackground(); }
juce::Colour gridDot()          { return glow::getColourGridMinor(); }
juce::Colour wireColour()       { return glow::getColourWire(); }
juce::Colour junctionColour()   { return glow::getColourWire(); }
juce::Colour selectionColour()  { return glow::getColourSelected(); }
juce::Colour handleFill()       { return glow::getColourCanvasBackground(); }
juce::Colour ghostColour()      { return glow::getColourSelected().withAlpha(0.35f); }
juce::Colour boxEncloseColour() { return glow::getColourBoxEnclose(); }
juce::Colour boxCrossColour()   { return glow::getColourBoxCrossing(); }
juce::Colour pinColour()        { return glow::getColourWire(); }
juce::Colour bodyColour()       { return glow::getColourElement(); }
juce::Colour labelColour()      { return glow::getColourElement(); }
juce::Colour valueColour()      { return glow::getColourElement().withAlpha(0.7f); }
juce::Colour badgeBackground()  { return glow::getColourCanvasBackground().withAlpha(0.85f); }
juce::Colour badgeText()        { return glow::getColourWire(); }

// ============================================================================
// Geometry constants, in grid units unless stated
// ============================================================================

constexpr float kMinGridSize = 4.0f;    // pixels per square: zoomed right out
constexpr float kMaxGridSize = 40.0f;   // and zoomed right in
constexpr float kZoomStep = 1.12f;      // one wheel notch

/// A pin is picked within this many *pixels*, not squares: a hit target is
/// something you aim a pointer at, so it should not shrink when you zoom out.
constexpr float kPinPickPixels = 14.0f;

/// A wire is picked within this many pixels of the line.
constexpr float kWirePickPixels = 7.0f;

/// A part is picked within this many *pixels* of its drawn ink. Much tighter
/// than the old bounding-box test, but still a pointer-sized target: the symbol's
/// own stroke width plus this is what the hand can reasonably hit. It does not
/// grow with the symbol, so an empty corner of a large valve is not part of it.
constexpr float kPartPickPixels = 4.0f;

/// The diamond drawn at a selected wire's ends, and the reach that grabs it, are
/// both `SchematicEditor::wireHandleReach()` -- one number, so what is drawn and
/// what is grabbable cannot disagree.

/// Grid dots are skipped below this zoom: closer together than this they read as
/// a solid fill rather than as a grid. (Celine's threshold, kept because it is
/// the right one for these dot sizes.)
constexpr float kGridDotThreshold = 8.0f;

std::string keyForPartType(const std::string& type) {
    const PartSpec* spec = findPart(type);
    return spec ? type : std::string();
}

/// Does `key` type `character`?
///
/// Ported **verbatim** from Celine's `SchematicCanvas::keyIs`, because the bug it
/// avoids is not obvious and Celine already paid for it:
///
///   * the *character the layout produced* is matched first, with the key code as
///     a fallback -- so R is whichever key types an "r" in front of you rather
///     than a position on an American keyboard, and Caps Lock does not break it;
///   * with a Command/Ctrl modifier the text character is **not** the letter:
///     X11 and Windows deliver the ASCII control code (Ctrl-Z arrives as 0x1A),
///     which is non-zero, so matching on it silently compares 0x1A against 'z'.
///     The key code is the layout's letter on every platform, so it is preferred
///     there.
bool keyIs(const juce::KeyPress& key, juce::juce_wchar character) {
    const auto byCode = [&] {
        const int code = key.getKeyCode();
        return code == static_cast<int>(juce::CharacterFunctions::toUpperCase(character))
               || code == static_cast<int>(character);
    };

    if (key.getModifiers().isCommandDown())
        return byCode();

    const auto typed = key.getTextCharacter();

    return typed != 0 ? juce::CharacterFunctions::toLowerCase(typed) == character : byCode();
}

} // namespace
} // namespace livewdf::editor