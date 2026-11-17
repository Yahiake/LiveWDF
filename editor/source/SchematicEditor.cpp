#include "SchematicEditor.h"
#include "SchematicEditorInternal.h"
#include "EditorLookAndFeel.h"
#include "format/PartLibrary.h"
#include "Shortcut.h"
#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace livewdf::editor {
// ============================================================================
// Construction
// ============================================================================

SchematicEditor::SchematicEditor() {
    setWantsKeyboardFocus(true);
    setInterceptsMouseClicks(true, false);
    setMouseCursor(juce::MouseCursor::NormalCursor);

    // A default sheet, so the editor opens on something rather than a void: an
    // **Input terminal**, a resistor, a capacitor and a **ground**, wired as a
    // lowpass. Small enough to read at a glance and enough to show that every
    // gesture works.
    //
    // The Input terminal and not a signal source, and that is the shape of a
    // circuit the *plugin* would play: the audio comes in at the terminal, and
    // the editor's scope shows the input and the response. A signal source is
    // for a circuit that makes its own signal -- an AC generator, a supply --
    // and the editor leaves those alone.
    //
    // **The wires are straight, and that matters more than it looks.** A
    // diagonal wire becomes an L -- across then down -- and the L's *corner* is
    // a point the wire passes through, so a corner that lands on a pin joins
    // it. The first version of this sheet was wired with diagonals, and the
    // corner of the wire from the resistor landed exactly on the capacitor's
    // lower terminal: the capacitor was shorted to ground, the resistor's far
    // end was on the ground with it, and the sheet said "0 nodes" because
    // there was nothing left to solve for. It looked like a lowpass and it was
    // a resistor.
    const std::string vs = schematic_.addPart("in", 2, 10);
    const std::string r = schematic_.addPart("res", 10, 10);
    const std::string c = schematic_.addPart("cap", 12, 12);

    // The ground, drawn under the capacitor's lower terminal. The capacitor's
    // "b" pin is already on net "0", but a sheet that *says* "0" in a net name
    // and shows a floating wire is not a circuit a reader can trust -- the
    // ground symbol is what says "this is earth", and now there is one to draw.
    const std::string g = schematic_.addPart("gnd", 12, 16);

    // **The Output terminal**, so the sheet is the whole shape of a circuit the
    // plugin would play: something in, something out, and the scope has a node
    // to trace.
    const std::string o = schematic_.addPart("out", 16, 10);

    // **The resistor lies down**, so the lowpass reads left to right: the
    // signal comes in from the left, through the resistor, and off to the
    // right, with the capacitor hanging off the node between them.
    if (auto* res = schematic_.findPart(r))
        res->visual.rotation = 270;

    if (auto* src = schematic_.findPart(vs))
        src->pins["sig"] = "in";
    if (auto* res = schematic_.findPart(r)) {
        res->pins["a"] = "in";
        res->pins["b"] = "mid";
    }
    if (auto* cap = schematic_.findPart(c)) {
        cap->pins["a"] = "mid";
        cap->pins["b"] = "0";
    }
    if (auto* out = schematic_.findPart(o))
        out->pins["sig"] = "mid";

    const auto pinOf = [this](const std::string& partId, int pinIndex) {
        const auto* p = schematic_.findPart(partId);
        return p ? Schematic::pinPosition(*p, pinIndex) : PinOffset{};
    };

    // The wire from the Input terminal to the resistor: straight, along the
    // line both pins are on.
    const auto a = pinOf(vs, 0), b = pinOf(r, 0);
    schematic_.addWire(a.x, a.y, b.x, b.y);

    // And from the node between them to the Output terminal: straight too, so
    // its corner cannot land on a pin and short something.
    if (!o.empty()) {
        const auto resPin = pinOf(r, 1), outPin = pinOf(o, 0);
        schematic_.addWire(resPin.x, resPin.y, outPin.x, outPin.y);
    }

    syncDocument();
    rebuildGeometry();
    startTimerHz(30);
}

SchematicEditor::~SchematicEditor() { stopTimer(); }

// ============================================================================
// Model
// ============================================================================

void SchematicEditor::setDocument(const wdf::LiveWDFDocument& doc) {
    doc_ = doc;
    schematic_.fromDocument(doc);
    selectedIds_.clear();
    selectedWireIds_.clear();
    livePotPositions_.clear();
    liveSwitchThrowsB_.clear();
    dirty_ = false;
    rebuildGeometry();

    // **The document is regenerated from the model, so the nets it carries are
    // the ones the drawing actually has.**
    //
    // A file carries a pin's net name, and a pin's net name in a file can be
    // the one it was *born* with rather than the one it *ended up* on -- the
    // two disagree, and the converter, which reads the pins, builds a circuit
    // the drawing does not describe. The model derives the nets from the
    // geometry, so the model is right and the file is not.
    //
    // Without this, a file that was saved by an older editor (or written by
    // hand) loads as a circuit the solver cannot solve, and the only way out
    // is to nudge a wire and rebuild -- which is what it looked like.
    syncDocument();

    frameAll();
    repaint();

    if (onSelectionChanged)
        onSelectionChanged();
}

void SchematicEditor::reloadFromDocument() {
    // Keeps the *selection*: an inspector edit to a part's value should not drop
    // the part you were editing, which is what rebuilding from scratch would do.
    // Selection is by part id, and a reload does not renumber ids, so the ids
    // survive -- only the geometry is rebuilt.
    schematic_.fromDocument(doc_);
    rebuildGeometry();
    repaint();
    if (onSelectionChanged)
        onSelectionChanged();
}

void SchematicEditor::syncDocument() {
    // The document is generated from the model rather than edited alongside it,
    // so the two cannot drift: the model is the truth and the document is its
    // serialised form.
    const auto name = doc_.name;
    const std::string inNode = doc_.inputNode, outNode = doc_.outputNode;
    doc_ = schematic_.toDocument();
    doc_.name = name;
    doc_.inputNode = inNode;
    doc_.outputNode = outNode;
}

void SchematicEditor::setNodeVoltages(const std::map<std::string, double>& v) {
    // The plugin hands us a fresh map at 30 Hz. Repainting the whole sheet when
    // nothing changed makes an idle plugin burn a core for no visible reason.
    if (voltages_ == v)
        return;

    voltages_ = v;
    repaint();
}

void SchematicEditor::setSignalActivity(float level) {
    signalActivity_ = std::clamp(level, 0.0f, 1.0f);
}

void SchematicEditor::setLiveControlState(
    const std::map<std::string, double>& potPositions,
    const std::map<std::string, bool>& switchThrowsB) {
    // The plugin calls this from its 30 Hz timer with a fresh map every tick.
    // Repainting when nothing moved would make an idle editor burn a core for
    // no visible reason, which is the same rule `setNodeVoltages` follows.
    if (livePotPositions_ == potPositions && liveSwitchThrowsB_ == switchThrowsB)
        return;

    livePotPositions_ = potPositions;
    liveSwitchThrowsB_ = switchThrowsB;
    repaint();
}

void SchematicEditor::setProblemPart(const std::string& id) {
    if (problemPart_ == id)
        return;

    problemPart_ = id;
    repaint();
}

void SchematicEditor::timerCallback() {
    // The signal-flow beads: advance the phase and repaint only if something is
    // actually flowing. An idle sheet then costs nothing, which matters because
    // this fires whether or not anyone is looking.
    if (signalActivity_ <= 0.001f || flowPaths_.empty())
        return;

    flowPhase_ = std::fmod(flowPhase_ + 0.04f, 1.0f);
    repaint();
}

// ============================================================================
// Tools
// ============================================================================

void SchematicEditor::setTool(Tool t) {
    tool_ = t;
    if (t != Tool::Place) {
        pendingType_.clear();
        pendingClone_.reset();
        pendingSpec_ = nullptr;
    }

    setMouseCursor(t == Tool::Select ? juce::MouseCursor::NormalCursor
                                     : juce::MouseCursor::CrosshairCursor);

    if (isShowing())
        grabKeyboardFocus();

    repaint();
    if (onToolChanged)
        onToolChanged();
}


std::string SchematicEditor::selectedPartId() const {
    return selectedIds_.empty() ? std::string() : selectedIds_.front();
}

bool SchematicEditor::hasSelection() const noexcept {
    return !selectedIds_.empty() || !selectedWireIds_.empty();
}

const Wire* SchematicEditor::selectedWire(int id) const {
    return schematic_.findWire(id);
}

std::vector<SchematicEditor::ProbeInfo> SchematicEditor::probes() const {
    std::vector<ProbeInfo> result;

    // The model answers "which node is this probe on" directly, so the badge and
    // the netlist cannot disagree about it.
    for (const auto& part : schematic_.getParts()) {
        if (part.type != "scope")
            continue;

        ProbeInfo info;
        info.partId = part.id;
        const auto readout = schematic_.probeReadout(part.id);
        info.node = readout.node;
        info.reference = readout.reference;
        result.push_back(std::move(info));
    }

    return result;
}

// ============================================================================
// Undo
// ============================================================================
//
// The stack itself is the editor's (`SchematicHistory`). What the canvas owes it
// is two facts: whether a drag is still running, and that one has ended. The
// Command-key shortcuts ask the owner through `onUndoRequested`.

bool SchematicEditor::isMidGesture() const noexcept {
    return draggingPart_ || boxSelectionStarted_ || resizingWireEnd_ >= 0 || draggingWire_
           || boxSelecting_ || panning_;
}

void SchematicEditor::adoptRestoredState(const Schematic& state) {
    // The history has already decided what the sheet is; this puts it on screen.
    schematic_ = state;
    selectedIds_.clear();
    selectedWireIds_.clear();
    rebuildGeometry();
    notifyChanged();
    if (onSelectionChanged)
        onSelectionChanged();
}


float SchematicEditor::wireThickness() const noexcept {
    // Heavier as you zoom in, but never thinner than a pixel: a hairline that
    // vanishes when zoomed out makes a connected circuit look unconnected.
    return std::max(1.0f, view_.gridSize * 0.12f);
}

void SchematicEditor::resized() { repaint(); }


} // namespace livewdf::editor