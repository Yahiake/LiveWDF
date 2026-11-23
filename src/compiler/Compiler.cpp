#include "compiler/Compiler.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "circuit/RootLayout.h"

namespace livewdf::compiler {

namespace {

std::string_view typeName(RootKind kind) noexcept {
    switch (kind) {
    case RootKind::Triode: return "triode";
    case RootKind::Diode: return "diode";
    case RootKind::ControlledRes: return "ctrlres";
    case RootKind::Pentode: return "pentode";
    case RootKind::Jfet: return "jfet";
    case RootKind::Mosfet: return "mosfet";
    case RootKind::Bjt: return "bjt";
    case RootKind::OpAmp: return "opamp";
    }
    return "unknown";
}

bool positiveFinite(double v) noexcept {
    return std::isfinite(v) && v > 0.0;
}

int resIndexByName(const wdf::Circuit& circuit, const std::string& name) noexcept {
    for (int i = 0; i < static_cast<int>(circuit.res.size()); ++i)
        if (circuit.res[static_cast<std::size_t>(i)].name == name)
            return i;
    return -1;
}

void add(std::vector<Diagnostic>& out, Severity severity, std::string code,
         std::string message, std::string part = {}, std::string pin = {}) {
    out.push_back({severity, std::move(code), std::move(message), std::move(part),
                   std::move(pin)});
}

void addValueFinding(std::vector<Diagnostic>& out, const CompileOptions& options,
                     const std::string& what, std::string part, double value) {
    add(out, options.strict ? Severity::Error : Severity::Warning,
        "element.invalid-value",
        what + " must be finite and greater than zero (got " + std::to_string(value) + ")",
        std::move(part));
}

// ---------------------------------------------------------------------------
// Revision fingerprint
// ---------------------------------------------------------------------------

void mixBytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

void mixString(std::uint64_t& hash, std::string_view text) noexcept {
    mixBytes(hash, text.data(), text.size());
    const unsigned char separator = 0xff;
    mixBytes(hash, &separator, 1);
}

void mixDouble(std::uint64_t& hash, double value) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    mixBytes(hash, &bits, sizeof(bits));
}

void mixElement(std::uint64_t& hash, std::string_view a, std::string_view b,
                double value, std::string_view name) noexcept {
    mixString(hash, a);
    mixString(hash, b);
    mixDouble(hash, value);
    mixString(hash, name);
}

/// A fingerprint of the netlist's elements, nodes, names and values.
///
/// It deliberately does not walk device model-card parameters: those are
/// aggregates with padding, and a field-by-field scheme that is easy to forget
/// to update is worse than an honest boundary. Callers that need a revision to
/// cover the whole input (the document loader does) pass one in
/// `CompileOptions::revision`.
std::string fingerprint(const wdf::Circuit& c) {
    std::uint64_t hash = 1469598103934665603ull;

    for (const auto& e : c.res) mixElement(hash, e.a, e.b, e.value, e.name);
    for (const auto& e : c.cap) {
        mixElement(hash, e.a, e.b, e.value, e.name);
        if (e.initialVolts)
            mixDouble(hash, *e.initialVolts);
    }
    for (const auto& e : c.ind) mixElement(hash, e.a, e.b, e.value, e.name);
    for (const auto& e : c.isrc) {
        mixElement(hash, e.a, e.b, e.dc, e.name);
        mixDouble(hash, e.ac);
        for (double s : e.sin) mixDouble(hash, s);
    }
    for (const auto& e : c.src) {
        mixElement(hash, e.a, e.b, e.dc, e.name);
        mixDouble(hash, e.ac);
        for (double s : e.sin) mixDouble(hash, s);
    }
    for (const auto& e : c.pots) {
        mixString(hash, e.n1);
        mixString(hash, e.wiper);
        mixString(hash, e.n2);
        mixDouble(hash, e.totalResistance);
        mixDouble(hash, e.position);
        mixString(hash, e.name);
    }
    for (const auto& e : c.varRes) {
        mixElement(hash, e.a, e.b, e.maxResistance, e.name);
        mixDouble(hash, e.position);
    }
    for (const auto& e : c.ctrlRes) {
        mixElement(hash, e.a, e.b, e.initialOhms, e.name);
        if (e.photo) {
            mixString(hash, e.photo->sensePos);
            mixString(hash, e.photo->senseNeg);
            mixDouble(hash, e.photo->brightOhms);
            mixDouble(hash, e.photo->referenceVolts);
            mixDouble(hash, e.photo->gamma);
            mixDouble(hash, e.photo->attackSeconds);
            mixDouble(hash, e.photo->releaseSeconds);
        }
        if (e.vca) {
            mixString(hash, e.vca->control);
            mixString(hash, e.vca->reference);
            mixDouble(hash, e.vca->fullGainVolts);
            mixDouble(hash, e.vca->blackmer ? 1.0 : 0.0);
        }
        if (e.ota) {
            mixString(hash, e.ota->bias);
            mixString(hash, e.ota->biasDiode);
        }
    }
    for (const auto& e : c.diode) mixElement(hash, e.a, e.b, 0.0, e.name);
    for (const auto& e : c.bucketBrigades) {
        mixString(hash, e.input);
        mixString(hash, e.control);
        mixString(hash, e.common);
        mixString(hash, e.name);
        mixDouble(hash, e.model.stages);
        mixDouble(hash, e.model.clockHz);
        mixDouble(hash, e.model.periodSecondsPerVolt);
        mixDouble(hash, e.model.inputBias);
        mixDouble(hash, e.model.outputBias);
        mixDouble(hash, e.model.inputSwing);
        mixDouble(hash, e.model.gain);
        mixDouble(hash, e.model.outputOhms);
    }
    for (const auto& e : c.tri) {
        mixString(hash, e.g);
        mixString(hash, e.p);
        mixString(hash, e.k);
        mixString(hash, e.gi);
        mixString(hash, e.name);
    }
    for (const auto& e : c.pent) {
        mixString(hash, e.p);
        mixString(hash, e.s);
        mixString(hash, e.g);
        mixString(hash, e.k);
        mixString(hash, e.name);
    }
    for (const auto& e : c.jfet) {
        mixString(hash, e.d);
        mixString(hash, e.g);
        mixString(hash, e.s);
        mixString(hash, e.dExt);
        mixString(hash, e.gExt);
        mixString(hash, e.sExt);
        mixString(hash, e.name);
    }
    for (const auto& e : c.mosfet) {
        mixString(hash, e.d);
        mixString(hash, e.g);
        mixString(hash, e.s);
        mixString(hash, e.name);
    }
    for (const auto& e : c.bjt) {
        mixString(hash, e.c);
        mixString(hash, e.b);
        mixString(hash, e.e);
        mixString(hash, e.name);
    }
    for (const auto& e : c.opamp) {
        mixString(hash, e.out);
        mixString(hash, e.neg);
        mixString(hash, e.pos);
        mixString(hash, e.vcc);
        mixString(hash, e.vee);
        mixString(hash, e.name);
    }
    for (const auto& e : c.vcvs) {
        mixString(hash, e.outA);
        mixString(hash, e.outB);
        mixString(hash, e.inA);
        mixString(hash, e.inB);
        mixDouble(hash, e.gain);
        mixDouble(hash, e.ro);
        mixString(hash, e.name);
    }
    for (const auto& e : c.mutual) {
        mixString(hash, e.name);
        mixDouble(hash, e.k);
        for (const auto& n : e.indNames)
            mixString(hash, n);
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = hex[hash & 0x0full];
        hash >>= 4;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

void validateValues(const wdf::Circuit& c, const CompileOptions& options,
                    std::vector<Diagnostic>& out) {
    for (const auto& r : c.res)
        if (!positiveFinite(r.value))
            addValueFinding(out, options, "resistance", r.name, r.value);
    for (const auto& e : c.cap) {
        if (!positiveFinite(e.value))
            addValueFinding(out, options, "capacitance", e.name, e.value);
        if (e.initialVolts && !std::isfinite(*e.initialVolts))
            addValueFinding(out, options, "capacitor initial voltage", e.name, *e.initialVolts);
    }
    for (const auto& e : c.ind)
        if (!positiveFinite(e.value))
            addValueFinding(out, options, "inductance", e.name, e.value);
    for (const auto& e : c.ctrlRes)
        if (!positiveFinite(e.initialOhms))
            addValueFinding(out, options, "controlled resistance", e.name, e.initialOhms);

    for (const auto& pot : c.pots) {
        if (!positiveFinite(pot.totalResistance))
            addValueFinding(out, options, "potentiometer resistance", pot.name,
                            pot.totalResistance);
        if (!std::isfinite(pot.position))
            addValueFinding(out, options, "potentiometer position", pot.name, pot.position);
        if (resIndexByName(c, pot.name + "_1") < 0 ||
            resIndexByName(c, pot.name + "_2") < 0) {
            add(out, options.strict ? Severity::Error : Severity::Warning,
                "pot.missing-resistor",
                "potentiometer '" + pot.name + "' has no backing resistors '" +
                    pot.name + "_1' / '" + pot.name + "_2'",
                pot.name);
        }
    }

    for (const auto& vr : c.varRes) {
        if (!positiveFinite(vr.maxResistance))
            addValueFinding(out, options, "variable resistance", vr.name,
                            vr.maxResistance);
        if (!std::isfinite(vr.position))
            addValueFinding(out, options, "variable-resistor position", vr.name,
                            vr.position);
        if (resIndexByName(c, vr.name) < 0) {
            add(out, options.strict ? Severity::Error : Severity::Warning,
                "varres.missing-resistor",
                "variable resistor '" + vr.name + "' has no backing resistor",
                vr.name);
        }
    }

    for (const auto& v : c.vcvs) {
        if (!positiveFinite(v.ro)) {
            add(out, options.strict ? Severity::Error : Severity::Warning,
                "vcvs.invalid-output-resistance",
                "VCVS '" + v.name + "' output resistance must be finite and greater "
                "than zero",
                v.name);
        }
    }
}

/// The parameters a control can address, in a deterministic order: pots,
/// rheostats, then named resistors. Backing resistors are omitted because the
/// control that derives them owns their value.
std::vector<ParameterTable::Entry> buildParameters(const wdf::Circuit& c,
                                                   std::vector<Diagnostic>& out) {
    std::vector<ParameterTable::Entry> entries;
    std::unordered_set<int> derivedResistors;

    for (int i = 0; i < static_cast<int>(c.pots.size()); ++i) {
        const auto& pot = c.pots[static_cast<std::size_t>(i)];
        entries.push_back({pot.name, ParameterKind::PotPosition, i, pot.position});
        const int r1 = resIndexByName(c, pot.name + "_1");
        const int r2 = resIndexByName(c, pot.name + "_2");
        if (r1 >= 0) derivedResistors.insert(r1);
        if (r2 >= 0) derivedResistors.insert(r2);
    }

    for (int i = 0; i < static_cast<int>(c.varRes.size()); ++i) {
        const auto& vr = c.varRes[static_cast<std::size_t>(i)];
        entries.push_back({vr.name, ParameterKind::VarResPosition, i, vr.position});
        const int r = resIndexByName(c, vr.name);
        if (r >= 0) derivedResistors.insert(r);
    }

    for (int i = 0; i < static_cast<int>(c.res.size()); ++i) {
        const auto& r = c.res[static_cast<std::size_t>(i)];
        if (r.name.empty() || derivedResistors.count(i) > 0)
            continue;
        entries.push_back({r.name, ParameterKind::Resistance, i, r.value});
    }

    std::unordered_set<std::string> seen;
    for (const auto& entry : entries) {
        if (!seen.insert(entry.name).second) {
            add(out, Severity::Warning, "parameter.duplicate-name",
                "more than one control is named '" + entry.name +
                    "'; the first one wins in lookup",
                entry.name);
        }
    }
    return entries;
}

std::vector<DeviceInstance> buildDevices(const wdf::Circuit& c,
                                         const RootLayout& layout) {
    std::vector<DeviceInstance> devices;
    devices.reserve(layout.devices.size());
    for (const auto& device : layout.devices) {
        DeviceInstance instance;
        instance.type = std::string(typeName(device.kind));
        instance.kind = device.kind;
        instance.componentIndex = device.compIndex;
        instance.rootPortBase = device.portBase;
        instance.rootPortCount = device.numPorts;
        const auto ci = static_cast<std::size_t>(device.compIndex);
        switch (device.kind) {
        case RootKind::Triode: instance.name = c.tri[ci].name; break;
        case RootKind::Diode: instance.name = c.diode[ci].name; break;
        case RootKind::ControlledRes: instance.name = c.ctrlRes[ci].name; break;
        case RootKind::Pentode: instance.name = c.pent[ci].name; break;
        case RootKind::Jfet: instance.name = c.jfet[ci].name; break;
        case RootKind::Mosfet: instance.name = c.mosfet[ci].name; break;
        case RootKind::Bjt: instance.name = c.bjt[ci].name; break;
        case RootKind::OpAmp: instance.name = c.opamp[ci].name; break;
        }
        devices.push_back(std::move(instance));
    }
    return devices;
}

}  // namespace

CompileResult Compiler::compile(const wdf::Circuit& netlist,
                                const CompileOptions& options) const {
    CompileResult result;
    validateValues(netlist, options, result.diagnostics);

    std::vector<ParameterTable::Entry> parameters =
        buildParameters(netlist, result.diagnostics);

    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.severity == Severity::Error)
            return result;
    }

    wdf::Compiled compiled;
    try {
        compiled = wdf::compile(netlist, {});
    } catch (const std::exception& e) {
        add(result.diagnostics, Severity::Error, "circuit.compile-failed",
            std::string("circuit could not be compiled: ") + e.what());
        return result;
    } catch (...) {
        add(result.diagnostics, Severity::Error, "circuit.compile-failed",
            "circuit could not be compiled (unknown error)");
        return result;
    }

    const RootLayout layout = wdf::buildRootLayout(compiled);
    TopologyPlan topology = buildTopologyPlan(netlist, layout);
    NodeTable nodes(compiled.nodes);
    std::vector<DeviceInstance> devices = buildDevices(netlist, layout);

    std::string revision = options.revision;
    if (revision.empty())
        revision = fingerprint(netlist);

    result.circuit = std::shared_ptr<const CompiledCircuit>(new CompiledCircuit(
        netlist, std::move(compiled), std::move(nodes),
        ParameterTable(std::move(parameters)), std::move(topology),
        std::move(devices), std::move(revision)));
    return result;
}

}  // namespace livewdf::compiler
