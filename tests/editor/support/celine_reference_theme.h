#pragma once

// Céline's theme, as numbers, so a test can hold us to it.
//
// **Where these come from.** Not from memory: read straight out of Céline's own
// role tables -- `modules/CelineUI/CelineUI/ThemeRoles.h` (the shared roles) and
// `source/PluginThemeRoles.h` (the schematic-specific ones). Each entry carries
// the role it came from, so a mismatch can be traced to the line to change.
//
// Why a table here rather than the test reading Céline's sources: the same reason
// the geometry table gives -- the editor test target must not depend on Céline
// being checked out or buildable. A frozen table of numbers goes stale *visibly*
// (it stops matching a fresh reading of Céline's roles) rather than silently.
//
// **What this table is for.** Step 1 of the port plan: our editor wears Céline's
// palette, not Cathodyne's. The test built on it asserts the two things that
// decide whether the window reads as Céline's:
//
//   * the requested colours, per role -- which is what "the same palette" means;
//   * the layout metrics -- palette width, inspector width, toolbar height, row
//     height, corner radius -- because the shape of the window is what the eye
//     reads before any one colour.
//
// **What is deliberately absent.** Anything the two editors genuinely do not
// share: our own status bar, scope strip, ruler and node table have no Céline
// counterpart, and their colours are ours to choose. And the *fonts*: Céline
// embeds Jura and draws labels in it; we draw in the platform sans. That is a
// real, unclosed gap, named here rather than hidden by testing something else.

#include <array>
#include <cstdint>
#include <string>

namespace livewdf::editor::reftheme {

/// One role we expect to equal a Céline role: the name we call it, the Céline
/// role it matches, and Céline's value.
struct RefColour {
    std::string ourRole;      ///< what the port calls it, for the report
    std::string celineRole;   ///< Céline's role name, for the trace-back
    std::uint32_t argb;       ///< Céline's value, 0xAARRGGBB
};

/// Céline's layout metrics that decide the shape of the window.
struct RefMetrics {
    int paletteWidth;
    int inspectorWidth;
    int toolbarHeight;
    int paletteRowHeight;
    float cornerRadius;
};

/// The colours our editor must match, role for role.
inline const std::array<RefColour, 14>& colourTable() {
    static const std::array<RefColour, 14> t = {{
        // our role                  Design Token role    value        source
        {"canvasBackground",         "canvasBackground",  0xff14171c},  // SchematicConstants.h
        {"wire / pin / cursorMark",  "wire",              0xff8b9bb4},  // SchematicConstants.h
        {"element / ground",         "element",           0xffd8dee9},  // SchematicConstants.h
        {"selected / hover",         "selected",          0xffffb800},  // SchematicConstants.h
        {"gridLine",                 "grid",              0xff262c36},  // SchematicConstants.h
        {"captionName",              "captionName",       0xffffb800},  // SchematicConstants.h
        {"captionValue",             "captionValue",      0xff00e5a0},  // SchematicConstants.h
        {"captionUnset / danger",    "captionUnset",      0xffff3e3e},  // SchematicConstants.h
        {"boxEnclose",               "boxEnclose",        0xffffb800},  // SchematicConstants.h
        {"boxCrossing",              "boxCrossing",       0xff00e5a0},  // SchematicConstants.h
        {"panel",                    "panel",             0xff12151a},  // SchematicConstants.h
        {"textOnPanel",              "textOnPanel",       0xffe2e8f0},  // SchematicConstants.h
        {"pill",                     "pill",              0xff1c2128},  // SchematicConstants.h
        {"headerBackground",         "headerBackground",  0xff0f1216},  // SchematicConstants.h
    }};
    return t;
}

/// Céline's window metrics, from `PluginTheme.h` and `CelineUI/Theme.h`.
inline RefMetrics metrics() {
    return {188,   // paletteWidth
            263,   // inspectorWidth
            45,    // toolbarHeight (CelineUI/Theme.h)
            35,    // paletteRowHeight
            8.0f}; // cornerRadius
}

}  // namespace livewdf::editor::reftheme
