#pragma once

// The `.livewdf` document model: parts, wires, probes, and the questions the
// editor and plugin ask of a sheet. Serialization and circuit conversion are
// implemented in `DocumentIO.cpp` and `DocumentCircuit.cpp`; the declarations
// stay here so the model has one public shape.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "spice/Circuit.h"

namespace wdf {
// ==============================================================================
// LiveWDF Document Structures
// ==============================================================================

struct VisualPlacement {
    float x = 0.0f;
    float y = 0.0f;
    int rotation = 0; // 0, 90, 180, 270 degrees
    bool flipped = false;
};

struct SchematicComponent {
    std::string id;       // Unique component identifier, e.g. "R_plate", "V1", "POT_gain"
    std::string type;     // "res", "cap", "ind", "pot", "diode", "triode", "pentode", "transformer", "transformer_ct", "vcvs", "src", "isrc", ...

    /// Which net each pin is on, by pin name (`format/PartLibrary.h`'s order).
    ///
    /// **Filled at load from the drawing, never read from a file.** A `.livewdf`
    /// file carries no pin names at all -- see the version note -- and a
    /// component's `pins` is what `DocumentIO` derives from the geometry via
    /// `format/SchematicNets.h`. It stays a field rather than a question because
    /// everything that asks about connectivity (`toCircuit`, `inputNet()`,
    /// `unconnectedPins()`, the plugin) wants this answer many times per frame,
    /// and deriving it from the drawing is exactly as cheap as storing it was;
    /// what is not cheap-and-honest is keeping a *stored* copy that can disagree
    /// with the drawing it came with.
    std::map<std::string, std::string> pins;

    std::map<std::string, double> values;    // e.g. "value", "totalR", "pos", "gain", "ro"

    /// Free-form part facts: "model", "taper", "label", "select", "mode"...
    ///
    /// A *display* vocabulary: nothing here says which net a part sits on. A rail
    /// that used to carry a `node` was naming a net from a property -- the
    /// floating-node bug the version note describes -- and the migration dropped
    /// the field, since the drawing already says what the name meant.
    std::map<std::string, std::string> properties;

    VisualPlacement visual;
};

// ---------------------------------------------------------------------------
// A source's mode: DC or AC.
//
// **A source is one or the other, not both.** It used to carry a DC level *and*
// an AC amplitude, side by side, which is not a choice but a contradiction: the
// sheet said "0 V DC and 1 V at 1 Hz" and the reader had to guess which one was
// meant. The mode says which, and everything else follows from it -- the
// inspector shows only that mode's fields, and the compiled circuit carries only
// that mode's numbers.
//
// It lives in the model layer rather than in the editor because it is a fact
// about the *document*: the editor, the plugin and the file format all have to
// agree on it.
// ---------------------------------------------------------------------------

enum class SourceMode {
    DC,  ///< a constant voltage
    AC,  ///< a sine: an amplitude and a frequency
};

/// The name of a source's mode, as it is stored in the document ("DC" or "AC").
inline std::string sourceModeName(SourceMode mode) {
    return mode == SourceMode::AC ? "AC" : "DC";
}

/// The mode a source is in, read from its `properties["mode"]`.
///
/// A source with no mode set is a **DC** source: the plainest reading, and the
/// one that needs no setting. A document written before the mode existed
/// therefore keeps working, and keeps meaning what it meant.
inline SourceMode sourceModeOf(const SchematicComponent& part) {
    const auto it = part.properties.find("mode");
    if (it == part.properties.end())
        return SourceMode::DC;
    return it->second == "AC" ? SourceMode::AC : SourceMode::DC;
}

/// The value keys a source shows, for the mode it is in.
///
/// DC: the DC level. AC: the amplitude and the frequency. The other mode's keys
/// stay in the document -- the file format does not change shape because the
/// inspector hides a field -- but they are not *offered*, because offering both
/// is what made the source ambiguous in the first place.
inline std::vector<std::string> sourceValueKeys(SourceMode mode) {
    if (mode == SourceMode::AC)
        return { "ac", "freq" };
    return { "dc" };
}

/// The two resistances of an SPDT throw. Celine's switch constants: a closed
/// throw is ten milliohms, an open one is a gigaohm -- high enough to read as
/// open, low enough to keep the node tied to something for the solver.
inline constexpr double kSpdtClosedOhms = 0.01;
inline constexpr double kSpdtOpenOhms = 1.0e9;

/// True when a `switch_res` is on throw B.
///
/// The document stores `select` as "0" (throw A, the default) or "1" (throw B).
/// Celine's SPDT stores the complementary `closed` flag -- closed means throw A
/// -- so the converter, the inspector and the plugin ask here rather than
/// spelling the inversion out in several places.
inline bool switchResSelectsB(const SchematicComponent& part) {
    const auto it = part.properties.find("select");
    return it != part.properties.end() && it->second == "1";
}

struct WirePoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct SchematicWire {
    /// The net this wire runs in. **Derived at load, never stored** -- see the
    /// version note and `DerivedNets::netOfWire`, which is where the answer
    /// comes from. The field exists so the questions above (`unconnectedPins()`)
    /// and the editor's "which net does this wire draw on" can be answered per
    /// frame without re-running the union-find.
    std::string net;

    /// The routed polyline, in grid units. One polyline is one drawn line: its
    /// segments all join, and a pin landing along any of them is joined too.
    std::vector<WirePoint> points;
};

/// A probe that names a net rather than watching one that is drawn.
///
/// **Legacy, and never written by version 3.** A probe is a `scope` part on the
/// sheet -- a thing in the drawing -- and `allProbes()`/`probeNet()` already
/// answer it from that part's `probe` pin. What this struct stores instead is a
/// net *name*, which is a second account of connectivity: it can name a net the
/// drawing does not make, the same way a stale `pins` map can. So a v3 file that
/// carries a non-empty `probes` array is refused, and a probe that should be
/// watching is placed as a `scope` part where the user wants it. The field and
/// this struct stay for documents built in memory (a test or a caller that
/// already knows the net it wants), where naming a net is not a lie about a
/// drawing.
struct ProbeMarker {
    std::string name;
    std::string node;
    std::string colorHex = "#00D4FF"; // Default cyan
};

// ============================================================================
// The format version, and what a `.livewdf` file stores
// ============================================================================
//
// **Version 3 stores a drawing and nothing else about connectivity**: parts
// (type, placement, values, properties) and wires (routed polylines). What a
// pin is connected to is answered by where it is drawn -- coincident points, a
// wire, a point landing along a wire -- by `format/SchematicNets.h`, the way
// Celine's `Schematic::extractNets()` and LiveSPICE's `RebuildNodes()` do it.
//
// Versions 1 and 2 stored *connectivity as strings*: a `pins` map on every
// component and a `net` on every wire. That is a second account of the drawing,
// and it can say something the drawing does not -- a pin named onto a net it
// was drawn nowhere near (a floating node), or two rails claiming one net for
// conflicting values. Those are not bugs to work around; they are documents
// that do not describe what was drawn. So version 3 **refuses** them, plainly,
// rather than quietly re-reading them behind the user's back: a file that gets
// silently reconnected is a file the user cannot see the difference in, and
// every "it worked when I opened it" story becomes a bug hunt. A legacy sheet
// is migrated by stripping its names to get the drawing, not by a converter
// that invents connections to keep an old netlist intact.
//
// Version 2 moved a DC rail's pin from the right-hand lead to the centre of its
// line. Under version 1 a rail was moved by two grids at load so its drawn wire
// stayed joined; there is no longer anything to keep in step with, because a
// rail's pin position is a table fact (`format/PartLibrary.h`) that both the
// symbol and the reader ask, and a document whose rails were drawn against the
// old symbol is refused rather than shifted behind the user's back.
inline constexpr int kLiveWDFVersion = 3;

struct LiveWDFDocument {
    int version = kLiveWDFVersion;
    std::string name = "Untitled Circuit";
    std::string category = "Custom";
    std::string author = "LiveWDF User";
    std::string description = "";
    // Permit Preview to test a compensated input pad over a verified clean range.
    bool allowPreviewInputPad = false;
    // Optional user-selectable Preview attenuation with reciprocal wet output gain, in dB.
    double previewPadDb = 0.;
    static constexpr bool validPreviewPadDb(double db) noexcept { return db >= 0. && db <= 24.; }

    // Standard I/O Nodes
    //
    // The *fallback* names, for a document written by hand with no terminals on
    // the sheet. A document that has an Input or an Output terminal is
    // answered by that terminal, and these are only consulted when it has
    // neither -- see `inputNet()` and `outputNet()`.
    std::string inputNode = "in";
    std::string outputNode = "out";
    std::string groundNode = "0";

    std::vector<SchematicComponent> components;
    std::vector<SchematicWire> wires;

    /// Legacy probes -- see `ProbeMarker`. A v3 document's probes are `scope`
    /// parts in `components`, and a file that brings this array non-empty is
    /// refused.
    std::vector<ProbeMarker> probes;

    // Plugin Layout: Custom order of live controls (knobs & switches)
    std::vector<std::string> potOrder;
    std::vector<std::string> switchOrder;

    /// What a load had to say about the drawing, in Celine's sense: not a
    /// refusal (the sheet loaded and means what it draws) and not silence (the
    /// sheet would build differently from how it looks).
    ///
    /// A rail drawn on the air, a part whose type this build does not have, a
    /// pin left dangling -- these are facts about the *drawing*, and the user
    /// needs them before the solver refuses over a singular matrix. Every
    /// caller that loads prints these; leaving them here rather than returning
    /// them is what lets a load that succeeded still be a load worth talking
    /// about.
    std::vector<std::string> loadWarnings;

    // -------------------------------------------------------------------------
    // Where the signal goes in and out.
    //
    // **Why these live on the document.** The editor and the plugin both need
    // to know the same two things -- which net the audio enters, and which net
    // it leaves -- and they must agree. The document is the one place they
    // already share, so the questions are asked here rather than answered
    // twice: a copy in the plugin and a copy in the editor would be two
    // answers to one question, and they would disagree the first time one of
    // them was changed.
    //
    // The answer is the Input and Output *terminals* on the sheet. The
    // fallback names above are for a document with no terminals, which is what
    // a hand-written file looks like.
    // -------------------------------------------------------------------------

    /// The component of type `type`, or nullptr when the sheet has none.
    const SchematicComponent* terminal(const std::string& type) const {
        for (const auto& comp : components)
            if (comp.type == type)
                return &comp;
        return nullptr;
    }

    /// The net the audio enters: the Input terminal's pin, or the fallback
    /// name when the sheet has no Input terminal.
    std::string inputNet() const {
        if (const auto* in = terminal("in")) {
            const auto it = in->pins.find("sig");
            if (it != in->pins.end() && !it->second.empty())
                return it->second;
        }
        return inputNode;
    }

    /// The net the audio leaves: the Output terminal's pin, or the fallback
    /// name when the sheet has no Output terminal.
    std::string outputNet() const {
        if (const auto* out = terminal("out")) {
            const auto it = out->pins.find("sig");
            if (it != out->pins.end() && !it->second.empty())
                return it->second;
        }
        return outputNode;
    }

    /// The id of the Input terminal, or "" when the sheet has none. The plugin
    /// needs it because the Input converts to a *source* named after it, and a
    /// source is what the engine drives.
    std::string inputId() const {
        const auto* in = terminal("in");
        return in ? in->id : std::string{};
    }

    /// The parts that are joined to nothing at all -- an island.
    ///
    /// **An island is not in the circuit.** A part whose every terminal is on
    /// a net of its own is not connected to the circuit it is drawn on, and
    /// compiling it in makes the solver's matrix singular: the part's two
    /// terminals are joined by nothing but the part itself, which is a loop
    /// with no reference to ground in it.
    ///
    /// So the circuit skips them, and the editor says which ones it skipped
    /// rather than leaving the user to work it out from a refusal.
    ///
    /// The parts stay on the sheet and in the file: a part the user has pulled
    /// out of the circuit is a thing they drew, and it is not the editor's
    /// business to delete it.
    std::set<std::string> isolatedParts() const {
        // The loose terminals, by part.
        std::map<std::string, std::vector<std::string>> loose;
        for (const auto& [partId, pin] : unconnectedPins())
            loose[partId].push_back(pin);

        std::set<std::string> islands;

        for (const auto& comp : components) {
            const auto it = loose.find(comp.id);
            if (it == loose.end())
                continue;   // nothing loose: the part is connected

            // A part is an island when *every* terminal it has is loose. A
            // part with one loose terminal is still connected by its others.
            std::size_t terminals = 0;
            for (const auto& [pin, net] : comp.pins)
                if (!net.empty())
                    ++terminals;

            if (terminals == it->second.size())
                islands.insert(comp.id);
        }

        return islands;
    }

    /// The parts that are joined to nothing, as (componentId, pin) pairs.
    ///
    /// **Why this is asked of the document and not of the engine.** The user is
    /// looking at a *drawing*, and the question they have is "which part is not
    /// connected?". The engine's answer is a matrix and this is the drawing's.
    ///
    /// The rule: a net with only one thing on it is a part that connects to
    /// nothing. Two nets are not reported, because neither is a connection
    /// that is missing:
    ///
    ///   * the **ground**, which is a reference rather than a connection; and
    ///   * an **ideal source's** own terminals, which are the same node at DC
    ///     (a source with nothing on its far end drives nothing, which is
    ///     legal and not the engine's business).
    ///
    /// An SPDT's unused throw is the other exception. It is a spare contact,
    /// not a missing connection, so it is reported only when the whole switch
    /// is an island.
    ///
    /// A part whose *every* terminal is on such a net is an island, and an
    /// island is what the solver cannot do without; a part with one loose end
    /// is a warning. The islands come first, so a message that names a part
    /// names the one most likely to be the reason a build failed.
    std::vector<std::pair<std::string, std::string>> unconnectedPins() const {
        // How many things each net has, and whether an ideal source reaches it.
        //
        // **A wire counts.** A net with a terminal on it and a wire running away
        // from it has two things on it -- the terminal and the wire -- so it is
        // not dangling: the wire is there, it just ends in the air. A net with
        // only a terminal on it has nothing at all, and that is the case worth
        // saying out loud.
        std::map<std::string, int> things;
        std::map<std::string, bool> drivenBySource;

        const auto isSource = [](const std::string& type) {
            return type == "src" || type == "in";
        };

        for (const auto& comp : components) {
            for (const auto& [pin, net] : comp.pins) {
                if (net.empty())
                    continue;

                ++things[net];
                if (isSource(comp.type))
                    drivenBySource[net] = true;
            }
        }

        // The wires are things on a net too: a wire's net is the net it runs
        // in, and a wire that reaches nothing is a wire that connects nothing.
        for (const auto& wire : wires) {
            if (!wire.net.empty())
                ++things[wire.net];
        }

        const std::string ground = groundNode.empty() ? std::string("0") : groundNode;

        // The parts on those nets, and whether each is an island.
        std::vector<std::pair<std::string, std::string>> loose;
        std::set<std::string> islands;

        for (const auto& comp : components) {
            bool anyConnected = false;
            bool anyLoose = false;

            for (const auto& [pin, net] : comp.pins) {
                if (net.empty())
                    continue;

                const auto it = things.find(net);
                const bool alone = it != things.end() && it->second == 1
                                   && net != ground
                                   && !drivenBySource[net];

                if (alone) {
                    anyLoose = true;
                    loose.emplace_back(comp.id, pin);
                } else {
                    anyConnected = true;
                }
            }

            if (anyLoose && !anyConnected)
                islands.insert(comp.id);
        }

        // **An unused SPDT throw is not a mistake.** The switch has two
        // contacts and an ordinary use wires one; the other is meant to be in
        // the air. If the switch is otherwise in the circuit, drop its dangling
        // throws from the diagnosis so a real failure elsewhere is not blamed
        // on the contact the user did not need. A switch that is an island
        // keeps them: then the whole part is the thing to point at.
        std::map<std::string, const SchematicComponent*> byId;
        for (const auto& comp : components)
            byId[comp.id] = &comp;

        loose.erase(std::remove_if(loose.begin(), loose.end(),
                                   [&](const auto& entry) {
                                       const auto it = byId.find(entry.first);
                                       if (it == byId.end()
                                           || it->second->type != "switch_res"
                                           || islands.count(entry.first) != 0)
                                           return false;
                                       return entry.second == "outA"
                                              || entry.second == "outB";
                                   }),
                    loose.end());

        // The islands first: they are the ones the solver cannot do without.
        std::stable_sort(loose.begin(), loose.end(),
                         [&islands](const auto& a, const auto& b) {
                             return islands.count(a.first) > islands.count(b.first);
                         });

        return loose;
    }

    /// The net the scope probe watches: the "probe" pin of the first Scope Probe
    /// part on the sheet, or "" when the sheet has none.
    ///
    /// **This is the editor's "look here" control.** The scope probe stamps
    /// nothing into the circuit -- watching a node must not change it -- so all
    /// it does is name a net. The editor's scope then traces *that* net instead
    /// of the output, which is what makes the probe a place you put your eye
    /// rather than a badge.
    ///
    /// When the sheet has several probes, the *selected* one is the one being
    /// watched: clicking a probe is how you say "this one". With none selected,
    /// the first on the sheet wins, so a sheet with one probe needs no clicking.
    std::string probeNet() const {
        for (const auto& comp : components) {
            if (comp.type != "scope")
                continue;
            const auto it = comp.pins.find("probe");
            if (it != comp.pins.end() && !it->second.empty())
                return it->second;
        }
        return {};
    }

    /// The net a *named* scope probe watches, or "" when that part is not a
    /// probe. The selection path uses this so clicking a probe is what makes
    /// the scope look there.
    std::string probeNetFor(const std::string& componentId) const {
        if (componentId.empty())
            return {};
        for (const auto& comp : components) {
            if (comp.id != componentId || comp.type != "scope")
                continue;
            const auto it = comp.pins.find("probe");
            if (it != comp.pins.end() && !it->second.empty())
                return it->second;
        }
        return {};
    }

    /// **Every** scope probe on the sheet, as (id, net).
    ///
    /// The single-probe accessors above answer "the probe I am watching"; this
    /// one answers "the probes on the sheet", so the scope can trace all of
    /// them at once -- each on its own colour, so a sheet with three meters
    /// shows three signals rather than one.
    std::vector<std::pair<std::string, std::string>> allProbes() const {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& comp : components) {
            if (comp.type != "scope")
                continue;
            const auto it = comp.pins.find("probe");
            if (it != comp.pins.end() && !it->second.empty())
                out.emplace_back(comp.id, it->second);
        }
        return out;
    }

    /// The scope probe's id, or "" when the sheet has none. The editor's scope
    /// labels its trace with it, so the user can tell *which* probe they are
    /// looking at when there is more than one.
    std::string probeId() const {
        for (const auto& comp : components)
            if (comp.type == "scope")
                return comp.id;
        return {};
    }

    // RMS millivolts of a −18 dBFS sine when a full-scale sample drives one volt.
    static constexpr double kReferenceMillivolts = 89.01946956877224;

    // Between inserts a −18 dBFS sine carries +4 dBu (1228.29 mV RMS).
    static constexpr double kChainVoltsPerFullScale = 1228.292224157363 / kReferenceMillivolts;

    /// Peak volts applied to the input by a full-scale digital sample at unity trim.
    /// Older files sometimes annotate the input with the output-only fullScaleVolts key.
    static double inputVoltsPerFullScale(const SchematicComponent& input) {
        const auto it = input.values.find("referenceMillivoltsRms");
        if (it != input.values.end() && std::isfinite(it->second) && it->second > 0.)
            return it->second / kReferenceMillivolts;
        return 1.;
    }

    double inputVoltsPerFullScale() const {
        const auto* input = terminal("in");
        return input ? inputVoltsPerFullScale(*input) : 1.;
    }

    double previewPeakVolts() const {
        if (const auto* input = terminal("in")) {
            const auto reference = input->values.find("referenceMillivoltsRms");
            if (reference != input->values.end() && std::isfinite(reference->second) && reference->second > 0.)
                return 2. * (reference->second / kReferenceMillivolts);
            const auto scale = input->values.find("fullScaleVolts");
            if (scale != input->values.end() && std::isfinite(scale->second) && scale->second > 1.2)
                return scale->second;
        }
        return 2.;
    }

    /// The voltage the circuit produces at the output when the input is at full
    /// scale -- the number the user writes on the Output terminal.
    ///
    /// This is LiveSPICE's `Speaker.V0dBFS` ("Voltage of the full signal level
    /// at this component"), and it is what stops a preamp that makes 100 volts
    /// from handing 100 to the audio buffer. The plugin divides the output by
    /// it; the circuit is not changed, only scaled.
    ///
    /// Defaults to 1 when there is no Output terminal or it carries no number:
    /// a document that has not said what its output is gets the output as it
    /// is, which is the honest answer.
    double outputFullScaleVolts() const {
        const auto* out = terminal("out");
        if (out == nullptr)
            return 1.0;
        const auto it = out->values.find("fullScaleVolts");
        if (it == out->values.end())
            return 1.0;
        return std::isfinite(it->second) && it->second > 0.0 ? it->second : 1.0;
    }

    /// Fast and Preview solve some devices with cheaper laws; their optional
    /// trims scale the output divisor so every mode lands on the same level.
    double outputTrim(bool preview) const {
        const auto* out = terminal("out");
        if (out == nullptr)
            return 1.0;
        const auto it = out->values.find(preview ? "previewTrim" : "fastTrim");
        return it != out->values.end() && std::isfinite(it->second) && it->second > 0.0 ? it->second : 1.0;
    }

    const SchematicComponent* preampTap() const;
    bool makePreampDocument(LiveWDFDocument& preamp, std::string& error) const;

    /// One source's AC settings: how big the swing is, and how fast it moves.
    struct SourceAc {
        double ac = 0.0;
        double freq = 0.0;
    };

    bool hasAcPowerSupply() const;

    /// The AC settings of every source, **in the order `toCircuit` creates
    /// them** -- the voltage sources first, then the current sources -- so a
    /// driver can pair them with the engine's source-value vector by index.
    ///
    /// The Input terminal is in that list with zeros: it converts to a source
    /// (that is how a WDF engine takes a signal in), but the audio is what
    /// drives it, not a sine.
    ///
    /// Why the settings live here rather than on the compiled source: the
    /// frequency is the *signal's*, not the network's, and the compiled circuit
    /// is a topology description. See `format/AcSource.h`.
    std::vector<SourceAc> sourceAcSettings(const Circuit& circuit) const {
        std::vector<SourceAc> out;
        out.reserve(circuit.src.size() + circuit.isrc.size());
        const auto append = [&](const auto& sources) {
            for (const auto& source : sources) {
                SourceAc settings;
                const auto part = std::ranges::find(components, source.name, &SchematicComponent::id);
                if (part != components.end() && source.ac != 0.0 && sourceModeOf(*part) == SourceMode::AC) {
                    const auto freq = part->values.find("freq");
                    if (freq != part->values.end())
                        settings = {source.ac, freq->second};
                }
                out.push_back(settings);
            }
        };
        append(circuit.src);
        append(circuit.isrc);
        return out;
    }

    // -------------------------------------------------------------------------
    // Convert to executable wdf::Circuit
    // -------------------------------------------------------------------------
    Circuit toCircuit() const;

    // -------------------------------------------------------------------------
    // JSON Serialization & Deserialization
    // -------------------------------------------------------------------------
    std::string toJson() const;
    static bool fromJson(const std::string& jsonStr, LiveWDFDocument& doc, std::string& error);
    bool saveToFile(const std::string& filepath) const;
    bool loadFromFile(const std::string& filepath, std::string& error);
    static bool loadFromFile(const std::string& filepath, LiveWDFDocument& doc, std::string& error);
};

} // namespace wdf
