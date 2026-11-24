// The factory schematics are shipped inside both binaries. These tests exercise
// the shared installer directly, so a change to the archive layout or the
// version marker cannot silently leave the factory folder half-written.

#include <catch2.hpp>

#include "FactorySchematics.h"
#include "PresetLibraryPanel.h"

using livewdf::FactorySchematics;
using livewdf::editor::FactoryPresetStepper;
using livewdf::editor::PresetLibraryPanel;

namespace {

struct TempFolder {
    juce::File root;

    TempFolder() {
        root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("livewdf_factory_" +
                                 juce::Uuid().toString().replace("-", ""));
        root.createDirectory();
    }

    ~TempFolder() { root.deleteRecursively(); }
};

juce::File firstLivewdf(const juce::File& root) {
    for (const auto& entry :
         juce::RangedDirectoryIterator(root, true, "*.livewdf", juce::File::findFiles))
        return entry.getFile();
    return {};
}

int rowWithText(const PresetLibraryPanel& panel, const juce::String& text) {
    for (int i = 0; i < panel.getRowCount(); ++i)
        if (panel.getRowText(i) == text)
            return i;
    return -1;
}

void selectFilter(PresetLibraryPanel& panel, const juce::String& title, const juce::String& item) {
    for (auto* child : panel.getChildren()) {
        auto* box = dynamic_cast<juce::ComboBox*>(child);
        if (box == nullptr || box->getTitle() != title)
            continue;
        for (int i = 0; i < box->getNumItems(); ++i) {
            if (box->getItemText(i) == item) {
                box->setSelectedItemIndex(i, juce::sendNotificationSync);
                return;
            }
        }
    }
    FAIL("Could not find filter " + title.toStdString() + " = " + item.toStdString());
}

void selectCollection(PresetLibraryPanel& panel, const juce::String& label) {
    for (auto* child : panel.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child);
            button != nullptr && button->getButtonText() == label && button->onClick) {
            button->onClick();
            CHECK(button->getToggleState());
            return;
        }
    FAIL("Could not find collection " + label.toStdString());
}

juce::TextButton* buttonWithTitle(juce::Component& parent, const juce::String& title) {
    for (auto* child : parent.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child);
            button != nullptr && button->getTitle() == title)
            return button;
    return nullptr;
}

}  // namespace

TEST_CASE("Factory schematics install into a folder and are idempotent") {
    TempFolder temp;
    const auto factoryFolder = temp.root.getChildFile("Factory");
    REQUIRE(factoryFolder.createDirectory().wasOk());
    const auto firstRunFile = factoryFolder.getChildFile("Already Here.livewdf");
    REQUIRE(firstRunFile.replaceWithText("{}"));

    const auto installed = FactorySchematics::ensureInstalledAt(factoryFolder);
    INFO(installed.getErrorMessage().toStdString());
    REQUIRE(installed.wasOk());
    CHECK(firstRunFile.existsAsFile());

    CHECK(factoryFolder.getChildFile("amps").isDirectory());
    CHECK(factoryFolder.getChildFile("pedals_fx").isDirectory());
    CHECK(factoryFolder.getChildFile(".factory-version").existsAsFile());
    CHECK(factoryFolder.getChildFile(".factory-hidden").existsAsFile());

    const auto userPreset = temp.root.getChildFile("User Preset.livewdf");
    REQUIRE(userPreset.replaceWithText("{}"));

    const auto sample = firstLivewdf(factoryFolder.getChildFile("amps"));
    REQUIRE(sample.existsAsFile());
    const auto before = sample.getLastModificationTime();

    // A matching marker and intact files must not touch anything: factory
    // files are user-visible, and rewriting them on every launch would make an
    // edited copy look like it had been saved when it had not.
    REQUIRE(FactorySchematics::ensureInstalledAt(factoryFolder).wasOk());
    CHECK(sample.getLastModificationTime() == before);
    CHECK(userPreset.existsAsFile());

    // A missing factory file is repaired even when the marker matches.
    REQUIRE(sample.deleteFile());
    REQUIRE(FactorySchematics::ensureInstalledAt(factoryFolder).wasOk());
    CHECK(sample.existsAsFile());
    CHECK(userPreset.existsAsFile());

    // A stale marker refreshes the factory set without deleting user presets
    // beside the Factory folder.
    const auto marker = factoryFolder.getChildFile(".factory-version");
    REQUIRE(marker.replaceWithText("stale"));
    const auto stale = factoryFolder.getChildFile("amps").getChildFile("Old Factory.livewdf");
    REQUIRE(stale.replaceWithText("{}"));
    REQUIRE(FactorySchematics::ensureInstalledAt(factoryFolder).wasOk());
    CHECK(marker.loadFileAsString().trim() != "stale");
    CHECK(userPreset.existsAsFile());
    CHECK(!stale.existsAsFile());
}

TEST_CASE("Factory circuits get their own preset-library section") {
    TempFolder temp;

    const auto factoryAmp = temp.root.getChildFile("Factory").getChildFile("amps");
    REQUIRE(factoryAmp.createDirectory().wasOk());
    REQUIRE(factoryAmp.getChildFile("Factory Amp.livewdf").replaceWithText("{}"));
    REQUIRE(temp.root.getChildFile("User Preset.livewdf").replaceWithText("{}"));

    PresetLibraryPanel panel;
    panel.setFolder(temp.root);

    CHECK(rowWithText(panel, "AMPS") >= 0);
    CHECK(rowWithText(panel, "MY PRESETS") >= 0);
    CHECK(rowWithText(panel, "Factory Amp") >= 0);
    CHECK(rowWithText(panel, "User Preset") >= 0);
}

TEST_CASE("Factory browsing filters a separate user folder by collection and effect") {
    TempFolder temp;
    const auto factory = temp.root.getChildFile("Factory");
    const auto user = temp.root.getChildFile("Custom location");
    REQUIRE(user.createDirectory().wasOk());
    REQUIRE(user.getChildFile("My recording.livewdf").replaceWithText("{}"));
    for (const auto* file : {"pedals_fx/Boss CE-2 Chorus.livewdf",
                             "pedals_fx/MXR Carbon Copy Analog Delay.livewdf",
                             "studio/eq/EQ 10.livewdf", "studio/eq/EQ 2.livewdf",
                             "studio/dynamics/Compressor.livewdf"}) {
        const auto preset = factory.getChildFile(file);
        REQUIRE(preset.getParentDirectory().createDirectory().wasOk());
        REQUIRE(preset.replaceWithText("{}"));
    }

    PresetLibraryPanel editor;
    editor.setFactoryFolder(factory);
    editor.setFolder(user);
    const int featured = rowWithText(editor, "FEATURED");
    REQUIRE(featured >= 0);
    CHECK(rowWithText(editor, "Boss CE-2 Chorus") > featured);
    CHECK(rowWithText(editor, "My recording") > featured);
    CHECK(rowWithText(editor, "EQ 2") < rowWithText(editor, "EQ 10"));

    selectCollection(editor, "Pedals");
    selectFilter(editor, "Effect type", "Delay");
    REQUIRE(editor.getRowCount() == 1);
    CHECK(editor.getRowText(0) == "MXR Carbon Copy Analog Delay");

    selectCollection(editor, "Studio");
    selectFilter(editor, "Effect type", "EQ");
    REQUIRE(editor.getRowCount() == 2);
    CHECK(editor.getRowText(0) == "EQ 2");
    CHECK(editor.getRowText(1) == "EQ 10");

    selectCollection(editor, "All");
    selectFilter(editor, "Effect type", "All types");
    editor.setSearchText("studio eq 10");
    REQUIRE(editor.getRowCount() == 1);
    CHECK(editor.getRowFile(0) == factory.getChildFile("studio/eq/EQ 10.livewdf"));

    PresetLibraryPanel plugin(PresetLibraryPanel::Mode::Factory);
    plugin.setFolder(user);
    plugin.setFactoryFolder(factory);
    CHECK(rowWithText(plugin, "New empty sheet") == -1);
    CHECK(rowWithText(plugin, "Save current as preset...") == -1);
    CHECK(rowWithText(plugin, "My recording") == -1);
    CHECK(rowWithText(plugin, "Open factory folder...") >= 0);
    selectCollection(plugin, "Featured");
    REQUIRE(plugin.getRowCount() == 1);
    CHECK(plugin.getRowText(0) == "Boss CE-2 Chorus");
}

TEST_CASE("Factory preset chevrons follow the visible factory rows") {
    TempFolder temp;
    const auto factory = temp.root.getChildFile("Factory");
    const auto first = factory.getChildFile("studio/eq/EQ 2.livewdf");
    const auto middle = factory.getChildFile("studio/eq/EQ 10.livewdf");
    const auto last = factory.getChildFile("studio/eq/EQ 11.livewdf");
    const auto hidden = factory.getChildFile("studio/eq/EQ 9.livewdf");
    const auto preview = factory.getChildFile("studio/eq/EQ 7.livewdf");
    const auto other = factory.getChildFile("amps/Amp.livewdf");
    const auto custom = temp.root.getChildFile("User EQ.livewdf");
    for (const auto& file : {first, middle, last, hidden, preview, other, custom}) {
        REQUIRE(file.getParentDirectory().createDirectory().wasOk());
        REQUIRE(file.replaceWithText("{}"));
    }
    REQUIRE(factory.getChildFile(".factory-hidden").replaceWithText(
        "# Hidden factory presets\nstudio/eq/EQ 9.livewdf\n[compiled-preview-48k-1x]\nstudio/eq/EQ 7.livewdf\n"));

    PresetLibraryPanel browser;
    browser.setFolder(temp.root);
    CHECK(browser.visibleFactoryOrder().size() == 4);
    CHECK(rowWithText(browser, "EQ 9") == -1);
    CHECK(hidden.existsAsFile());
    selectCollection(browser, "Studio");
    selectFilter(browser, "Effect type", "EQ");
    browser.setSearchText("eq");
    const auto order = browser.visibleFactoryOrder();
    CHECK(order == FactoryPresetStepper::Order{first, middle, last});

    PresetLibraryPanel reopened(PresetLibraryPanel::Mode::Factory);
    reopened.setFactoryFolder(factory);
    reopened.setView(browser.view());
    CHECK(reopened.visibleFactoryOrder() == order);
    CHECK(reopened.view().effect == "EQ");
    CHECK(reopened.view().search == "eq");

    juce::Component parent;
    juce::Label label;
    parent.addAndMakeVisible(label);
    FactoryPresetStepper stepper(parent, label);
    stepper.setBounds({0, 0, 260, 24});
    stepper.setFactoryFolder(factory);
    auto* previous = buttonWithTitle(parent, "Previous factory preset");
    auto* next = buttonWithTitle(parent, "Next factory preset");
    REQUIRE(previous != nullptr);
    REQUIRE(next != nullptr);

    juce::File chosen;
    stepper.onPresetChosen = [&](const juce::File& file) {
        chosen = file;
        stepper.setCurrentFile(file);
    };
    stepper.setCurrentFile(first);
    REQUIRE(previous->isEnabled());
    previous->onClick();
    CHECK(chosen == other);
    stepper.setCurrentFile(first);
    next->onClick();
    CHECK(chosen == middle);

    stepper.setBrowseOrder(order);
    stepper.setCurrentFile(first);
    stepper.setName("EQ 2");
    CHECK(previous->isVisible());
    CHECK(next->getBounds().getRight() < parent.getX() + 260);
    CHECK(previous->getBounds().getRight() <= label.getBounds().getX());
    CHECK(next->getBounds().getX() >= label.getBounds().getRight());
    CHECK_FALSE(previous->isEnabled());
    CHECK(next->isEnabled());
    next->onClick();
    CHECK(chosen == middle);
    next->onClick();
    CHECK(chosen == last);
    CHECK_FALSE(next->isEnabled());
    CHECK(previous->isEnabled());
    REQUIRE(middle.deleteFile());
    previous->onClick();
    CHECK(chosen == first);

    stepper.setCurrentFile(hidden);
    CHECK_FALSE(previous->isVisible());
    CHECK_FALSE(next->isVisible());

    stepper.setCurrentFile(custom);
    CHECK_FALSE(previous->isVisible());
    CHECK_FALSE(next->isVisible());
    CHECK(label.getBounds().getWidth() == 260);

    browser.setFactoryFolder(factory, true);
    stepper.setFactoryFolder(factory, true);
    REQUIRE(rowWithText(browser, "EQ 7") >= 0);
    CHECK(rowWithText(browser, "EQ 9") == -1);
    stepper.setBrowseOrder(browser.visibleFactoryOrder());
    stepper.setCurrentFile(first);
    next->onClick();
    CHECK(chosen == preview);

    browser.setFactoryFolder(factory);
    stepper.setFactoryFolder(factory);
    CHECK(rowWithText(browser, "EQ 7") == -1);
    CHECK_FALSE(next->isVisible());
    stepper.setCurrentFile(first);
    next->onClick();
    CHECK(chosen == last);
}

TEST_CASE("Preset search ranks exact names before broader matches") {
    TempFolder temp;
    const auto user = temp.root.getChildFile("User");
    const auto factory = temp.root.getChildFile("Factory");
    for (const auto* file : {"pedals_fx/A Vintage Fuzz Face.livewdf",
                             "pedals_fx/Face Silicon Fuzz.livewdf",
                             "pedals_fx/Fuzz Face Bright.livewdf",
                             "pedals_fx/Fuzz Face.livewdf"}) {
        const auto preset = factory.getChildFile(file);
        REQUIRE(preset.getParentDirectory().createDirectory().wasOk());
        REQUIRE(preset.replaceWithText("{}"));
    }
    const auto custom = user.getChildFile("Face collection/Fuzz Companion.livewdf");
    REQUIRE(custom.getParentDirectory().createDirectory().wasOk());
    REQUIRE(custom.replaceWithText("{}"));

    PresetLibraryPanel browser;
    browser.setFactoryFolder(factory);
    browser.setFolder(user);
    browser.setSearchText("fuZZ FACE");

    REQUIRE(browser.getRowCount() == 5);
    CHECK(browser.getRowText(0) == "Fuzz Face");
    CHECK(browser.getRowText(1) == "Fuzz Face Bright");
    CHECK(browser.getRowText(2) == "A Vintage Fuzz Face");
    CHECK(browser.getRowText(3) == "Face Silicon Fuzz");
    CHECK(browser.getRowText(4) == "Fuzz Companion");
    CHECK(browser.visibleFactoryOrder().front() == factory.getChildFile("pedals_fx/Fuzz Face.livewdf"));
}
