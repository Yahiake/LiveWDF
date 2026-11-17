#include "VintageKnob.h"
#include "Fonts.h"
#include <cmath>

namespace livewdf {

VintageLookAndFeel::VintageLookAndFeel() {
    setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xFFF8FAFC));
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

juce::Label* VintageLookAndFeel::createSliderTextBox(juce::Slider& slider) {
    auto* l = LookAndFeel_V4::createSliderTextBox(slider);
    l->setFont(editor::Fonts::monoSemiBold(12.5f));
    l->setColour(juce::Label::textColourId, juce::Colour(0xFFF8FAFC));
    l->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour(juce::TextEditor::textColourId, juce::Colour(0xFFFFFFFF));
    l->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF14171E));
    l->setColour(juce::TextEditor::highlightColourId, accentColor_.withAlpha(0.35f));
    l->setColour(juce::TextEditor::outlineColourId, accentColor_.withAlpha(0.7f));
    l->setJustificationType(juce::Justification::centred);
    return l;
}

juce::Font VintageLookAndFeel::getSliderPopupFont(juce::Slider&) {
    return editor::Fonts::monoSemiBold(12.5f);
}

void VintageLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label) {
    if (!label.isBeingEdited()) {
        auto bounds = label.getLocalBounds().toFloat();
        // Slider value readout box styling: sleek dark recessed pill
        if (label.getParentComponent() != nullptr &&
            dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr)
        {
            auto pill = bounds.reduced(2.0f, 1.0f);
            g.setColour(juce::Colour(0xFF11141A));
            g.fillRoundedRectangle(pill, 4.0f);
            g.setColour(juce::Colour(0xFF222935));
            g.drawRoundedRectangle(pill, 4.0f, 1.0f);
        }

        g.setColour(label.findColour(juce::Label::textColourId));
        g.setFont(label.getFont());
        g.drawFittedText(label.getText(), label.getLocalBounds(), label.getJustificationType(), 1);
    }
}

void VintageLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPosProportional, float rotaryStartAngle,
                                           float rotaryEndAngle, juce::Slider& slider) {
    auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float size = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const float radius = size * 0.5f - 3.0f;
    const float centreX = bounds.getCentreX();
    const float centreY = bounds.getCentreY();
    const float currentAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    // 1. Recessed Chassis Well (Drop shadow into chassis plate)
    const float wellRadius = radius - 1.0f;
    const auto wellRect = juce::Rectangle<float>(centreX - wellRadius, centreY - wellRadius,
                                                 wellRadius * 2.0f, wellRadius * 2.0f);
    g.setColour(juce::Colour(0x66000000));
    g.fillEllipse(wellRect.expanded(2.5f));
    g.setColour(juce::Colour(0xFF0C0E13));
    g.fillEllipse(wellRect);
    g.setColour(juce::Colour(0x28455366));
    g.drawEllipse(wellRect, 1.0f);

    // 2. Precision Calibration Tick Ring (11 calibrated graduation marks)
    const int numTicks = 11;
    for (int i = 0; i < numTicks; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(numTicks - 1);
        const float tickAngle = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);
        const float cosA = std::cos(tickAngle - juce::MathConstants<float>::halfPi);
        const float sinA = std::sin(tickAngle - juce::MathConstants<float>::halfPi);

        const bool isMajor = (i == 0 || i == numTicks - 1 || i == 5);
        const float innerR = radius - 3.0f;
        const float outerR = radius + (isMajor ? 1.5f : 0.0f);
        const bool isActive = t <= (sliderPosProportional + 0.015f);

        if (isActive) {
            // Illuminated active tick
            g.setColour(accentColor_.withAlpha(0.35f));
            g.drawLine(centreX + innerR * cosA, centreY + innerR * sinA,
                       centreX + outerR * cosA, centreY + outerR * sinA,
                       isMajor ? 3.0f : 2.0f);
            g.setColour(isMajor ? juce::Colour(0xFFFFFFFF) : accentColor_);
            g.drawLine(centreX + innerR * cosA, centreY + innerR * sinA,
                       centreX + outerR * cosA, centreY + outerR * sinA,
                       isMajor ? 1.8f : 1.2f);
        } else {
            // Muted dark tick
            g.setColour(isMajor ? juce::Colour(0xFF384354) : juce::Colour(0xFF202630));
            g.drawLine(centreX + innerR * cosA, centreY + innerR * sinA,
                       centreX + outerR * cosA, centreY + outerR * sinA,
                       isMajor ? 1.4f : 1.0f);
        }
    }

    // 3. Dynamic Glowing Value Arc
    const float trackRadius = radius - 5.5f;
    juce::Path bgArc;
    bgArc.addCentredArc(centreX, centreY, trackRadius, trackRadius, 0.0f,
                        rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(juce::Colour(0xFF13171F));
    g.strokePath(bgArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(juce::Colour(0x20FFFFFF));
    g.strokePath(bgArc, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (sliderPosProportional > 0.001f) {
        juce::Path valueArc;
        valueArc.addCentredArc(centreX, centreY, trackRadius, trackRadius, 0.0f,
                               rotaryStartAngle, currentAngle, true);

        // Ambient halo glow
        g.setColour(accentColor_.withAlpha(0.20f));
        g.strokePath(valueArc, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Mid bloom
        g.setColour(accentColor_.withAlpha(0.48f));
        g.strokePath(valueArc, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Core arc
        g.setColour(slider.isMouseOverOrDragging() ? accentColor_.brighter(0.25f) : accentColor_);
        g.strokePath(valueArc, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Active tip bead
        const float tipCos = std::cos(currentAngle - juce::MathConstants<float>::halfPi);
        const float tipSin = std::sin(currentAngle - juce::MathConstants<float>::halfPi);
        const float tipX = centreX + trackRadius * tipCos;
        const float tipY = centreY + trackRadius * tipSin;

        g.setColour(accentColor_.withAlpha(0.45f));
        g.fillEllipse(tipX - 3.5f, tipY - 3.5f, 7.0f, 7.0f);
        g.setColour(juce::Colour(0xFFFFFFFF));
        g.fillEllipse(tipX - 1.8f, tipY - 1.8f, 3.6f, 3.6f);
    }

    // 4. Anodized Machined Rotor Body (Neural DSP cylindrical aluminum dial)
    const float knobRadius = radius - 10.0f;
    const auto knobRect = juce::Rectangle<float>(centreX - knobRadius, centreY - knobRadius,
                                                 knobRadius * 2.0f, knobRadius * 2.0f);

    // Bezel shadow from dial down onto well
    g.setColour(juce::Colour(0x95000000));
    g.fillEllipse(knobRect.translated(0.0f, 2.5f).expanded(1.5f));

    // Outer 45-degree chamfer skirt (directional studio illumination from top-left)
    juce::ColourGradient skirtGrad(juce::Colour(0xFF38404E), centreX - knobRadius * 0.7f, centreY - knobRadius * 0.7f,
                                  juce::Colour(0xFF0F1217), centreX + knobRadius * 0.7f, centreY + knobRadius * 0.7f,
                                  false);
    g.setGradientFill(skirtGrad);
    g.fillEllipse(knobRect);

    // Outer rim highlight specular crescent
    g.setColour(juce::Colour(0x55A0B2C6));
    g.drawEllipse(knobRect, 1.0f);

    // Rotor top face
    const float faceRadius = knobRadius - 1.8f;
    const auto faceRect = juce::Rectangle<float>(centreX - faceRadius, centreY - faceRadius,
                                                 faceRadius * 2.0f, faceRadius * 2.0f);

    juce::ColourGradient faceGrad(juce::Colour(0xFF232832), centreX - faceRadius * 0.4f, centreY - faceRadius * 0.4f,
                                 juce::Colour(0xFF13161C), centreX + faceRadius * 0.5f, centreY + faceRadius * 0.5f,
                                 false);
    g.setGradientFill(faceGrad);
    g.fillEllipse(faceRect);

    // Concentric lathe micro-grooves
    g.setColour(juce::Colour(0x18FFFFFF));
    g.drawEllipse(faceRect.reduced(faceRadius * 0.16f), 0.75f);
    g.drawEllipse(faceRect.reduced(faceRadius * 0.32f), 0.75f);
    g.drawEllipse(faceRect.reduced(faceRadius * 0.48f), 0.75f);

    // Recessed center hub
    const float hubRadius = faceRadius * 0.42f;
    const auto hubRect = juce::Rectangle<float>(centreX - hubRadius, centreY - hubRadius,
                                                hubRadius * 2.0f, hubRadius * 2.0f);
    g.setColour(juce::Colour(0x60000000));
    g.fillEllipse(hubRect.expanded(1.0f));

    juce::ColourGradient hubGrad(juce::Colour(0xFF161920), centreX, centreY - hubRadius,
                                juce::Colour(0xFF222832), centreX, centreY + hubRadius,
                                false);
    g.setGradientFill(hubGrad);
    g.fillEllipse(hubRect);

    g.setColour(juce::Colour(0x35FFFFFF));
    g.drawEllipse(hubRect, 0.8f);

    // 5. Engraved Milled Indicator Notch with Lacquer Infill (Neural DSP style)
    const float pCos = std::cos(currentAngle - juce::MathConstants<float>::halfPi);
    const float pSin = std::sin(currentAngle - juce::MathConstants<float>::halfPi);

    const float slotStartR = hubRadius + 1.5f;
    const float slotEndR = faceRadius - 1.5f;

    // Slot inner shadow
    g.setColour(juce::Colour(0xAA000000));
    g.drawLine(centreX + slotStartR * pCos + 0.8f, centreY + slotStartR * pSin + 0.8f,
               centreX + slotEndR * pCos + 0.8f, centreY + slotEndR * pSin + 0.8f,
               3.6f);

    // Slot metal edge
    g.setColour(juce::Colour(0xFF0B0D11));
    g.drawLine(centreX + slotStartR * pCos, centreY + slotStartR * pSin,
               centreX + slotEndR * pCos, centreY + slotEndR * pSin,
               3.2f);

    // Luminous lacquer fill
    if (slider.isMouseOverOrDragging()) {
        g.setColour(accentColor_.withAlpha(0.45f));
        g.drawLine(centreX + slotStartR * pCos, centreY + slotStartR * pSin,
                   centreX + slotEndR * pCos, centreY + slotEndR * pSin,
                   4.2f);
        g.setColour(juce::Colour(0xFFFFFFFF));
        g.drawLine(centreX + slotStartR * pCos, centreY + slotStartR * pSin,
                   centreX + slotEndR * pCos, centreY + slotEndR * pSin,
                   2.4f);
    } else {
        g.setColour(juce::Colour(0xFFF1F5F9));
        g.drawLine(centreX + slotStartR * pCos, centreY + slotStartR * pSin,
                   centreX + slotEndR * pCos, centreY + slotEndR * pSin,
                   2.2f);
    }
}

// -----------------------------------------------------------------------------
// VintageKnob
// -----------------------------------------------------------------------------
VintageKnob::VintageKnob(const juce::String& name, const juce::String& suffix)
    : suffix_(suffix) {
    slider_.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 74, 20);
    slider_.setTextValueSuffix(suffix);
    slider_.setLookAndFeel(&lookAndFeel_);
    slider_.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                juce::MathConstants<float>::pi * 2.75f, true);
    addAndMakeVisible(slider_);

    label_.setText(name, juce::dontSendNotification);
    label_.setJustificationType(juce::Justification::centred);
    label_.setFont(editor::Fonts::semiBold(12.5f));
    label_.setColour(juce::Label::textColourId, juce::Colour(0xFFCBD5E1));
    addAndMakeVisible(label_);
}

VintageKnob::~VintageKnob() {
    slider_.setLookAndFeel(nullptr);
}

void VintageKnob::setLabel(const juce::String& text) {
    label_.setText(text, juce::dontSendNotification);
}

void VintageKnob::setSuffix(const juce::String& suffix) {
    suffix_ = suffix;
    slider_.setTextValueSuffix(suffix);
}

void VintageKnob::setAccentColor(juce::Colour color) {
    lookAndFeel_.setAccentColor(color);
    repaint();
}

void VintageKnob::paint(juce::Graphics& g) {
    juce::ignoreUnused(g);
}

void VintageKnob::resized() {
    auto bounds = getLocalBounds();
    const int labelHeight = 20;
    label_.setBounds(bounds.removeFromTop(labelHeight));
    slider_.setBounds(bounds);
}

} // namespace livewdf
