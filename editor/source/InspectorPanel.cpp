#include "InspectorPanel.h"
#include "CircuitSession.h"
#include "DiagLog.h"
#include "ElementAccess.h"
#include "devices/Jfet.h"
#include "devices/Mosfet.h"
#include "devices/MosfetPresets.h"
#include "format/TransistorPolarity.h"
#include "format/PartLibrary.h"
#include "format/PartModels.h"
#include "format/ValueFormat.h"
#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <algorithm>
#include <future>

namespace livewdf::editor {

namespace {

/// A combo box in the inspector's colours: the same dark ground as a text
/// field, the same accent for the open list.
///
/// A picker that looked like a different control from the fields above it read
/// as a different *kind* of thing, and the whole point of it is that it is the
/// same kind of thing with a list behind it.
void stylePicker(juce::ComboBox& box) {
    box.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xFF161A22));
    box.setColour(juce::ComboBox::textColourId, glow::getColourHighlight());
    box.setColour(juce::ComboBox::outlineColourId, glow::getColourBorder());
    box.setColour(juce::ComboBox::arrowColourId, glow::getColourTextSecondary());
    box.setColour(juce::ComboBox::buttonColourId, juce::Colours::transparentBlack);
}

/// The caption for a property row. The file format uses lower-case keys
/// (`model`, `preset`, `taper`, `mode`); the inspector is read by a person, so
/// the few that are shown as labels get their ordinary capitalisation.
juce::String propertyLabel(const std::string& key) {
    if (key == "model")  return "Model";
    if (key == "preset") return "Preset";
    if (key == wdf::kPolarityProperty) return "Polarity";
    if (key == "taper")  return "Taper";
    if (key == "mode")   return "Mode";
    // **Not "Node".** Under the geometry-only format this row does not name a
    // net: the drawing decides which net a source feeds, and this is a label
    // the rail shows beside its voltage.
    if (key == "node")   return "Label";
    return juce::String(key);
}

}  // namespace

struct InspectorPanel::CalibrationJob {
    std::atomic_bool cancelled{false};
    std::string sheet;
    std::future<CircuitSession::OutputCalibration> result;
};

InspectorPanel::~InspectorPanel() {
    stopTimer();
    cancelOutputCalibration();
}

void InspectorPanel::cancelOutputCalibration() {
    if (calibration_) calibration_->cancelled = true;
    calibrationMessage_.clear();
}

void InspectorPanel::startOutputCalibration() {
    if (calibration_ || !doc_) return;
    const auto* output = doc_->terminal("out");
    if (!output || output->id != id_) return;
    auto job = std::make_unique<CalibrationJob>();
    try {
        job->sheet = doc_->toJson();
        job->result = std::async(std::launch::async, [doc = *doc_, work = job.get()] {
            return CircuitSession::measureOutputReference(doc, work->cancelled);
        });
        calibration_ = std::move(job);
        startTimerHz(10);
    } catch (const std::exception& error) {
        calibrationMessage_ = error.what();
    }
    refreshCalibrationControls();
}

void InspectorPanel::refreshCalibrationControls() {
    if (calibrationButton_) {
        calibrationButton_->setButtonText(calibration_ ? "Measuring..." : "Calibrate output");
        calibrationButton_->setEnabled(!calibration_);
    }
    if (calibrationLabel_) {
        const auto text = calibrationMessage_.isEmpty() ? juce::String("Output level calibration") : calibrationMessage_;
        calibrationLabel_->setText(text, juce::dontSendNotification);
        calibrationLabel_->setTooltip(text);
    }
}

void InspectorPanel::timerCallback() {
    if (!calibration_ || calibration_->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    stopTimer();
    auto job = std::move(calibration_);
    CircuitSession::OutputCalibration result;
    try {
        result = job->result.get();
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    if (!job->cancelled && doc_ && doc_->toJson() == job->sheet) {
        if (result.error.empty()) {
            commitEdit("fullScaleVolts", juce::String(result.voltsPerFullScale, 15));
            if (outputReferenceField_)
                outputReferenceField_->setText(formatValue(doc_->outputFullScaleVolts(), ""), false);
            calibrationMessage_ = "Measured at -9 dBFS";
        } else {
            calibrationMessage_ = result.error;
        }
    }
    refreshCalibrationControls();
}

InspectorPanel::InspectorPanel() {
    // The rows scroll; the title and Delete button do not. See the member note
    // in the header for why the rows cannot stay direct children of the panel.
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(8);
    viewport_.setOpaque(false);
    viewport_.getVerticalScrollBar().setColour(
        juce::ScrollBar::thumbColourId, glow::getColourNormal().withAlpha(0.55f));
    viewport_.getVerticalScrollBar().setColour(
        juce::ScrollBar::trackColourId, juce::Colour(0xFF161A22));
    addAndMakeVisible(viewport_);

    addAndMakeVisible(removeButton_);
    removeButton_.onClick = [this] {
        if (!doc_ || id_.empty()) return;
        auto it = std::find_if(doc_->components.begin(), doc_->components.end(),
                               [&](const wdf::SchematicComponent& c) { return c.id == id_; });
        if (it != doc_->components.end()) {
            doc_->components.erase(it);
            setTarget(doc_, "");
            if (onChanged) onChanged();
        }
    };

    rebuild();
}


void InspectorPanel::setTarget(wdf::LiveWDFDocument* doc, const std::string& id) {
    // Selecting a *different* part should start at the top. A rebuild caused by
    // editing the part that is already selected (the host re-targets it after a
    // model change) should not jump away from the row the user is using.
    const bool sameTarget = (doc == doc_ && id == id_);

    if (!sameTarget) cancelOutputCalibration();
    doc_ = doc;
    id_ = id;
    wire_ = {};          // a part target and a wire target are mutually exclusive
    rebuild();

    if (!sameTarget)
        viewport_.setViewPosition(0, 0);

    repaint();
}

void InspectorPanel::setWireTarget(const WireInfo& wire) {
    cancelOutputCalibration();
    doc_ = nullptr;
    id_.clear();
    wire_ = wire;
    rebuild();
    viewport_.setViewPosition(0, 0);
    repaint();
}

void InspectorPanel::rebuild() {
    // **A rebuild can be re-entered, and that is not a corner case.**
    //
    // A widget's callback can ask for one: a picker's opening selection fires a
    // change, the change is committed, the host reacts by re-targeting the
    // inspector, and the inspector rebuilds -- while the outer rebuild is still
    // running.
    //
    // When the widgets were built straight into the member arrays, the inner
    // rebuild cleared them *underneath* the outer one, which then carried on
    // adding to arrays that no longer matched its labels, and the layout code
    // indexed past the end of them. The editor died on the next click of a part
    // with a picker -- a triode, a pentode, a potentiometer -- and only on those,
    // because only those have a picker to fire the change.
    //
    // So the widgets are built into **locals** and swapped in at the end. An
    // inner rebuild then replaces the members wholesale, and the outer one
    // replaces them again when it finishes; neither can pull the ground out from
    // under the other.
    //
    // The widgets this panel made are owned by the owned arrays, and clearing
    // an array of components removes them from their parent as they are
    // destroyed -- so the panel's children are cleared without asking.
    //
    // It used to call `removeAllChildren()` here, and that was a bug too:
    // `removeAllChildren` *deletes* its children, and one of this panel's
    // children is `removeButton_` -- a *member*, not something the panel owns.
    // So every rebuild freed the button and then put the freed pointer back
    // into the panel.
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Component> editors;

    addAndMakeVisible(removeButton_);

    // Hidden by default: a wire has nothing to edit (it is two endpoints, and
    // Delete removes it through the Delete tool), and an empty selection has
    // nothing either. Shown again below, once a real part is found.
    removeButton_.setVisible(false);



    if (!doc_ || id_.empty()) {
        labels_ = std::move(labels);
        editors_ = std::move(editors);
        resized();
        return;
    }

    auto it = std::find_if(doc_->components.begin(), doc_->components.end(),
                           [&](const wdf::SchematicComponent& c) { return c.id == id_; });
    if (it == doc_->components.end()) {
        labels_ = std::move(labels);
        editors_ = std::move(editors);
        resized();
        return;
    }

    // **A copy, not a reference.** The document's `components` vector is
    // replaced whenever the canvas syncs -- which is what happens as soon as
    // anything on the sheet is edited -- and a reference into a vector that has
    // just been replaced is a reference to freed memory. The rebuild only
    // *reads* the part; every write goes back through `commitEdit`, which
    // finds the component again.
    const wdf::SchematicComponent comp = *it;
    const TransistorFacts transistor = transistorFacts(comp);

    removeButton_.setVisible(true);

    int row = 0;

    // Read-only identity values sit on the same (caption, value) grid as the
    // editors. They are Labels rather than disabled text boxes: there is
    // nothing to type, and a field that looks editable but refuses input is
    // worse than saying the fact plainly.
    const auto addInfoRow = [&](const juce::String& caption,
                                const juce::String& value,
                                const juce::String& tooltip) {
        auto* lab = labels.add(new juce::Label({}, caption + ":"));
        lab->setColour(juce::Label::textColourId, glow::getColourTextSecondary());
        lab->setFont(Fonts::semiBold(11.5f));
        content_.addAndMakeVisible(lab);

        auto* info = new juce::Label({}, value);
        info->setFont(Fonts::monoSemiBold(13.0f));
        info->setMinimumHorizontalScale(0.8f);
        info->setColour(juce::Label::textColourId, glow::getColourSignal());
        info->setColour(juce::Label::backgroundColourId,
                        juce::Colour(0xFF161A22));
        info->setColour(juce::Label::outlineColourId,
                        glow::getColourBorder());
        info->setJustificationType(juce::Justification::centredLeft);
        info->setBorderSize({0, 8, 0, 8});
        info->setTooltip(tooltip);
        content_.addAndMakeVisible(info);
        editors.add(info);

        ++row;
    };

    // **Every part gets a name field.** It used to be built only when the part
    // already carried a `label` property, so a new resistor had no way to be
    // renamed until something else had happened to create one. A blank name is
    // legal and means "use the id"; the faint text is the id it will fall back
    // to, not a value written into the file.
    {
        auto* lab = labels.add(new juce::Label({}, "Label:"));
        lab->setColour(juce::Label::textColourId, glow::getColourTextSecondary());
        lab->setFont(Fonts::semiBold(11.5f));
        content_.addAndMakeVisible(lab);

        auto* ed = new juce::TextEditor();
        ed->setFont(Fonts::monoSemiBold(13.5f));
        const auto labelIt = comp.properties.find("label");
        ed->setText(labelIt != comp.properties.end() ? juce::String(labelIt->second)
                                                     : juce::String(),
                    false);
        ed->setTextToShowWhenEmpty(juce::String(comp.id),
                                   glow::getColourBorder());
        ed->setSelectAllWhenFocused(true);
        ed->setTooltip("The name shown on the sheet. Blank uses the part id.");
        ed->setColour(juce::TextEditor::backgroundColourId,
                      juce::Colour(0xFF161A22));
        ed->setColour(juce::TextEditor::textColourId, glow::getColourHighlight());
        ed->setColour(juce::TextEditor::outlineColourId,
                      glow::getColourBorder());
        ed->setColour(juce::TextEditor::highlightColourId,
                      glow::getColourElectrical().withAlpha(0.35f));
        content_.addAndMakeVisible(ed);
        editors.add(ed);

        ed->onReturnKey = [this, ed] { commitEdit("label", ed->getText()); };
        ed->onFocusLost = [this, ed] { commitEdit("label", ed->getText()); };

        ++row;
    }

    // **Say what kind of transistor this is.** The model/preset picker names a
    // card and the polarity row says which way the device conducts; the Type
    // row puts both in one read-only place, and the artwork follows.
    if (!transistor.type.empty()) {
        const juce::String tooltip =
            comp.type == "bjt"
                ? "The card's polarity is simulated and drawn: NPN and PNP cards "
                  "are both supported."
                : "Channel polarity is simulated and drawn. Named cards carry "
                  "their own polarity; changing the polarity row makes the part "
                  "a Custom device.";
        addInfoRow("Type", juce::String(transistor.type), tooltip);
    }

    // **Identity before numbers.** A model-bearing part's model is what the
    // part *is*; its parameters are how it behaves. These rows used to be built
    // after every numeric value, which put the JFET's model picker below eight
    // parameter rows -- and, before the viewport, below the panel entirely.
    // Now the picker for a part that has one (model/preset/polarity/taper/mode)
    // comes first, and the numbers follow it. `std::map` alone would put a
    // MOSFET's `polarity` before `preset`, so the order is stated here.
    std::vector<std::string> propertyOrder;
    for (const char* key : {"model", "preset", wdf::kPolarityProperty,
                            "taper", "mode", "node", "select"}) {
        if (comp.properties.count(key) != 0 &&
            std::find(propertyOrder.begin(), propertyOrder.end(), key) ==
                propertyOrder.end())
            propertyOrder.emplace_back(key);
    }
    for (const auto& [key, val] : comp.properties) {
        (void)val;
        if (std::find(propertyOrder.begin(), propertyOrder.end(), key) ==
            propertyOrder.end())
            propertyOrder.emplace_back(key);
    }

    for (const std::string& key : propertyOrder) {
        const std::string& val = comp.properties.at(key);
        // The name has its own row above.
        if (key == "label")
            continue;

        // The throw already has its own row above.
        if (comp.type == "switch_res" && key == "select")
            continue;

        auto* lab = labels.add(new juce::Label({}, propertyLabel(key) + ":"));
        lab->setColour(juce::Label::textColourId, glow::getColourTextSecondary());
        lab->setFont(Fonts::semiBold(11.5f));
        content_.addAndMakeVisible(lab);

        // A property with a fixed list of choices -- a device name, a taper --
        // gets a **picker**, not a text box. The list is the point: the user
        // asked to be shown what is available, and a text box asks them to
        // already know the name. It also removes the failure where a typo
        // became a part with no model, which the converter then quietly
        // replaced with a default.
        const auto choices = propertyChoices(comp.type, key);

        if (!choices.empty()) {
            auto* box = new juce::ComboBox();
            for (const auto& choice : choices)
                box->addItem(choice, box->getNumItems() + 1);

            // The part's current value is shown if it is in the list; if it is
            // not -- a hand-edited file, or a model this build does not carry --
            // it is *added* to the list rather than silently replaced. Swapping
            // it for the first entry would change the circuit under the user's
            // hands, and the point of showing the value is that it is the value.
            const juce::String currentText(choiceDisplayText(key, val));
            int index = -1;
            for (int i = 0; i < box->getNumItems(); ++i)
                if (box->getItemText(i) == currentText) {
                    index = i;
                    break;
                }

            if (index >= 0) {
                // **Without the notification.** `setSelectedId` notifies *by
                // default*, and asynchronously -- so setting the picker's
                // opening position posted a callback that fired later, by which
                // time this panel had been rebuilt and the picker was gone. The
                // callback then wrote to a destroyed widget: a crash, on
                // clicking any part that has a picker.
                //
                // It was wrong twice over. The opening position is not a
                // *change* -- nobody chose it -- so notifying about it was
                // asking the host to rewrite the model the user had not
                // touched.
                box->setSelectedId(index + 1);
            } else {
                box->addItem(currentText, box->getNumItems() + 1);
                box->setSelectedId(box->getNumItems());
            }

            stylePicker(*box);

            if (comp.type == "bjt" && key == "model")
                box->setTooltip("NPN and PNP cards are both compiled with their "
                                "own polarity; the symbol follows the card.");
            if (comp.type == "jfet" && key == "model")
                box->setTooltip("N-channel and P-channel JFET cards; choosing a "
                                "card loads its parameters and sets the polarity.");
            if (comp.type == "mosfet" && key == "preset")
                box->setTooltip("N-channel and P-channel MOSFET presets; choosing "
                                "one loads its parameters and sets the polarity.");
            if (key == wdf::kPolarityProperty)
                box->setTooltip("Changing polarity keeps the parameter values and "
                                "turns the model/preset into Custom.");

            const std::string captured = key;
            box->onChange = [this, captured, box] { commitPicker(captured, *box); };

            content_.addAndMakeVisible(*box);
            editors.add(box);
        } else {
            auto* ed = new juce::TextEditor();
            ed->setText(juce::String(val), false);
            ed->setSelectAllWhenFocused(true);
            if (key == "node")
                ed->setTooltip("Display label only. Which net this source feeds is "
                               "decided by the drawing: the wire drawn to its pin. "
                               "A DC rail's voltage is between that net and ground.");
            ed->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF161A22));
            ed->setColour(juce::TextEditor::textColourId, glow::getColourHighlight());
            ed->setColour(juce::TextEditor::outlineColourId, glow::getColourBorder());
            ed->setColour(juce::TextEditor::highlightColourId, glow::getColourElectrical().withAlpha(0.35f));
            content_.addAndMakeVisible(ed);
            editors.add(ed);

            const std::string captured = key;
            ed->onFocusLost = [ed, this, captured] { commitEdit(captured, ed->getText()); };
        }

        ++row;
    }

    // The part's own value key first -- asked through the accessor rather than
    // hard-coded, so which key is "the value" is decided in one place -- then
    // the common keys, then whatever else this part carries. Ordered so the same
    // key sits in the same place across part types.
    static const char* kPreferred[] = {"pos", "gain", "ro", "dc", "ac", "freq", "turnsPrimary", "turnsSecondary"};

    std::vector<std::string> order;
    const char* primary = primaryValueKey(comp.type);

    // **A part whose identity is a model has no number to type**, so its value
    // is not offered: the model picker says "12AX7", and a "Value" field beside
    // it holding a zero says nothing at all. The question is asked of the
    // accessor, which is where it is answered for everyone else too.
    if ((comp.values.count(primary) || comp.type == "in" || comp.type == "out") && hasNumericValue(comp))
        order.push_back(primary);

    if (comp.type == "in")
        order.push_back("inputPeakVolts");

    for (const char* k : kPreferred)
        if (comp.values.count(k) && std::find(order.begin(), order.end(), k) == order.end())
            order.push_back(k);
    for (const auto& [k, v] : comp.values)
        if (!(comp.type == "in" && k == "fullScaleVolts") && k != primary && std::find(order.begin(), order.end(), k) == order.end())
            order.push_back(k);

    // **A source is either DC or AC, so only that mode's fields are shown.**
    //
    // The other mode's numbers stay in the document -- the file format does not
    // change shape because the inspector hides a field -- but they are not
    // *offered*, because offering both is what made the source ambiguous in the
    // first place: a "DC volts" box and an "AC volts" box under a picker that
    // says which one it is.
    if (comp.type == "src" || comp.type == "isrc") {
        const auto wanted = wdf::sourceValueKeys(wdf::sourceModeOf(comp));
        order.erase(std::remove_if(order.begin(), order.end(),
                                   [&](const std::string& k) {
                                       return std::find(wanted.begin(), wanted.end(), k) == wanted.end();
                                   }),
                    order.end());
    }

    // **A transformer only offers the low-level model card in Real — Manual.** The
    // ordinary controls are the turns ratio plus the model picker; `Ideal` and
    // `Real — Auto` derive the card numerically, so showing hidden winding
    // coefficients would make the inspector look editable where the model says it
    // is not. Older saved files may say just `Manual`, so recognize both spellings.
    if (comp.type == "transformer" || comp.type == "transformer_ct") {
        if (!transformerCardIsManual(comp)) {
            std::vector<std::string> visible;
            for (const std::string& key : {"turnsPrimary", "turnsSecondary", "Lm", "leakage",
                                           "dcrPrimary", "dcrSecondary", "cp", "rc"}) {
                if ((key == "turnsPrimary" || key == "turnsSecondary") &&
                    std::find(order.begin(), order.end(), key) != order.end())
                    visible.push_back(key);
            }
            order = std::move(visible);
        }
    }

    // **A BJT offers the Ebers-Moll parameters only.** The active law is the
    // reduced Ebers-Moll one, and the only numbers it reads are Is/Bf/Br/Nf/Nr.
    // The Gummel-Poon card fields -- Early voltages Vaf/Var, knee currents
    // Ikf/Ikr, terminal resistances Rc/Rb/Re -- are *backend* material: a file
    // that carries them still loads (the Fuzz Face's per-instance GP overrides
    // are read and kept so the file round-trips), but they are not offered here,
    // because a row the engine ignores is a control that does nothing.
    if (comp.type == "bjt") {
        std::vector<std::string> em;
        for (const std::string& key : {"is", "bf", "br", "nf", "nr"})
            if (std::find(order.begin(), order.end(), key) != order.end())
                em.push_back(key);
        order = std::move(em);
    }

    // **A triode offers its three quadric coefficients only when Custom.**
    // The named tubes are fixed fits; the parameters are not editable.
    // When "Custom" is chosen, the user provides kP2, kPg, kP.
    if (comp.type == "triode") {
        const std::string model = wdf::componentProperty(comp, "model", "12AX7");
        if (model == "Custom") {
            std::vector<std::string> quadric;
            for (const std::string& key : {"kp2", "kpg", "kp"})
                if (std::find(order.begin(), order.end(), key) != order.end())
                    quadric.push_back(key);
            order = std::move(quadric);
        } else {
            order.clear();
        }
    }

    // **A pentode offers its Koren parameters only when Custom.**
    // The named pentodes are fixed table entries; the parameters are not editable.
    // When "Custom" is chosen, the user provides the 11 law parameters.
    if (comp.type == "pentode") {
        const std::string model = wdf::componentProperty(comp, "model", "EL34");
        if (model == "Custom") {
            std::vector<std::string> koren;
            for (const std::string& key : {"mu", "ex", "kg1", "kg2", "kp", "kvb",
                                            "vg2", "vmax", "count", "gridis", "gridvt"})
                if (std::find(order.begin(), order.end(), key) != order.end())
                    koren.push_back(key);
            order = std::move(koren);
        } else {
            order.clear();
        }
    }


    for (const auto& key : order) {
        auto* lab = labels.add(new juce::Label({}, juce::String(valueKeyLabel(key)) + ":"));
        lab->setColour(juce::Label::textColourId, glow::getColourTextSecondary());
        lab->setFont(Fonts::semiBold(11.5f));
        content_.addAndMakeVisible(lab);
        auto* ed = new juce::TextEditor();
        ed->setFont(Fonts::monoSemiBold(13.5f));

        // Show the value the way the sheet shows it -- engineering notation, and
        // a pot as its marking. The stored number is raw ("250000"), which is
        // right for the file and wrong for a person; `parseValue` reads the
        // formatted text straight back, so editing and returning changes nothing.
        const auto stored = comp.values.find(key);
        double raw = stored != comp.values.end() ? stored->second : 1.0;
        if (key == "inputPeakVolts")
            raw = wdf::LiveWDFDocument::inputVoltsPerFullScale(comp);
        else if (key == "referenceMillivoltsRms" && stored == comp.values.end())
            raw = wdf::LiveWDFDocument::kReferenceMillivolts;
        const auto taper = comp.properties.find("taper");
        const bool isPotValue = (key == primaryValueKey(comp.type) && taper != comp.properties.end());

        ed->setText(juce::String(isPotValue ? formatPotValue(raw, taper->second)
                                            : formatValue(raw, "")),
                    false);
        ed->setSelectAllWhenFocused(true);
        ed->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF161A22));
        ed->setColour(juce::TextEditor::textColourId, glow::getColourHighlight());
        ed->setColour(juce::TextEditor::outlineColourId, glow::getColourBorder());
        ed->setColour(juce::TextEditor::highlightColourId, glow::getColourElectrical().withAlpha(0.35f));
        content_.addAndMakeVisible(ed);
        editors.add(ed);

        // Capture the key by value: the vector is rebuilt often, and a dangling
        // reference here would write to the wrong field after an edit.
        const std::string captured = key;
        ed->onReturnKey = [this, captured] {
            auto* e = dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent());
            if (!e) return;
            commitEdit(captured, e->getText());
        };
        ed->onFocusLost = [ed, this, captured] { commitEdit(captured, ed->getText()); };
        if (comp.type == "out" && key == "fullScaleVolts") {
            outputReferenceField_ = ed;
            ed->onTextChange = [this] {
                cancelOutputCalibration();
                refreshCalibrationControls();
            };
        }

        ++row;
    }

    const auto* playbackOutput = doc_->terminal("out");
    if (comp.type == "out" && playbackOutput && playbackOutput->id == comp.id) {
        auto* label = labels.add(new juce::Label());
        label->setFont(Fonts::semiBold(11.5f));
        content_.addAndMakeVisible(label);
        auto* button = new juce::TextButton();
        button->setTooltip("Match output to a -9 dBFS multitone at the saved controls. You can edit the measured voltage above.");
        button->onClick = [this] { startOutputCalibration(); };
        content_.addAndMakeVisible(button);
        calibrationButton_ = button;
        calibrationLabel_ = label;
        refreshCalibrationControls();
        editors.add(button);
    }

    // **The SPDT throw is an intrinsic control, not a generic property field.**
    // It gets a caption and a button even when a hand-edited file omits
    // `select`: both the converter and the artwork treat a missing value as
    // throw A, so the inspector must not silently offer no way to change it.
    if (comp.type == "switch_res") {
        const bool throwB = wdf::switchResSelectsB(comp);

        auto* lab = labels.add(new juce::Label({}, "Position:"));
        lab->setColour(juce::Label::textColourId, glow::getColourTextSecondary());
        lab->setFont(Fonts::semiBold(11.5f));
        content_.addAndMakeVisible(lab);

        auto* btn = new juce::TextButton(throwB ? "Throw B" : "Throw A");
        btn->setClickingTogglesState(true);
        btn->setToggleState(throwB, juce::dontSendNotification);
        btn->setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF161A22));
        btn->setColour(juce::TextButton::textColourOffId, glow::getColourHighlight());
        content_.addAndMakeVisible(*btn);
        editors.add(btn);

        // Read the document at click time. Capturing the value that was on
        // screen when the panel was built makes a second click write the same
        // value again, because the host's rebuild is deferred.
        btn->onClick = [this, btn] {
            if (!doc_ || id_.empty())
                return;

            auto sw = std::find_if(doc_->components.begin(), doc_->components.end(),
                                   [&](const wdf::SchematicComponent& c) { return c.id == id_; });
            if (sw == doc_->components.end() || sw->type != "switch_res")
                return;

            const bool onB = wdf::switchResSelectsB(*sw);
            commitEdit("select", juce::String(onB ? "0" : "1"));

            // Keep the button honest even when no host callback rebuilds the
            // panel (the tests drive `onClick` directly), and so a second click
            // reads the new state rather than the old one.
            btn->setButtonText(onB ? "Throw A" : "Throw B");
            btn->setToggleState(!onB, juce::dontSendNotification);
        };

        ++row;
    }

    // The swap. Everything above built into locals, so a rebuild that was
    // re-entered cannot have had the members pulled out from under it.
    labels_ = std::move(labels);
    editors_ = std::move(editors);

    ++rebuildCount_;
    // **Diagnostic.** See `commitPicker`. This is the other half of the story:
    // a rebuild is what destroys the picker, so the log needs to say when one
    // happened and what it was for.
    diagLog("rebuild: target '" + id_ + "' of type '"
              + (id_.empty() ? std::string() : typeOfTarget()) + "', "
              + std::to_string(editors_.size()) + " rows");

    resized();
}

std::string InspectorPanel::typeOfTarget() const {
    if (!doc_ || id_.empty())
        return {};
    for (const auto& c : doc_->components)
        if (c.id == id_)
            return c.type;
    return {};
}

void InspectorPanel::commitPicker(const std::string& key, juce::ComboBox& box) {
    // The text is read *before* the commit. The commit can make the host
    // rebuild this panel, which destroys the box -- and a callback that then
    // went on to touch the box would be reading a freed object.
    const int id = box.getSelectedId();
    if (id <= 0)
        return;

    const juce::String text = box.getItemText(id - 1);

    // **Diagnostic.** The crash being chased was "clicking a part with a picker
    // takes the editor down", and every explanation for it was a theory. This
    // writes what actually happened, in order, so the next crash comes with a
    // story attached instead of a guess.
    diagLog("picker: change '" + key + "' -> '" + text.toStdString() + "'");

    // The picker shows "On"/"Off" for grid conduction; the file stores
    // "true"/"false". Every other choice is stored exactly as shown.
    commitEdit(key, juce::String(choiceStorageText(key, text.toStdString())));

    // Did the widget survive its own callback? If the panel rebuilt inside the
    // commit, it did not -- and that is the use-after-free, named. The picker
    // lives inside the scrolling content component now, not directly in the
    // panel, so "still in the panel's tree" is the question, not "is it one of
    // the panel's immediate children".
    const bool alive = (box.getParentComponent() == &content_);

    diagLog(alive ? "picker: survived" : "picker: DESTROYED DURING ITS OWN CALLBACK");
}

void InspectorPanel::quickValueEdit(const juce::String& text) {
    if (!doc_ || id_.empty())
        return;

    auto it = std::find_if(doc_->components.begin(), doc_->components.end(),
                           [&](const wdf::SchematicComponent& c) { return c.id == id_; });
    if (it == doc_->components.end())
        return;

    // The one number the palette says this type carries. `commitEdit` decides
    // value vs property by the maps themselves, so the key here only has to be
    // the right name; a type with no such key (a terminal, a model-bearing
    // part's non-numeric read) has nothing to answer.
    const std::string key = primaryValueKey(it->type);
    if (key.empty() || (it->values.count(key) == 0 && it->type != "in" && it->type != "out"))
        return;

    commitEdit(key, text);
}

void InspectorPanel::commitEdit(const std::string& displayedKey, const juce::String& text) {
    if (!doc_ || id_.empty()) return;
    auto it = std::find_if(doc_->components.begin(), doc_->components.end(),
                           [&](const wdf::SchematicComponent& c) { return c.id == id_; });
    if (it == doc_->components.end()) return;

    // Decide value vs property by asking which map already has the key; this
    // keeps the callback free of the ordering bookkeeping in `rebuild`.
    // Both input fields edit the same alignment, expressed at different digital sine levels.
    const bool peakInput = it->type == "in" && displayedKey == "inputPeakVolts";
    const std::string key = peakInput ? "referenceMillivoltsRms" : displayedKey;
    const bool inputReference = it->type == "in" && key == "referenceMillivoltsRms";
    const bool outputReference = it->type == "out" && key == "fullScaleVolts";
    if (it->values.count(key) || inputReference || outputReference) {
        const bool isPotValue =
            (key == primaryValueKey(it->type) && it->properties.count("taper") != 0);

        double parsed = 0.0;
        std::string taper;
        bool taperGiven = false;

        if (isPotValue && parsePotValue(text.toStdString(), parsed, taper, taperGiven)) {
            // "A250K" sets the taper and the value at once -- one field, because
            // that is how the marking is written on the part. A plain "250K"
            // leaves the taper alone.
            const bool valueChanged = parsed != it->values[key];
            const bool taperChanged = taperGiven && taper != it->properties["taper"];

            if (taperChanged)
                it->properties["taper"] = taper;
            if (valueChanged)
                it->values[key] = parsed;

            if (!valueChanged && !taperChanged)
                return;   // nothing changed: nothing to tell the host
        } else {
            // A number, or nothing. Refusing rubbish (rather than writing 0
            // through it and turning a resistor into a short) is the whole point
            // of parsing through the formatter's own reader.
            bool ok = false;
            double value = parseValue(text.toStdString(), &ok);
            if (peakInput) {
                if (text == juce::String(formatValue(wdf::LiveWDFDocument::inputVoltsPerFullScale(*it), ""))) return;
                value *= wdf::LiveWDFDocument::kReferenceMillivolts;
            }
            if (!ok || ((inputReference || outputReference)
                        && (!std::isfinite(value) || value <= 0.)))
                return;

            // A pot position is a normalized fraction, not a resistance. This
            // field is shown in engineering notation, so "1M" reads as one
            // million -- the same text that means one megohm in a resistance
            // field. Clamp here so the document cannot store a position the
            // engine would only clamp later; 0 and 1 are the physical ends.
            if (key == "pos" && (it->type == "pot" || it->type == "varres"))
                value = std::clamp(value, 0.0, 1.0);

            const auto stored = it->values.find(key);
            if (inputReference && !peakInput) {
                const double original = stored != it->values.end() ? stored->second : wdf::LiveWDFDocument::kReferenceMillivolts;
                if (text == juce::String(formatValue(original, ""))) return;
            }
            if (outputReference) {
                const double original = stored == it->values.end() ? 1. : stored->second;
                if (value == original || text == juce::String(formatValue(original, ""))) return;
            }
            if (stored != it->values.end() && value == stored->second)
                return;   // nothing changed: nothing to tell the host
            it->values[key] = value;
        }

        // **Pot ganging.** A pot and a rheostat that share a label are one
        // physical control: moving the one you are editing moves every member
        // of the group. The plugin applies the same rule when it builds its
        // knobs, so the saved document and the live control cannot disagree.
        if (key == "pos" && (it->type == "pot" || it->type == "varres")) {
            const auto label = it->properties.find("label");
            if (label != it->properties.end() && !label->second.empty()) {
                const double pos = it->values["pos"];
                for (auto& other : doc_->components) {
                    if (other.id == it->id)
                        continue;
                    if (other.type != "pot" && other.type != "varres")
                        continue;
                    const auto otherLabel = other.properties.find("label");
                    if (otherLabel == other.properties.end() || otherLabel->second != label->second)
                        continue;
                    other.values["pos"] = pos;
                }
            }
        }
    } else {
        // **Nothing to write is nothing to do.**
        //
        // This is the fix for the editor spinning on a part with a picker.
        //
        // A picker sets its own *opening position* when it is built, and JUCE
        // posts a change notification for that -- not through the notification
        // type the caller chose, but because the ComboBox's internal label
        // changes its text, and its text-change handler asks for an update.
        // `dontSendNotification` does not stop it.
        //
        // So the panel was rebuilding itself in answer to a change the *user*
        // had not made: rebuild -> build a picker -> post a change -> rebuild ->
        // ... The editor spun, building and destroying widgets as fast as the
        // message loop would go, and took a widget down with it. That was the
        // crash on the triode, the pentode, the potentiometer and the rheostat
        // -- the parts with a picker.
        //
        // The way out is the obvious one: **if the value is already what the
        // picker says, there is no change**, so there is nothing to write and
        // nothing to tell the host about. The user's own choice of a different
        // value still goes through; only the echo of the value that was already
        // set is dropped.
        const auto existing = it->properties.find(key);
        if (existing != it->properties.end() && existing->second == text.toStdString())
            return;

        // A blank name on a part that never had one is not an edit: it already
        // shows its id. Writing the empty property would make the document
        // dirty just for clicking through the field.
        if (key == "label" && text.isEmpty() && existing == it->properties.end())
            return;

        it->properties[key] = text.toStdString();

        // When selecting a named preset/model, load its nominal parameters back
        // into the editable values so the user starts from the preset.
        if ((key == "model" && it->type == "jfet") || (key == "preset" && it->type == "mosfet") ||
            (key == "model" && it->type == "bjt")) {
            const std::string presetName = text.toStdString();
            if (it->type == "jfet") {
                const auto* jm = wdf::findJfetByName(presetName.c_str());
                if (jm) {
                    const auto& p = jm->model.p;
                    it->properties[wdf::kPolarityProperty] =
                        p.pChannel ? wdf::kPChannel : wdf::kNChannel;
                    it->values["beta"] = p.Is0 / (p.Vp * p.Vp);
                    it->values["vto"] = p.Vp;
                    it->values["lambda"] = p.lambda;
                    it->values["rd"] = p.rd;
                    it->values["rs"] = p.rs;
                    it->values["cgs"] = p.cgs;
                    it->values["cgd"] = p.cgd;
                    it->values["cds"] = p.cds;
                }
            } else if (it->type == "mosfet") {
                if (const auto* preset = wdf::findMosfetPreset(presetName)) {
                    it->properties[wdf::kPolarityProperty] =
                        preset->pChannel ? wdf::kPChannel : wdf::kNChannel;
                    it->values["k"] = preset->K;
                    it->values["vth"] = preset->Vth;
                    it->values["lambda"] = preset->lambda;
                }
            } else if (it->type == "bjt") {
                // Only the Ebers-Moll parameters are reloaded. The Gummel-Poon
                // fields a legacy file carries stay untouched in the document
                // as backend data -- the law never reads them -- so choosing a
                // card cannot invent rows the inspector would not show.
                if (const auto* entry = wdf::findBjtByName(presetName.c_str())) {
                    const auto& m = entry->model;
                    it->values["is"] = m.Is;
                    it->values["bf"] = m.Bf;
                    it->values["br"] = m.Br;
                    it->values["nf"] = m.Nf;
                    it->values["nr"] = m.Nr;
                }
            }
        }

        // A transformer reloads nothing by name: its card is a *rule* over the
        // turns ratio, not an entry in a registry. `Ideal` and `Real — Auto` have
        // no low-level rows, so there is nothing to load; `Real — Manual` has rows
        // the user tunes, and they start from the card the turns ratio implies --
        // otherwise picking Manual on a part just placed from the palette offers
        // a card with nothing under it.
        if (key == "model" && (it->type == "transformer" || it->type == "transformer_ct"))
            applyTransformerCard(*it);

        // Changing polarity on its own is a custom device: a named card must
        // not keep its N-channel name while conducting as a P-channel part.
        // The parameter values stay exactly as they are, so this is a polarity
        // edit, not a model reload.
        if (key == wdf::kPolarityProperty &&
            (it->type == "jfet" || it->type == "mosfet")) {
            const bool pChannel = text.toStdString() == wdf::kPChannel;
            const char* identityKey = it->type == "jfet" ? "model" : "preset";
            const auto identity = it->properties.find(identityKey);
            bool matches = false;
            if (identity != it->properties.end()) {
                if (it->type == "jfet") {
                    if (const auto* card = wdf::findJfetByName(identity->second.c_str()))
                        matches = card->model.p.pChannel == pChannel;
                } else {
                    if (const auto* card = wdf::findMosfetPreset(identity->second))
                        matches = card->pChannel == pChannel;
                }
            }
            if (!matches)
                it->properties[identityKey] = "Custom";
        }

        // **Ganging.** switch_res parts that share a `label` are one changeover:
        // flipping one flips them all, exactly like a hardware ganged switch.
        if (it->type == "switch_res" && key == "select") {
            const std::string myLabel = it->properties.count("label") ? it->properties["label"] : std::string();
            if (!myLabel.empty()) {
                for (auto& other : doc_->components) {
                    if (other.id == it->id)
                        continue;
                    if (other.type != "switch_res")
                        continue;
                    if (other.properties.count("label") == 0 || other.properties["label"] != myLabel)
                        continue;
                    other.properties["select"] = text.toStdString();
                }
            }
        }
    }

    if (onChanged) {
        // **Deferred, and deliberately so.**
        //
        // The host's reaction to a change re-reads the canvas, and that calls
        // back into `setTarget` -- which rebuilds this panel and destroys the
        // very widget whose callback is running. The widget is a ComboBox, and a
        // ComboBox's `onChange` is a `std::function` *owned by the ComboBox*: so
        // destroying the box while its callback is running destroys the very
        // function being invoked. That is a use-after-free, and it is why
        // clicking a triode, a pentode or a potentiometer -- the parts with a
        // picker -- could take the editor down.
        //
        // Doing the notification from the message loop, after the callback has
        // returned, is the only order that cannot pull the ground out from
        // under itself. `tests/editor/test_inspector_picker.cpp` checks exactly
        // this: with a synchronous reaction the widget is gone by the time the
        // callback returns, and the test segfaults.
        //
        // The `SafePointer` is because "after the callback returns" is not the
        // same as "after the panel is still alive": a deferred call that fires
        // after the window has closed would otherwise call through a dangling
        // pointer.
        juce::Component::SafePointer<InspectorPanel> self(this);
        juce::MessageManager::callAsync([self] {
            if (self != nullptr && self->onChanged)
                self->onChanged();
        });
    }
}

void InspectorPanel::paint(juce::Graphics& g) {
    g.fillAll(glow::getColourPanel());

    // --- Header band: darker strip with the section title ---
    const int headerH = 38;
    g.setColour(glow::getColourPanel().darker(0.06f));
    g.fillRect(0, 0, getWidth(), headerH);

    // Subtle bottom edge — the depth cue that separates chrome from content.
    g.setColour(glow::getColourBorder());
    g.drawHorizontalLine(headerH, 0.0f, static_cast<float>(getWidth()));

    // Section title: small caps, dim — the label for the panel, not the content.
    g.setColour(glow::getColourTextSecondary());
    g.setFont(Fonts::semiBold(10.5f));
    g.drawText("PROPERTIES", juce::Rectangle<int>(10, 4, getWidth() - 20, 12),
               juce::Justification::centredLeft);

    if (wire_.valid()) {
        // Wire info — read-only facts about the selected wire.
        g.setColour(glow::getColourHighlight());
        g.setFont(Fonts::bold(14.0f));
        g.drawText("Wire", juce::Rectangle<int>(10, 16, getWidth() - 20, 18),
                   juce::Justification::centredLeft);

        g.setColour(glow::getColourTextSecondary());
        g.setFont(Fonts::mono(11.5f));
        const auto dir = (wire_.ax == wire_.bx) ? "vertical" : "horizontal";
        g.drawText(juce::String("runs ") + dir,
                   juce::Rectangle<int>(10, headerH + 8, getWidth() - 20, 14),
                   juce::Justification::centredLeft);

        const auto endText = [](const juce::String& net, int x, int y) {
            return (net.isNotEmpty() ? net : juce::String("(") + juce::String(x) + "," +
                                                   juce::String(y) + ")");
        };
        g.drawText("from " + endText(wire_.netA, wire_.ax, wire_.ay),
                   juce::Rectangle<int>(10, headerH + 24, getWidth() - 20, 14),
                   juce::Justification::centredLeft);
        g.drawText("to " + endText(wire_.netB, wire_.bx, wire_.by),
                   juce::Rectangle<int>(10, headerH + 40, getWidth() - 20, 14),
                   juce::Justification::centredLeft);
    } else if (doc_ && !id_.empty()) {
        // Part name — gold, prominent.
        g.setColour(glow::getColourHighlight());
        g.setFont(Fonts::bold(14.0f));
        g.drawText(id_, juce::Rectangle<int>(10, 16, getWidth() - 20, 18),
                   juce::Justification::centredLeft);
    } else {
        g.setColour(glow::getColourTextSecondary().withAlpha(0.6f));
        g.setFont(Fonts::medium(12.0f));
        g.drawText("No component selected", juce::Rectangle<int>(10, 16, getWidth() - 20, 18),
                   juce::Justification::centredLeft);
    }
}

void InspectorPanel::resized() {
    const int top = 42;
    const int rowH = 38;
    const int contentPad = 6;

    // The Delete button is a fixed footer; the viewport stops short of it so a
    // long parameter list can never push the button off the bottom.
    const int buttonH = 26;
    const int buttonGap = 8;
    const int viewportH =
        juce::jmax(0, getHeight() - top - buttonH - buttonGap - buttonGap);
    viewport_.setBounds(0, top, getWidth(), viewportH);

    // Every row is a (caption, control) pair. The assertion catches a new row
    // that forgot its caption while it is still a development build; the `jmin`
    // keeps a release build from indexing past the labels and taking the editor
    // down, which is exactly how selecting the SPDT used to crash.
    jassert(labels_.size() == editors_.size());
    const int rows = juce::jmin(labels_.size(), editors_.size());

    const int contentH =
        juce::jmax(viewportH, contentPad + rows * rowH + contentPad);
    const int scrollW = contentH > viewportH ? viewport_.getScrollBarThickness() : 0;
    const int contentW = juce::jmax(1, viewport_.getWidth() - scrollW);
    content_.setSize(contentW, contentH);

    for (int i = 0; i < rows; ++i) {
        const int y = contentPad + i * rowH;
        const int w = juce::jmax(1, contentW - 16);
        labels_[i]->setBounds(8, y, w, 14);
        editors_[i]->setBounds(8, y + 14, w, 22);
    }

    removeButton_.setBounds(8, getHeight() - buttonH - buttonGap,
                            juce::jmax(0, getWidth() - 16), buttonH);
}

}  // namespace livewdf::editor