#include "TabBar.h"

#include "EditorLookAndFeel.h"
#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <algorithm>

namespace livewdf::editor {

namespace {

/// The height of the strip.
constexpr int kTabHeight = 26;

/// The gap between the tabs.
constexpr int kTabGap = 2;

/// The width of a tab, and the most it may take: a long file name is trimmed
/// rather than allowed to push the other tabs off the strip.
constexpr int kTabWidth = 150;

class TabCloseButton : public juce::Button {
public:
    TabCloseButton() : juce::Button("close") {
        setTooltip("Close this circuit");
    }

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        auto bounds = getLocalBounds().toFloat();
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float r = 7.0f;
        const float crossR = 3.5f;

        if (shouldDrawButtonAsDown) {
            g.setColour(juce::Colour(0x55E05252));
            g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
        } else if (shouldDrawButtonAsHighlighted) {
            g.setColour(juce::Colour(0x30E05252));
            g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
        }

        const juce::Colour crossColour = shouldDrawButtonAsHighlighted
            ? juce::Colour(0xFFFF7070)
            : juce::Colour(0xFF8B9BB4);

        g.setColour(crossColour);
        g.drawLine(cx - crossR, cy - crossR, cx + crossR, cy + crossR, 1.6f);
        g.drawLine(cx - crossR, cy + crossR, cx + crossR, cy - crossR, 1.6f);
    }
};

}  // namespace

TabBar::TabBar() {
    setInterceptsMouseClicks(true, false);
}

TabBar::~TabBar() = default;

void TabBar::setTabs(const std::vector<std::string>& names, int current) {
    // **Rebuilt only when the set of tabs changes.** The names change all the
    // time -- a tab's name is its file name and its modified mark -- and
    // rebuilding the buttons then would take the tab away under the pointer
    // that is clicking it.
    const bool sameCount = names_.size() == names.size();
    bool sameNames = sameCount;

    if (sameCount)
        for (size_t i = 0; i < names.size(); ++i)
            if (names_[i] != names[i]) {
                sameNames = false;
                break;
            }

    if (!sameNames) {
        names_ = names;
        needsRebuild_ = true;
    }

    current_ = current;

    if (needsRebuild_) {
        for (auto& button : selectButtons_)
            removeChildComponent(button.get());
        for (auto& button : closeButtons_)
            removeChildComponent(button.get());

        selectButtons_.clear();
        closeButtons_.clear();

        for (size_t i = 0; i < names_.size(); ++i) {
            auto select = std::make_unique<juce::TextButton>("", "Select this circuit");
            select->setClickingTogglesState(true);
            auto close = std::make_unique<TabCloseButton>();

            addAndMakeVisible(*select);
            addAndMakeVisible(*close);

            selectButtons_.push_back(std::move(select));
            closeButtons_.push_back(std::move(close));
        }

        needsRebuild_ = false;
        rebuild();
    } else {
        // The names are the same, but the buttons say so.
        for (size_t i = 0; i < selectButtons_.size(); ++i) {
            selectButtons_[i]->setButtonText(names_[i]);
            selectButtons_[i]->setTooltip(names_[i]);
        }
    }

    for (size_t i = 0; i < selectButtons_.size(); ++i)
        selectButtons_[i]->setToggleState(static_cast<int>(i) == current_, juce::dontSendNotification);

    repaint();
}

const std::string& TabBar::tabName(int index) const {
    static const std::string empty;
    if (index < 0 || index >= static_cast<int>(names_.size()))
        return empty;
    return names_[static_cast<size_t>(index)];
}

void TabBar::rebuild() {
    // The click handlers are bound to the tab's *index*, so a rebuild cannot
    // leave a button pointing at the wrong tab.
    for (size_t i = 0; i < selectButtons_.size(); ++i) {
        const int index = static_cast<int>(i);

        selectButtons_[i]->onClick = [this, index] {
            if (onTabSelected)
                onTabSelected(index);
        };

        closeButtons_[i]->onClick = [this, index] {
            if (onTabClosed)
                onTabClosed(index);
        };

        selectButtons_[i]->setButtonText(names_[i]);
        selectButtons_[i]->setTooltip(names_[i]);
    }

    layoutTabs();
}

void TabBar::layoutTabs() {
    int x = 0;
    for (size_t i = 0; i < selectButtons_.size(); ++i) {
        selectButtons_[i]->setBounds(x, 2, kTabWidth, kTabHeight - 4);
        closeButtons_[i]->setBounds(x + kTabWidth - 22, (kTabHeight - 16) / 2, 16, 16);

        x += kTabWidth + kTabGap;
    }
}

void TabBar::paint(juce::Graphics& g) {
    g.fillAll(glow::getColourPanel());
    g.setColour(glow::getColourBorder());
    g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
}

void TabBar::resized() {
    layoutTabs();
}

}  // namespace livewdf::editor
