#include "format/PartModels.h"

#include "devices/Bjt.h"
#include "devices/Diode.h"
#include "devices/Jfet.h"
#include "devices/Mosfet.h"
#include "devices/MosfetPresets.h"
#include "devices/OpAmp.h"
#include "devices/Pentode.h"
#include "devices/QuadricTriode.h"
#include "format/TransistorPolarity.h"
#include "format/Document.h"
#include "format/ValueFormat.h"
#include "spice/Circuit.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace livewdf::editor {

namespace {

constexpr const char* kNChannel = "N-channel";
constexpr const char* kPChannel = "P-channel";

/// The card names are compared lower-cased: the picker writes `Real — Manual`
/// while an older file writes `Manual` and a hand-typed file may write either.
std::string lowered(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// The taper names, in the order the converter reads them. These are the three
/// curves `PotTaper` has, and they are spelled the way `LiveWDFFile.h` spells
/// them, because that is what the file format stores.
const char* const kTaperNames[] = {"Linear", "Log", "RevLog"};

}  // namespace

std::vector<std::string> modelChoices(ChoiceKind kind) {
    std::vector<std::string> out;

    switch (kind) {
        case ChoiceKind::DiodeModel:
            for (int i = 0; i < wdf::kNumDiodes; ++i)
                out.emplace_back(wdf::kDiodeTable[i].name);
            break;

        case ChoiceKind::TriodeModel:
            for (int i = 0; i < wdf::kNumQuadricTriodes; ++i)
                out.emplace_back(wdf::kQuadricTriodeTable[i].name);
            out.emplace_back("Custom");
            break;

        case ChoiceKind::PentodeModel:
            for (const auto& entry : wdf::kPentodeTable)
                out.emplace_back(entry.name);
            out.emplace_back("Custom");
            break;

        case ChoiceKind::JfetModel:
            for (int i = 0; i < wdf::kNumJfets; ++i)
                out.emplace_back(wdf::kJfetTable[i].name);
            break;

        case ChoiceKind::MosfetPreset:
            for (const auto& preset : wdf::kMosfetPresets)
                out.emplace_back(preset.name);
            break;

        case ChoiceKind::BjtModel:
            for (int i = 0; i < wdf::kNumBjts; ++i)
                out.emplace_back(wdf::kBjtTable[i].name);
            break;

        case ChoiceKind::OpAmpModel:
            for (int i = 0; i < wdf::kNumOpAmpTypes; ++i)
                out.emplace_back(wdf::kOpAmpTypeNames[i]);
            break;

        case ChoiceKind::Polarity:
            out.emplace_back(kNChannel);
            out.emplace_back(kPChannel);
            break;

        case ChoiceKind::TransformerModel:
            out.emplace_back("Ideal");
            out.emplace_back("Real — Auto");
            out.emplace_back("Real — Manual");
            break;

        case ChoiceKind::Taper:
            for (const char* name : kTaperNames)
                out.emplace_back(name);
            break;

        case ChoiceKind::SourceMode:
            out.emplace_back(sourceModeName(wdf::SourceMode::DC));
            out.emplace_back(sourceModeName(wdf::SourceMode::AC));
            break;

        case ChoiceKind::None:
            break;
    }

    return out;
}

ChoiceKind choiceKindFor(const std::string& type, const std::string& key) {
    // The taper is a property of a pot, and only a pot. The device name is a
    // property of the part that *is* that device.
    if (key == "taper")
        return (type == "pot" || type == "varres") ? ChoiceKind::Taper : ChoiceKind::None;

    // A source's mode: DC or AC, one or the other.
    if (key == "mode")
        return (type == "src" || type == "isrc") ? ChoiceKind::SourceMode : ChoiceKind::None;

    // Explicit polarity choice for JFET/MOSFET -- shows N-channel/P-channel picker.
    if (key == wdf::kPolarityProperty)
        return (type == "jfet" || type == "mosfet") ? ChoiceKind::Polarity : ChoiceKind::None;

    if (key != "model" && key != "preset")
        return ChoiceKind::None;

    if (key == "model" && (type == "transformer" || type == "transformer_ct"))
        return ChoiceKind::TransformerModel;

    if (type == "diode")
        return ChoiceKind::DiodeModel;
    if (type == "triode")
        return ChoiceKind::TriodeModel;
    if (type == "pentode")
        return ChoiceKind::PentodeModel;
    if (type == "jfet" && key == "model")
        return ChoiceKind::JfetModel;
    if (type == "mosfet" && key == "preset")
        return ChoiceKind::MosfetPreset;
    if (type == "bjt" && key == "model")
        return ChoiceKind::BjtModel;
    if (type == "opamp" && key == "model")
        return ChoiceKind::OpAmpModel;

    return ChoiceKind::None;
}

std::vector<std::string> propertyChoices(const std::string& type, const std::string& key) {
    return modelChoices(choiceKindFor(type, key));
}

// No property needs a display/storage translation now that the triode's grid
// switch is gone; the hooks stay so the editor has one place to add one.
std::string choiceDisplayText(const std::string&, const std::string& stored) {
    return stored;
}

std::string choiceStorageText(const std::string&, const std::string& displayed) {
    return displayed;
}

// A transformer's card is not a name in a registry: it is a turns ratio plus
// three ways of turning that ratio into numbers. `Ideal` and `Real — Auto` do
// the turning themselves, so the low-level keys are the user's only under
// `Manual` -- and the picker has to make those numbers exist, or `Real — Manual`
// is a card that can be chosen but not tuned.
bool transformerCardIsManual(const wdf::SchematicComponent& part) {
    if (part.type != "transformer" && part.type != "transformer_ct")
        return false;

    // `Ideal` wins over `Manual` in a name that somehow says both, which is the
    // rule the resolver uses: an ideal card is a numerical limit, and its
    // winding numbers are not the user's to edit either way.
    const std::string model = lowered(wdf::componentProperty(part, "model", "Real — Auto"));
    return model.find("ideal") == std::string::npos &&
           model.find("manual") != std::string::npos;
}

void applyTransformerCard(wdf::SchematicComponent& part) {
    if (part.type != "transformer" && part.type != "transformer_ct")
        return;

    if (!transformerCardIsManual(part))
        return;  // The derived cards have no low-level numbers to load.

    // Seed from the ratio the user actually set, so the card they start from is
    // the one that describes their transformer rather than the palette's 1:1.
    // A missing or nonsense turn falls back to 1, because the palette always
    // writes both keys and a hand-edited file that does not is exactly what has
    // to keep working.
    auto turns = [&](const char* key) {
        const auto it = part.values.find(key);
        if (it != part.values.end() && it->second > 0.0 && std::isfinite(it->second))
            return it->second;
        return 1.0;
    };

    wdf::TransformerSpec seed;
    seed.turnsPrimary = turns("turnsPrimary");
    seed.turnsSecondary = turns("turnsSecondary");
    seed.model = "auto";  // the card to seed from; Manual keeps what it is given

    try {
        const wdf::TransformerSpec card = wdf::resolveTransformerSpec(seed);
        part.values["Lm"] = card.Lm;
        part.values["leakage"] = card.leakage;
        part.values["dcrPrimary"] = card.dcrPrimary;
        part.values["dcrSecondary"] = card.dcrSecondary;
        // `cp` and `rc` stay out: they are optional extras that a card does not
        // derive, so inventing 0 for them would add rows that say "nothing".
    } catch (const std::invalid_argument&) {
        // Turns that are not finite and positive are already what they are; the
        // build reports them, and inventing a card for them would describe a
        // different circuit than the one drawn.
    }
}

TransistorFacts transistorFacts(const wdf::SchematicComponent& part) {
    TransistorFacts facts;

    if (part.type != "bjt" && part.type != "jfet" && part.type != "mosfet")
        return facts;

    const wdf::TransistorPolarity polarity = wdf::transistorPolarity(part);
    switch (polarity) {
    case wdf::TransistorPolarity::NType:
        facts.type = part.type == "bjt" ? "NPN" : "N-channel";
        break;
    case wdf::TransistorPolarity::PType:
        facts.type = part.type == "bjt" ? "PNP" : "P-channel";
        break;
    case wdf::TransistorPolarity::Unknown:
        facts.type = "Unknown";
        break;
    }

    if (part.type == "bjt") {
        const std::string model = wdf::componentProperty(part, "model", "2N3904");
        if (const auto* entry = wdf::findBjtByName(model.c_str())) {
            const auto& m = entry->model;
            facts.details = "Is " + formatValue(m.Is, "A") +
                            "  \u2002  Bf " + formatValue(m.Bf, "") +
                            "  \u2002  Br " + formatValue(m.Br, "");
        }
    } else if (part.type == "jfet") {
        const std::string model = wdf::componentProperty(part, "model", "2N5952");
        if (const auto* entry = wdf::findJfetByName(model.c_str())) {
            const auto& p = entry->model.p;
            facts.details = "Beta " + formatValue(p.Is0 / (p.Vp * p.Vp), "A/V^2") +
                            "  \u2002  Vto " + formatValue(p.Vp, "V") +
                            "  \u2002  Lambda " + formatValue(p.lambda, "1/V");
        } else if (!model.empty() && model != "Custom") {
            // Hand-edited custom JFET -- show the stored values
            const auto beta = part.values.find("beta");
            const auto vto = part.values.find("vto");
            const auto lambda = part.values.find("lambda");
            if (beta != part.values.end() && vto != part.values.end() && lambda != part.values.end()) {
                facts.details = "Beta " + formatValue(beta->second, "A/V^2") +
                                "  \u2002  Vto " + formatValue(vto->second, "V") +
                                "  \u2002  Lambda " + formatValue(lambda->second, "1/V");
            }
        }
    } else if (part.type == "mosfet") {
        const std::string preset = wdf::componentProperty(part, "preset", "2N7000");
        if (const auto* entry = wdf::findMosfetPreset(preset)) {
            facts.details = "K " + formatValue(entry->K, "A/V^2") +
                            "  \u2002  Vth " + formatValue(entry->Vth, "V") +
                            "  \u2002  Lambda " + formatValue(entry->lambda, "1/V");
        } else {
            // Custom MOSFET -- show stored values
            const auto k = part.values.find("k");
            const auto vth = part.values.find("vth");
            const auto lambda = part.values.find("lambda");
            if (k != part.values.end() && vth != part.values.end() && lambda != part.values.end()) {
                facts.details = "K " + formatValue(k->second, "A/V^2") +
                                "  \u2002  Vth " + formatValue(vth->second, "V") +
                                "  \u2002  Lambda " + formatValue(lambda->second, "1/V");
            }
        }
    }

    return facts;
}

}  // namespace livewdf::editor