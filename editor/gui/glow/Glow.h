#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "SchematicConstants.h"

namespace livewdf::glow {

inline constexpr juce::PathStrokeType::JointStyle STROKE_JOINT_STYLE = juce::PathStrokeType::curved;
inline constexpr juce::PathStrokeType::EndCapStyle STROKE_END_STYLE = juce::PathStrokeType::rounded;

void drawGlowAndCorePath(juce::Graphics& g,
                        const juce::Path& path,
                        float intensity, 
                        juce::Colour coreColour, 
                        juce::Colour glowColour, 
                        bool highlight = false);

void drawCorePath(juce::Graphics& g,
                  const juce::Path& path,
                  juce::Colour coreColour, 
                  bool highlight = false);

void drawSolidCorePath(juce::Graphics& g,
                       const juce::Path& path,
                       bool highlight = false,
                       const juce::Colour& col = getColourNormal());

void drawGlowPath(juce::Graphics& g,
                  const juce::Path& path,
                  juce::Colour glowColour,
                  float intensity);

void drawPowerGlowPath(juce::Graphics& g,
                       const juce::Path& path,
                       float intensity);

} // namespace livewdf::glow
