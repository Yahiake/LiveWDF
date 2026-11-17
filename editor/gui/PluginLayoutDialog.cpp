#include "PluginLayoutDialog.h"
#include "Fonts.h"
#include "glow/Glow.h"
#include "format/ControlGroups.h"

namespace livewdf::editor {

namespace {

PluginLayoutDialog::Item makeItemFromGroup(const wdf::LiveControlGroup& g) {
    PluginLayoutDialog::Item item;
    item.name = g.name;
    std::string info;
    for (size_t m = 0; m < g.members.size(); ++m) {
        if (m > 0) info += ", ";
        info += g.members[m].id;
    }
    if (g.members.size() > 1)
        info += " (Ganged)";
    item.memberInfo = info;
    return item;
}

}  // namespace

PluginLayoutDialog::PluginLayoutDialog(const wdf::LiveWDFDocument& doc) {
    setAlwaysOnTop(true);
    setWantsKeyboardFocus(true);

    // Extract pot groups and switch groups
    const auto potGroups = wdf::potControlGroups(doc);
    for (const auto& g : potGroups)
        pots_.push_back(makeItemFromGroup(g));

    const auto swGroups = wdf::switchControlGroups(doc);
    for (const auto& g : swGroups)
        switches_.push_back(makeItemFromGroup(g));

    // Setup Pot ListBox
    potList_.setModel(this);
    potList_.setRowHeight(30);
    potList_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xFF0F1217));
    potList_.setColour(juce::ListBox::outlineColourId, juce::Colour(0xFF262E3B));
    addAndMakeVisible(potList_);

    // Setup Switch ListBox
    switchList_.setModel(&switchModel_);
    switchList_.setRowHeight(30);
    switchList_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xFF0F1217));
    switchList_.setColour(juce::ListBox::outlineColourId, juce::Colour(0xFF262E3B));
    addAndMakeVisible(switchList_);

    // Pot Up/Down buttons
    potUpBtn_.onClick = [this] {
        const int sel = potList_.getSelectedRow();
        if (sel > 0 && sel < static_cast<int>(pots_.size())) {
            std::swap(pots_[static_cast<size_t>(sel)], pots_[static_cast<size_t>(sel - 1)]);
            potList_.updateContent();
            potList_.selectRow(sel - 1);
        }
    };
    addAndMakeVisible(potUpBtn_);

    potDownBtn_.onClick = [this] {
        const int sel = potList_.getSelectedRow();
        if (sel >= 0 && sel < static_cast<int>(pots_.size()) - 1) {
            std::swap(pots_[static_cast<size_t>(sel)], pots_[static_cast<size_t>(sel + 1)]);
            potList_.updateContent();
            potList_.selectRow(sel + 1);
        }
    };
    addAndMakeVisible(potDownBtn_);

    // Switch Up/Down buttons
    switchUpBtn_.onClick = [this] {
        const int sel = switchList_.getSelectedRow();
        if (sel > 0 && sel < static_cast<int>(switches_.size())) {
            std::swap(switches_[static_cast<size_t>(sel)], switches_[static_cast<size_t>(sel - 1)]);
            switchList_.updateContent();
            switchList_.selectRow(sel - 1);
        }
    };
    addAndMakeVisible(switchUpBtn_);

    switchDownBtn_.onClick = [this] {
        const int sel = switchList_.getSelectedRow();
        if (sel >= 0 && sel < static_cast<int>(switches_.size()) - 1) {
            std::swap(switches_[static_cast<size_t>(sel)], switches_[static_cast<size_t>(sel + 1)]);
            switchList_.updateContent();
            switchList_.selectRow(sel + 1);
        }
    };
    addAndMakeVisible(switchDownBtn_);

    // Apply button
    applyBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF382914));
    applyBtn_.setColour(juce::TextButton::textColourOffId, glow::getColourAccent());
    applyBtn_.onClick = [this] {
        if (onApply) {
            std::vector<std::string> pOrder, sOrder;
            pOrder.reserve(pots_.size());
            for (const auto& p : pots_)
                pOrder.push_back(p.name);
            sOrder.reserve(switches_.size());
            for (const auto& s : switches_)
                sOrder.push_back(s.name);
            onApply(std::move(pOrder), std::move(sOrder));
        }
        if (auto* parent = getParentComponent())
            parent->removeChildComponent(this);
    };
    addAndMakeVisible(applyBtn_);

    // Reset button
    resetBtn_.onClick = [this, doc] {
        wdf::LiveWDFDocument copy = doc;
        copy.potOrder.clear();
        copy.switchOrder.clear();
        pots_.clear();
        for (const auto& g : wdf::potControlGroups(copy))
            pots_.push_back(makeItemFromGroup(g));
        switches_.clear();
        for (const auto& g : wdf::switchControlGroups(copy))
            switches_.push_back(makeItemFromGroup(g));
        potList_.updateContent();
        switchList_.updateContent();
    };
    addAndMakeVisible(resetBtn_);

    // Close button
    closeBtn_.onClick = [this] {
        if (auto* parent = getParentComponent())
            parent->removeChildComponent(this);
    };
    addAndMakeVisible(closeBtn_);
}

int PluginLayoutDialog::getNumRows() {
    return static_cast<int>(pots_.size());
}

void PluginLayoutDialog::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool sel) {
    if (row < 0 || row >= static_cast<int>(pots_.size())) return;
    if (sel) {
        g.setColour(glow::getColourAccent().withAlpha(0.20f));
        g.fillRect(0, 0, width, height);
        g.setColour(glow::getColourAccent());
        g.drawRect(0, 0, width, height, 1);
    }
    const auto& item = pots_[static_cast<size_t>(row)];
    g.setFont(Fonts::semiBold(13.0f));
    g.setColour(sel ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFFCBD5E1));
    const juce::String title = juce::String(row + 1) + ".  " + juce::String(item.name).toUpperCase();
    g.drawText(title, 10, 0, width - 110, height, juce::Justification::centredLeft);

    if (!item.memberInfo.empty()) {
        g.setFont(Fonts::monoSemiBold(11.0f));
        g.setColour(sel ? glow::getColourAccent() : juce::Colour(0xFF8B9BB4));
        g.drawText(item.memberInfo, width - 105, 0, 95, height, juce::Justification::centredRight);
    }
}

void PluginLayoutDialog::SwitchListModel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool sel) {
    if (row < 0 || row >= static_cast<int>(owner.switches_.size())) return;
    if (sel) {
        g.setColour(glow::getColourAccent().withAlpha(0.20f));
        g.fillRect(0, 0, width, height);
        g.setColour(glow::getColourAccent());
        g.drawRect(0, 0, width, height, 1);
    }
    const auto& item = owner.switches_[static_cast<size_t>(row)];
    g.setFont(Fonts::semiBold(13.0f));
    g.setColour(sel ? juce::Colour(0xFFFFFFFF) : juce::Colour(0xFFCBD5E1));
    const juce::String title = juce::String(row + 1) + ".  " + juce::String(item.name).toUpperCase();
    g.drawText(title, 10, 0, width - 110, height, juce::Justification::centredLeft);

    if (!item.memberInfo.empty()) {
        g.setFont(Fonts::monoSemiBold(11.0f));
        g.setColour(sel ? glow::getColourAccent() : juce::Colour(0xFF8B9BB4));
        g.drawText(item.memberInfo, width - 105, 0, 95, height, juce::Justification::centredRight);
    }
}

bool PluginLayoutDialog::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        if (auto* parent = getParentComponent())
            parent->removeChildComponent(this);
        return true;
    }
    return false;
}

void PluginLayoutDialog::mouseDown(const juce::MouseEvent& e) {
    if (!cardBounds_.contains(e.getPosition())) {
        if (auto* parent = getParentComponent())
            parent->removeChildComponent(this);
    }
}

void PluginLayoutDialog::paint(juce::Graphics& g) {
    // Backdrop dimming
    g.fillAll(juce::Colour(0xDD0B0D10));

    // Card background
    auto card = cardBounds_.toFloat();
    g.setColour(juce::Colour(0x90000000));
    g.fillRoundedRectangle(card.expanded(4.0f).translated(0.0f, 6.0f), 10.0f);

    g.setColour(juce::Colour(0xFF14171D));
    g.fillRoundedRectangle(card, 8.0f);
    g.setColour(juce::Colour(0xFF273140));
    g.drawRoundedRectangle(card, 8.0f, 1.2f);

    // Title & description
    const int pad = 24;
    g.setFont(Fonts::bold(18.0f));
    g.setColour(juce::Colour(0xFFFFFFFF));
    g.drawText("PLUGIN LAYOUT // CONTROL ORDER", cardBounds_.getX() + pad, cardBounds_.getY() + 18,
               cardBounds_.getWidth() - 2 * pad, 24, juce::Justification::left);

    g.setFont(Fonts::medium(12.5f));
    g.setColour(juce::Colour(0xFF8B9BB4));
    g.drawText("Reorder knobs and switches for the audio plugin. Parts with identical labels form one ganged group.",
               cardBounds_.getX() + pad, cardBounds_.getY() + 44,
               cardBounds_.getWidth() - 2 * pad, 18, juce::Justification::left);

    // Column Headers
    const int colW = (cardBounds_.getWidth() - 2 * pad - 20) / 2;
    const int colY = cardBounds_.getY() + 72;

    g.setFont(Fonts::semiBold(12.5f));
    g.setColour(glow::getColourAccent());
    g.drawText("KNOBS & POTENTIOMETERS", cardBounds_.getX() + pad, colY, colW, 18, juce::Justification::left);
    g.drawText("SWITCHES (SPDT)", cardBounds_.getX() + pad + colW + 20, colY, colW, 18, juce::Justification::left);
}

void PluginLayoutDialog::resized() {
    const int cardW = juce::jmin(640, getWidth() - 40);
    const int cardH = 470;
    cardBounds_ = juce::Rectangle<int>((getWidth() - cardW) / 2,
                                       (getHeight() - cardH) / 2,
                                       cardW, cardH);

    const int pad = 24;
    const int colW = (cardBounds_.getWidth() - 2 * pad - 20) / 2;
    const int listY = cardBounds_.getY() + 96;
    const int listH = 270;

    // Column 1 (Pots)
    potList_.setBounds(cardBounds_.getX() + pad, listY, colW - 70, listH);
    potUpBtn_.setBounds(cardBounds_.getX() + pad + colW - 65, listY + 80, 60, 32);
    potDownBtn_.setBounds(cardBounds_.getX() + pad + colW - 65, listY + 125, 60, 32);

    // Column 2 (Switches)
    const int col2X = cardBounds_.getX() + pad + colW + 20;
    switchList_.setBounds(col2X, listY, colW - 70, listH);
    switchUpBtn_.setBounds(col2X + colW - 65, listY + 80, 60, 32);
    switchDownBtn_.setBounds(col2X + colW - 65, listY + 125, 60, 32);

    // Bottom action buttons
    const int btnY = cardBounds_.getBottom() - 48;
    applyBtn_.setBounds(cardBounds_.getRight() - pad - 140, btnY, 140, 32);
    resetBtn_.setBounds(cardBounds_.getRight() - pad - 275, btnY, 125, 32);
    closeBtn_.setBounds(cardBounds_.getX() + pad, btnY, 90, 32);
}

}  // namespace livewdf::editor
