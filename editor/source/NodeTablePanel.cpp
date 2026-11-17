#include "NodeTablePanel.h"
#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <cmath>

namespace livewdf::editor {

namespace {
constexpr int kHeaderH = 46;
constexpr int kRowH = 19;

// A voltage this small is DC zero as far as a reader is concerned; showing
// "0.000" for a node that is at 3e-14 V reads as "grounded", which is right.
bool isEffectivelyZero(double v) { return std::abs(v) < 1e-9; }

juce::String formatVolts(double v) {
    if (isEffectivelyZero(v)) return "0.000 V";
    if (std::abs(v) >= 1000.0) return juce::String(v / 1000.0, 3) + " kV";
    if (std::abs(v) >= 1.0)    return juce::String(v, 3) + " V";
    if (std::abs(v) >= 1e-3)   return juce::String(v * 1e3, 3) + " mV";
    return juce::String(v * 1e6, 3) + " uV";
}

juce::String formatAmps(double a) {
    if (isEffectivelyZero(a)) return "0.000 A";
    if (std::abs(a) >= 1.0)    return juce::String(a, 3) + " A";
    if (std::abs(a) >= 1e-3)   return juce::String(a * 1e3, 3) + " mA";
    if (std::abs(a) >= 1e-6)   return juce::String(a * 1e6, 3) + " uA";
    return juce::String(a * 1e9, 3) + " nA";
}
}  // namespace

NodeTablePanel::NodeTablePanel() {
    setOpaque(true);
}

void NodeTablePanel::setOperatingPoint(const std::map<std::string, double>& volts) {
    volts_ = volts;
    repaint();
}

void NodeTablePanel::setDeviceCurrents(const std::map<std::string, double>& amps) {
    amps_ = amps;
    repaint();
}

void NodeTablePanel::setStatus(const std::string& text, bool isError) {
    status_ = text;
    statusIsError_ = isError;
    repaint();
}

void NodeTablePanel::clear() {
    volts_.clear();
    amps_.clear();
    status_.clear();
    repaint();
}

void NodeTablePanel::paint(juce::Graphics& g) {
    g.fillAll(glow::getColourBackground());

    g.setColour(glow::getColourNormal());
    g.setFont(Fonts::bold(14.0f));
    g.drawText("DC OPERATING POINT",
               juce::Rectangle<int>(8, 2, getWidth() - 16, 18),
               juce::Justification::centredLeft);

    if (!status_.empty()) {
        g.setColour(statusIsError_ ? glow::getColourHotRed() : glow::getColourLaserGreen());
        g.setFont(Fonts::light(13.0f));
        g.drawFittedText(status_, juce::Rectangle<int>(8, 20, getWidth() - 16, 16),
                         juce::Justification::centredLeft, 1);
    }

    int y = kHeaderH;

    if (volts_.empty() && amps_.empty()) {
        g.setColour(glow::getColourNormal().withAlpha(0.55f));
        g.setFont(Fonts::light(13.0f));
        g.drawFittedText("no circuit -- place a part and wire it to a source",
                         juce::Rectangle<int>(8, y, getWidth() - 16, 32),
                         juce::Justification::topLeft, 2);
        return;
    }

    // Column header row.
    g.setColour(glow::getColourNormal().withAlpha(0.7f));
    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    g.drawText("NODE", juce::Rectangle<int>(10, y, getWidth() - 100, kRowH),
               juce::Justification::centredLeft);
    g.drawText("VOLTAGE", juce::Rectangle<int>(getWidth() - 100, y, 92, kRowH),
               juce::Justification::centredRight);
    y += kRowH;

    g.setFont(juce::FontOptions(11.0f));
    bool band = false;
    for (const auto& [name, v] : volts_) {
        const juce::Rectangle<int> row(6, y, getWidth() - 12, kRowH);
        if (band) {
            g.setColour(glow::getColourNormal().withAlpha(0.06f));
            g.fillRect(row);
        }
        band = !band;

        g.setColour(glow::getColourNormal());
        g.drawFittedText(name, row.withTrimmedLeft(4), juce::Justification::centredLeft, 1);

        // Colour by magnitude: cold blue at 0 V, hot amber near the rail. Same
        // scale the probe badges use, so the table and the canvas agree.
        const float mag = static_cast<float>(std::min(1.0, std::abs(v) / 350.0));
        g.setColour(glow::getColourElectrical().interpolatedWith(glow::getColourAmber(), mag));
        g.drawText(formatVolts(v), row.withTrimmedRight(4), juce::Justification::centredRight);

        y += kRowH;
        if (y > getHeight() - kRowH) break;
    }

    if (!amps_.empty() && y < getHeight() - 2 * kRowH) {
        y += 6;
        g.setColour(glow::getColourNormal().withAlpha(0.7f));
        g.setFont(Fonts::bold(14.0f));
        g.drawText("DEVICE CURRENT", juce::Rectangle<int>(10, y, getWidth() - 100, kRowH),
                   juce::Justification::centredLeft);
        y += kRowH;

        g.setFont(Fonts::light(13.0f));
        for (const auto& [name, a] : amps_) {
            const juce::Rectangle<int> row(6, y, getWidth() - 12, kRowH);
            g.setColour(glow::getColourNormal());
            g.drawFittedText(name, row.withTrimmedLeft(4), juce::Justification::centredLeft, 1);
            g.setColour(glow::getColourPurple());
            g.drawText(formatAmps(a), row.withTrimmedRight(4), juce::Justification::centredRight);
            y += kRowH;
            if (y > getHeight() - kRowH) break;
        }
    }
}

}  // namespace livewdf::editor
