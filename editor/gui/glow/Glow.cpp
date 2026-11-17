#include "Glow.h"
#include <algorithm>

namespace livewdf::glow {

void drawGlowAndCorePath(juce::Graphics& g,
                        const juce::Path& path,
                        float intensity, 
                        juce::Colour coreColour, 
                        juce::Colour glowColour, 
                        bool highlight) {
    drawGlowPath(g, path, glowColour, intensity);
    drawCorePath(g, path, coreColour, highlight);
}

void drawSolidCorePath(juce::Graphics& g,
                       const juce::Path& path,
                       bool highlight,
                       const juce::Colour& col) {
    // Cathodyne: a solid stroke is white when highlighted, else the part's own
    // colour. (The reference default is the *highlight* colour; our callers
    // always pass a colour, so the default is `normal` -- the visible behaviour
    // is identical because the parameter is never omitted.)
    g.setColour(highlight ? juce::Colours::white : col);
    g.strokePath(path, juce::PathStrokeType(highlight ? STROKE_HIGHLIGHT : STROKE_NORMAL,
                                            STROKE_JOINT_STYLE, STROKE_END_STYLE));
}

void drawCorePath(juce::Graphics& g,
                  const juce::Path& path,
                  juce::Colour coreColour, 
                  bool highlight) {
    // ===== Cathodyne, exact =====
    // Four stacked strokes, widest/darkest first. The dark under-stroke is the
    // *background* colour, not black: it keeps the plasma off the warm ground
    // without drawing a hard outline. The white-hot centre is opaque, which is
    // what makes a wire read as lit rather than painted.
    const float coreIntensity = 1.0f;

    g.setColour(getColourBackground().withAlpha(0.5f * coreIntensity));
    g.strokePath(path, juce::PathStrokeType(5.5f, STROKE_JOINT_STYLE, STROKE_END_STYLE));

    // Bright plasma
    g.setColour(coreColour.withAlpha(0.6f * coreIntensity));
    g.strokePath(path, juce::PathStrokeType(4.0f, STROKE_JOINT_STYLE, STROKE_END_STYLE));
    g.setColour(coreColour.withAlpha(0.7f * coreIntensity));
    g.strokePath(path, juce::PathStrokeType(3.0f, STROKE_JOINT_STYLE, STROKE_END_STYLE));

    // White hot core
    g.setColour(juce::Colours::white.withAlpha(coreIntensity));
    g.strokePath(path, juce::PathStrokeType(1.5f, STROKE_JOINT_STYLE, STROKE_END_STYLE));

    if (highlight) {
        g.setColour(juce::Colours::white.withAlpha(coreIntensity));
        g.strokePath(path, juce::PathStrokeType(STROKE_HIGHLIGHT, STROKE_JOINT_STYLE, STROKE_END_STYLE));
    }
}

void drawGlowPath(juce::Graphics& g,
                  const juce::Path& path,
                  juce::Colour glowColour,
                  float intensity) {
    // ===== Cathodyne, exact =====
    // Up to nine stacked strokes, 60 px down to 12 px, alpha quadratic in `i`
    // and linear in intensity. `init` walks the widest strokes off as intensity
    // rises, so a bright path has a *tighter* aura, not merely a brighter one.
    intensity = std::clamp(intensity, 0.0f, 1.0f);

    if (intensity > 0.0f) {
        const int init = static_cast<int>(5.0f - intensity * 5.0f);
        for (int i = init; i < 9; ++i) {
            g.setColour(glowColour.withAlpha(0.006f * static_cast<float>((i + 1) * (i + 1)) * intensity));
            g.strokePath(path, juce::PathStrokeType(6.0f * static_cast<float>(10 - i),
                                                    STROKE_JOINT_STYLE, STROKE_END_STYLE));
        }
    }
}

void drawPowerGlowPath(juce::Graphics& g,
                       const juce::Path& path,
                       float intensity) {
    // ===== Cathodyne, exact =====
    const auto colorE2 = getPowerColourGradient(0.0f);
    const auto colorE1 = getPowerColourGradient(0.5f);
    const auto colorE0 = getPowerColourGradient(1.0f);
    intensity = std::clamp(intensity, 0.0f, 1.0f);

    const auto intensityE2 = std::clamp(intensity * 2e2f, 0.0f, 1.0f);
    const auto intensityE1 = std::clamp(intensity * 2e1f, 0.0f, 1.0f);
    const auto intensityE0 = std::clamp(intensity * 2e0f, 0.0f, 1.0f);

    drawGlowPath(g, path, colorE2, intensityE2 * 0.5f);
    if (intensity > 0.01f) {
        drawGlowPath(g, path, colorE1, intensityE1 * 0.5f);
        if (intensity > 0.1f) {
            drawGlowPath(g, path, colorE0, intensityE0 * 0.5f);
        }
    }
}

} // namespace livewdf::glow
