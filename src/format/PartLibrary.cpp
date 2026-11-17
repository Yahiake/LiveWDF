#include "format/PartLibrary.h"

#include "format/PartModels.h"
#include "devices/Bjt.h"
#include "devices/Jfet.h"
#include "devices/MosfetPresets.h"
#include "format/TransistorPolarity.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace livewdf::editor {

namespace {

// The palette's base rows: one per *part type*. Model-bearing types are
// expanded below into one row per device in the engine's registry.
//
// Pin names here are the *document* pin names the converter in
// `LiveWDFFile::toCircuit` reads -- they are not free-form. "res" wants `a`/`b`,
// "triode" wants `grid`/`plate`/`cathode`, "src" wants `pos`/`neg`, and so on.
// Getting one of these wrong does not fail loudly; `getPin` silently falls back
// to ground and the part is quietly mis-wired. So each entry below is written
// against the converter, and `pinSides` is written against where the canvas
// draws that pin.
//
// Sides: 0 left, 1 right, 2 up, 3 down, pre-rotation.
//
// The fields in order: type, label, idPrefix, pins, pinSides, pinOffsets,
// defaultValue, valueKey, key, defaultRotation, defaultFlipped, category
const std::vector<PartSpec>& basePaletteStorage() {
    static const std::vector<PartSpec> kPalette = {
        // clang-format off
        {"res",        "Resistor",     "R",   {"a", "b"},               {0, 1},      {{0, -2}, {0, 2}},         1000.0,     "value",      "R", 0, false, "Passive/Resistors"},
        {"cap",        "Capacitor",    "C",   {"a", "b"},               {0, 1},      {{0, -2}, {0, 2}},         1e-6,       "value",      "C", 0, false, "Passive/Capacitors"},
        {"ind",        "Inductor",     "L",   {"a", "b"},               {0, 1},      {{0, -2}, {0, 2}},         1e-3,       "value",      "L", 0, false, "Passive/Inductors"},
        // `H` above `L` by default, which is the pin frame placed as it is:
        // Céline's (`high` at (0,-2)), whose wiper already sits on the right,
        // so no rotation or mirror is needed to land the wiper there. The
        // two trailing fields are the default placement, kept in the row so
        // the arming path and `makeComponent` seed one promise.
        {"pot",        "Potentiometer","POT", {"high", "wiper", "low"}, {0, 1, 3},   {{0, -2}, {3, 0}, {0, 2}}, 100000.0,   "totalR",     "P", 0, false, "Passive/Potentiometers"},
        {"varres",     "Rheostat",     "VR",  {"a", "b"},               {0, 1},      {{0, -2}, {0, 2}},         10000.0,    "maxR",       "V", 0, false, "Passive/Potentiometers"},
        // The controlled resistor: the plain resistor's body with a line across
        // it, the IEC mark for a resistor whose value is under control. Its pins
        // are the resistor's, top and bottom -- the artwork is drawn for them,
        // and a body with its leads vertical and its pins on the sides is a
        // symbol that cannot be wired sensibly.
        {"ctrlres",    "Controlled R", "CR",  {"a", "b"},               {0, 1},      {{0, -2}, {0, 2}},         1000.0,     "initialOhms","K", 0, false, "Passive/Controlled"},
        // A diode, a triode and a pentode carry a value key like every other
        // part -- the key is what the *file* calls their number, and the file
        // format must not change shape because the inspector happens to hide
        // the field.
        //
        // What hides it is `hasNumericValue`: a part whose identity is a
        // *model* has no number to type, so the inspector offers the model
        // picker and nothing else. It used to offer both -- a "Value" field
        // holding a zero, under a picker holding the tube.
        {"diode",      "Diode",        "D",   {"anode", "cathode"},     {0, 1},      {{0, -2}, {0, 2}},         0.0,        "value",      "D", 0, false, "Semiconductors/Diodes"},
        {"triode",     "Triode",       "V",   {"plate", "grid", "cathode"}, {2, 0, 3}, {{0, -3}, {-3, 0}, {0, 3}}, 0.0,      "value",      "T", 0, false, "Tubes/Triodes"},
        // The screen terminal is on the **right**, opposite the control grid on
        // the left. Céline puts both leads on the left; we split them because on
        // our sheet a wire bound for the screen would have to cross the envelope
        // to reach the far side. The strap stays where Céline puts it, on the
        // **right**, and the moved terminal joins it: the screen's lead stops at
        // the strap, which carries it to its filament, and the grid's lead runs
        // straight in from the left with nothing in its way. The strap is the
        // part's one internal line, so it belongs to whichever terminal needs it
        // rather than to whichever side happens to be free. Pin *names* and the
        // `addPentode(plate, screen, grid, cathode)` argument order are untouched.
        {"pentode",    "Pentode",      "XP",  {"plate", "screen", "grid", "cathode"}, {2, 1, 0, 3}, {{0, -3}, {3, 0}, {-3, 1}, {0, 3}}, 0.0, "value", "E", 0, false, "Tubes/Pentodes"},
        // A generic three-terminal JFET. The pin names are the document contract
        // the converter reads, not artwork vocabulary. The placed default is the
        // N-channel 2N5952; the inspector's model and polarity rows select any
        // N- or P-channel card, and the symbol follows.
        {"jfet",       "JFET",         "J",   {"drain", "gate", "source"}, {2, 0, 3}, {{0, -3}, {-3, 0}, {0, 3}}, 0.0, "value", "Ctrl+J", 0, false, "Semiconductors/Transistors/JFET"},
        // Generic three-terminal MOSFET (paper eqs 21-23). No body/diode/caps.
        // The label is the part's name, not a model: "NMOS" would read as though
        // the palette were offering a device family when it offers one part
        // whose inspector selects the polarity and preset.
        {"mosfet",     "MOSFET",       "M",   {"drain", "gate", "source"}, {2, 0, 3}, {{0, -3}, {-3, 0}, {0, 3}}, 20.0e-3, "k", "Ctrl+M", 0, false, "Semiconductors/Transistors/MOSFET"},
        // The bipolar junction transistor. Both NPN and PNP cards are selectable
        // and simulated; the pin names are the converter's contract and the
        // artwork's C/B/E leads land on these offsets exactly.
        {"bjt",        "BJT",          "Q",   {"collector", "base", "emitter"}, {2, 0, 3}, {{1, -3}, {-3, 0}, {1, 3}}, 0.0, "value", "Ctrl+B", 0, false, "Semiconductors/Transistors/BJT"},
        // The plain transformer's editable identity is a turns ratio: primary
        // turns and secondary turns. The model card chooses Ideal/Auto/Manual;
        // manual model cards can store the low-level inductance and leakage,
        // while ordinary users never need to see those keys.
        {"transformer","Transformer",  "T",   {"p1", "p2", "s1", "s2"}, {2, 3, 2, 3}, {{-3, -3}, {-3, 3}, {3, -3}, {3, 3}}, 1.0, "turnsPrimary", "N", 0, false, "Transformers"},
        // The centre-tapped transformer has three terminals on the primary side,
        // with the tap in the middle, and two on the secondary side. It is a
        // type of its own because a five-terminal pin frame cannot share a
        // four-terminal type and still draw honestly.
        {"transformer_ct","Xfmr CT",         "T",
         {"p1", "pt", "p2", "s1", "s2"}, {2, 0, 3, 2, 3}, {{-3, -3}, {-3, 0}, {-3, 3}, {3, -3}, {3, 3}},
         1.0, "turnsPrimary", "Ctrl+T", 0, false, "Transformers"},
        {"vcvs",       "Gain Block",   "E",   {"in", "out"},           {0, 1},      {},                        1.0,        "gain",      "Ctrl+G", 0, false, "Active/Gain Blocks"},
        // The ideal op-amp: infinite gain, no bandwidth limit, no slew, no
        // clipping and **no supply terminals**. A real card's V+/V- are how its
        // headroom is set; this part has no headroom to set, so it exposes the
        // three signal terminals only.
        //
        // Its own `type`, not another model row, because the pin frame is keyed
        // by type: a three-pin part and a five-pin part cannot share a type and
        // both keep an honest geometry. That is also why it carries "A" -- the
        // key arms the part a user reaches for to draw gain.
        {"idealopamp", "Ideal Op-Amp", "U",   {"out", "neg", "pos"},
         {1, 0, 0},                   {{3, 0}, {-3, -1}, {-3, 1}},
         0.0,        "",               "A", 0, false, "Active/Op-Amps"},
        // The operational amplifier. The pin names are the document contract
        // `toCircuit` reads: two inputs on the left, output on the right, and
        // the two supply pins above and below. The model picker selects the
        // real card (TL072, JRC4558, LM308, ...); the symbol follows nothing,
        // because every card draws the same triangle.
        //
        // **No shortcut.** "A" belongs to the ideal op-amp above; a card is
        // reached from the palette and the inspector.
        {"opamp",      "Op-Amp",       "U",   {"out", "neg", "pos", "vcc", "vee"},
         {1, 0, 0, 2, 3},             {{3, 0}, {-3, -1}, {-3, 1}, {0, -3}, {0, 3}},
         0.0,        "",               "",  0, false, "Active/Op-Amps"},
        {"src",        "Signal Source","V",   {"pos", "neg"},          {1, 3},      {{0, -2}, {0, 2}},         0.0,        "dc",         "B", 0, false, "Sources/Voltage"},
        // The current source, vertical like every other two-pin part: `pos` at
        // grid (0,-2) and `neg` at (0,2). Its artwork is drawn for those pins,
        // and a symbol whose leads stop short of its pins leaves a gap the eye
        // reads as a mistake.
        {"isrc",       "Current Src",  "I",   {"pos", "neg"},          {2, 3},      {{0, -2}, {0, 2}},         0.0,        "dc",         "J", 0, false, "Sources/Current"},

        // The two terminals. They are the *ends* of the signal path, and they
        // are not the signal source: the source is a DC (or AC) voltage the
        // circuit is built around, while these say where the audio comes in
        // and where it leaves.
        //
        // The Input converts to a voltage source between its pin and ground --
        // the only way a WDF engine can have a signal injected -- named after
        // the part, so the plugin finds *this* one rather than guessing which
        // source was meant. The Output stamps nothing electrically; it names
        // the node the plugin reads, and carries the voltage the circuit makes
        // there at full input, so the plugin can scale what it hands back.
        //
        // The pin frames are Céline's own (`Element.h`): one pin, offset so the
        // flag hangs off the wire end -- the input's at grid (2, 0), the
        // output's at (-2, 0).
        {"in",         "Input",        "IN",  {"sig"},                {1},         {{2, 0}},                  0.0,        "",           "I", 0, false, "Measurement/Terminals"},
        {"out",        "Output",       "OUT", {"sig"},                {0},         {{-2, 0}},                 1.0,        "fullScaleVolts", "O", 0, false, "Measurement/Terminals"},

        // The ground. **It is a marker, not an element**: its one pin sits on the
        // ground net from the moment it is placed, so wiring it to a part puts
        // that part on ground -- which is what a ground symbol in a schematic
        // means, and what the sheet was missing without it.
        //
        // The pin is at the *top* of the symbol, at grid (0,-2), because the
        // ground symbol hangs below the wire that comes into it.
        {"gnd",        "Ground",       "GND", {"sig"},                {2},         {{0, -2}},                 0.0,        "",           "G", 0, false, "Measurement/Ground"},

        // The DC rail's electrical connection is the **centre of the line**,
        // not a lead at one end. The pin used to be at (2,0), two grids right of
        // the centre, so a wire drawn to the middle of the rail did not connect
        // -- and the rail's net was serialized with whatever stale name its
        // passive neighbours carried. `rail.svg` draws the line centred on 0.
        {"rail",       "DC Rail",     "VDC", {"pos"},                {1},         {{0, 0}},                  0.0,        "dc",         "H", 0, false, "Sources/Supply"},

        // A changeover (SPDT) switch: common connects to one throw or the other,
        // never both. Three pins: `in` is the common, `outA` and `outB` route to
        // external resistors (or anything else). A single `select` property holds
        // "0" or "1" -- which throw is closed. No numeric value is exposed; the
        // part is just a switch, like the hardware it models.
        //
        // The pin frame is Celine's SPDT frame from `Element.h`: common at
        // (0,2), the two throws at (-1,-2) and (1,-2). The artwork is authored
        // for exactly those leads.
        {"switch_res", "SPDT Switch", "SPDT",
         {"in", "outA", "outB"},       {3, 0, 1},                {{0, 2}, {-1, -2}, {1, -2}},
         0.0,        "",               "M", 0, false, "Other/Switches"},

        // A scope probe. The one part that is *not in the circuit*: it stamps
        // nothing, because watching a node must not change it. Its two pins
        // reach net extraction, and the nets they land on are the pair of node
        // names the editor reads after the solve -- which is how a probe becomes
        // a live badge on the drawing with no separate "which node am I on" UI.
        {"scope",      "Scope Probe",  "SC",  {"probe", "ref"},        {1, 3},       {},                        0.0,        "value",      "Q", 0, false, "Measurement/Probes"},
        // clang-format on
    };
    return kPalette;
}

// Which registry a model-bearing part draws its palette rows from. The mapping
// is by `type`, because the type is the electrical contract (a triode is a
// triode whatever bottle it names); the model is what the row chooses.
ChoiceKind paletteModelKind(const std::string& type) {
    if (type == "diode") return ChoiceKind::DiodeModel;
    if (type == "triode") return ChoiceKind::TriodeModel;
    if (type == "pentode") return ChoiceKind::PentodeModel;
    if (type == "jfet") return ChoiceKind::JfetModel;
    if (type == "mosfet") return ChoiceKind::MosfetPreset;
    if (type == "bjt") return ChoiceKind::BjtModel;
    if (type == "opamp") return ChoiceKind::OpAmpModel;
    return ChoiceKind::None;
}

// The model a plain `makeComponent(type, ...)` starts with, and the one the
// family's keyboard shortcut arms. Kept explicit because the registry order is
// not always the "obvious" one: the op-amp registry begins with `generic`, but
// a fresh op-amp has always been a TL072.
std::string defaultModelForType(const std::string& type) {
    if (type == "diode") return "1N4148";
    if (type == "triode") return "12AX7";
    if (type == "pentode") return "EL34";
    if (type == "jfet") return "2N5952";
    if (type == "mosfet") return "2N7000";
    if (type == "bjt") return "2N3904";
    if (type == "opamp") return "TL072";
    return {};
}

// The full palette: base rows, with every model-bearing row expanded into one
// entry per registry model.
//
// **Why expand here rather than at the call sites.** The palette is the one
// place that says "what can I place"; the category tree and the panel both walk
// it. Expanding in this function means they see the models with no separate
// code path, and `findPart(type)` still resolves a family (its first row is the
// default model).
const std::vector<PartSpec>& paletteStorage() {
    static const std::vector<PartSpec> kPalette = [] {
        std::vector<PartSpec> out;
        for (const auto& base : basePaletteStorage()) {
            std::vector<std::string> models = modelChoices(paletteModelKind(base.type));
            if (models.empty()) {
                out.push_back(base);
                continue;
            }

            // The default model leads, so the family shortcut and the first
            // row are the device the single generic row used to place.
            const std::string preferred = defaultModelForType(base.type);
            if (const auto it = std::find(models.begin(), models.end(), preferred);
                it != models.end())
                std::rotate(models.begin(), it, it + 1);

            const std::string property = base.type == "mosfet" ? "preset" : "model";
            bool first = true;
            for (auto& model : models) {
                PartSpec spec = base;
                spec.modelProperty = property;
                spec.model = std::move(model);

                // One shortcut per family, on its default row: "T" arms the
                // 12AX7, and the other triodes are reachable by name or by the
                // inspector, not by fourteen identical key labels.
                if (!first)
                    spec.key.clear();
                first = false;

                out.push_back(std::move(spec));
            }
        }
        return out;
    }();
    return kPalette;
}

bool isGround(const std::string& net) {
    return net == "0" || net.empty();
}

// Build the category tree from the flat part palette.
//
// **Two passes, and no pointers into the tree while it is being built.** The
// first version stored `CategorySpec*` into a growing `std::vector` and looked
// them up again later; every `push_back` that reallocated that vector left the
// earlier pointers dangling, so most categories' parts were written into freed
// memory. The symptom was a palette that showed only the handful of entries
// whose pointers happened to survive -- and a render test that segfaulted
// reading a category name out of a stale node.
//
// So: first record the paths and their members in maps keyed by path, keeping
// palette order for both roots and siblings, then build the value tree from
// those maps. Nothing points into a vector that is still growing.
const std::vector<CategorySpec>& buildCategoryTree() {
    static const std::vector<CategorySpec> kCategories = [] {
        const auto& palette = paletteStorage();

        std::vector<std::string> rootPaths;
        // Parent path -> child paths, in first-appearance (palette) order.
        std::unordered_map<std::string, std::vector<std::string>> childrenByParent;
        // Category path -> the palette entries directly in it, in palette order.
        std::unordered_map<std::string, std::vector<const PartSpec*>> partsByPath;
        std::set<std::string> knownPaths;

        // Register every prefix of `path`, so a parent is created before its
        // children and the first time a category is seen fixes its order.
        auto registerPath = [&](const std::string& path) {
            std::string parent;
            size_t start = 0;
            while (start <= path.size()) {
                size_t slash = path.find('/', start);
                if (slash == std::string::npos)
                    slash = path.size();

                const std::string current = path.substr(0, slash);
                if (knownPaths.insert(current).second) {
                    if (parent.empty())
                        rootPaths.push_back(current);
                    else
                        childrenByParent[parent].push_back(current);
                }

                if (slash == path.size())
                    break;
                parent = current;
                start = slash + 1;
            }
        };

        for (const auto& spec : palette) {
            if (spec.category.empty())
                continue;
            registerPath(spec.category);
            // The palette vector is built once and never resized, so a pointer
            // to one of its entries is stable for the life of the program.
            partsByPath[spec.category].push_back(&spec);
        }

        // Depth-first construction from the ordered path data. The tree holds
        // categories by value; no external pointer is retained.
        std::function<CategorySpec(const std::string&)> build =
            [&](const std::string& path) -> CategorySpec {
                CategorySpec cat;
                cat.path = path;

                const auto slash = path.rfind('/');
                cat.name = (slash == std::string::npos) ? path : path.substr(slash + 1);

                if (const auto it = partsByPath.find(path); it != partsByPath.end())
                    cat.parts = it->second;

                // **A category with a handful of parts starts open; a long
                // model list starts folded.** The all-collapsed tree made the
                // palette look empty on first launch: every visible row was a
                // folder, so "where is the resistor" had no answer without a
                // click. Roots and small leaves open, while the registries with
                // a dozen transistor or tube models stay closed until asked
                // for. Search still shows everything that matches, open.
                cat.defaultExpanded = cat.parts.size() <= 8;

                if (const auto it = childrenByParent.find(path); it != childrenByParent.end())
                    for (const auto& childPath : it->second)
                        cat.children.push_back(build(childPath));

                return cat;
            };

        std::vector<CategorySpec> roots;
        roots.reserve(rootPaths.size());
        for (const auto& path : rootPaths)
            roots.push_back(build(path));

        return roots;
    }();
    return kCategories;
}

// Write a model name onto a fresh part, and copy the registry card's numbers
// into the part's values.
//
// **Why one function.** A palette row now chooses a model at placement time, and
// `makeComponent(type, ...)` still has to work for callers that only have a
// type. Both paths must land on the exact same card values, or a placed 2N5457
// and a JFET whose inspector was set to 2N5457 would simulate differently.
// Diode/triode/pentode/op-amp models are just the name; the converter reads the
// card itself. JFET/MOSFET/BJT store their card numbers in the document, so a
// renamed model cannot silently change the circuit.
void applyModelDefault(wdf::SchematicComponent& comp, const std::string& property,
                       const std::string& model) {
    if (property.empty() || model.empty())
        return;

    comp.properties[property] = model;

    if (comp.type == "jfet") {
        const auto* entry = wdf::findJfetByName(model.c_str());
        if (!entry)
            return;
        const auto& p = entry->model.p;
        comp.properties[wdf::kPolarityProperty] =
            p.pChannel ? wdf::kPChannel : wdf::kNChannel;
        comp.values["beta"] = p.Is0 / (p.Vp * p.Vp);
        comp.values["vto"] = p.Vp;
        comp.values["lambda"] = p.lambda;
        comp.values["rd"] = p.rd;
        comp.values["rs"] = p.rs;
        comp.values["cgs"] = p.cgs;
        comp.values["cgd"] = p.cgd;
        comp.values["cds"] = p.cds;
    } else if (comp.type == "mosfet") {
        const auto* preset = wdf::findMosfetPreset(model);
        if (!preset)
            return;
        comp.properties[wdf::kPolarityProperty] =
            preset->pChannel ? wdf::kPChannel : wdf::kNChannel;
        comp.values["k"] = preset->K;
        comp.values["vth"] = preset->Vth;
        comp.values["lambda"] = preset->lambda;
    } else if (comp.type == "bjt") {
        const auto* entry = wdf::findBjtByName(model.c_str());
        if (!entry)
            return;
        const auto& m = entry->model;
        // The five the reduced Ebers-Moll law actually reads. The Gummel-Poon
        // fields exist on the card as reference material, but a part placed
        // here carries only what the engine and the inspector both use.
        comp.values["is"] = m.Is;
        comp.values["bf"] = m.Bf;
        comp.values["br"] = m.Br;
        comp.values["nf"] = m.Nf;
        comp.values["nr"] = m.Nr;
    }
}

}  // namespace

const std::vector<PartSpec>& partPalette() {
    return paletteStorage();
}

const std::vector<CategorySpec>& partCategories() {
    return buildCategoryTree();
}

const PartSpec* findPart(const std::string& type) {
    const auto& p = partPalette();
    auto it = std::find_if(p.begin(), p.end(), [&](const PartSpec& s) { return s.type == type; });
    return it == p.end() ? nullptr : &*it;
}

std::string partLabel(const std::string& type) {
    if (const PartSpec* s = findPart(type))
        return s->label;
    return type;
}

int rotateSide(int side, int quarterTurns) {
    int turns = ((quarterTurns % 4) + 4) % 4;
    return (side + turns) % 4;
}

// ============================================================================
// The pin frame
//
// Why this lives with the table rather than in the canvas: a part's terminals
// have exactly one position, and two readers need it. The canvas draws a dot
// there and starts a wire from it; the document reader decides which pins share
// a net -- "do two parts put a pin on the same grid point?" -- which is the
// whole of how a `.livewdf` file's wiring is read. A copy in each reader is the
// drift this project keeps getting told off for, and it is silent: a pin that
// lands one grid square off in one of the two is a part wired to nothing.
// ============================================================================

namespace {

/// The body size a **derived** pin frame is spread over, in grid squares.
///
/// The size is Celine's, ±3/±3.5, and not the 44x52 pixels the first port used
/// -- that made every part five times Celine's own ±2, so two parts placed
/// anywhere near each other overlapped into one blob. A part is about four
/// squares across, artwork drawn in its own frame lands on the pins 1:1 with no
/// scaling step, and the default sheet's part spacing has room to breathe.
constexpr float kFrameHalfWidth = 3.0f;
constexpr float kFrameHalfHeight = 3.5f;

/// The frame of one palette row: the stated one when the part states it, the
/// side-and-spread rule otherwise.
///
/// The stated frame is the only way to reproduce Celine's valves: a triode's
/// plate, grid and cathode are not on a symmetric spread, and rounding them
/// onto one is what made the valve artwork unusable.
///
/// The rule is the fallback for our own parts (`vcvs`, `scope`) that Celine has
/// no counterpart for, so there is no reference frame to copy. Pins sharing a
/// side are spread along it so two pins on one side do not land on the same dot,
/// and the spread is the full half-extent rather than 0.8 of it: at 0.8 the two
/// pins sat within a wire's own thickness of each other and a click could not
/// tell them apart.
std::vector<PinOffsetSpec> frameForRow(const PartSpec& row) {
    if (!row.pinOffsets.empty())
        return row.pinOffsets;

    std::vector<PinOffsetSpec> offsets(row.pins.size());

    for (size_t i = 0; i < row.pins.size(); ++i) {
        if (i >= row.pinSides.size())
            continue;   // no side stated: the pin stays at the centre

        const int side = row.pinSides[i];

        int countOnSide = 0, ordinal = 0;
        for (size_t j = 0; j < row.pinSides.size(); ++j) {
            if (row.pinSides[j] == side) {
                if (j < i)
                    ++ordinal;
                ++countOnSide;
            }
        }

        const float t = (countOnSide <= 1)
                            ? 0.0f
                            : (static_cast<float>(ordinal) / static_cast<float>(countOnSide - 1)) *
                                  2.0f -
                                1.0f;

        PinOffsetSpec o;
        switch (side) {
        case 0:
            o = {static_cast<int>(std::lround(-kFrameHalfWidth)), static_cast<int>(std::lround(t * kFrameHalfHeight))};
            break;
        case 1:
            o = {static_cast<int>(std::lround(kFrameHalfWidth)), static_cast<int>(std::lround(t * kFrameHalfHeight))};
            break;
        case 2:
            o = {static_cast<int>(std::lround(t * kFrameHalfWidth)), static_cast<int>(std::lround(-kFrameHalfHeight))};
            break;
        default:
            o = {static_cast<int>(std::lround(t * kFrameHalfWidth)), static_cast<int>(std::lround(kFrameHalfHeight))};
            break;
        }
        offsets[i] = o;
    }
    return offsets;
}

/// Each type's frame once: the frame of a type does not depend on where the
/// part is placed, so it is built the first time it is asked for and every
/// reader pays a lookup after that. Model rows share their family's frame, so
/// the first row for a type wins.
const std::map<std::string, std::vector<PinOffsetSpec>>& framesByType() {
    static const std::map<std::string, std::vector<PinOffsetSpec>> kFrames = [] {
        std::map<std::string, std::vector<PinOffsetSpec>> built;
        for (const auto& row : partPalette())
            if (built.find(row.type) == built.end())
                built.emplace(row.type, frameForRow(row));
        return built;
    }();
    return kFrames;
}

}  // namespace

const std::vector<PinOffsetSpec>& pinFrame(const std::string& type) {
    static const auto& kFrames = framesByType();
    const auto it = kFrames.find(type);
    static const std::vector<PinOffsetSpec> kUnknown;
    return it == kFrames.end() ? kUnknown : it->second;
}

PinOffsetSpec pinAt(const wdf::SchematicComponent& part, int pinIndex) {
    const auto& frame = pinFrame(part.type);
    if (pinIndex < 0 || pinIndex >= static_cast<int>(frame.size()))
        return {};

    const PinOffsetSpec& o = frame[static_cast<size_t>(pinIndex)];

    // Mirror first, in the part's own frame, then rotate -- Celine's order, and
    // the one that makes "flip" mean the same thing whichever way the part is
    // turned.
    int px = part.visual.flipped ? -o.x : o.x;
    int py = o.y;

    for (int turn = 0; turn < ((part.visual.rotation / 90) & 3); ++turn) {
        const int rx = -py;
        py = px;
        px = rx;
    }

    return {static_cast<int>(std::lround(part.visual.x)) + px,
            static_cast<int>(std::lround(part.visual.y)) + py};
}

std::string uniqueId(const std::string& type,
                     const std::vector<wdf::SchematicComponent>& existing) {
    std::set<std::string> used;
    for (const auto& c : existing)
        used.insert(c.id);

    const PartSpec* spec = findPart(type);
    const std::string prefix = spec ? spec->idPrefix : type;

    for (int n = 1; n < 100000; ++n) {
        std::string candidate = prefix + std::to_string(n);
        if (used.find(candidate) == used.end())
            return candidate;
    }
    return prefix + "X";
}

std::string danglingNet(const std::string& compId, const std::string& pin) {
    return "n_" + compId + "_" + pin;
}

wdf::SchematicComponent makeComponent(const std::string& type,
                                      const std::vector<wdf::SchematicComponent>& existing,
                                      float x, float y,
                                      const std::string& modelProperty,
                                      const std::string& model) {
    wdf::SchematicComponent comp;
    const PartSpec* spec = findPart(type);
    if (!spec)
        return comp;  // empty type: caller tests this

    comp.type = type;
    comp.id = uniqueId(type, existing);
    comp.visual.x = x;
    comp.visual.y = y;
    comp.visual.rotation = spec->defaultRotation;
    comp.visual.flipped = spec->defaultFlipped;

    // `pins` is a **derived cache**, never serialised: the reader fills it from
    // the drawing at load (`format/SchematicNets.h`), and so does the editor
    // before it hands a document to the converter. These seeds only make a part
    // built in memory answer the question at all -- "this pin's net is not
    // shared with any other part", which is what a fresh, unwired part is. Wiring
    // is an edit to the drawing, and derivation is what makes the answer; a pin
    // that kept a name from being wired to something is the v2 model this
    // replaces.
    for (const auto& pin : spec->pins)
        comp.pins[pin] = danglingNet(comp.id, pin);

    // A source's two terminals are the one case where a fresh part should not
    // float: every circuit needs a ground reference, so a placed source is
    // grounded on `neg` and driven from the panel thereafter.
    if (type == "src")
        comp.pins["neg"] = "0";

    // The ground symbol's pin is on the ground net from the moment it is
    // placed. That is what makes it a *marker*: wiring it to a part puts that
    // part on ground, which is what a ground symbol in a schematic means.
    if (type == "gnd")
        comp.pins["sig"] = "0";

    // **A rail names nothing** (see `format/SchematicNets.h`): it drives
    // whatever net it is drawn onto. LiveSPICE's `Rail` puts its own name into
    // the equations as "i"+Name, an unknown current, and Celine's PSU does the
    // same -- neither names the net it feeds. So there is no pin name to seed
    // here: seeding one is what let a part be *named* onto a net it was drawn
    // nowhere near, and what let two rails claim one net at once, which the
    // converter built and the solver then refused by crashing.
    //
    // `node` stays, and only as display: the inspector shows a rail as "Node",
    // and the rail's own id is the label for it. Nothing downstream reads it --
    // the writer drops the key, and the net a rail is on comes from the drawing.
    if (type == "rail")
        comp.properties["node"] = comp.id;

    // A source is **either** a DC voltage **or** an AC signal, and the mode says
    // which. It used to be a DC level *and* an AC amplitude, side by side,
    // which is not a choice but a contradiction: the sheet said "0 V DC and
    // 1 V at 1 Hz" and the reader had to guess which one was meant.
    //
    // The frequency is 1 Hz by default because 1 Hz is slow enough to *see* on
    // the scope, which is the whole reason a source has an AC part at all. The
    // amplitude starts at zero, so a fresh source is a plain DC source until the
    // user asks for a signal.
    if (type == "src" || type == "isrc") {
        comp.properties["mode"] = "DC";
        comp.values["ac"] = 0.0;
        comp.values["freq"] = 1.0;
    }

    if (type == "vcvs") {
        comp.values["gain"] = 1.0;
        comp.values["ro"] = 1e-4;
    }

    // A transformer's visible controls are the turns ratio and the model card.
    // `Auto` derives a real, non-singular model from the turns ratio; `Ideal`
    // is a stiff numerical limit, and `Manual` exposes the saved low-level card.
    if (type == "transformer" || type == "transformer_ct") {
        comp.values["turnsPrimary"] = 1.0;
        comp.values["turnsSecondary"] = 1.0;
        comp.properties["model"] = "Real — Auto";
    }

    // A part with no value key has no number to show -- the Input terminal is
    // one: it says where the signal comes in, and there is nothing about it to
    // set. Writing an empty key into the map would put a nameless value in the
    // file, which is worse than nothing.
    if (!spec->valueKey.empty())
        comp.values[spec->valueKey] = spec->defaultValue;

    // Model-bearing parts need a default model, or the converter silently picks
    // one and the inspector has nothing to show. A palette row that chose a
    // specific device passes it in; otherwise the family's first row is the
    // default.
    applyModelDefault(comp,
                      modelProperty.empty() ? spec->modelProperty : modelProperty,
                      model.empty() ? spec->model : model);

    if (type == "pot" || type == "varres")
        comp.properties["taper"] = "Linear";

    if (type == "pot" || type == "varres")
        comp.values["pos"] = 0.5;

    if (type == "switch_res")
        comp.properties["select"] = "0";

    return comp;
}

}  // namespace livewdf::editor