#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

namespace livewdf::glow {

// Stroke widths
inline constexpr float STROKE_NORMAL = 2.0f;
inline constexpr float STROKE_HIGHLIGHT = 3.0f;
inline constexpr float STROKE_THIN = 1.5f;

// ===== Modern Audio-Plugin Palette =====
//
// Dark graphite surfaces, crisp platinum component ink, warm gold accent, teal signal flow.
inline juce::Colour getColourCanvasBackground() { return juce::Colour (0xff14171c); } // Canvas BG (graphite)
inline juce::Colour getColourBackground()       { return juce::Colour (0xff0f1216); } // Window ground / panels
inline juce::Colour getColourPanel()            { return juce::Colour (0xff12151a); } // Parts panel, inspector, toolbar
inline juce::Colour getColourElevated()         { return juce::Colour (0xff1c2128); } // Hovered rows, dropdowns, tooltips
inline juce::Colour getColourBorder()           { return juce::Colour (0xff262c36); } // Hairlines, dividers, focus rings

// Text
inline juce::Colour getColourTextPrimary()      { return juce::Colour (0xfff1f5f9); } // Labels, part names, values
inline juce::Colour getColourTextSecondary()    { return juce::Colour (0xff94a3b8); } // Placeholders, disabled, shortcuts
inline juce::Colour getColourTextOnPanel()      { return juce::Colour (0xffe2e8f0); } // Text on panel surfaces
inline juce::Colour getColourTextOnAccent()     { return juce::Colour (0xff000000); } // Text on gold accent

// Accents
inline juce::Colour getColourAccent()           { return juce::Colour (0xffffb800); } // Gold — armed, active, focus
inline juce::Colour getColourAccentDim()        { return juce::Colour (0xffcc9200); } // Pressed / muted emphasis
inline juce::Colour getColourSignal()           { return juce::Colour (0xff00e5a0); } // Teal — live signal flow

// Wire / Part
inline juce::Colour getColourWire()             { return juce::Colour (0xff8b9bb4); } // Clean technical unselected wires
inline juce::Colour getColourWireHot()          { return juce::Colour (0xffffb800); } // Selected/hovered wires
inline juce::Colour getColourElement()          { return juce::Colour (0xffd8dee9); } // Crisp high-contrast component ink
inline juce::Colour getColourElementStroke()    { return juce::Colour (0xffe2e8f0); } // Part outline
inline juce::Colour getColourPin()              { return juce::Colour (0xffffb800); } // Pin circles, junction dots

// Semantic
inline juce::Colour getColourError()            { return juce::Colour (0xffff3e3e); } // Problem parts, invalid
inline juce::Colour getColourSuccess()          { return juce::Colour (0xff00d47e); } // Valid drops, compiled OK
inline juce::Colour getColourWarning()          { return juce::Colour (0xffffb800); } // Pending / armed

// Selection / Box
inline juce::Colour getColourSelected()         { return juce::Colour (0xffffb800); } // Selected (gold)
inline juce::Colour getColourBoxEnclose()       { return juce::Colour (0xffffb800); } // Box: encloses
inline juce::Colour getColourBoxCrossing()      { return juce::Colour (0xff00e5a0); } // Box: crosses (teal)

// Legacy compatibility — map old names to new palette
inline juce::Colour getColourNormal()           { return getColourElement(); }
inline juce::Colour getColourHighlight()        { return getColourAccent(); }
inline juce::Colour getColourAmber()            { return getColourAccent(); }
inline juce::Colour getColourElectrical()       { return getColourAccent(); }
inline juce::Colour getColourPurple()           { return juce::Colour (0xff8b5cf6); } // Violet for compatibility
inline juce::Colour getColourHotRed()           { return getColourError(); }
inline juce::Colour getColourLaserGreen()       { return getColourSuccess(); }
inline juce::Colour getColourGrey()             { return getColourTextSecondary(); }
inline juce::Colour getColourGridLine()         { return getColourBorder(); }
inline juce::Colour getColourCursorMark()       { return getColourAccent(); }
inline juce::Colour getColourPending()          { return getColourWarning(); }
inline juce::Colour getColourGridMinor()        { return juce::Colour (0xff1f242d); }
inline juce::Colour getColourGridMajor()        { return juce::Colour (0xff2a313d); }
inline juce::Colour getColourInsetBackground()  { return getColourBackground(); }
inline juce::Colour getColourHeaderBackground() { return getColourBackground(); }
inline juce::Colour getColourHeaderText()       { return getColourTextPrimary(); }
inline juce::Colour getColourPill()             { return getColourElevated(); }

// Voltage gradient for signal flow beads (teal → gold → red)
inline juce::Colour getVoltageColourGradient(float alpha) {
    alpha = juce::jlimit(0.0f, 1.0f, alpha);
    if (alpha < 0.33f)
        return getColourSignal().interpolatedWith(getColourAccent(), alpha * 3.0f);
    if (alpha < 0.66f)
        return getColourAccent().interpolatedWith(getColourWarning(), (alpha - 0.33f) * 3.0f);
    return getColourWarning().interpolatedWith(getColourError(), (alpha - 0.66f) * 3.0f);
}

inline juce::Colour getPowerColourGradient(float alpha) {
    return juce::Colours::black.interpolatedWith(juce::Colours::white, alpha);
}

// Power scaling for display
inline constexpr float POWER_SCALING = 5e3f;
inline constexpr float INTENSITY_SCALING = 0.5e3f;

// Tube dimensions
inline constexpr float TUBE_WIDTH = 150.0f;
inline constexpr float TUBE_HEIGHT = 200.0f;

// Resistor zigzag pattern
inline constexpr int RESISTOR_ZIGZAG_COUNT = 6;
inline constexpr float RESISTOR_ZIGZAG_AMPLITUDE = 10.0f;
inline constexpr int RESISTOR_ZIGZAG_LENGTH = 40;

// Capacitor plates
inline constexpr int CAPACITOR_PLATE_WIDTH = 35;
inline constexpr float CAPACITOR_PLATE_GAP = 10.0f;

// Schematic terminal & symbol sizes
inline constexpr float SCHEMATIC_GROUND_SIZE = 20.0f;
inline constexpr float SCHEMATIC_GAIN_SIZE = 50.0f;
inline constexpr float SCHEMATIC_DIODE_SIZE = 20.0f;
inline constexpr float SCHEMATIC_JUNC_SIZE = 5.0f;
inline constexpr float SCHEMATIC_JACK_SIZE = 35.0f;

// Signal Path Beads
inline constexpr float SIGNAL_PATH_MAX_VOLTAGE = 450.0f;
inline constexpr float SIGNALPATH_SPEED = 4.0f;
inline constexpr float SIGNALPATH_BEAD_SPACING = 16.0f;
inline constexpr float SIGNALPATH_BEAD_SIZE = 2.0f;
inline constexpr float SIGNALPATH_SAMPLE_SPACING = 4.0f; // pixels
inline constexpr float MONITORING_SMOOTHING_ALPHA = 0.1f;

// Signal path modes
inline constexpr int SIGNALPATH_MODE_NORMAL_FORWARD = 0;
inline constexpr int SIGNALPATH_MODE_REVERSE_FORWARD = 1;   
inline constexpr int SIGNALPATH_MODE_NORMAL_BACKWARD = 2;
inline constexpr int SIGNALPATH_MODE_REVERSE_BACKWARD = 3;   

} // namespace livewdf::glow
