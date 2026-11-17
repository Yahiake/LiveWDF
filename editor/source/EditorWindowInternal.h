#pragma once

// Window-local proportions, test-signal constants, and chrome colours.
//
// `EditorWindow.cpp` is split by job (shell/content, files/tabs, engine/scope,
// undo, self-test). These values are shared by those translation units but are
// not part of the window's public API.

#include <juce_gui_extra/juce_gui_extra.h>

#include "EditorLayout.h"
#include "gui/glow/Glow.h"

namespace livewdf::editor {
namespace {
// Short local names for the window's proportions, which live in `EditorLayout.h`
// so the theme parity test can read them. Binding them here keeps this file's own
// call sites short without a second copy of the numbers.
constexpr int kToolbarH = layout::kToolbarHeight;
constexpr int kPaletteW = layout::kPaletteWidth;
constexpr int kRightW   = layout::kInspectorWidth;
constexpr int kScopeH   = layout::kScopeHeight;
constexpr int kStatusH  = layout::kStatusHeight;

/// The tab strip's height, between the toolbar and the sheet.
constexpr int kTabBarH = 26;

/// The message drawer's height when open. Tall enough for a build error and a
/// handful of unconnected parts without leaving the sheet unusable.
constexpr int kConsoleH = 150;

/// The editor's scratch file extension and wildcard.
const char* kFileWildcard = "*.livewdf";

/// The editor's test signal for the scope: 440 Hz at 100 mV. A level that will
/// not slam the first stage into a hard clip, so the trace shows the circuit's
/// own shape, and a round number the reader can do arithmetic with.
constexpr double kTestFreq = 440.0;
constexpr double kTestAmplitude = 0.1;

// Window chrome, two-tone like Celine's: a dark sheet with a slightly lighter
// header band across the top, so the toolbar reads as chrome rather than as more
// sheet. The band colour is Celine's `headerBackground`.
juce::Colour bg() { return glow::getColourBackground(); }
juce::Colour headerBand() { return glow::getColourHeaderBackground(); }
juce::Colour panelEdge() { return glow::getColourBorder(); }
} // namespace
} // namespace livewdf::editor