#pragma once

// The strip of open circuits.
//
// **Why the editor has tabs.** A schematic editor is a place where more than one
// circuit is open at a time: a filter and the amplifier it goes in, the
// original and the copy being edited, a whole project. One canvas and one
// "Open" button made every one of them a round trip through the file system.
//
// The strip is a row of buttons, one per open circuit, with the current one
// lit. A tab can be closed, and the last one cannot -- an editor with no
// circuit open has nothing to edit, and a window that vanishes is a window the
// user did not ask to vanish.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace livewdf::editor {

class TabBar : public juce::Component {
public:
    TabBar();
    ~TabBar() override;

    /// The tab that was clicked.
    std::function<void(int)> onTabSelected;

    /// The tab's close button was clicked.
    std::function<void(int)> onTabClosed;

    /// The tabs to show: their names, and which one is current.
    ///
    /// Rebuilt only when the set of tabs changes, so a tab does not flicker
    /// every time the title is refreshed.
    void setTabs(const std::vector<std::string>& names, int current);

    /// The name of a tab, for the host (a tooltip, say).
    const std::string& tabName(int index) const;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    /// The tabs' names.
    std::vector<std::string> names_;

    /// The buttons, one pair per tab.
    ///
    /// **Held by pointer, not by value.** A `juce::TextButton` cannot be
    /// copied, and a vector of them cannot be either -- so the buttons live
    /// here and the names live beside them.
    std::vector<std::unique_ptr<juce::TextButton>> selectButtons_;
    std::vector<std::unique_ptr<juce::Button>> closeButtons_;

    int current_ = -1;
    bool needsRebuild_ = true;

    void rebuild();
    void layoutTabs();
};

}  // namespace livewdf::editor
