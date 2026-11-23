// The factory schematics are shipped inside both binaries. These tests exercise
// the shared installer directly, so a change to the archive layout or the
// version marker cannot silently leave the factory folder half-written.

#include <catch2.hpp>

#include "FactorySchematics.h"
#include "PresetLibraryPanel.h"

using livewdf::FactorySchematics;
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
