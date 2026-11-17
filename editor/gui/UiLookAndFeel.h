#pragma once

// LiveWDF Unified UI LookAndFeel
// Implements a modern, dark graphite, engineered visual identity across the
// standalone editor and audio plugin, inspired by high-end studio hardware.

#include <juce_gui_basics/juce_gui_basics.h>

namespace livewdf::editor {

class UiLookAndFeel : public juce::LookAndFeel_V4 {
public:
    UiLookAndFeel();
    ~UiLookAndFeel() override = default;

    // --- Typeface resolution ----------------------------------------------
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& font) override;

    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;
    juce::Font getComboBoxFont(juce::ComboBox& box) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getLabelFont(juce::Label& label) override;

    // --- Buttons ----------------------------------------------------------
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;

    // --- ComboBox & Popup Menus -------------------------------------------
    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override;

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override;

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu,
                           const juce::String& text, const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;

    // --- TextEditor -------------------------------------------------------
    void fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                  juce::TextEditor& textEditor) override;

    void drawTextEditorOutline(juce::Graphics& g, int width, int height,
                               juce::TextEditor& textEditor) override;

    // --- Scrollbars -------------------------------------------------------
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar,
                       int x, int y, int width, int height,
                       bool isScrollbarVertical, int thumbStartPosition,
                       int thumbSize, bool isMouseOver, bool isMouseDown) override;

    int getDefaultScrollbarWidth() override { return 6; }

    // --- Tooltips ---------------------------------------------------------
    void drawTooltip(juce::Graphics& g, const juce::String& text,
                     int width, int height) override;

    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText,
                                         juce::Point<int> screenPos,
                                         juce::Rectangle<int> parentArea) override;

    // --- Sliders ----------------------------------------------------------
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style, juce::Slider& slider) override;

    // Typography metrics
    static constexpr float buttonFontFraction = 0.55f;
    static constexpr float buttonFontMaxHeight = 14.5f;
    static constexpr float comboBoxFontFraction = 0.55f;
    static constexpr float comboBoxFontMaxHeight = 14.0f;
    static constexpr float popupMenuFontHeight = 14.0f;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UiLookAndFeel)
};

}  // namespace livewdf::editor
