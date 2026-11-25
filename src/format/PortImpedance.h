#pragma once

#include "format/Document.h"

#include <array>
#include <optional>
#include <stdexcept>

namespace wdf {

// A fixture is external to the instrument. Position zero leaves the netlist exactly as drawn.
struct PortImpedanceRange {
    double minOhms, maxOhms;
    std::string resistor;
    double wiredOhms = 0., defaultOhms = 0., defaultPosition = 0.;
    static constexpr double step = 0.001;

    bool atDefault(double position) const noexcept {
        return defaultPosition > 0. && std::abs(position - defaultPosition) <= 1.e-7;
    }

    bool needsFixture(double position, bool alreadyAdjustable = false) const noexcept {
        return position > 0. && (resistor.empty() || alreadyAdjustable || !atDefault(position));
    }

    void setDefault(double ohms) {
        if (!(ohms >= minOhms && ohms <= maxOhms)) return;
        defaultOhms = ohms;
        defaultPosition = std::round(positionForOhms(ohms) / step) * step;
    }

    double ohmsAt(double position) const {
        if (!std::isfinite(position) || position <= 0.) return 0.;
        // The host stores this parameter on a 0.001 grid. Keep a specified
        // reference resistor exact when the knob is at its factory detent.
        if (atDefault(position))
            return defaultOhms;
        const double t = (std::clamp(position, step, 1.) - step) / (1. - step);
        return minOhms * std::pow(maxOhms / minOhms, t);
    }

    double positionForOhms(double ohms) const {
        if (!std::isfinite(ohms) || ohms <= 0.) return 0.;
        const double t = std::log(std::clamp(ohms, minOhms, maxOhms) / minOhms) /
                         std::log(maxOhms / minOhms);
        return step + (1. - step) * t;
    }
};

inline std::optional<PortImpedanceRange> portImpedanceRange(const LiveWDFDocument& doc, bool input) {
    const auto* port = doc.terminal(input ? "in" : "out");
    if (!port) return {};
    const auto lo = port->values.find("externalMinOhms");
    const auto hi = port->values.find("externalMaxOhms");
    if (lo == port->values.end() || hi == port->values.end()) return {};
    const double low = lo->second, high = hi->second;
    // The linear rheostat used by the live solver reaches this fraction of its full travel.
    if (!(low > 0. && low < high && high <= 1.e9 && low >= VarRes::kEps * high))
        return {};

    const auto pin = port->pins.find("sig");
    const auto ground = doc.groundNode.empty() ? std::string("0") : doc.groundNode;
    if (pin == port->pins.end() || pin->second.empty() || pin->second == ground) return {};
    const auto named = port->properties.find("externalResistor");
    PortImpedanceRange range{low, high, {}};
    if (named == port->properties.end()) {
        const auto value = port->values.find("externalDefaultOhms");
        if (value != port->values.end()) range.setDefault(value->second);
        return range;
    }
    const auto& id = named->second;
    const auto fixture = std::find_if(doc.components.begin(), doc.components.end(),
                                      [&](const auto& part) { return part.id == id; });
    if (fixture == doc.components.end() || fixture->type != "res" ||
        std::any_of(fixture + 1, doc.components.end(), [&](const auto& part) { return part.id == id; }))
        return {};
    const auto a = fixture->pins.find("a"), b = fixture->pins.find("b");
    if (a == fixture->pins.end() || b == fixture->pins.end() || a->second == b->second)
        return {};
    const auto& net = pin->second;
    if (a->second != net && b->second != net) return {};
    const auto& other = a->second == net ? b->second : a->second;
    if (other.empty() || (input ? other == ground : other != ground)) return {};
    if (input) {
        // A named source resistor must be in series with every driven branch.
        for (const auto& part : doc.components) {
            if (&part == port || &part == &*fixture || part.type == "scope" || part.type == "out")
                continue;
            for (const auto& [name, connected] : part.pins)
                if (connected == net) return {};
        }
    }
    range.resistor = id;
    const auto value = fixture->values.find("value");
    range.wiredOhms = value == fixture->values.end() ? 1000. : value->second;
    range.setDefault(range.wiredOhms);
    return range;
}

// Called on a private circuit during compilation. An existing termination becomes
// adjustable in place; an absent fixture adds a series source R or shunt load R.
inline std::string applyPortImpedance(Circuit& circuit, const LiveWDFDocument& doc,
                                      bool input, double position) {
    if (!(position > 0.) || !std::isfinite(position)) return {};
    const auto range = portImpedanceRange(doc, input);
    if (!range) return {};
    std::string name = range->resistor;
    auto a = input ? doc.inputNet() : doc.outputNet();
    auto b = doc.groundNode.empty() ? std::string("0") : doc.groundNode;
    const auto hasRheostat = [&](const auto& id) {
        return std::any_of(circuit.varRes.begin(), circuit.varRes.end(), [&](const auto& r) { return r.name == id; });
    };
    if (!name.empty()) {
        auto resistor = std::find_if(circuit.res.begin(), circuit.res.end(),
                                     [&](const auto& r) { return r.name == name; });
        if (resistor == circuit.res.end() || hasRheostat(name) ||
            std::any_of(resistor + 1, circuit.res.end(), [&](const auto& r) { return r.name == name; }))
            throw std::runtime_error("The external resistor must identify one fixed circuit resistor: " + name);
        a = resistor->a;
        b = resistor->b;
        circuit.res.erase(resistor);
    } else {
        name = input ? "$external_source" : "$external_load";
        while (hasRheostat(name) || std::any_of(circuit.res.begin(), circuit.res.end(),
                                                [&](const auto& r) { return r.name == name; }))
            name += "_";
        if (input) {
            const auto src = std::find_if(circuit.src.begin(), circuit.src.end(),
                                          [&](const auto& source) { return source.name == doc.inputId(); });
            if (src == circuit.src.end())
                throw std::runtime_error("The external source impedance has no driven input");
            b = name + "_drive";
            const auto nodes = circuit.allNodes();
            while (std::find(nodes.begin(), nodes.end(), b) != nodes.end()) b += "_";
            src->a = b;
        }
    }
    circuit.addVarRes(a, b, range->maxOhms, range->ohmsAt(position) / range->maxOhms, PotTaper::Linear, name);
    return name;
}

inline Circuit circuitWithPortImpedances(const LiveWDFDocument& doc,
        const std::array<double, 2>* positions = nullptr, std::array<std::string, 2>* fixtureNames = nullptr) {
    auto circuit = doc.toCircuit();
    for (size_t i = 0; i < 2; ++i) {
        const bool input = i == 0;
        const auto range = portImpedanceRange(doc, input);
        const double position = positions ? (*positions)[i] : (range ? range->defaultPosition : 0.);
        std::string fixture;
        if (range && range->needsFixture(position))
            fixture = applyPortImpedance(circuit, doc, input, position);
        if (fixtureNames) (*fixtureNames)[i] = std::move(fixture);
    }
    return circuit;
}

inline std::string playbackCacheKey(const LiveWDFDocument& doc, const std::array<std::string, 2>& fixtureNames) {
    auto key = doc.toJson();
    for (const auto& fixture : fixtureNames)
        if (!fixture.empty()) key += "\nport fixture " + fixture;
    return key;
}

} // namespace wdf
