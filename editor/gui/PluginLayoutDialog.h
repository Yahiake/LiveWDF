#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "format/Document.h"

#include <functional>
#include <string>
#include <vector>

namespace livewdf::editor {

class PluginLayoutDialog : public juce::Component, public juce::ListBoxModel {
public:
    explicit PluginLayoutDialog(const wdf::LiveWDFDocument& doc);
    ~PluginLayoutDialog() override = default;

    /// Callback when user applies layout changes: (potOrder, switchOrder)
    std::function<void(std::vector<std::string>, std::vector<std::string>)> onApply;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& e) override;

    // ListBoxModel interface
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;

    struct Item {
        std::string name;
        std::string memberInfo;
    };

private:
    std::vector<Item> pots_;
    std::vector<Item> switches_;

    juce::ListBox potList_;
    juce::ListBox switchList_;

    juce::TextButton potUpBtn_ { "^ UP" };
    juce::TextButton potDownBtn_ { "v DOWN" };

    juce::TextButton switchUpBtn_ { "^ UP" };
    juce::TextButton switchDownBtn_ { "v DOWN" };

    juce::TextButton applyBtn_ { "APPLY LAYOUT" };
    juce::TextButton resetBtn_ { "RESET DEFAULT" };
    juce::TextButton closeBtn_ { "CANCEL" };

    juce::Rectangle<int> cardBounds_;

    // Helper model for switch list box
    struct SwitchListModel : public juce::ListBoxModel {
        PluginLayoutDialog& owner;
        explicit SwitchListModel(PluginLayoutDialog& o) : owner(o) {}
        int getNumRows() override { return static_cast<int>(owner.switches_.size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool sel) override;
    } switchModel_ { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginLayoutDialog)
};

}  // namespace livewdf::editor
