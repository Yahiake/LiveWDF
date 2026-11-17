// Holds the editor's model pickers to the engine's device registry (U4.1).
//
// **The complaint this answers.** "For some components I got many variants but
// instead of giving me the ones that are available, you can only edit the text
// and enter the variant name yourself." The engine has the registry -- six
// diodes, fourteen triodes, twelve pentodes -- and the inspector had a text box
// and no way to show it.
//
// **What this test holds.** That the pickers offer exactly the devices the
// engine carries, and that each name in a picker *resolves* in the engine. That
// second half is the one that matters: a picker is a promise that choosing a
// name gives you that device, and a name that does not resolve is a part the
// converter silently replaces with a default -- the circuit stops being the one
// that is drawn, with nothing saying so.
//
// The list is read from the engine's own tables, so a device added there appears
// in the picker with no other edit. This test is what says the two have not
// drifted.

#include "ElementAccess.h"
#include "format/PartLibrary.h"
#include "format/PartModels.h"

#include "devices/Bjt.h"
#include "devices/Diode.h"
#include "devices/Jfet.h"
#include "devices/MosfetPresets.h"
#include "devices/OpAmp.h"
#include "devices/Pentode.h"
#include "devices/QuadricTriode.h"
#include "format/TransistorPolarity.h"
#include "spice/Circuit.h"

#include <catch2.hpp>

#include <algorithm>
#include <set>
#include <string>

using namespace livewdf::editor;

namespace {

/// Every model-bearing part type, and the kind of choice it offers. Named here
/// so a new model-bearing type has to be added deliberately: a part that
/// silently lost its picker would be the bug this file exists to prevent.
struct ModelBearing {
    const char* type;
    ChoiceKind kind;
    const char* identityKey;  ///< "model", "preset", or "taper"
};

const std::vector<ModelBearing>& modelBearingTypes() {
    static const std::vector<ModelBearing> v = {
        {"diode",   ChoiceKind::DiodeModel,   "model"},
        {"triode",  ChoiceKind::TriodeModel,  "model"},
        {"pentode", ChoiceKind::PentodeModel, "model"},
        {"jfet",    ChoiceKind::JfetModel,    "model"},
        {"mosfet",  ChoiceKind::MosfetPreset, "preset"},
        {"bjt",     ChoiceKind::BjtModel,     "model"},
        {"opamp",   ChoiceKind::OpAmpModel,   "model"},
        {"pot",     ChoiceKind::Taper,        "taper"},
        {"varres",  ChoiceKind::Taper,        "taper"},
        {"transformer", ChoiceKind::TransformerModel, "model"},
        {"transformer_ct", ChoiceKind::TransformerModel, "model"},
    };
    return v;
}

const ModelBearing* modelBearingFor(const std::string& type) {
    for (const auto& mb : modelBearingTypes())
        if (type == mb.type)
            return &mb;
    return nullptr;
}

}  // namespace

TEST_CASE("Every model-bearing part offers a picker") {
    // The failure this exists for: a part that has variants in the engine and
    // no picker in the editor, so the user has to know the name by heart.
    for (const auto& spec : partPalette()) {
        const ModelBearing* mb = modelBearingFor(spec.type);
        if (!mb)
            continue;

        INFO("type: " << spec.type);
        CHECK(choiceKindFor(spec.type, mb->identityKey) != ChoiceKind::None);
    }
}

TEST_CASE("Every name a picker offers resolves in the engine") {
    // The promise a picker makes. A name that does not resolve is a part the
    // converter quietly replaces with a default, so the circuit stops being the
    // one that was drawn.
    for (const auto& mb : modelBearingTypes()) {
        const auto choices = modelChoices(mb.kind);
        INFO("type: " << mb.type);

        REQUIRE(!choices.empty());

        for (const auto& name : choices) {
            INFO("model: " << name);

            // "Custom" is a sentinel meaning "use custom parameters from the
            // part's values map", not an engine model name. It is valid in the
            // picker but does not resolve in the engine tables.
            if (name == "Custom")
                continue;

            switch (mb.kind) {
                case ChoiceKind::DiodeModel:
                    CHECK(wdf::findDiodeByName(name) != nullptr);
                    break;
                case ChoiceKind::TriodeModel:
                    CHECK(wdf::quadricTriodeByName(name.c_str()) != nullptr);
                    break;
                case ChoiceKind::PentodeModel:
                    CHECK(wdf::findPentodeByName(name) != nullptr);
                    break;
                case ChoiceKind::JfetModel:
                    CHECK(wdf::findJfetByName(name) != nullptr);
                    break;
                case ChoiceKind::BjtModel:
                    CHECK(wdf::findBjtByName(name) != nullptr);
                    break;
                case ChoiceKind::MosfetPreset:
                    CHECK(wdf::findMosfetPreset(name) != nullptr);
                    break;
                case ChoiceKind::OpAmpModel: {
                    // The picker entry is the canonical model name, so parsing
                    // it and spelling the result back must be the same string.
                    const auto type = wdf::opAmpTypeFromString(name);
                    CHECK(std::string(wdf::opAmpTypeName(type)) == name);
                    break;
                }
                case ChoiceKind::TransformerModel: {
                    // A transformer model card is an editor choice, not a
                    // device registry entry: the engine resolves the card's
                    // numerical meaning when it compiles the coupled matrix.
                    const bool known =
                        name == "Ideal" || name == "Real — Auto" ||
                        name == "Real — Manual";
                    CHECK(known);
                    break;
                }
                case ChoiceKind::Polarity: {
                    const bool known =
                        name == std::string(wdf::kNChannel) ||
                        name == std::string(wdf::kPChannel);
                    CHECK(known);
                    break;
                }
                case ChoiceKind::Taper: {
                    // The three tapers the converter understands. Computed
                    // outside the CHECK: Catch2 decomposes a compound
                    // expression, and a `const char[]` comparison does not
                    // survive being decomposed.
                    const bool known =
                        (name == std::string("Linear")) ||
                        (name == std::string("Log")) ||
                        (name == std::string("RevLog"));
                    CHECK(known);
                    break;
                }
                case ChoiceKind::None:
                    break;
            }
        }
    }
}

TEST_CASE("The picker offers every device the engine carries") {
    // The other direction: a device in the registry that the picker does not
    // offer is a device the user cannot select, which is the same complaint
    // from the other side. Triode/Pentode also include "Custom" as a sentinel
    // for user-defined parameters, so their picker size is engine count + 1.
    CHECK(modelChoices(ChoiceKind::DiodeModel).size()
          == static_cast<size_t>(wdf::kNumDiodes));
    CHECK(modelChoices(ChoiceKind::TriodeModel).size()
          == static_cast<size_t>(wdf::kNumQuadricTriodes) + 1);
    CHECK(modelChoices(ChoiceKind::PentodeModel).size()
          == std::size(wdf::kPentodeTable) + 1);
    CHECK(modelChoices(ChoiceKind::JfetModel).size()
          == static_cast<size_t>(wdf::kNumJfets));
    CHECK(modelChoices(ChoiceKind::BjtModel).size()
          == static_cast<size_t>(wdf::kNumBjts));
    CHECK(modelChoices(ChoiceKind::MosfetPreset).size()
          == static_cast<size_t>(wdf::kNumMosfetPresets));
    CHECK(modelChoices(ChoiceKind::OpAmpModel).size()
          == static_cast<size_t>(wdf::kNumOpAmpTypes));
    CHECK(modelChoices(ChoiceKind::TransformerModel).size() == 3);
}

TEST_CASE("The JFET picker offers N-channel and P-channel cards") {
    const auto choices = modelChoices(ChoiceKind::JfetModel);
    for (const char* name : {"2N3819", "J202", "2N5460", "J175"}) {
        INFO("model: " << name);
        CHECK(std::find(choices.begin(), choices.end(), std::string(name)) !=
              choices.end());
        CHECK(wdf::findJfetByName(name) != nullptr);
    }

    const auto* pCard = wdf::findJfetByName("2N5460");
    REQUIRE(pCard != nullptr);
    CHECK(pCard->model.p.pChannel);
    CHECK_FALSE(wdf::findJfetByName("2N5952")->model.p.pChannel);
}

TEST_CASE("The BJT picker offers both NPN and PNP cards") {
    // A card name is a promise about the device: 2N3906 must compile and draw
    // as a PNP, not as an NPN with the same numbers. The picker and the
    // registry are now the same table, so this checks the promise directly.
    const auto choices = modelChoices(ChoiceKind::BjtModel);
    for (const char* name : {"2N3904", "2N2222A", "2N2222", "BC547B", "2N5088",
                             "2N3906", "2N2907", "BC557B", "2N5087"}) {
        INFO("model: " << name);
        CHECK(std::find(choices.begin(), choices.end(), std::string(name)) !=
              choices.end());
        const auto* card = wdf::findBjtByName(name);
        REQUIRE(card != nullptr);
        const bool expectedPnp = std::string(name) == "2N3906" ||
                                 std::string(name) == "2N2907" ||
                                 std::string(name) == "BC557B" ||
                                 std::string(name) == "2N5087";
        CHECK(card->model.pnp == expectedPnp);
    }
}

TEST_CASE("Transistor facts name the polarity and a BJT's card values") {
    // The inspector's Type row has to agree with the device the engine will
    // build: the card (or a custom part's explicit polarity) is the authority.
    wdf::SchematicComponent bjt = makeComponent("bjt", {}, 0.0f, 0.0f);
    auto facts = transistorFacts(bjt);
    CHECK(facts.type == "NPN");
    CHECK(facts.details.find("Bf") != std::string::npos);
    CHECK(facts.details.find("Br") != std::string::npos);

    for (const auto& name : modelChoices(ChoiceKind::BjtModel)) {
        bjt.properties["model"] = name;
        const auto* card = wdf::findBjtByName(name);
        REQUIRE(card != nullptr);
        INFO("model: " << name);
        CHECK(transistorFacts(bjt).type == (card->model.pnp ? "PNP" : "NPN"));
    }

    bjt.properties["model"] = "2N3906";
    facts = transistorFacts(bjt);
    CHECK(facts.type == "PNP");
    CHECK(facts.details.find("Bf") != std::string::npos);

    bjt.properties["model"] = "not-a-transistor";
    CHECK(transistorFacts(bjt).type == "Unknown");

    // The field-effect parts follow their card, and a custom part follows the
    // explicit polarity property.
    wdf::SchematicComponent jfet = makeComponent("jfet", {}, 0.0f, 0.0f);
    CHECK(transistorFacts(jfet).type == "N-channel");
    jfet.properties["model"] = "2N5460";
    CHECK(transistorFacts(jfet).type == "P-channel");
    jfet.properties["model"] = "Custom";
    jfet.properties[wdf::kPolarityProperty] = wdf::kPChannel;
    CHECK(transistorFacts(jfet).type == "P-channel");
    jfet.properties.erase(wdf::kPolarityProperty);
    CHECK(transistorFacts(jfet).type == "N-channel");

    wdf::SchematicComponent mosfet = makeComponent("mosfet", {}, 0.0f, 0.0f);
    CHECK(transistorFacts(mosfet).type == "N-channel");
    mosfet.properties["preset"] = "IRF9540N";
    CHECK(transistorFacts(mosfet).type == "P-channel");
    mosfet.properties["preset"] = "Custom";
    mosfet.properties[wdf::kPolarityProperty] = wdf::kPChannel;
    CHECK(transistorFacts(mosfet).type == "P-channel");
    mosfet.properties.erase("preset");
    CHECK(transistorFacts(mosfet).type == "P-channel");

    // A part that is not a transistor gets no identity row.
    CHECK(transistorFacts(makeComponent("res", {}, 0.0f, 0.0f)).type.empty());
}

TEST_CASE("A part's default model is one of the choices offered") {
    // The default is what a freshly dropped part carries, and it is what the
    // picker opens on. If it is not in the list, the picker would show the
    // default as an unknown extra entry -- or, worse, silently select a
    // different device and change the part under the user's hands.
    for (const auto& mb : modelBearingTypes()) {
        if (mb.kind == ChoiceKind::Taper)
            continue;

        INFO("type: " << mb.type);

        // Build a part the way the palette does, and read its identity back.
        wdf::SchematicComponent comp = makeComponent(mb.type, {}, 0.0f, 0.0f);
        const std::string model = livewdf::editor::modelOf(comp);
        REQUIRE(!model.empty());

        const auto choices = modelChoices(mb.kind);
        CHECK(std::find(choices.begin(), choices.end(), model) != choices.end());
    }
}

TEST_CASE("A part type with no variants offers no picker") {
    // The other side of the contract: a resistor has no model, so its inspector
    // must not offer one. A picker with an empty list is a control that shows
    // nothing, which is worse than no control.
    CHECK(choiceKindFor("res", "model") == ChoiceKind::None);
    CHECK(propertyChoices("res", "model").empty());
    CHECK(propertyChoices("cap", "model").empty());

    // And a taper is a pot's, not a resistor's.
    CHECK(choiceKindFor("res", "taper") == ChoiceKind::None);
    CHECK(choiceKindFor("pot", "taper") == ChoiceKind::Taper);
}

TEST_CASE("A choice is shown and stored exactly as it is") {
    // The picker's text and the file's text agree: no property needs a display
    // translation now that the triode's grid switch is gone, so the round-trip
    // has to be the identity. The hooks stay so that a property which ever does
    // need one has a single place to say it.
    CHECK(choiceStorageText("taper", "Log") == "Log");
    CHECK(choiceDisplayText("taper", "Log") == "Log");
    CHECK(choiceStorageText("preset", "N") == "N");
    CHECK(choiceDisplayText("preset", "N") == "N");
}

TEST_CASE("A transformer's card is a rule, and the inspector offers what it says",
          "[transformer]") {
    // The transformer is the one model-bearing part whose card is not a name in a
    // registry: `Ideal` and `Real — Auto` turn the turns ratio into numbers when
    // the engine compiles, and `Real — Manual` is where those numbers become the
    // user's. Two halves of the editor lean on that split -- the rows the
    // inspector offers, and the numbers a picker change writes -- and they ask
    // one predicate now, so they cannot drift apart.
    wdf::SchematicComponent xfmr = makeComponent("transformer", {}, 0.0f, 0.0f);

    // The palette's own default is a derived card: nothing to offer, nothing to
    // load. A card the engine derives has no numbers stored beside it, which is
    // what the file says too.
    CHECK_FALSE(transformerCardIsManual(xfmr));
    applyTransformerCard(xfmr);
    CHECK(xfmr.values.count("Lm") == 0);
    CHECK(xfmr.values.count("leakage") == 0);

    // `Real — Manual` is the card the low-level rows belong to, and choosing it
    // has to put numbers there -- a card that can be chosen but not tuned is a
    // picker that lies.
    xfmr.properties["model"] = "Real — Manual";
    CHECK(transformerCardIsManual(xfmr));
    applyTransformerCard(xfmr);
    CHECK(xfmr.values.count("Lm") == 1);
    CHECK(xfmr.values.count("leakage") == 1);
    CHECK(xfmr.values.count("dcrPrimary") == 1);
    CHECK(xfmr.values.count("dcrSecondary") == 1);

    // The seed is the card the turns ratio *in force* implies: the same resolver
    // the converter runs, so the part that is drawn and the part that is
    // compiled are the same transformer rather than two different ones.
    wdf::TransformerSpec expected;
    expected.turnsPrimary = xfmr.values.at("turnsPrimary");
    expected.turnsSecondary = xfmr.values.at("turnsSecondary");
    expected.model = "auto";
    const auto card = wdf::resolveTransformerSpec(expected);
    CHECK(xfmr.values.at("Lm") == Approx(card.Lm));
    CHECK(xfmr.values.at("leakage") == Approx(card.leakage));
    CHECK(xfmr.values.at("dcrPrimary") == Approx(card.dcrPrimary));
    CHECK(xfmr.values.at("dcrSecondary") == Approx(card.dcrSecondary));

    // A transformer the user already tuned starts from *its own* ratio, not the
    // palette's 1:1 -- a 4:1 is a different card, and the numbers the engine
    // would compile are the 4:1 ones. `Manual` on its own is what an older saved
    // file writes, and it means the same card.
    auto tuned = makeComponent("transformer", {}, 0.0f, 0.0f);
    tuned.values["turnsPrimary"] = 4.0;
    tuned.values["turnsSecondary"] = 1.0;
    tuned.properties["model"] = "Manual";
    CHECK(transformerCardIsManual(tuned));
    applyTransformerCard(tuned);
    wdf::TransformerSpec wide;
    wide.turnsPrimary = 4.0;
    wide.turnsSecondary = 1.0;
    wide.model = "auto";
    const auto wideCard = wdf::resolveTransformerSpec(wide);
    CHECK(tuned.values.at("Lm") == Approx(wideCard.Lm));
    CHECK(tuned.values.at("dcrPrimary") == Approx(wideCard.dcrPrimary));

    // The optional cross-coupling is not part of any card, so it is not invented
    // as 0: a row that reads zero is a control that does nothing.
    CHECK(tuned.values.count("cp") == 0);
    CHECK(tuned.values.count("rc") == 0);

    // `Ideal` is a numerical limit, not a card the user winds, so it offers no
    // rows -- and moving back to a derived card leaves whatever a file carried
    // alone, because the file format does not change shape to suit the
    // inspector.
    auto ideal = makeComponent("transformer", {}, 0.0f, 0.0f);
    ideal.properties["model"] = "Ideal";
    CHECK_FALSE(transformerCardIsManual(ideal));
    applyTransformerCard(ideal);
    CHECK(ideal.values.count("Lm") == 0);

    // The centre-tapped shape speaks the same card vocabulary.
    auto ct = makeComponent("transformer_ct", {}, 0.0f, 0.0f);
    ct.properties["model"] = "Real — Manual";
    CHECK(transformerCardIsManual(ct));
    applyTransformerCard(ct);
    CHECK(ct.values.count("Lm") == 1);

    // A part that is not a transformer is untouched: the picker hook runs for
    // every change, and answering for a resistor would put a winding row on it.
    auto res = makeComponent("res", {}, 0.0f, 0.0f);
    CHECK_FALSE(transformerCardIsManual(res));
    applyTransformerCard(res);
    CHECK(res.values.count("Lm") == 0);
}

TEST_CASE("Every model-bearing part type is covered by the list above") {
    // If a new model-bearing part type is added to the palette, it has to be
    // added here too -- otherwise the checks above quietly stop covering it.
    // The failure this prevents is the one the user reported: a part with
    // variants and no picker.
    std::set<std::string> covered;
    for (const auto& mb : modelBearingTypes())
        covered.insert(mb.type);

    for (const auto& spec : partPalette()) {
        // A part is model-bearing if the editor gives it a default identity.
        wdf::SchematicComponent comp = makeComponent(spec.type, {}, 0.0f, 0.0f);
        const bool hasModel = !livewdf::editor::modelOf(comp).empty();

        INFO("type: " << spec.type);
        if (hasModel)
            CHECK(covered.count(spec.type) > 0);
    }
}
