#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace livewdf::editor {

class AboutDialog : public juce::Component {
public:
    enum class Mode {
        Editor,
        Plugin,
        // The keyboard card: every shortcut the sheet answers to, in one
        // place. A discoverability fix, not a new interaction -- it opens
        // through a button and closes on Esc like the About card does.
        Keys,
    };

    explicit AboutDialog(Mode mode);
    ~AboutDialog() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& e) override;

    /// Show this dialog modally within a parent component
    static void show(juce::Component* parent, Mode mode);

private:
    Mode mode_;
    juce::TextButton closeBtn_ { "X" };
    juce::TextButton githubBtn_ { "GitHub" };
    juce::TextButton docBtn_ { "Documentation" };
    juce::TextButton licenseBtn_ { "Licenses" };
    juce::Rectangle<int> cardBounds_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutDialog)
};

}  // namespace livewdf::editor
