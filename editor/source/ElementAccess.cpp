#include "ElementAccess.h"
#include "format/ValueFormat.h"

namespace livewdf::editor {

const char* primaryValueKey(const std::string& type) {
    const PartSpec* spec = findPart(type);
    return spec ? spec->valueKey.c_str() : "value";
}

double valueOf(const wdf::SchematicComponent& part) {
    const auto it = part.values.find(primaryValueKey(part.type));
    return it != part.values.end() ? it->second : 0.0;
}

std::string modelOf(const wdf::SchematicComponent& part) {
    // A MOSFET stores its preset under `preset`, but it is the same kind of
    // identity: the sheet caption and the default-model checks should read it
    // without knowing which key a particular part type happens to use.
    if (part.type == "mosfet") {
        const auto preset = part.properties.find("preset");
        if (preset != part.properties.end())
            return preset->second;
    }
    const auto it = part.properties.find("model");
    return it != part.properties.end() ? it->second : std::string();
}

bool hasModelChoice(const wdf::SchematicComponent& part) {
    // The parts whose sheet identity is a model name rather than a bare number.
    // Kept as an explicit set (not derived from the palette) because it is the
    // *converter* that gives these a default model -- see
    // `LiveWDFDocument::toCircuit`, which reads `properties["model"]` for
    // exactly these six. If another model-bearing type is added there, it is
    // added here in the same edit.
    return part.type == "diode" || part.type == "triode" || part.type == "pentode" ||
           part.type == "jfet" || part.type == "bjt" || part.type == "opamp";
}

bool hasNumericValue(const wdf::SchematicComponent& part) {
    // **A part whose identity is a model has no number to type.** "12AX7" says
    // what the tube is; a number beside it says nothing. So the inspector
    // offers the model picker and nothing else -- and it used to offer both, a
    // "Value" field holding a zero under a picker holding the tube.
    //
    // The question is asked here, at the accessor, rather than by changing the
    // palette's value key: the key is the *file format's* word for a part's
    // number, and the file format must not change shape because the inspector
    // happens to hide the field. (An earlier attempt did exactly that -- an
    // empty value key -- and it broke `primaryValueKey` for those types, which
    // is a contract everything else leans on.)
    if (hasModelChoice(part))
        return false;

    // Otherwise: a part "has a value you type" exactly when its palette entry
    // carries a value key -- i.e. every part but an unknown type. Asked this way
    // rather than by naming types, so adding a part to the palette cannot
    // silently miss this question. (Celine: `hasNumericValue`.)
    const PartSpec* spec = findPart(part.type);
    return spec != nullptr && !spec->valueKey.empty();
}

const std::vector<std::string>& pinNames(const std::string& type) {
    static const std::vector<std::string> kNone;
    const PartSpec* spec = findPart(type);
    return spec ? spec->pins : kNone;
}

int pinCount(const std::string& type) {
    return static_cast<int>(pinNames(type).size());
}

std::string pinNet(const wdf::SchematicComponent& part, const std::string& pinName) {
    const auto it = part.pins.find(pinName);
    return it != part.pins.end() ? it->second : std::string();
}

std::string pinNetAt(const wdf::SchematicComponent& part, int index) {
    const auto& names = pinNames(part.type);
    if (index < 0 || index >= static_cast<int>(names.size()))
        return {};
    return pinNet(part, names[static_cast<size_t>(index)]);
}

bool hasPin(const std::string& type, const std::string& pinName) {
    const auto& names = pinNames(type);
    for (const auto& n : names)
        if (n == pinName)
            return true;
    return false;
}

std::string displayLabel(const wdf::SchematicComponent& part) {
    return partLabel(part.type);
}

std::string partLabelText(const wdf::SchematicComponent& part) {
    const auto it = part.properties.find("label");
    if (it != part.properties.end() && !it->second.empty())
        return it->second;

    // A ground is a marker, not a named part: it has no caption unless the
    // user gives it a label. Everything else falls back to its id.
    return part.type == "gnd" ? std::string() : part.id;
}

std::string valueKeyLabel(const std::string& key) {
    // The keys are the file format's words. These are the ones whose key is
    // not already something a person would want to read on a label.
    if (key == "fullScaleVolts") return "V at full input";
    if (key == "dc")             return "DC volts";
    if (key == "ac")             return "AC volts";
    if (key == "freq")           return "Frequency (Hz)";
    if (key == "pos")            return "Position";
    if (key == "ro")             return "Output resistance";
    if (key == "totalR")         return "Total resistance";
    if (key == "maxR")           return "Max resistance";
    if (key == "initialOhms")    return "Initial resistance";
    if (key == "initialVolts")   return "Initial capacitor voltage (V)";
    if (key == "timingFarads")    return "Timing capacitance (F)";
    if (key == "referenceAmps")  return "Calibrated reference current (A RMS)";
    if (key == "inputOhms")      return "Input resistance (ohm)";
    if (key == "fullGainVolts")  return "Full current gain control (V)";
    if (key == "darkOhms")       return "Dark resistance (ohm)";
    if (key == "brightOhms")     return "Bright resistance (ohm)";
    if (key == "referenceVolts") return "Full light sense (V)";
    if (key == "gamma")          return "Light curve exponent";
    if (key == "attackSeconds")  return "Light response (s)";
    if (key == "releaseSeconds") return "Dark response (s)";
    if (key == "attackTimeConstantSeconds")  return "Attack time constant (s, 0 = shared)";
    if (key == "releaseTimeConstantSeconds") return "Release time constant (s, 0 = shared)";
    if (key == "turnsPrimary")   return "Primary turns";
    if (key == "turnsSecondary") return "Secondary turns";
    if (key == "Lm")             return "Magnetizing Lm (H)";
    if (key == "leakage")        return "Leakage (H)";
    if (key == "dcrPrimary")     return "Primary DCR (ohm)";
    if (key == "dcrSecondary")   return "Secondary DCR (ohm)";
    if (key == "cp")             return "Cross cap (F)";
    if (key == "rc")             return "Cross res (ohm)";
    if (key == "gain")           return "Gain";
    if (key == "stages")         return "Bucket-brigade stages";
    if (key == "clockHz")        return "Clock at zero control (Hz)";
    if (key == "periodMicrosecondsPerVolt") return "Clock period slope (µs/V)";
    if (key == "inputBias")      return "Input center relative to common (V)";
    if (key == "outputBias")     return "Output bias relative to common (V)";
    if (key == "inputSwing")     return "Input swing to clipping (V)";
    if (key == "outputOhms")     return "Output resistance (ohm)";
    // The JFET/MOSFET parameter names are the engine's symbols, but they are
    // displayed to a person; the one-letter keys are capitalised so a column of
    // captions does not read as a list of variable names.
    if (key == "beta")           return "Beta";
    if (key == "vto")            return "Vto";
    if (key == "vth")            return "Vth";
    if (key == "lambda")         return "Lambda";
    if (key == "rd")             return "Rd";
    if (key == "rs")             return "Rs";
    if (key == "cgs")            return "Cgs";
    if (key == "cgd")            return "Cgd";
    if (key == "cds")            return "Cds";
    if (key == "is")             return "Is";
    if (key == "bf")             return "Bf";
    if (key == "br")             return "Br";
    if (key == "nf")             return "Nf";
    if (key == "nr")             return "Nr";

    // Quadric triode coefficients (kP2, kPg, kP).
    if (key == "kp2")            return "kP2";
    if (key == "kpg")            return "kPg";
    if (key == "kp")             return "kP";

    // Pentode Koren parameters.
    if (key == "mu")             return "Mu";
    if (key == "ex")             return "Ex";
    if (key == "kg1")            return "Kg1";
    if (key == "kg2")            return "Kg2";
    if (key == "kp")             return "kp";
    if (key == "kvb")            return "Kvb";
    if (key == "vg2")            return "Vg2";
    if (key == "vmax")           return "Vmax";
    if (key == "count")          return "Count";
    if (key == "gridis")         return "Grid Is";
    if (key == "gridvt")         return "Grid Vt";

    return key;
}

std::string valueText(const wdf::SchematicComponent& part) {
    // A model-bearing part reads better as its model name: "12AX7" says more
    // than "0".
    if (hasModelChoice(part)) {
        const std::string model = modelOf(part);
        if (!model.empty())
            return model;
    }

    // A MOSFET's identity is its preset even though its tuning values stay
    // editable. It is not in `hasModelChoice` because that predicate also hides
    // the numeric rows, which would hide K/Vth/lambda.
    if (part.type == "mosfet") {
        const auto preset = part.properties.find("preset");
        if (preset != part.properties.end() && !preset->second.empty())
            return preset->second;
    }

    // **A transformer reads as its turns ratio, not as one of its numbers.**
    // Its identity is a pair -- `valueOf` answers with the primary turns, so the
    // sheet would print "1" under a part whose whole point is 2:1 or 1:4.
    // `formatRatio` is the pair formatter the inspector's own turns rows already
    // speak, so the sheet and the inspector cannot disagree about the same part.
    if (part.type == "transformer" || part.type == "transformer_ct") {
        const auto primary = part.values.find("turnsPrimary");
        const auto secondary = part.values.find("turnsSecondary");
        if (primary != part.values.end() && secondary != part.values.end())
            return formatRatio(primary->second, secondary->second);
        // A hand-edited file that carries only one of the pair falls through to
        // the plain number below, which is what the inspector shows anyway.
    }

    // **A part with no value has no value to show.** The Input terminal is one:
    // it has no number, and the caption used to read "IN1  0", which said
    // nothing at all and looked like a setting the user had forgotten.
    if (primaryValueKey(part.type)[0] == '\0')
        return {};

    const double v = valueOf(part);
    if (v == 0.0)
        return "0";

    // A pot reads as its *marking* ("A250K"), the way the part is sold and the way
    // the inspector shows it -- which is the one part whose number is more useful
    // said that way than as a bare resistance. (Celine's `valueToLabel` does the
    // same for a pot.)
    const auto taper = part.properties.find("taper");
    if (taper != part.properties.end())
        return formatPotValue(v, taper->second);

    // Everything else is the plain engineering number. **One formatter**: this
    // used to be a second, worse copy here ("1.0M", no u/n/p, trailing zeros
    // kept), which is exactly the kind of divergence the accessor layer exists to
    // stop -- the sheet and the inspector now cannot disagree about what a
    // 1000-ohm part reads as.
    // No unit: our parts have no unit column yet, so a suffix would be a guess.
    return formatValue(v, "");
}

}  // namespace livewdf::editor
