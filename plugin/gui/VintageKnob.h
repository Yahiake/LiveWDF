#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace livewdf {

class VintageLookAndFeel : public juce::LookAndFeel_V4 {
public:
    VintageLookAndFeel();
    ~VintageLookAndFeel() override = default;

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider& slider) override;

    void drawLabel(juce::Graphics& g, juce::Label& label) override;
    juce::Label* createSliderTextBox(juce::Slider& slider) override;
    juce::Font getSliderPopupFont(juce::Slider& slider) override;

    void setAccentColor(juce::Colour c) { accentColor_ = c; }
    juce::Colour getAccentColor() const { return accentColor_; }

private:
    juce::Colour accentColor_ { 0xFFFFAB00 }; // Warm amber
};

class VintageKnob : public juce::Component {
public:
    VintageKnob(const juce::String& name = "", const juce::String& suffix = "");
    ~VintageKnob() override;

    void setLabel(const juce::String& text);
    void setSuffix(const juce::String& suffix);
    void setAccentColor(juce::Colour color);

    juce::Slider& slider() { return slider_; }
    const juce::Slider& slider() const { return slider_; }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    VintageLookAndFeel lookAndFeel_;
    juce::Slider slider_;
    juce::Label label_;
    juce::String suffix_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VintageKnob)
};

} // namespace livewdf
