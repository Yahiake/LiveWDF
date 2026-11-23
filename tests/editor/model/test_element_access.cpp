// Tests for the typed accessor layer over `wdf::SchematicComponent`.
//
// Why this exists: the whole point of `ElementAccess` is that the *key* a part's
// number lives under is decided in one place rather than guessed by each reader.
// A regression here is silent -- a caller that reads `values["value"]` for a pot
// gets 0 and draws "0" instead of "100k" -- so the contract is pinned by
// assertion: the primary key, the pin order, the model, and the sheet text.

#include <catch2.hpp>

#include "ElementAccess.h"
#include "format/PartLibrary.h"
#include "devices/MosfetPresets.h"
#include "wdf/Engine.h"

#include <cctype>
#include <set>
#include <string>

using namespace livewdf::editor;

namespace {

/// A fresh, un-wired part of `type`, so tests read against the palette's own
/// defaults rather than a hand-built component.
wdf::SchematicComponent freshPart(const std::string& type) {
    return makeComponent(type, {}, 0.0f, 0.0f);
}

}  // namespace

TEST_CASE("The primary value key follows the part type, not the caller",
          "[access][value]") {
    // Each of these stores its number under a different key; a caller that
    // hard-coded "value" would read 0 for three of them.
    CHECK(std::string(primaryValueKey("res")) == "value");
    CHECK(std::string(primaryValueKey("pot")) == "totalR");
    CHECK(std::string(primaryValueKey("varres")) == "maxR");
    CHECK(std::string(primaryValueKey("ctrlres")) == "initialOhms");
    CHECK(std::string(primaryValueKey("vcvs")) == "gain");

    // An unknown type still answers, so a caller has no null case to handle.
    CHECK(std::string(primaryValueKey("not_a_part")) == "value");
}

TEST_CASE("valueOf reads the part's own number wherever it lives",
          "[access][value]") {
    const auto res = freshPart("res");
    CHECK(valueOf(res) == 1000.0);

    const auto pot = freshPart("pot");
    CHECK(valueOf(pot) == 100000.0);

    const auto rheostat = freshPart("varres");
    CHECK(valueOf(rheostat) == 10000.0);
}

TEST_CASE("A part with a model reads as its model, not as its number",
          "[access][value]") {
    const auto diode = freshPart("diode");
    CHECK(hasModelChoice(diode));
    CHECK(modelOf(diode) == "1N4148");
    CHECK(valueText(diode) == "1N4148");

    const auto triode = freshPart("triode");
    CHECK(hasModelChoice(triode));
    CHECK(valueText(triode) == "12AX7");

    // A resistor has no model, so it reads as its engineering number -- the
    // same string the inspector shows and the parser reads back, because both go
    // through `ValueFormat`.
    const auto res = freshPart("res");
    CHECK_FALSE(hasModelChoice(res));
    CHECK(modelOf(res).empty());
    CHECK(valueText(res) == "1k");
}

TEST_CASE("valueText compresses the number the way the sheet wants",
          "[access][value]") {
    auto cap = freshPart("cap");
    cap.values["value"] = 1e-6;
    // Engineering notation with no trailing zeros: a microfarad reads "1u".
    CHECK(valueText(cap) == "1u");

    auto big = freshPart("res");
    big.values["value"] = 2.2e6;
    CHECK(valueText(big) == "2.2M");

    auto zero = freshPart("res");
    zero.values["value"] = 0.0;
    // Zero is loud, not silent: it reads as a bare "0", the value that refuses
    // to build.
    CHECK(valueText(zero) == "0");
}

TEST_CASE("A pot reads on the sheet as its marking, not as a bare resistance",
          "[access][value][pot]") {
    // The one part whose number is more useful said the way the part is sold.
    // A pot's taper lives in `properties`, so the accessor can tell it apart from
    // a plain resistor without a type check.
    auto pot = freshPart("pot");
    pot.values["totalR"] = 250000.0;
    pot.properties["taper"] = "Log";
    CHECK(valueText(pot) == "A250K");

    pot.properties["taper"] = "Linear";
    CHECK(valueText(pot) == "B250K");

    // The same pot with no taper recorded still reads as a number, so an older
    // file without the property is not mislabelled.
    pot.properties.erase("taper");
    CHECK(valueText(pot) == "250k");
}

TEST_CASE("Pin names come from the palette in the extractor's own order",
          "[access][pins]") {
    // "triode" declares plate, grid, cathode -- Céline's order, which the net
    // extractor walks in exactly this sequence, so a reader that draws a pin must
    // too. (It used to be grid, plate, cathode; the port to Céline's pin table
    // moved plate to first because that is the order Céline's model and its
    // builder both use.)
    CHECK(pinCount("triode") == 3);
    CHECK(pinNames("triode").at(0) == "plate");
    CHECK(pinNames("triode").at(1) == "grid");
    CHECK(pinNames("triode").at(2) == "cathode");

    CHECK(pinCount("bjt") == 3);
    CHECK(pinNames("bjt").at(0) == "collector");
    CHECK(pinNames("bjt").at(1) == "base");
    CHECK(pinNames("bjt").at(2) == "emitter");

    CHECK(pinCount("res") == 2);
    CHECK(pinCount("not_a_part") == 0);
    CHECK_FALSE(hasPin("res", "anode"));
    CHECK(hasPin("diode", "anode"));
}

TEST_CASE("pinNet reads by name and by index, and never invents ground",
          "[access][pins]") {
    auto res = freshPart("res");
    res.pins["a"] = "n1";
    res.pins["b"] = "0";

    CHECK(pinNet(res, "a") == "n1");
    CHECK(pinNet(res, "b") == "0");
    CHECK(pinNetAt(res, 0) == "n1");
    CHECK(pinNetAt(res, 1) == "0");

    // A pin the part does not have is empty, not ground -- "absent" and
    // "grounded" are different answers.
    CHECK(pinNet(res, "cathode").empty());
    CHECK(pinNetAt(res, 7).empty());
}

TEST_CASE("The label shown is the user's label when set, else the id",
          "[access][label]") {
    auto res = freshPart("res");
    CHECK(partLabelText(res) == res.id);  // R1, from the palette's prefix

    res.properties["label"] = "Plate load";
    CHECK(partLabelText(res) == "Plate load");

    // A ground is normally just a marker, but if the user names it, the name is
    // real. This is what makes "every part can have a label" true rather than
    // true-except-ground.
    auto gnd = freshPart("gnd");
    CHECK(partLabelText(gnd).empty());
    gnd.properties["label"] = "Earth";
    CHECK(partLabelText(gnd) == "Earth");
}

TEST_CASE("Every palette part carrying a value key reads as having a value",
          "[access][value]") {
    // Asked of the palette rather than asserted part by part, so a part added
    // later is covered without editing the test.
    for (const auto& spec : partPalette()) {
        const auto part = freshPart(spec.type);

        // A part whose identity is a *model* has no number to type, whatever
        // its value key says: "12AX7" says what the tube is, and a number
        // beside it says nothing. So the inspector offers the model picker and
        // nothing else -- it used to offer both, a "Value" field holding a zero
        // under a picker holding the tube.
        const bool expect = !spec.valueKey.empty() && !hasModelChoice(part);
        CHECK(hasNumericValue(part) == expect);
    }

    // A type that is not in the palette has no value to type.
    CHECK_FALSE(hasNumericValue(freshPart("not_a_part")));

    // And the model-bearing parts are exactly the ones with no number.
    for (const char* type : {"diode", "triode", "pentode", "jfet", "bjt"}) {
        INFO("type: " << type);
        CHECK_FALSE(hasNumericValue(freshPart(type)));
        CHECK(hasModelChoice(freshPart(type)));
    }
}

TEST_CASE("A BJT part converts with its model and its document pins",
          "[access][bjt]") {
    // The engine has had a BJT device for a while; the document converter did
    // not, so a BJT on the sheet could not reach the circuit. The pin names are
    // the palette's contract, so this drives them through the converter rather
    // than trusting that `addBjt` was called with the right nodes.
    wdf::LiveWDFDocument doc;

    auto bjt = freshPart("bjt");
    bjt.pins["collector"] = "collector";
    bjt.pins["base"] = "base";
    bjt.pins["emitter"] = "0";

    // Two loads keep the transistor out of `isolatedParts`: every one of its
    // terminals is on a net that something else also touches (or on ground).
    auto collectorLoad = freshPart("res");
    collectorLoad.pins["a"] = "collector";
    collectorLoad.pins["b"] = "0";
    auto baseLoad = freshPart("res");
    baseLoad.pins["a"] = "base";
    baseLoad.pins["b"] = "0";

    doc.components = {bjt, collectorLoad, baseLoad};

    const auto circuit = doc.toCircuit();
    REQUIRE(circuit.bjt.size() == 1);
    CHECK(circuit.bjt[0].c == "collector");
    CHECK(circuit.bjt[0].b == "base");
    CHECK(circuit.bjt[0].e == "0");
    CHECK(circuit.bjt[0].model.valid());

    // The default model is a real NPN card, not a silently-substituted blank.
    CHECK(modelOf(bjt) == "2N3904");
    CHECK(wdf::findBjtByName(modelOf(bjt)) != nullptr);

    // A PNP card compiles as a PNP, not as an NPN with the same numbers.
    doc.components[0].properties["model"] = "2N3906";
    const auto pnpCircuit = doc.toCircuit();
    REQUIRE(pnpCircuit.bjt.size() == 1);
    CHECK(pnpCircuit.bjt[0].model.pnp);

    // An unknown name still fails loudly rather than becoming a default.
    doc.components[0].properties["model"] = "NotARealBjt";
    CHECK_THROWS(doc.toCircuit());
}

TEST_CASE("The ideal op-amp has no supply pins and compiles to a VCVS",
          "[access][idealopamp]") {
    // The part the "A" key arms: an op-amp with nothing to wire but its signal
    // terminals. It is a separate type precisely so that its pin frame can be
    // three pins while the real cards keep five; `findPart` is the one table
    // both the canvas and the converter read.
    const auto* spec = findPart("idealopamp");
    REQUIRE(spec != nullptr);
    CHECK(spec->pins == std::vector<std::string>{"out", "neg", "pos"});
    CHECK(spec->pinOffsets.size() == 3);
    CHECK(spec->key == "A");

    const auto* card = findPart("opamp");
    REQUIRE(card != nullptr);
    CHECK(card->key.empty());   // the key moved to the ideal part, not copied
    CHECK(card->pins.size() == 5);

    // A non-inverting stage of 1 + Rf/Ri, driven through the document converter
    // rather than `addVCVS` directly: the pin names are the contract.
    wdf::LiveWDFDocument doc;
    doc.inputNode = "in";
    doc.outputNode = "out";

    auto add = [&](const wdf::SchematicComponent& c) {
        doc.components.push_back(c);
    };

    auto vin = freshPart("src");
    vin.pins = {{"pos", "in"}, {"neg", "0"}};
    vin.values = {{"dc", 0.1}};
    add(vin);

    auto ri = freshPart("res");
    ri.pins = {{"a", "neg"}, {"b", "0"}};
    ri.values = {{"value", 10000.0}};
    add(ri);

    auto rf = freshPart("res");
    rf.pins = {{"a", "neg"}, {"b", "out"}};
    rf.values = {{"value", 100000.0}};
    add(rf);

    auto rload = freshPart("res");
    rload.pins = {{"a", "out"}, {"b", "0"}};
    rload.values = {{"value", 1000000.0}};
    add(rload);

    auto u1 = freshPart("idealopamp");
    u1.pins = {{"out", "out"}, {"neg", "neg"}, {"pos", "in"}};
    add(u1);

    const auto circuit = doc.toCircuit();
    CHECK(circuit.opamp.empty());
    REQUIRE(circuit.vcvs.size() == 1);
    CHECK(circuit.vcvs[0].outA == "out");
    CHECK(circuit.vcvs[0].outB == "0");
    CHECK(circuit.vcvs[0].inA == "in");
    CHECK(circuit.vcvs[0].inB == "neg");

    // 0.1 V in, gain 11: the virtual short holds the inverting input at 0.1 V
    // and the output at 1.1 V. The finite 100 dB open-loop gain leaves a
    // fraction-of-a-millivolt error, which is what "ideal" has to mean here.
    wdf::Engine engine(circuit, 48000.0);
    std::vector<double> nodeV, rootV;
    REQUIRE(engine.solveDc(nodeV, rootV));
    const int out = engine.nodeIndex("out");
    REQUIRE(out >= 0);
    CHECK(nodeV.at(static_cast<size_t>(out)) == Approx(1.1).margin(2e-4));
}

TEST_CASE("A transformer reads as its turns ratio, not as one of its turns",
          "[access][transformer]") {
    // The transformer is neither of the two shapes the accessors were built for:
    // it has a picker (so a `model` row exists) *and* two numbers the user types
    // (so the rows are not hidden), and its identity is the *pair*. Printing
    // `valueOf`'s single number put a bare "1" beside a part whose whole point
    // is the ratio.
    const auto xfmr = freshPart("transformer");
    CHECK(hasNumericValue(xfmr));
    CHECK_FALSE(hasModelChoice(xfmr));
    CHECK(valueText(xfmr) == "1:1");

    auto tuned = freshPart("transformer");
    tuned.values["turnsPrimary"] = 2.0;
    tuned.values["turnsSecondary"] = 0.5;
    CHECK(valueText(tuned) == "2:0.5");

    // The centre-tapped shape is the same identity: same pair of keys, and the
    // tap is a pin, not a number.
    CHECK(valueText(freshPart("transformer_ct")) == "1:1");
}

TEST_CASE("A MOSFET reads as its preset and keeps its numeric parameters",
          "[access][mosfet]") {
    auto mosfet = freshPart("mosfet");

    // A MOSFET's identity is its preset, even though K/Vth/lambda stay editable.
    // `hasModelChoice` deliberately stays false because that accessor also hides
    // the numeric rows; `modelOf`/`valueText` carry the identity for the sheet.
    CHECK_FALSE(hasModelChoice(mosfet));
    CHECK(hasNumericValue(mosfet));
    CHECK(modelOf(mosfet) == "2N7000");
    CHECK(valueText(mosfet) == "2N7000");
    CHECK(mosfet.properties.at("polarity") == "N-channel");

    const auto jfet = freshPart("jfet");
    CHECK(jfet.properties.at("polarity") == "N-channel");

    // A P-channel card loads the values and records P-channel.
    const auto* card = wdf::findMosfetPreset("IRF9540N");
    REQUIRE(card != nullptr);
    mosfet.properties["preset"] = card->name;
    mosfet.properties["polarity"] = "P-channel";
    mosfet.values["k"] = card->K;
    mosfet.values["vth"] = card->Vth;
    mosfet.values["lambda"] = card->lambda;
    CHECK(modelOf(mosfet) == "IRF9540N");
    CHECK(valueText(mosfet) == "IRF9540N");
}

TEST_CASE("Palette shortcuts are unique and clear of the tools", "[access][palette]") {
    // The key table is hand-written, so it is exactly the kind of thing that
    // drifts: two parts on the same shortcut means one of them can never be
    // armed from the keyboard, and a key on S/W/X/F means it shadows a tool.
    // Both are silent in a running app and cheap to pin here.
    //
    // S/W/X/F are the tool and verb letters (Select, Wire, Delete, frame) and are
    // matched *before* the part loop, so a part must never use them.
    const std::string reserved = "swxf";

    std::set<std::string> seen;
    for (const auto& spec : partPalette()) {
        INFO("part type: " << spec.type);

        if (spec.key.empty())
            continue;

        // A shortcut is either a bare letter ("R") or a letter with the command
        // modifier ("Ctrl+G"). The *combination* is what must be unique, not the
        // letter: "G" and "Ctrl+G" are two different shortcuts, and both are
        // wanted -- the ground is a bare G and the gain block is Ctrl+G.
        CHECK(seen.insert(spec.key).second);   // not already used

        // A bare letter has to avoid the tool letters. A modified one cannot
        // shadow a tool either -- the command branch claims the key first -- but
        // its letter is still checked, so that a future tool letter is not
        // quietly taken.
        const auto letter = spec.key.find('+') != std::string::npos
                                ? spec.key.substr(spec.key.find('+') + 1)
                                : spec.key;

        REQUIRE(letter.size() == 1);

        const auto k = static_cast<char>(std::tolower(static_cast<unsigned char>(letter[0])));
        CHECK(reserved.find(k) == std::string::npos);
    }
}
