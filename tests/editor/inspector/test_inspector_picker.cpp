// The inspector's pickers: the variant list, and the crash that was in it (U4.6).
//
// **The crash.** Choosing a device (a triode, a pentode, a diode) and then
// clicking the part on the sheet crashed the editor. The chain was:
//
//   1. the inspector builds a combo box for the part's model;
//   2. `setSelectedId` -- which **notifies by default**, and asynchronously --
//      sets the box's opening position, so a callback is *posted*;
//   3. the panel is rebuilt (any selection change does that), the combo box is
//      destroyed, and the posted callback fires later against a dead widget.
//
// The fix is in two layers, and both are here rather than only in the code:
// `setSelectedId` is called with `dontSendNotification`, because the opening
// position is not a *change* -- nobody chose it -- and the host notification is
// deferred, because the host's reaction to a change rebuilds the very widget
// whose callback is running.
//
// **What this test holds.** That clicking a part which has a model does not
// rewrite that model (the symptom of the first half of the bug, and a *silent*
// one: the circuit would change under the user's hands), and that changing the
// model through the picker does write it, and does not take the panel down with
// it (the second half).

#include "EditorLookAndFeel.h"
#include "ElementAccess.h"
#include "InspectorPanel.h"
#include "format/PartLibrary.h"
#include "format/PartModels.h"
#include "devices/MosfetPresets.h"

#include <catch2.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace livewdf::editor;

namespace {

/// A document with one part of `type`, carrying whatever model the palette
/// gives it.
wdf::LiveWDFDocument documentWith(const std::string& type) {
    wdf::LiveWDFDocument doc;
    doc.name = "Picker Test";
    doc.inputNode = "in";
    doc.outputNode = "out";

    const auto comp = makeComponent(type, {}, 4.0f, 4.0f);
    doc.components.push_back(comp);
    return doc;
}

/// Walk every component below `root`. The inspector's rows live inside a
/// viewport's content component now, so "find it in the panel" has to mean the
/// whole tree rather than the immediate children.
void visitDescendants(juce::Component& root,
                      const std::function<void(juce::Component&)>& fn) {
    for (auto* child : root.getChildren()) {
        fn(*child);
        visitDescendants(*child, fn);
    }
}

/// The combo box the panel built for a property, or nullptr.
///
/// The panel's widgets are its own business, so the test finds them the way
/// anything else would: by looking at the tree.
juce::ComboBox* findPicker(juce::Component& panel) {
    juce::ComboBox* found = nullptr;
    visitDescendants(panel, [&](juce::Component& child) {
        if (found == nullptr)
            if (auto* box = dynamic_cast<juce::ComboBox*>(&child))
                found = box;
    });
    return found;
}

/// The caption a row was built with, or nullptr.
juce::Label* findCaption(juce::Component& panel, const juce::String& text) {
    juce::Label* found = nullptr;
    visitDescendants(panel, [&](juce::Component& child) {
        if (found == nullptr)
            if (auto* label = dynamic_cast<juce::Label*>(&child))
                if (label->getText() == text)
                    found = label;
    });
    return found;
}

/// The editor in the row whose caption is `text`. The panel lays a row out as
/// caption at y and its control immediately under it, so the pairing is
/// structural rather than a guess at child order. The offset is read from the
/// caption itself: spacing is a theme decision, and a test that hard-codes it
/// fails when the inspector is restyled even though the row is still one row.
juce::TextEditor* findEditorBelowCaption(juce::Component& panel, const juce::String& text) {
    auto* caption = findCaption(panel, text);
    if (caption == nullptr)
        return nullptr;

    auto* parent = caption->getParentComponent();
    if (parent == nullptr)
        return nullptr;

    const int controlY = caption->getY() + caption->getHeight();

    for (auto* child : parent->getChildren())
        if (auto* editor = dynamic_cast<juce::TextEditor*>(child))
            if (editor->getY() == controlY)
                return editor;
    return nullptr;
}

/// The combo box in the row whose caption is `text`.
juce::ComboBox* findPickerBelowCaption(juce::Component& panel,
                                       const juce::String& text) {
    auto* caption = findCaption(panel, text);
    if (caption == nullptr)
        return nullptr;

    auto* parent = caption->getParentComponent();
    if (parent == nullptr)
        return nullptr;

    const int controlY = caption->getY() + caption->getHeight();

    for (auto* child : parent->getChildren())
        if (auto* box = dynamic_cast<juce::ComboBox*>(child))
            if (box->getY() == controlY)
                return box;
    return nullptr;
}

/// The read-only value in the row whose caption is `text`. Info rows use
/// Labels, not TextEditors, so they need their own structural lookup.
juce::Label* findInfoBelowCaption(juce::Component& panel, const juce::String& text) {
    auto* caption = findCaption(panel, text);
    if (caption == nullptr)
        return nullptr;

    auto* parent = caption->getParentComponent();
    if (parent == nullptr)
        return nullptr;

    const int controlY = caption->getY() + caption->getHeight();

    for (auto* child : parent->getChildren())
        if (auto* label = dynamic_cast<juce::Label*>(child))
            if (label != caption && label->getY() == controlY)
                return label;
    return nullptr;
}

/// The viewport that makes the inspector's rows reachable.
juce::Viewport* findViewport(juce::Component& panel) {
    juce::Viewport* found = nullptr;
    visitDescendants(panel, [&](juce::Component& child) {
        if (found == nullptr)
            if (auto* viewport = dynamic_cast<juce::Viewport*>(&child))
                found = viewport;
    });
    return found;
}

}  // namespace

TEST_CASE("Clicking a part with a model does not rewrite the model") {
    // The symptom of the bug that crashed the editor: the picker's *opening*
    // position was set with a notification, so merely selecting a triode wrote
    // its model back. Silent, and it changed the circuit under the user's hands.
    auto doc = documentWith("triode");
    const auto before = modelOf(doc.components.front());
    REQUIRE_FALSE(before.empty());

    InspectorPanel panel;
    panel.setTarget(&doc, doc.components.front().id);

    // **Deliver the notification that the picker posted.** `setSelectedId`
    // notifies asynchronously, so the callback is sitting in the message queue
    // waiting; `handleAsyncUpdate` is what the queue ends up calling. Without
    // this the test would pass even with the bug, because nothing had delivered
    // the message yet.
    if (auto* box = findPicker(panel))
        box->handleAsyncUpdate();

    CHECK(modelOf(doc.components.front()) == before);
}

TEST_CASE("A rebuild that is re-entered does not corrupt the panel") {
    // **The crash.** A rebuild can be re-entered: a widget's callback asks for
    // one, and the host's reaction to that callback re-targets the inspector
    // while the outer rebuild is still running. When the widgets were built
    // straight into the member arrays, the inner rebuild cleared them
    // underneath the outer one, and the layout code then indexed past the end
    // of them.
    //
    // This drives exactly that: the panel's host reacts to a change by
    // re-targeting the panel, and the picker's change is delivered while the
    // panel is mid-rebuild.
    auto doc = documentWith("pot");

    InspectorPanel panel;

    // The host's behaviour: react to a change by re-targeting the inspector.
    panel.onChanged = [&] { panel.setTarget(&doc, doc.components.front().id); };

    for (int i = 0; i < 50; ++i) {
        panel.setTarget(&doc, doc.components.front().id);

        // Deliver the picker's opening notification, which is what re-enters.
        if (auto* box = findPicker(panel))
            box->handleAsyncUpdate();
    }

    // The panel is still coherent: it has a picker, and the taper is the one
    // the part carries.
    auto* box = findPicker(panel);
    REQUIRE(box != nullptr);

    const auto taper = doc.components.front().properties.at("taper");
    CHECK(box->getText() == juce::String(taper));
}

TEST_CASE("A part with a model gets a picker, and it shows the model") {
    auto doc = documentWith("pentode");

    InspectorPanel panel;
    panel.setTarget(&doc, doc.components.front().id);

    auto* box = findPicker(panel);
    REQUIRE(box != nullptr);

    // The picker offers every pentode the engine carries, and the one the part
    // has is the one that is selected.
    const auto choices = modelChoices(ChoiceKind::PentodeModel);
    REQUIRE(!choices.empty());
    CHECK(box->getNumItems() == static_cast<int>(choices.size()));

    const std::string model = modelOf(doc.components.front());
    CHECK(box->getText() == juce::String(model));
}

TEST_CASE("Choosing a model in the picker writes it, and the panel survives") {
    // The whole chain, driven the way the user drives it: pick a different
    // device, and let the host react the way the host reacts -- by rebuilding
    // the panel, which destroys the widget whose callback is running.
    auto doc = documentWith("triode");

    InspectorPanel panel;

    // The host's behaviour, which is what made the crash: reacting to a change
    // re-reads the canvas, which re-targets the inspector, which rebuilds it.
    panel.onChanged = [&] { panel.setTarget(&doc, doc.components.front().id); };

    panel.setTarget(&doc, doc.components.front().id);

    auto* box = findPicker(panel);
    REQUIRE(box != nullptr);

    // Choose a different device, and fire the change the way the widget does.
    const std::string before = modelOf(doc.components.front());
    const auto choices = modelChoices(ChoiceKind::TriodeModel);
    const auto other = std::find_if(choices.begin(), choices.end(),
                                    [&](const std::string& c) { return c != before; });
    REQUIRE(other != choices.end());

    const int index = static_cast<int>(std::distance(choices.begin(), other));
    REQUIRE(index >= 0);

    box->setSelectedId(index + 1, juce::dontSendNotification);
    REQUIRE(box->getText() == juce::String(*other));

    // The callback, invoked the way a click invokes it. This is where the old
    // code destroyed the widget it was running in.
    if (box->onChange)
        box->onChange();

    // The model is written -- the picker works.
    CHECK(modelOf(doc.components.front()) == *other);
}

TEST_CASE("The inspector survives the document being replaced under it") {
    // The canvas replaces the document's contents whenever the sheet is edited
    // -- that is how the model and the file stay in step. So the panel's view of
    // the document can be pulled out from under it at any moment, and a panel
    // that holds a *reference* into it is holding a reference to freed memory.
    //
    // The host here does exactly what the real one does: reacting to a change
    // re-reads the canvas, and re-reading replaces the document.
    auto doc = documentWith("pot");

    InspectorPanel panel;
    panel.onChanged = [&] {
        // The document is rebuilt from scratch -- new components, new storage.
        doc.components.clear();
        doc.components.push_back(makeComponent("pot", {}, 4.0f, 4.0f));
    };

    for (int i = 0; i < 50; ++i) {
        panel.setTarget(&doc, doc.components.front().id);
        if (auto* box = findPicker(panel))
            box->handleAsyncUpdate();
    }

    // The panel is still coherent.
    auto* box = findPicker(panel);
    REQUIRE(box != nullptr);
    CHECK(box->getText() == juce::String(doc.components.front().properties.at("taper")));
}

TEST_CASE("A source is DC or AC, and only that mode's fields are offered") {
    // The complaint was that a source had its DC and its AC in the same place:
    // two boxes, side by side, with no saying which one was meant. The mode is a
    // picker, and the inspector shows only the fields that belong to it.
    auto doc = documentWith("src");
    auto& src = doc.components.front();

    // A fresh source is a DC source, and says so.
    CHECK(src.properties.at("mode") == "DC");
    CHECK(wdf::sourceModeOf(src) == wdf::SourceMode::DC);

    // DC mode: the level, and nothing else.
    const auto dcKeys = wdf::sourceValueKeys(wdf::SourceMode::DC);
    CHECK(dcKeys.size() == 1);
    CHECK(dcKeys.front() == "dc");

    // AC mode: the amplitude and the frequency.
    const auto acKeys = wdf::sourceValueKeys(wdf::SourceMode::AC);
    CHECK(acKeys.size() == 2);
    CHECK(std::find(acKeys.begin(), acKeys.end(), "ac") != acKeys.end());
    CHECK(std::find(acKeys.begin(), acKeys.end(), "freq") != acKeys.end());

    // And the mode itself is a choice, offered to a source and to nothing else.
    const auto choices = modelChoices(ChoiceKind::SourceMode);
    REQUIRE(choices.size() == 2);
    CHECK(choices[0] == "DC");
    CHECK(choices[1] == "AC");
    CHECK(choiceKindFor("src", "mode") == ChoiceKind::SourceMode);
    CHECK(choiceKindFor("res", "mode") == ChoiceKind::None);
}

TEST_CASE("A source in DC mode carries no AC amplitude into the circuit") {
    // The mode is not only a label in the inspector: in DC mode the amplitude
    // is not in the compiled circuit at all, so the two can never both apply.
    auto doc = documentWith("src");
    doc.components.front().properties["mode"] = "DC";
    doc.components.front().values["ac"] = 5.0;

    const auto circuit = doc.toCircuit();
    REQUIRE(circuit.src.size() == 1);
    CHECK(circuit.src[0].ac == 0.0);

    // In AC mode it is there.
    doc.components.front().properties["mode"] = "AC";
    const auto acCircuit = doc.toCircuit();
    REQUIRE(acCircuit.src.size() == 1);
    CHECK(acCircuit.src[0].ac == 5.0);
}

TEST_CASE("The widget whose callback is running is still alive when it returns") {
    // **The crash, stated as something a test can check.**
    //
    // The crash was: clicking a part with a picker -- a triode, a pentode, a
    // potentiometer, the parts that *have* a picker -- could take the editor
    // down. The mechanism is a use-after-free: a ComboBox's `onChange` is a
    // `std::function` *owned by the ComboBox*, so if the panel rebuilds while
    // that callback is running, the box is destroyed and so is the function
    // being invoked.
    //
    // A use-after-free does not reliably crash -- it reads whatever the freed
    // memory happens to hold -- so the check cannot be "does it crash". It has
    // to be about the *structure*: **the panel must not have rebuilt while the
    // callback was running.** If it had, the widget would be gone.
    auto doc = documentWith("triode");

    InspectorPanel panel;

    // The host's reaction, which is what rebuilds the panel.
    panel.onChanged = [&] { panel.setTarget(&doc, doc.components.front().id); };

    panel.setTarget(&doc, doc.components.front().id);

    auto* box = findPicker(panel);
    REQUIRE(box != nullptr);

    // Change the picker's value, the way the user does. `sendNotificationSync`
    // is the synchronous one: the callback runs before `setSelectedId` returns,
    // which is the case the crash lived in.
    const std::string before = modelOf(doc.components.front());
    const auto choices = modelChoices(ChoiceKind::TriodeModel);
    const auto other = std::find_if(choices.begin(), choices.end(),
                                    [&](const std::string& c) { return c != before; });
    REQUIRE(other != choices.end());

    box->setSelectedId(static_cast<int>(std::distance(choices.begin(), other)) + 1,
                       juce::sendNotificationSync);

    // The change was delivered, so the callback ran.
    CHECK(modelOf(doc.components.front()) == *other);

    // **And the widget it ran in was not destroyed underneath it.** The panel
    // rebuilds when the host reacts, and the host's reaction is deferred -- so
    // the rebuild has not happened yet, and the box is still there.
    //
    // With a synchronous reaction this is false: the panel rebuilt during the
    // callback, the box was deleted, and the `std::function` that was running
    // was deleted with it. That is the crash, and this is the check for it.
    bool alive = false;
    visitDescendants(panel, [&](juce::Component& child) {
        if (&child == box)
            alive = true;
    });
    CHECK(alive);

    // And the picker now shows the new model.
    CHECK(box->getText() == juce::String(*other));
}

TEST_CASE("A part with no variants gets no picker") {
    // A resistor has no model, so it must not get a control that offers one. A
    // picker with an empty list is a control that shows nothing, which is worse
    // than no control.
    auto doc = documentWith("res");

    InspectorPanel panel;
    panel.setTarget(&doc, doc.components.front().id);

    CHECK(findPicker(panel) == nullptr);
}

TEST_CASE("A BJT inspector offers the Ebers-Moll card values") {
    // The picker changes the name and reloads the five parameters the active
    // reduced-Ebers-Moll law reads: Is/Bf/Br/Nf/Nr. The Gummel-Poon fields
    // (Vaf/Var, Ikf/Ikr, Rb/Rc/Re) are backend material -- the card carries them,
    // a legacy file may carry them too -- but the engine never reads them, so
    // they are not offered as controls and a part placed here does not store
    // them at all.
    auto doc = documentWith("bjt");
    const std::string id = doc.components.front().id;

    InspectorPanel panel;
    panel.setSize(320, 520);
    panel.setTarget(&doc, id);

    auto* type = findInfoBelowCaption(panel, "Type:");
    REQUIRE(type != nullptr);
    CHECK(type->getText() == "NPN");

    const char* labels[] = {"Is:", "Bf:", "Br:", "Nf:", "Nr:"};
    const char* keys[] = {"is", "bf", "br", "nf", "nr"};
    for (const char* key : labels) {
        INFO("value key: " << key);
        auto* ed = findEditorBelowCaption(panel, key);
        REQUIRE(ed != nullptr);
        CHECK(ed->getText().isNotEmpty());
    }
    for (const char* key : keys) {
        INFO("stored key: " << key);
        CHECK(doc.components.front().values.count(key) == 1);
    }

    // The Gummel-Poon rows are not offered, and a freshly placed part carries
    // none of those numbers.
    for (const char* key : {"Vaf:", "Var:", "Ikf:", "Ikr:", "Rc:", "Rb:", "Re:"}) {
        INFO("hidden label: " << key);
        CHECK(findEditorBelowCaption(panel, key) == nullptr);
    }
    for (const char* key : {"vaf", "var", "ikf", "ikr", "rc", "rb", "re"}) {
        INFO("unstowed key: " << key);
        CHECK(doc.components.front().values.count(key) == 0);
    }

    // Choose another NPN card the way the picker does, then let the host
    // re-target the inspector as it does after a change.
    const auto choices = modelChoices(ChoiceKind::BjtModel);
    auto* picker = findPicker(panel);
    REQUIRE(picker != nullptr);
    const std::string before = modelOf(doc.components.front());
    const auto other = std::find_if(choices.begin(), choices.end(),
                                    [&](const std::string& c) { return c != before; });
    REQUIRE(other != choices.end());

    picker->setSelectedId(static_cast<int>(std::distance(choices.begin(), other)) + 1,
                          juce::dontSendNotification);
    if (picker->onChange)
        picker->onChange();

    CHECK(modelOf(doc.components.front()) == *other);

    panel.setTarget(&doc, id);
    auto* newType = findInfoBelowCaption(panel, "Type:");
    REQUIRE(newType != nullptr);
    CHECK(newType->getText() == "NPN");

    // The new card's five EM values are loaded and shown; the GP rows are still
    // not offered, so changing cards cannot add a control that does nothing.
    for (const char* key : labels) {
        INFO("value key after model change: " << key);
        auto* ed = findEditorBelowCaption(panel, key);
        REQUIRE(ed != nullptr);
        CHECK(ed->getText().isNotEmpty());
    }
    for (const char* key : keys) {
        INFO("stored after model change: " << key);
        CHECK(doc.components.front().values.count(key) == 1);
    }
    for (const char* key : {"Vaf:", "Var:", "Ikf:", "Ikr:", "Rc:", "Rb:", "Re:"}) {
        INFO("hidden label after model change: " << key);
        CHECK(findEditorBelowCaption(panel, key) == nullptr);
    }
    for (const char* key : {"vaf", "var", "ikf", "ikr", "rc", "rb", "re"}) {
        INFO("unstowed after model change: " << key);
        CHECK(doc.components.front().values.count(key) == 0);
    }
}

TEST_CASE("Choosing a PNP card changes the Type row and parameter values") {
    auto doc = documentWith("bjt");
    const std::string id = doc.components.front().id;

    InspectorPanel panel;
    panel.setSize(320, 520);
    panel.setTarget(&doc, id);

    const auto choices = modelChoices(ChoiceKind::BjtModel);
    const auto pnp = std::find(choices.begin(), choices.end(), std::string("2N3906"));
    REQUIRE(pnp != choices.end());
    auto* picker = findPickerBelowCaption(panel, "Model:");
    REQUIRE(picker != nullptr);

    // Capture NPN parameter values before switch.
    std::map<std::string, juce::String> npnValues;
    for (const char* key : {"Is:", "Bf:", "Br:", "Nf:", "Nr:"}) {
        auto* ed = findEditorBelowCaption(panel, key);
        REQUIRE(ed != nullptr);
        npnValues[key] = ed->getText();
    }

    picker->setSelectedId(static_cast<int>(std::distance(choices.begin(), pnp)) + 1,
                          juce::dontSendNotification);
    if (picker->onChange)
        picker->onChange();

    CHECK(modelOf(doc.components.front()) == "2N3906");
    panel.setTarget(&doc, id);
    auto* type = findInfoBelowCaption(panel, "Type:");
    REQUIRE(type != nullptr);
    CHECK(type->getText() == "PNP");

    // PNP parameters should differ from NPN values -- at minimum Bf differs
    // (300 vs 200). Most other parameters happen to be the same for 2N3904/2N3906.
    bool anyDiff = false;
    for (const char* key : {"Is:", "Bf:", "Br:", "Nf:", "Nr:"}) {
        auto* ed = findEditorBelowCaption(panel, key);
        REQUIRE(ed != nullptr);
        CHECK(ed->getText().isNotEmpty());
        if (ed->getText() != npnValues[key])
            anyDiff = true;
    }
    CHECK(anyDiff);  // At least one parameter changed (Bf: 300 -> 200)

    // The Gummel-Poon fields are still not offered after the switch, in either
    // the panel or the document: the card keeps them as backend data, the part
    // does not carry them.
    for (const char* key : {"Vaf:", "Var:", "Ikf:", "Ikr:", "Rc:", "Rb:", "Re:"}) {
        INFO("hidden after PNP switch: " << key);
        CHECK(findEditorBelowCaption(panel, key) == nullptr);
    }
    for (const char* key : {"vaf", "var", "ikf", "ikr", "rc", "rb", "re"}) {
        INFO("unstowed after PNP switch: " << key);
        CHECK(doc.components.front().values.count(key) == 0);
    }

    // Switching back is a real polarity change, not a one-way label.
    const auto npn = std::find(choices.begin(), choices.end(), std::string("2N3904"));
    REQUIRE(npn != choices.end());
    auto* back = findPickerBelowCaption(panel, "Model:");
    REQUIRE(back != nullptr);
    back->setSelectedId(static_cast<int>(std::distance(choices.begin(), npn)) + 1,
                        juce::dontSendNotification);
    if (back->onChange)
        back->onChange();
    panel.setTarget(&doc, id);
    CHECK(findInfoBelowCaption(panel, "Type:")->getText() == "NPN");
}

TEST_CASE("The MOSFET inspector selects a preset, its polarity and its values") {
    auto doc = documentWith("mosfet");
    const std::string id = doc.components.front().id;

    InspectorPanel panel;
    panel.setSize(360, 560);
    panel.setTarget(&doc, id);

    CHECK(findCaption(panel, "Preset:") != nullptr);
    CHECK(findCaption(panel, "Polarity:") != nullptr);
    CHECK(findInfoBelowCaption(panel, "Type:")->getText() == "N-channel");
    auto* presetPicker = findPickerBelowCaption(panel, "Preset:");
    REQUIRE(presetPicker != nullptr);
    CHECK(presetPicker->getText() == "2N7000");
    auto* polarityPicker = findPickerBelowCaption(panel, "Polarity:");
    REQUIRE(polarityPicker != nullptr);
    CHECK(polarityPicker->getText() == "N-channel");

    const auto presets = modelChoices(ChoiceKind::MosfetPreset);
    const auto pPreset =
        std::find(presets.begin(), presets.end(), std::string("IRF9540N"));
    REQUIRE(pPreset != presets.end());
    presetPicker->setSelectedId(
        static_cast<int>(std::distance(presets.begin(), pPreset)) + 1,
        juce::dontSendNotification);
    if (presetPicker->onChange)
        presetPicker->onChange();

    auto& part = doc.components.front();
    CHECK(part.properties.at("preset") == "IRF9540N");
    CHECK(part.properties.at("polarity") == "P-channel");
    const auto* card = wdf::findMosfetPreset("IRF9540N");
    REQUIRE(card != nullptr);
    CHECK(part.values.at("k") == Approx(card->K));
    CHECK(part.values.at("vth") == Approx(card->Vth));
    CHECK(part.values.at("lambda") == Approx(card->lambda));

    panel.setTarget(&doc, id);
    CHECK(findInfoBelowCaption(panel, "Type:")->getText() == "P-channel");
    CHECK(findPickerBelowCaption(panel, "Preset:")->getText() == "IRF9540N");
    CHECK(findPickerBelowCaption(panel, "Polarity:")->getText() == "P-channel");

    // Changing only the polarity keeps the numeric values and makes the part
    // Custom, so an N-channel preset name can never sit on a P-channel device.
    const double keptK = part.values.at("k");
    const double keptVth = part.values.at("vth");
    auto* pol = findPickerBelowCaption(panel, "Polarity:");
    REQUIRE(pol != nullptr);
    const auto polarityChoices = modelChoices(ChoiceKind::Polarity);
    const auto nChannel = std::find(polarityChoices.begin(), polarityChoices.end(),
                                    std::string("N-channel"));
    REQUIRE(nChannel != polarityChoices.end());
    pol->setSelectedId(
        static_cast<int>(std::distance(polarityChoices.begin(), nChannel)) + 1,
        juce::dontSendNotification);
    if (pol->onChange)
        pol->onChange();

    CHECK(part.properties.at("preset") == "Custom");
    CHECK(part.properties.at("polarity") == "N-channel");
    CHECK(part.values.at("k") == Approx(keptK));
    CHECK(part.values.at("vth") == Approx(keptVth));
    panel.setTarget(&doc, id);
    CHECK(findInfoBelowCaption(panel, "Type:")->getText() == "N-channel");
    CHECK(findPickerBelowCaption(panel, "Preset:")->getText() == "Custom");
}

TEST_CASE("The JFET inspector keeps every parameter and the model picker reachable") {
    // The complaint: a JFET has eight model parameters, and the model picker
    // was built after all of them. It fell below the bottom of the inspector,
    // and there was no scrolling, so it could not be reached at all.
    //
    // The panel now lays the rows out in a viewport whose content is taller than
    // the visible area. This checks both halves: every parameter is present,
    // and the picker is inside the laid-out content (so scrolling reaches it)
    // rather than clipped off the end.
    auto doc = documentWith("jfet");

    InspectorPanel panel;
    panel.setSize(320, 360);
    panel.setTarget(&doc, doc.components.front().id);

    // The model is the part's identity, so it comes first now -- visible
    // without scrolling -- and is a real picker, not a text box.
    CHECK(findCaption(panel, "Model:") != nullptr);
    auto* picker = findPicker(panel);
    REQUIRE(picker != nullptr);
    CHECK(picker->getText() == juce::String(modelOf(doc.components.front())));

    // The inspector also says which polarity the engine is using, and the
    // explicit polarity row is a real choice, not a read-only label.
    auto* type = findInfoBelowCaption(panel, "Type:");
    REQUIRE(type != nullptr);
    CHECK(type->getText() == "N-channel");
    auto* polarity = findPickerBelowCaption(panel, "Polarity:");
    REQUIRE(polarity != nullptr);
    CHECK(polarity->getText() == "N-channel");
    CHECK(polarity->getNumItems() == 2);

    // All eight editable parameters are built, even though they cannot all fit
    // on screen at once.
    for (const char* key : {"Beta:", "Vto:", "Lambda:", "Rd:",
                            "Rs:", "Cgs:", "Cgd:", "Cds:"}) {
        INFO("value key: " << key);
        CHECK(findCaption(panel, key) != nullptr);
    }

    auto* viewport = findViewport(panel);
    REQUIRE(viewport != nullptr);
    auto* content = viewport->getViewedComponent();
    REQUIRE(content != nullptr);

    INFO("viewport height " << viewport->getViewHeight()
         << ", content height " << content->getHeight());
    CHECK(content->getHeight() > viewport->getViewHeight());
    CHECK(picker->getBottom() <= content->getHeight());

    // The identity picker must be *on screen* before any scrolling, not merely
    // present in the content. That is the half of the old complaint that moving
    // properties ahead of the numbers was meant to fix.
    auto* modelCaption = findCaption(panel, "Model:");
    REQUIRE(modelCaption != nullptr);
    CHECK(modelCaption->getY() >= viewport->getViewPositionY());
    CHECK(picker->getBottom() <= viewport->getViewPositionY() + viewport->getViewHeight());

    // Scrolling to the bottom must actually move the view, which is what makes
    // the parameter rows reachable rather than merely laid out.
    viewport->setViewPosition(0, content->getHeight());
    CHECK(viewport->getViewPositionY() > 0);

    // The last parameter row must be inside the visible area once the viewport
    // is scrolled all the way down. Laying it out below the content is not
    // enough: it would still be unreachable.
    auto* lastCaption = findCaption(panel, "Cds:");
    REQUIRE(lastCaption != nullptr);
    CHECK(lastCaption->getBottom()
          <= viewport->getViewPositionY() + viewport->getViewHeight());
}

TEST_CASE("The inspector survives being rebuilt again and again") {
    // **The crash this catches.** The panel used to call `removeAllChildren()`
    // when it rebuilt, which *deletes* its children -- and one of them is the
    // Delete button, which is a *member* of the panel rather than something the
    // panel owns. So every rebuild freed the button and put the freed pointer
    // back, and the editor could die on the next click.
    //
    // Rebuilt a hundred times, because a use-after-free is a matter of what the
    // freed memory happens to hold: one rebuild might survive by luck, a
    // hundred will not.
    auto doc = documentWith("triode");

    InspectorPanel panel;
    for (int i = 0; i < 100; ++i) {
        panel.setTarget(&doc, doc.components.front().id);
        panel.setTarget(&doc, "");   // cleared
        panel.setTarget(&doc, doc.components.front().id);   // and back again
    }

    // The panel is still coherent: it has a picker for the model, and the
    // model is still the one the part carries.
    auto* box = findPicker(panel);
    REQUIRE(box != nullptr);
    CHECK(box->getText() == juce::String(modelOf(doc.components.front())));
}

TEST_CASE("Every part can be renamed from the inspector") {
    // A new part has no `label` property. The old panel only built a name field
    // when one already existed, so most parts could not be renamed at all.
    for (const auto& spec : partPalette()) {
        INFO("part: " << spec.type);

        auto doc = documentWith(spec.type);
        InspectorPanel panel;
        panel.setSize(320, 700);
        panel.setTarget(&doc, doc.components.front().id);

        REQUIRE(findCaption(panel, "Label:") != nullptr);
        auto* editor = findEditorBelowCaption(panel, "Label:");
        REQUIRE(editor != nullptr);
        CHECK(editor->getText().isEmpty());   // blank means "use the id"
    }

    // And the field writes a name without a pre-existing property.
    auto doc = documentWith("res");
    InspectorPanel panel;
    panel.setSize(320, 700);
    panel.setTarget(&doc, doc.components.front().id);

    auto* editor = findEditorBelowCaption(panel, "Label:");
    REQUIRE(editor != nullptr);
    editor->setText("Rin", false);
    editor->onFocusLost();

    CHECK(doc.components.front().properties.at("label") == "Rin");
    CHECK(partLabelText(doc.components.front()) == "Rin");
}

TEST_CASE("Pots and rheostats sharing a label move together") {
    wdf::LiveWDFDocument doc;

    auto p1 = makeComponent("pot", {}, 4.0f, 4.0f);
    auto p2 = makeComponent("pot", {p1}, 4.0f, 8.0f);
    auto vr = makeComponent("varres", {p1, p2}, 8.0f, 4.0f);

    for (auto* part : {&p1, &p2, &vr})
        part->properties["label"] = "GAIN";
    p1.values["pos"] = 0.5;
    p2.values["pos"] = 0.2;
    vr.values["pos"] = 0.8;
    doc.components = {p1, p2, vr};

    const auto groups = wdf::potControlGroups(doc);
    REQUIRE(groups.size() == 1);
    CHECK(groups.front().name == "GAIN");
    REQUIRE(groups.front().members.size() == 3);

    InspectorPanel panel;
    panel.setSize(320, 700);
    panel.setTarget(&doc, doc.components.front().id);

    auto* editor = findEditorBelowCaption(panel, "Position:");
    REQUIRE(editor != nullptr);
    editor->setText("0.25", false);
    editor->onFocusLost();

    CHECK(doc.components[0].values.at("pos") == 0.25);
    CHECK(doc.components[1].values.at("pos") == 0.25);
    CHECK(doc.components[2].values.at("pos") == 0.25);
}

TEST_CASE("Pot position is clamped to its normalized range") {
    wdf::LiveWDFDocument doc;
    auto pot = makeComponent("pot", {}, 4.0f, 4.0f);
    pot.values["totalR"] = 1000000.0;
    pot.values["pos"] = 0.5;
    pot.properties["taper"] = "Log";
    doc.components = {pot};

    InspectorPanel panel;
    panel.setSize(320, 700);
    panel.setTarget(&doc, doc.components.front().id);

    auto* editor = findEditorBelowCaption(panel, "Position:");
    REQUIRE(editor != nullptr);

    // "1M" is one million in engineering notation. A position is a fraction,
    // so the edit must land on 1.0 instead of writing a resistance into it.
    editor->setText("1M", false);
    editor->onFocusLost();
    CHECK(doc.components.front().values.at("pos") == 1.0);

    editor->setText("-2", false);
    editor->onFocusLost();
    CHECK(doc.components.front().values.at("pos") == 0.0);
}

TEST_CASE("Unlabelled pots remain separate controls") {
    wdf::LiveWDFDocument doc;
    auto p1 = makeComponent("pot", {}, 4.0f, 4.0f);
    auto p2 = makeComponent("pot", {p1}, 4.0f, 8.0f);
    doc.components = {p1, p2};

    const auto groups = wdf::potControlGroups(doc);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].name == doc.components[0].id);
    CHECK(groups[1].name == doc.components[1].id);
}

TEST_CASE("Peak volts and nominal RMS edit the selected input alignment") {
    auto doc = documentWith("in");
    doc.components.push_back(makeComponent("in", doc.components, 0.f, 4.f));
    auto& second = doc.components.back();
    second.values.clear();
    second.values["fullScaleVolts"] = 12.0;
    InspectorPanel panel;
    panel.setSize(320, 700);
    panel.setTarget(&doc, second.id);
    auto* peak = findEditorBelowCaption(panel, "Input V peak at 0 dBFS:");
    REQUIRE(peak != nullptr);
    peak->onFocusLost();
    CHECK(second.values.count("referenceMillivoltsRms") == 0);
    peak->setText("0.25", false);
    peak->onFocusLost();
    CHECK(wdf::LiveWDFDocument::inputVoltsPerFullScale(second) == Approx(.25));
    CHECK(doc.inputVoltsPerFullScale() == Approx(1.0));
    CHECK(second.values.count("inputPeakVolts") == 0);
    CHECK(second.values.at("fullScaleVolts") == 12.0);
}

TEST_CASE("The main input inspector edits the document's manual Preview pad") {
    auto doc = documentWith("in");
    doc.components.push_back(makeComponent("in", doc.components, 0.f, 4.f));
    InspectorPanel panel;
    panel.setSize(320, 700);
    panel.setTarget(&doc, doc.inputId());
    auto* pad = findEditorBelowCaption(panel, "Preview pad (dB):");
    REQUIRE(pad != nullptr);
    for (const auto& edit : {"12.5", "25"}) {
        pad->setText(edit, false);
        pad->onFocusLost();
        CHECK(doc.previewPadDb == 12.5);
    }
    pad->setText("0", false);
    pad->onFocusLost();
    CHECK(doc.previewPadDb == 0.);
    CHECK(doc.components.front().values.count("previewPadDb") == 0);
    panel.setTarget(&doc, doc.components.back().id);
    CHECK(findEditorBelowCaption(panel, "Preview pad (dB):") == nullptr);
}
