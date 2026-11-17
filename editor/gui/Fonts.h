#pragma once

// LiveWDF Typography System
// Bundled typefaces: Inter (UI) and IBM Plex Mono (Engineering Readouts).
// Both licensed under the SIL Open Font License 1.1.

#include <juce_gui_basics/juce_gui_basics.h>

namespace livewdf::editor {

namespace Fonts {

enum class Family {
    UI,     // Inter (clean, modern, highly legible at all sizes)
    Mono,   // IBM Plex Mono (precise technical engineering font)
};

enum class Weight {
    Light,
    Regular,
    Medium,
    SemiBold,
    Bold,
};

/// Retrieve underlying Typeface for a given family and weight
juce::Typeface::Ptr typeface(Family family, Weight weight = Weight::Medium);
juce::Typeface::Ptr typeface(Family family, bool bold);

/// Backwards compatibility for single-family weight lookup
juce::Typeface::Ptr typeface(Weight weight);

/// Construct a font with specified family, size (in pixels), and weight
juce::Font font(Family family, float heightInPixels, Weight weight = Weight::Medium);
juce::Font font(Family family, float heightInPixels, bool bold);
juce::Font font(Weight weight, float heightInPixels);

/// Primary UI Font (Inter)
inline juce::Font ui(float height, Weight weight = Weight::Medium) {
    return font(Family::UI, height, weight);
}
inline juce::Font ui(float height, bool bold) {
    return font(Family::UI, height, bold ? Weight::Bold : Weight::Medium);
}

/// Numerical & Engineering Monospace Font (IBM Plex Mono)
inline juce::Font mono(float height, bool bold = false) {
    return font(Family::Mono, height, bold ? Weight::SemiBold : Weight::Regular);
}
inline juce::Font monoSemiBold(float height) {
    return font(Family::Mono, height, Weight::SemiBold);
}

/// Standardized UI weights & backwards compatibility
inline juce::Font light(float height)    { return font(Family::UI, height, Weight::Medium); }
inline juce::Font regular(float height)  { return font(Family::UI, height, Weight::Regular); }
inline juce::Font medium(float height)   { return font(Family::UI, height, Weight::Medium); }
inline juce::Font semiBold(float height) { return font(Family::UI, height, Weight::SemiBold); }
inline juce::Font bold(float height)     { return font(Family::UI, height, Weight::Bold); }

/// Install Inter as the default application typeface across all JUCE widgets
void installDefaultTypeface();

}  // namespace Fonts

}  // namespace livewdf::editor
