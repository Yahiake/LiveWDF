#pragma once

// The editor window's proportions, in pixels.
//
// Why these are here and not `constexpr` inside `EditorWindow.cpp`: the theme
// parity test holds them to Céline's own metrics, and a constant in an anonymous
// namespace is invisible to it. Putting them where the test can read them is what
// makes "the window's shape is Céline's" a checkable statement instead of a claim.
//
// The values are Céline's (`PluginTheme.h`, `CelineUI/Theme.h`). Two of the three
// columns exist in both editors and take Céline's width; the scope strip and the
// status bar are ours and are not part of the comparison.

namespace livewdf::editor::layout {

inline constexpr int kToolbarHeight    = 45;   // Céline `Theme::toolbarHeight`
inline constexpr int kPaletteWidth     = 188;  // Céline `Theme::paletteWidth`
inline constexpr int kInspectorWidth   = 263;  // Céline `Theme::inspectorWidth`
inline constexpr int kPaletteRowHeight = 35;   // Céline `Theme::paletteRowHeight`

// Our own, with no Céline counterpart.
inline constexpr int kScopeHeight = 170;
inline constexpr int kStatusHeight = 22;

}  // namespace livewdf::editor::layout
