#include "format/Document.h"

#include "format/Json.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The pin frame and the drawing's own nets: a `.livewdf` file stores geometry,
// and the connectivity it means is derived on load -- see the version note in
// `Document.h` and the derivation in `format/SchematicNets.h`.
#include "format/PartLibrary.h"
#include "format/SchematicNets.h"

namespace wdf {

std::string LiveWDFDocument::toJson() const {
    json::Value root(json::Type::Object);
        root["version"] = version;
        root["name"] = name;
        root["category"] = category;
        root["author"] = author;
        root["description"] = description;
        if (allowPreviewInputPad)
            root["allowPreviewInputPad"] = true;
        if (previewPadDb > 0. && validPreviewPadDb(previewPadDb))
            root["previewPadDb"] = previewPadDb;

        json::Value io(json::Type::Object);
        io["inputNode"] = inputNode;
        io["outputNode"] = outputNode;
        io["groundNode"] = groundNode;
        root["io"] = io;

        json::Value comps(json::Type::Array);
        for (const auto& c : components) {
            json::Value jc(json::Type::Object);
            jc["id"] = c.id;
            jc["type"] = c.type;

            // **No `pins`.** What a pin is joined to is where it is drawn, and
            // the wires below are the drawing; a stored map of pin names is a
            // second account of the same thing, which is how a file came to be
            // able to name a net it had not drawn (a floating node) or claim one
            // net for two conflicting rails. `DocumentIO` derives the answer on
            // load instead, and refuses a file that still carries one.
            //
            // Likewise no `properties["node"]`: a rail used to name the net it
            // drives, and it feeds whatever net it is drawn onto.

            json::Value vals(json::Type::Object);
            for (const auto& [k, v] : c.values)
                vals[k] = v;
            jc["values"] = vals;

            json::Value props(json::Type::Object);
            for (const auto& [k, s] : c.properties) {
                // **The rail's `node` is not written.** It names the net a
                // source drives, and a source feeds whatever net it is drawn
                // onto -- the field is a display label, kept in memory so the
                // inspector can show it next to the rail's value. It is not the
                // answer to anything electrical, so a file that carried it
                // would be carrying a name beside a pin again: the same lie the
                // refusal gate below exists to catch. The label survives a
                // reopen anyway, because it is the part's id.
                if (k == "node")
                    continue;
                props[k] = s;
            }
            jc["properties"] = props;

            json::Value vis(json::Type::Object);
            vis["x"] = c.visual.x;
            vis["y"] = c.visual.y;
            vis["rotation"] = c.visual.rotation;
            vis["flipped"] = c.visual.flipped;
            jc["visual"] = vis;

            comps.arrVal.push_back(std::move(jc));
        }
        root["components"] = comps;

        json::Value wiresArr(json::Type::Array);
        for (const auto& w : wires) {
            json::Value jw(json::Type::Object);

            // **No `net`.** A wire's net is the net its points are drawn on, and
            // the points below are the drawing. Storing the name as well is what
            // let a wire say it belonged to a net its ends do not touch.
            json::Value pts(json::Type::Array);
            for (const auto& pt : w.points) {
                json::Value jpt(json::Type::Object);
                jpt["x"] = pt.x;
                jpt["y"] = pt.y;
                pts.arrVal.push_back(std::move(jpt));
            }
            jw["points"] = pts;
            wiresArr.arrVal.push_back(std::move(jw));
        }
        root["wires"] = wiresArr;

        // **No `probes`.** A probe is a `scope` part on the sheet, which is part
        // of the drawing above; `allProbes()` answers where they are. What a
        // `probes` array stored instead was a net *name*, a reference into
        // connectivity by string. A document that brings one still gets one at
        // build time (a caller that already knows the net it wants), and a file
        // that carries one non-empty is refused below.

        if (!potOrder.empty() || !switchOrder.empty()) {
            json::Value layout(json::Type::Object);
            if (!potOrder.empty()) {
                json::Value pArr(json::Type::Array);
                for (const auto& p : potOrder)
                    pArr.arrVal.push_back(json::Value(p));
                layout["pots"] = pArr;
            }
            if (!switchOrder.empty()) {
                json::Value sArr(json::Type::Array);
                for (const auto& s : switchOrder)
                    sArr.arrVal.push_back(json::Value(s));
                layout["switches"] = sArr;
            }
            root["layout"] = layout;
        }

        return root.serialize(2);}

bool LiveWDFDocument::fromJson(const std::string& jsonStr, LiveWDFDocument& doc, std::string& error) {
    json::Value root;
        if (!json::Value::parse(jsonStr, root, error))
            return false;

        doc.version = root["version"].asInt(1);
        doc.name = root["name"].asString("Untitled Circuit");
        doc.category = root["category"].asString("Custom");
        doc.author = root["author"].asString("");
        doc.description = root["description"].asString("");
        doc.allowPreviewInputPad = root["allowPreviewInputPad"].asBool(false);
        const double padDb = root["previewPadDb"].asDouble();
        doc.previewPadDb = validPreviewPadDb(padDb) ? padDb : 0.;

        // ==========================================================================
        // **A version-3 document is a drawing, and a file that *names*
        // connectivity is not one.**
        //
        // Nets come from the geometry below, by `format/SchematicNets.h` -- the
        // way Celine's `Schematic::extractNets()` and LiveSPICE's `RebuildNodes()`
        // read a sheet. Versions 1 and 2 stored connectivity as strings beside
        // the parts instead: a `pins` map on every component, a `net` on every
        // wire, a `probes` array of net names. That is a second account of the
        // same drawing, and a second account can *lie* -- a pin named onto a net
        // it was drawn nowhere near (a floating node), or two rails claiming one
        // net for conflicting values. Such a file is refused, and not quietly
        // re-read: running something the user did not draw, with no way to see
        // the difference, is how "it worked when I opened it" becomes a bug hunt
        // with no suspect. A legacy sheet is migrated by stripping the names
        // down to the drawing, where the difference can be seen and compared one
        // file at a time -- not decided at every open.
        //
        // The shape is checked as well as the version number, because a file can
        // claim a version and still carry the fields only old versions wrote.
        // ==========================================================================

        const json::Value& sheet = root; // reading must not add keys to the tree

        const auto hasKey = [](const json::Value& obj, const std::string& key) {
            return obj.isObject() && obj.objVal.find(key) != obj.objVal.end();
        };

        const auto refuseLegacy = [&](const std::string& what) {
            error = "This build reads a `.livewdf` file as a drawing (version " +
                    std::to_string(kLiveWDFVersion) + "); " + what +
                    " stores connectivity as names, which is a second account of the drawing "
                    "and can say something the drawing does not -- a part named onto a net it "
                    "was not drawn on, or two supplies claiming one net for different values. "
                    "Keep the geometry and drop the names to load it: the drawing is the circuit.";
            return false;
        };

        if (doc.version < kLiveWDFVersion)
            return refuseLegacy("a document of version " + std::to_string(doc.version));

        if (sheet["components"].isArray())
            for (std::size_t i = 0; i < sheet["components"].arrVal.size(); ++i) {
                const json::Value& jc = sheet["components"].arrVal[i];
                if (hasKey(jc, "pins"))
                    return refuseLegacy("component \"" +
                        (jc["id"].asString().empty() ? "entry " + std::to_string(i) : jc["id"].asString()) +
                        "\" carries a `pins` map");
            }

        if (sheet["wires"].isArray())
            for (std::size_t i = 0; i < sheet["wires"].arrVal.size(); ++i)
                if (hasKey(sheet["wires"].arrVal[i], "net"))
                    return refuseLegacy("wire entry " + std::to_string(i) + " carries a `net` name");

        if (sheet["probes"].isArray() && !sheet["probes"].arrVal.empty())
            return refuseLegacy("a `probes` array (a probe that watches the drawing is a `scope` part, and the drawing is above)");

        doc.loadWarnings.clear();

        const auto& io = root["io"];
        doc.inputNode = io["inputNode"].asString("in");
        doc.outputNode = io["outputNode"].asString("out");
        doc.groundNode = io["groundNode"].asString("0");

        doc.components.clear();
        const auto& comps = root["components"];
        if (comps.isArray()) {
            for (const auto& jc : comps.arrVal) {
                SchematicComponent c;
                c.id = jc["id"].asString();
                c.type = jc["type"].asString();

                // **No `pins` read.** The field the refusal above exists for: a
                // stored map of pin names is connectivity as strings, which is
                // how a file came to be able to name a net the drawing never
                // made. `c.pins` is filled from the drawing at the end of this
                // function, which is the one place the answer is decided.

                const auto& vals = jc["values"];
                if (vals.isObject()) {
                    for (const auto& [k, v] : vals.objVal)
                        c.values[k] = v.asDouble();
                }

                const auto& props = jc["properties"];
                if (props.isObject()) {
                    for (const auto& [k, v] : props.objVal)
                        c.properties[k] = v.asString();
                }

                const auto& vis = jc["visual"];
                if (vis.isObject()) {
                    c.visual.x = static_cast<float>(vis["x"].asDouble());
                    c.visual.y = static_cast<float>(vis["y"].asDouble());
                    c.visual.rotation = vis["rotation"].asInt();
                    c.visual.flipped = vis["flipped"].asBool();
                }
                doc.components.push_back(std::move(c));
            }
        }

        // **There is no rail shift any more.**
        //
        // Version 2 moved a DC rail's pin from the right-hand lead to the centre
        // of its line, and the old reader moved every version-1 rail by two grids
        // at load so its drawn wire stayed joined -- which moved the part out
        // from under the artwork it was drawn with, and made a file mean two
        // different things depending on who read it. Both halves are gone: where
        // a rail's pin sits is a table fact (`format/PartLibrary.h`) that the
        // symbol and the reader both ask, so the symbol and the join can never
        // disagree; and a document whose rails were drawn against the *old*
        // symbol is a legacy document, refused above rather than moved behind
        // the user's back.

        doc.wires.clear();
        const auto& wires = root["wires"];
        if (wires.isArray()) {
            for (const auto& jw : wires.arrVal) {
                SchematicWire w;

                // **No `net` read** -- the wire's net is the net its points are
                // drawn on, and the refusal above turned down the file that
                // stored one. `w.net` is filled from the drawing below.
                const auto& pts = jw["points"];
                if (pts.isArray()) {
                    for (const auto& jpt : pts.arrVal) {
                        w.points.push_back({ static_cast<float>(jpt["x"].asDouble()),
                                             static_cast<float>(jpt["y"].asDouble()) });
                    }
                }
                doc.wires.push_back(std::move(w));
            }
        }

        doc.probes.clear();

        // **No probes read.** A probe that watches what is drawn is a `scope`
        // part on the sheet, and `allProbes()`/`probeNet()` answer it from that
        // part's `probe` pin; what a `probes` array stored instead was a net
        // *name*, a reference into connectivity by string. A file that brings
        // one non-empty was refused above, so there is nothing to read here --
        // and a document built in memory can still carry probes, where naming a
        // net is not a lie about a drawing.

        doc.potOrder.clear();
        doc.switchOrder.clear();
        const auto& layout = root["layout"];
        if (layout.isObject()) {
            const auto& pots = layout["pots"];
            if (pots.isArray()) {
                for (const auto& p : pots.arrVal)
                    doc.potOrder.push_back(p.asString());
            }
            const auto& switches = layout["switches"];
            if (switches.isArray()) {
                for (const auto& s : switches.arrVal)
                    doc.switchOrder.push_back(s.asString());
            }
        }

        // ==========================================================================
        // The nets the drawing makes, filled into the fields every reader asks.
        //
        // `toCircuit()`, `inputNet()`, `unconnectedPins()`, the plugin and the
        // scope all read these, so deriving them here is the one place the
        // answer is decided -- and it is decided from the parts' positions and
        // the wires, which is the only answer that cannot disagree with the
        // drawing. Two nets sharing one name is not a collision here: every
        // ground symbol names its net by the document's ground name, and
        // `Circuit` maps nodes by name, so grounding a pin in two places
        // connects them with no wire drawn across the sheet. That is how ground
        // is meant to work, and it is Celine's and LiveSPICE's mechanism.
        // ==========================================================================

        const DerivedNets nets = deriveNets(doc);

        for (std::size_t e = 0; e < doc.components.size(); ++e) {
            SchematicComponent& c = doc.components[e];
            const livewdf::editor::PartSpec* spec = livewdf::editor::findPart(c.type);

            if (spec == nullptr) {
                // A type this build does not have was *drawn*, so it keeps its
                // place -- but it has no pins to place, so nothing can join
                // through it. Saying so is the alternative to inventing a
                // position, which is what used to connect a part to something
                // it was never drawn on.
                doc.loadWarnings.push_back("Part \"" + c.id + "\" is of type \"" + c.type +
                    "\" which this build does not have: it stays where it was drawn, but it has no pins, so nothing joins through it.");
                continue;
            }

            const auto& pins = livewdf::editor::partPins(c);
            const std::vector<int>& pinNets = nets.netOfPin[e];
            for (std::size_t pi = 0; pi < pins.size() && pi < pinNets.size(); ++pi)
                if (pinNets[pi] >= 0)
                    c.pins[pins[pi]] = nets.netNames[static_cast<std::size_t>(pinNets[pi])];
        }

        for (std::size_t w = 0; w < doc.wires.size() && w < nets.netOfWire.size(); ++w)
            if (nets.netOfWire[w] >= 0)
                doc.wires[w].net = nets.netNames[static_cast<std::size_t>(nets.netOfWire[w])];

        // The one case where a name from the file is still the only answer: a
        // sheet whose audio has no drawn entry or exit. Then `inputNet()`/
        // `outputNet()` fall back to the names the file carries -- and under a
        // geometry-only format those are names the *drawing* does not make, so
        // using them silently is a circuit that runs somewhere the user cannot
        // point at. Saying so is the whole point of putting the fallback here
        // rather than in every caller.
        if (doc.terminal("in") == nullptr)
            doc.loadWarnings.push_back("This sheet has no `in` terminal: the audio enters at the name the file carries ('" +
                doc.inputNode + "'), which is a name the drawing never made. Draw an `in` part where the signal enters.");
        if (doc.terminal("out") == nullptr)
            doc.loadWarnings.push_back("This sheet has no `out` terminal: the audio leaves at the name the file carries ('" +
                doc.outputNode + "'), which is a name the drawing never made. Draw an `out` part where the signal leaves.");

        return true;
}

namespace {
bool isPowerAmpPart(const SchematicComponent& part) {
    const auto mode = part.properties.find("powerAmp");
    return mode != part.properties.end() && mode->second == "1";
}
} // namespace

const SchematicComponent* LiveWDFDocument::preampTap() const {
    if (terminal("out") == nullptr)
        return nullptr;
    for (const auto& part : components) {
        const auto tap = part.properties.find("preampTap");
        if (tap == part.properties.end() || isPowerAmpPart(part))
            continue;
        const auto pin = part.pins.find(tap->second);
        if (pin != part.pins.end() && !pin->second.empty() && pin->second != groundNode)
            return &part;
    }
    return nullptr;
}

bool LiveWDFDocument::hasAcPowerSupply() const {
    return std::ranges::any_of(components, [](const auto& part) {
        return isPowerAmpPart(part) && (part.type == "src" || part.type == "rail") &&
               sourceModeOf(part) == SourceMode::AC;
    });
}

bool LiveWDFDocument::makePreampDocument(LiveWDFDocument& preamp, std::string& error) const {
    const auto* tap = preampTap();
    if (tap == nullptr) {
        error = "The circuit has no preamp output";
        return false;
    }
    const auto* spec = livewdf::editor::findPart(tap->type);
    if (spec == nullptr) {
        error = "The preamp output part is unavailable";
        return false;
    }
    const auto& pins = livewdf::editor::partPins(*tap);
    const auto pin = std::find(pins.begin(), pins.end(), tap->properties.at("preampTap"));
    if (pin == pins.end()) {
        error = "The preamp output pin is unavailable";
        return false;
    }
    const auto position = livewdf::editor::pinAt(*tap, static_cast<int>(pin - pins.begin()));
    auto drawing = *this;
    auto& out = drawing.components[terminal("out") - components.data()];
    const auto output = livewdf::editor::pinAt(out, 0);
    out.visual.x += static_cast<float>(position.x - output.x);
    out.visual.y += static_cast<float>(position.y - output.y);
    out.values.clear();
    out.properties.clear();
    const auto level = tap->values.find("preampFullScaleVolts");
    if (level != tap->values.end())
        out.values["fullScaleVolts"] = level->second;
    for (auto& part : drawing.components) {
        part.properties.erase("preampOnly");
        if (part.type != "rail")
            continue;
        const auto dc = part.values.find("preampDc");
        if (dc != part.values.end())
            part.values["dc"] = dc->second;
    }
    std::erase_if(drawing.components, isPowerAmpPart);
    return fromJson(drawing.toJson(), preamp, error);
}

bool LiveWDFDocument::saveToFile(const std::string& filepath) const {
    std::ofstream f(filepath);
        if (!f.is_open()) return false;
        f << toJson();
        return true;
}

bool LiveWDFDocument::loadFromFile(const std::string& filepath, std::string& error) {
    return loadFromFile(filepath, *this, error);
}

bool LiveWDFDocument::loadFromFile(const std::string& filepath, LiveWDFDocument& doc, std::string& error) {
    std::ifstream f(filepath);
        if (!f.is_open()) {
            error = "Could not open file: " + filepath;
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        return fromJson(ss.str(), doc, error);
}

} // namespace wdf
