#include "UiLookAndFeel.h"
#include "Fonts.h"
#include "glow/SchematicConstants.h"

#include <cmath>

namespace livewdf::editor {

UiLookAndFeel::UiLookAndFeel() {
    // General window and popup background
    setColour(juce::ResizableWindow::backgroundColourId, glow::getColourBackground());
    setColour(juce::PopupMenu::backgroundColourId, glow::getColourPanel());
    setColour(juce::PopupMenu::textColourId, glow::getColourTextPrimary());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, glow::getColourElevated());
    setColour(juce::PopupMenu::highlightedTextColourId, glow::getColourTextPrimary());
    setColour(juce::PopupMenu::headerTextColourId, glow::getColourAccent());

    // TextEditor defaults
    setColour(juce::TextEditor::backgroundColourId, glow::getColourElevated());
    setColour(juce::TextEditor::textColourId, glow::getColourTextPrimary());
    setColour(juce::TextEditor::outlineColourId, glow::getColourBorder());
    setColour(juce::TextEditor::focusedOutlineColourId, glow::getColourAccent());
    setColour(juce::TextEditor::highlightColourId, glow::getColourAccent().withAlpha(0.25f));
    setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);

    // ComboBox defaults
    setColour(juce::ComboBox::backgroundColourId, glow::getColourElevated());
    setColour(juce::ComboBox::textColourId, glow::getColourTextPrimary());
    setColour(juce::ComboBox::outlineColourId, glow::getColourBorder());
    setColour(juce::ComboBox::focusedOutlineColourId, glow::getColourAccent());
    setColour(juce::ComboBox::arrowColourId, glow::getColourTextSecondary());

    // Tooltip defaults
    setColour(juce::TooltipWindow::backgroundColourId, glow::getColourElevated());
    setColour(juce::TooltipWindow::textColourId, glow::getColourTextPrimary());
    setColour(juce::TooltipWindow::outlineColourId, glow::getColourBorder());

    // ScrollBar defaults
    setColour(juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, glow::getColourBorder());
    setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
}

// ============================================================================
// Typography
// ============================================================================

juce::Typeface::Ptr UiLookAndFeel::getTypefaceForFont(const juce::Font& font) {
    const bool isBold = font.isBold();
    return Fonts::typeface(Fonts::Family::UI, isBold ? Fonts::Weight::Bold : Fonts::Weight::Medium);
}

juce::Font UiLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight) {
    const float h = juce::jlimit(12.0f, buttonFontMaxHeight,
                                static_cast<float>(buttonHeight) * buttonFontFraction);
    return Fonts::semiBold(h);
}

juce::Font UiLookAndFeel::getComboBoxFont(juce::ComboBox& box) {
    const float h = juce::jlimit(12.0f, comboBoxFontMaxHeight,
                                static_cast<float>(box.getHeight()) * comboBoxFontFraction);
    return Fonts::medium(h);
}

juce::Font UiLookAndFeel::getPopupMenuFont() {
    return Fonts::medium(popupMenuFontHeight);
}

juce::Font UiLookAndFeel::getLabelFont(juce::Label& label) {
    const float h = label.getFont().getHeight();
    const bool isBold = label.getFont().isBold();
    return isBold ? Fonts::semiBold(h > 0.0f ? h : 13.5f) : Fonts::medium(h > 0.0f ? h : 13.5f);
}

// ============================================================================
// Buttons
// ============================================================================

void UiLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                        const juce::Colour& backgroundColour,
                                        bool shouldDrawButtonAsHighlighted,
                                        bool shouldDrawButtonAsDown) {
    auto bounds = button.getLocalBounds().toFloat();
    const float cornerSize = 4.5f;

    const bool isToggled = button.getToggleState();
    const bool isEnabled = button.isEnabled();

    juce::Colour base = backgroundColour.isTransparent()
                            ? glow::getColourElevated()
                            : backgroundColour;

    if (!isEnabled) {
        g.setColour(base.withAlpha(0.4f));
        g.fillRoundedRectangle(bounds, cornerSize);
        g.setColour(glow::getColourBorder().withAlpha(0.2f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);
        return;
    }

    juce::Colour topColour, bottomColour, borderColour;

    if (isToggled) {
        // Active/toggled state: warm gold glowing bezel
        topColour = glow::getColourAccent().withAlpha(0.22f);
        bottomColour = glow::getColourAccent().withAlpha(0.10f);
        borderColour = glow::getColourAccent().withAlpha(0.85f);
    } else if (shouldDrawButtonAsDown) {
        // Pressed down
        topColour = base.darker(0.18f);
        bottomColour = base.darker(0.08f);
        borderColour = glow::getColourAccent().withAlpha(0.6f);
    } else if (shouldDrawButtonAsHighlighted) {
        // Hover
        topColour = base.brighter(0.16f);
        bottomColour = base.brighter(0.05f);
        borderColour = glow::getColourBorder().brighter(0.25f);
    } else {
        // Resting
        topColour = base.brighter(0.05f);
        bottomColour = base.darker(0.08f);
        borderColour = glow::getColourBorder();
    }

    // Subtle dark gradient surface
    juce::ColourGradient grad(topColour, 0.0f, bounds.getY(),
                              bottomColour, 0.0f, bounds.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(bounds, cornerSize);

    // Subtle inner bevel highlight on top edge
    if (!shouldDrawButtonAsDown && !isToggled) {
        g.setColour(juce::Colour(0x18FFFFFF));
        g.drawLine(bounds.getX() + cornerSize, bounds.getY() + 1.0f,
                   bounds.getRight() - cornerSize, bounds.getY() + 1.0f, 1.0f);
    }

    // Border outline
    g.setColour(borderColour);
    g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);
}

void UiLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                  bool shouldDrawButtonAsHighlighted,
                                  bool shouldDrawButtonAsDown) {
    juce::ignoreUnused(shouldDrawButtonAsDown);
    auto bounds = button.getLocalBounds();

    juce::Colour textColour;
    if (!button.isEnabled()) {
        textColour = glow::getColourTextSecondary().withAlpha(0.5f);
    } else if (button.getToggleState()) {
        textColour = glow::getColourAccent();
    } else if (shouldDrawButtonAsHighlighted) {
        textColour = juce::Colours::white;
    } else {
        textColour = button.findColour(juce::TextButton::textColourOffId);
        if (textColour.isTransparent() || textColour == juce::Colour(0xff000000))
            textColour = glow::getColourTextPrimary();
    }

    g.setColour(textColour);
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.drawFittedText(button.getButtonText(), bounds.reduced(6, 2),
                     juce::Justification::centred, 1);
}

void UiLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                    bool shouldDrawButtonAsHighlighted,
                                    bool shouldDrawButtonAsDown) {
    juce::ignoreUnused(shouldDrawButtonAsDown);
    auto bounds = button.getLocalBounds().toFloat();
    const float boxSize = 14.0f;
    const float boxY = bounds.getCentreY() - boxSize * 0.5f;
    const float boxX = bounds.getX() + 2.0f;

    auto boxRect = juce::Rectangle<float>(boxX, boxY, boxSize, boxSize);

    // Toggle Box Background
    const bool on = button.getToggleState();
    if (on) {
        g.setColour(glow::getColourAccent().withAlpha(0.2f));
        g.fillRoundedRectangle(boxRect, 3.0f);
        g.setColour(glow::getColourAccent());
        g.drawRoundedRectangle(boxRect.reduced(0.5f), 3.0f, 1.2f);

        // Check mark or inner dot
        g.setColour(glow::getColourAccent());
        g.fillRoundedRectangle(boxRect.reduced(3.5f), 1.5f);
    } else {
        g.setColour(glow::getColourElevated());
        g.fillRoundedRectangle(boxRect, 3.0f);
        g.setColour(shouldDrawButtonAsHighlighted ? glow::getColourTextSecondary() : glow::getColourBorder());
        g.drawRoundedRectangle(boxRect.reduced(0.5f), 3.0f, 1.0f);
    }

    // Toggle Text
    g.setColour(button.isEnabled() ? glow::getColourTextPrimary() : glow::getColourTextSecondary().withAlpha(0.5f));
    g.setFont(Fonts::ui(13.0f, on));
    const auto textArea = bounds.withTrimmedLeft(boxSize + 8.0f).toNearestInt();
    g.drawFittedText(button.getButtonText(), textArea, juce::Justification::centredLeft, 1);
}

// ============================================================================
// ComboBox & Popup Menus
// ============================================================================

void UiLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                int buttonX, int buttonY, int buttonW, int buttonH,
                                juce::ComboBox& box) {
    juce::ignoreUnused(buttonX, buttonY, buttonW, buttonH);
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const float corner = 4.0f;

    const bool focused = box.hasKeyboardFocus(false);
    const bool hovered = box.isMouseOver(true) || isButtonDown;

    juce::Colour bg = glow::getColourElevated();
    if (hovered)
        bg = bg.brighter(0.08f);

    g.setColour(bg);
    g.fillRoundedRectangle(bounds, corner);

    // Border
    juce::Colour border = focused ? glow::getColourAccent()
                                  : (hovered ? glow::getColourBorder().brighter(0.2f)
                                             : glow::getColourBorder());
    g.setColour(border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), corner, 1.0f);

    // Minimal chevron arrow on the right
    const float arrowRight = static_cast<float>(width) - 12.0f;
    const float arrowY = static_cast<float>(height) * 0.5f;
    const float arrowW = 4.0f;

    juce::Path chevron;
    chevron.startNewSubPath(arrowRight - arrowW, arrowY - 2.0f);
    chevron.lineTo(arrowRight, arrowY + 2.5f);
    chevron.lineTo(arrowRight + arrowW, arrowY - 2.0f);

    g.setColour(hovered ? glow::getColourAccent() : glow::getColourTextSecondary());
    g.strokePath(chevron, juce::PathStrokeType(1.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
}

void UiLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label) {
    label.setBounds(1, 1, box.getWidth() - 24, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId, glow::getColourTextPrimary());
}

void UiLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height) {
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(glow::getColourPanel());
    g.fillRoundedRectangle(bounds, 5.0f);

    g.setColour(glow::getColourBorder().brighter(0.1f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.0f);
}

void UiLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                      bool isSeparator, bool isActive, bool isHighlighted,
                                      bool isTicked, bool hasSubMenu,
                                      const juce::String& text, const juce::String& shortcutKeyText,
                                      const juce::Drawable* icon, const juce::Colour* textColour) {
    juce::ignoreUnused(icon, hasSubMenu);
    if (isSeparator) {
        g.setColour(glow::getColourBorder());
        g.drawLine(static_cast<float>(area.getX() + 6), static_cast<float>(area.getCentreY()),
                   static_cast<float>(area.getRight() - 6), static_cast<float>(area.getCentreY()), 1.0f);
        return;
    }

    auto r = area.reduced(4, 1);

    if (isHighlighted && isActive) {
        g.setColour(glow::getColourElevated().brighter(0.12f));
        g.fillRoundedRectangle(r.toFloat(), 3.5f);

        // Subtle accent bar on the left of highlighted row
        g.setColour(glow::getColourAccent());
        g.fillRoundedRectangle(static_cast<float>(r.getX()),
                               static_cast<float>(r.getY() + 3),
                               2.5f,
                               static_cast<float>(r.getHeight() - 6),
                               1.0f);
    }

    juce::Colour tc = (textColour != nullptr) ? *textColour : glow::getColourTextPrimary();
    if (!isActive)
        tc = glow::getColourTextSecondary().withAlpha(0.4f);
    else if (isHighlighted)
        tc = juce::Colours::white;

    g.setColour(tc);
    g.setFont(getPopupMenuFont());

    auto textBounds = r.withTrimmedLeft(14).withTrimmedRight(shortcutKeyText.isNotEmpty() ? 50 : 6);
    g.drawFittedText(text, textBounds, juce::Justification::centredLeft, 1);

    if (shortcutKeyText.isNotEmpty()) {
        g.setColour(glow::getColourTextSecondary());
        g.setFont(Fonts::mono(11.0f));
        g.drawFittedText(shortcutKeyText, r.withTrimmedRight(8), juce::Justification::centredRight, 1);
    }

    if (isTicked) {
        g.setColour(glow::getColourAccent());
        g.fillEllipse(static_cast<float>(r.getX() + 4), static_cast<float>(r.getCentreY() - 2.5f), 5.0f, 5.0f);
    }
}

// ============================================================================
// TextEditor
// ============================================================================

void UiLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                            juce::TextEditor& textEditor) {
    juce::ignoreUnused(textEditor);
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(glow::getColourElevated());
    g.fillRoundedRectangle(bounds, 3.5f);
}

void UiLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                         juce::TextEditor& textEditor) {
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const bool focused = textEditor.hasKeyboardFocus(true);
    g.setColour(focused ? glow::getColourAccent() : glow::getColourBorder());
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.5f, focused ? 1.2f : 1.0f);
}

// ============================================================================
// Scrollbar
// ============================================================================

void UiLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar,
                                  int x, int y, int width, int height,
                                  bool isScrollbarVertical, int thumbStartPosition,
                                  int thumbSize, bool isMouseOver, bool isMouseDown) {
    juce::ignoreUnused(scrollbar, x, y);
    if (thumbSize <= 0)
        return;

    juce::Rectangle<float> thumbRect;
    if (isScrollbarVertical) {
        const float w = static_cast<float>(width);
        const float thumbW = juce::jmin(5.0f, w);
        const float thumbX = (w - thumbW) * 0.5f;
        thumbRect = juce::Rectangle<float>(thumbX, static_cast<float>(thumbStartPosition),
                                          thumbW, static_cast<float>(thumbSize));
    } else {
        const float h = static_cast<float>(height);
        const float thumbH = juce::jmin(5.0f, h);
        const float thumbY = (h - thumbH) * 0.5f;
        thumbRect = juce::Rectangle<float>(static_cast<float>(thumbStartPosition), thumbY,
                                          static_cast<float>(thumbSize), thumbH);
    }

    juce::Colour c = glow::getColourBorder().brighter(0.15f);
    if (isMouseDown)
        c = glow::getColourAccent().withAlpha(0.85f);
    else if (isMouseOver)
        c = c.brighter(0.35f);

    g.setColour(c);
    g.fillRoundedRectangle(thumbRect, 2.5f);
}

// ============================================================================
// Tooltips
// ============================================================================

void UiLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text,
                               int width, int height) {
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(glow::getColourElevated().brighter(0.04f));
    g.fillRoundedRectangle(bounds, 4.0f);

    g.setColour(glow::getColourBorder().brighter(0.2f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);

    g.setColour(glow::getColourTextPrimary());
    g.setFont(Fonts::medium(12.5f));
    g.drawFittedText(text, bounds.reduced(8.0f, 4.0f).toNearestInt(), juce::Justification::centred, 2);
}

juce::Rectangle<int> UiLookAndFeel::getTooltipBounds(const juce::String& tipText,
                                                    juce::Point<int> screenPos,
                                                    juce::Rectangle<int> parentArea) {
    const auto font = Fonts::medium(12.5f);
    const int w = juce::jmin(340, static_cast<int>(juce::GlyphArrangement::getStringWidth(font, tipText)) + 20);
    const int h = 26;

    auto r = juce::Rectangle<int>(screenPos.x + 12, screenPos.y + 16, w, h);
    if (r.getRight() > parentArea.getRight())
        r.setX(parentArea.getRight() - w - 4);
    if (r.getBottom() > parentArea.getBottom())
        r.setY(screenPos.y - h - 6);

    return r;
}

// ============================================================================
// Sliders (Linear)
// ============================================================================

void UiLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                    float sliderPos, float minSliderPos, float maxSliderPos,
                                    juce::Slider::SliderStyle style, juce::Slider& slider) {
    juce::ignoreUnused(minSliderPos, maxSliderPos, style);
    const bool isHorizontal = (slider.getSliderStyle() == juce::Slider::LinearHorizontal);
    const bool isEnabled = slider.isEnabled();

    if (isHorizontal) {
        const float trackHeight = 3.0f;
        const float trackY = static_cast<float>(y) + (static_cast<float>(height) - trackHeight) * 0.5f;
        const float trackX = static_cast<float>(x);
        const float trackW = static_cast<float>(width);

        // Inactive track
        g.setColour(glow::getColourElevated().darker(0.1f));
        g.fillRoundedRectangle(trackX, trackY, trackW, trackHeight, 1.5f);

        // Active fill
        if (isEnabled) {
            g.setColour(glow::getColourAccent());
            g.fillRoundedRectangle(trackX, trackY, sliderPos - trackX, trackHeight, 1.5f);
        }

        // Thumb
        const float thumbRadius = 6.0f;
        g.setColour(glow::getColourBackground());
        g.fillEllipse(sliderPos - thumbRadius, trackY + trackHeight * 0.5f - thumbRadius,
                      thumbRadius * 2.0f, thumbRadius * 2.0f);

        g.setColour(isEnabled ? glow::getColourAccent() : glow::getColourTextSecondary());
        g.drawEllipse(sliderPos - thumbRadius, trackY + trackHeight * 0.5f - thumbRadius,
                      thumbRadius * 2.0f, thumbRadius * 2.0f, 1.5f);
    } else {
        const float trackWidth = 3.0f;
        const float trackX = static_cast<float>(x) + (static_cast<float>(width) - trackWidth) * 0.5f;
        const float trackY = static_cast<float>(y);
        const float trackH = static_cast<float>(height);

        // Inactive track
        g.setColour(glow::getColourElevated().darker(0.1f));
        g.fillRoundedRectangle(trackX, trackY, trackWidth, trackH, 1.5f);

        // Active fill
        if (isEnabled) {
            g.setColour(glow::getColourAccent());
            g.fillRoundedRectangle(trackX, sliderPos, trackWidth, trackY + trackH - sliderPos, 1.5f);
        }

        // Thumb
        const float thumbRadius = 6.0f;
        g.setColour(glow::getColourBackground());
        g.fillEllipse(trackX + trackWidth * 0.5f - thumbRadius, sliderPos - thumbRadius,
                      thumbRadius * 2.0f, thumbRadius * 2.0f);

        g.setColour(isEnabled ? glow::getColourAccent() : glow::getColourTextSecondary());
        g.drawEllipse(trackX + trackWidth * 0.5f - thumbRadius, sliderPos - thumbRadius,
                      thumbRadius * 2.0f, thumbRadius * 2.0f, 1.5f);
    }
}

}  // namespace livewdf::editor
