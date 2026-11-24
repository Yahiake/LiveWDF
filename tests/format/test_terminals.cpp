// The Input and Output terminals, and the questions the document answers about
// them (U4.2).
//
// **What these are.** The Input is where the audio enters the circuit and the
// Output is where it leaves. They are *parts*, not conventions: the user places
// them, wires them, and the plugin finds them by looking.
//
// Before this, the plugin had to *guess* where the signal went in: it looked
// for a source called "Vin", or one with a non-zero AC amplitude, and fell back
// to the first source. The signal source -- a DC voltage the circuit is built
// around -- was wearing the input's hat, and a document with two sources had no
// way to say which one the audio belonged to.
//
// **What the document answers.** `inputNet()`, `outputNet()`,
// `outputFullScaleVolts()` -- the three things the editor and the plugin both
// need, asked of the one thing they already share. They live on
// `LiveWDFDocument` rather than in the editor *and* the plugin because two
// copies of one answer is two answers.
//
// **The output voltage.** The Output terminal carries the volts the circuit
// makes at full input, and the plugin divides by it. That is LiveSPICE's
// `Speaker.V0dBFS` -- "Voltage of the full signal level at this component" --
// and it is what keeps a preamp that makes 100 volts from handing 100 to the
// audio buffer.

#include <catch2.hpp>

#include "format/LiveWDFFile.h"

#include <string>

namespace {

/// A document with an Input terminal, a resistor and an Output terminal.
wdf::LiveWDFDocument terminalDocument(double fullScaleVolts = 1.0) {
    wdf::LiveWDFDocument doc;

    wdf::SchematicComponent in;
    in.id = "IN1";
    in.type = "in";
    in.pins = {{"sig", "audio"}};
    doc.components.push_back(in);

    wdf::SchematicComponent res;
    res.id = "R1";
    res.type = "res";
    res.pins = {{"a", "audio"}, {"b", "out"}};
    res.values = {{"value", 1000.0}};
    doc.components.push_back(res);

    wdf::SchematicComponent out;
    out.id = "OUT1";
    out.type = "out";
    out.pins = {{"sig", "out"}};
    out.values = {{"fullScaleVolts", fullScaleVolts}};
    doc.components.push_back(out);

    return doc;
}

}  // namespace

TEST_CASE("The document says where the signal goes in and out") {
    const auto doc = terminalDocument();

    CHECK(doc.inputNet() == "audio");
    CHECK(doc.outputNet() == "out");
    CHECK(doc.inputId() == "IN1");
}

TEST_CASE("A document with no terminals falls back to the named nodes") {
    // A hand-written file, or one written before the terminals existed, has no
    // Input or Output part. The old `io` names are then the answer, and the
    // plugin keeps working on it.
    wdf::LiveWDFDocument doc;
    doc.inputNode = "in";
    doc.outputNode = "out";

    CHECK(doc.inputNet() == "in");
    CHECK(doc.outputNet() == "out");
    CHECK(doc.inputId().empty());
}

TEST_CASE("The Input terminal becomes a source named after the part") {
    // The only way a WDF engine can have a signal injected is through a
    // *source*, so the Input converts to one. Naming it after the part is what
    // lets the plugin find *this* source rather than guess which of the
    // document's sources the audio belongs to.
    const auto doc = terminalDocument();
    const auto circuit = doc.toCircuit();

    REQUIRE(circuit.src.size() == 1);
    CHECK(circuit.src[0].name == "IN1");
    CHECK(circuit.src[0].a == "audio");
    CHECK(circuit.src[0].b == "0");
}

TEST_CASE("The Output terminal stamps nothing into the circuit") {
    // It is a marker that names the node to read. The node exists because some
    // other part's pin is on it.
    const auto doc = terminalDocument();
    const auto circuit = doc.toCircuit();

    // One source -- the Input. No source for the Output, and no extra
    // resistance or anything else.
    CHECK(circuit.src.size() == 1);
    CHECK(circuit.res.size() == 1);
}

TEST_CASE("The Output terminal carries the voltage the circuit makes at full input") {
    // The number the user writes so the plugin does not hand them 100 volts.
    CHECK(terminalDocument(1.0).outputFullScaleVolts() == 1.0);
    CHECK(terminalDocument(240.0).outputFullScaleVolts() == 240.0);
    CHECK(terminalDocument(12.5).outputFullScaleVolts() == 12.5);
}

TEST_CASE("A document that has not said what its output is gets the output as it is") {
    // No Output terminal, or one with no number: the honest answer is 1, which
    // scales nothing.
    wdf::LiveWDFDocument bare;
    CHECK(bare.outputFullScaleVolts() == 1.0);

    wdf::LiveWDFDocument doc = terminalDocument();
    doc.components[2].values.clear();
    CHECK(doc.outputFullScaleVolts() == 1.0);
}

TEST_CASE("A nonsense output voltage is not obeyed") {
    // A zero or negative target would divide by zero, or flip the signal's
    // sign. A document that says something impossible gets the honest answer
    // rather than a NaN in the audio path.
    CHECK(terminalDocument(0.0).outputFullScaleVolts() == 1.0);
    CHECK(terminalDocument(-5.0).outputFullScaleVolts() == 1.0);
}

TEST_CASE("Calibrated mic drive is explicit and survives reopening", "[format][terminals]") {
    auto doc = terminalDocument();
    auto& input = doc.components.front().values;
    input["fullScaleVolts"] = .01;
    REQUIRE(doc.inputVoltsPerFullScale() == 1.);
    input["referenceMillivoltsRms"] = -.1;
    REQUIRE(doc.inputVoltsPerFullScale() == 1.);
    input["referenceMillivoltsRms"] = 7.74;
    wdf::LiveWDFDocument back;
    std::string error;
    REQUIRE(wdf::LiveWDFDocument::fromJson(doc.toJson(), back, error));
    CHECK(back.inputVoltsPerFullScale() == Approx(.08694727161927697));
}

TEST_CASE("The Input and Output terminals survive a save and reopen") {
    // The file format is what carries them between the editor and the plugin,
    // so a terminal that does not survive the round trip is a terminal the
    // plugin cannot find.
    const auto doc = terminalDocument(240.0);
    const auto json = doc.toJson();

    wdf::LiveWDFDocument back;
    std::string error;
    REQUIRE(wdf::LiveWDFDocument::fromJson(json, back, error));

    REQUIRE(back.components.size() == 3);
    // **The reopen names the net the terminal is, not the name the sheet used to
    // carry it.** A `.livewdf` file stores no net names, so the one name the
    // reopen has for the Input's net is the one the terminal itself gives it --
    // `in`, `out`, `0` -- and the hand-written `"audio"` above is a name that
    // only ever existed inside one in-memory document.
    CHECK(back.inputNet() == "in");
    CHECK(back.outputNet() == "out");
    CHECK(back.outputFullScaleVolts() == 240.0);
}

TEST_CASE("A document permits Preview input padding only when explicitly saved") {
    auto document = terminalDocument();
    const auto existing = document.toJson();
    document.allowPreviewInputPad = true;
    wdf::LiveWDFDocument loaded;
    std::string error;
    REQUIRE(wdf::LiveWDFDocument::fromJson(document.toJson(), loaded, error));
    CHECK(loaded.allowPreviewInputPad);
    REQUIRE(wdf::LiveWDFDocument::fromJson(existing, loaded, error));
    CHECK_FALSE(loaded.allowPreviewInputPad);
}
