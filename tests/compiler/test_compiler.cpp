#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "compiler/Compiler.h"
#include "support/CircuitCases.h"

// Phase 1 compiler contract: compile is pure data, produces stable names and
// handles, and refuses only what the engine cannot represent.

namespace {

wdf::Circuit potAndRheostatCircuit() {
    wdf::Circuit c;
    c.addSrc("in", "0", 1.0);
    c.addRes("in", "a", 10e3, "R1");
    c.addPot("a", "b", "0", 100e3, 0.5, wdf::PotTaper::Linear, "GAIN");
    c.addVarRes("b", "0", 50e3, 0.5, wdf::PotTaper::Linear, "MID");
    return c;
}

}  // namespace

TEST_CASE("compiler: a valid circuit compiles to immutable data", "[compiler]") {
    const wdf::Circuit circuit = lwdf_test::commonCathodeBypassed();
    const auto result = livewdf::compiler::Compiler{}.compile(circuit);

    REQUIRE(result.ok());
    REQUIRE(result.circuit != nullptr);
    for (const auto& diagnostic : result.diagnostics)
        REQUIRE(diagnostic.severity != livewdf::compiler::Severity::Error);

    const auto& compiled = *result.circuit;
    REQUIRE(compiled.sourceCount() == static_cast<int>(circuit.src.size()));
    REQUIRE(compiled.topology().rootLayout().K == 3);  // one triode, three ports
    REQUIRE(compiled.topology().rootLayout().nRed == 2);
    REQUIRE(compiled.devices().size() == 1);
    REQUIRE(compiled.devices().front().type == "triode");
    REQUIRE(compiled.revision().size() == 16);
}

TEST_CASE("compiler: node table matches compiled free-node order", "[compiler]") {
    const wdf::Circuit circuit = lwdf_test::twoStageCascade();
    const auto result = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(result.ok());

    const auto& nodes = result.circuit->nodes();
    REQUIRE(nodes.size() == static_cast<std::size_t>(result.circuit->compiled().N));
    REQUIRE(nodes.index("0") < 0);  // ground is not a free node
    REQUIRE(nodes.index("out") == result.circuit->compiled().index.at("out"));
    REQUIRE(nodes.index("does-not-exist") == -1);
}

TEST_CASE("compiler: parameter table exposes controls, not their derived resistors",
          "[compiler]") {
    const wdf::Circuit circuit = potAndRheostatCircuit();
    const auto result = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(result.ok());

    const auto& parameters = result.circuit->parameters();
    REQUIRE(parameters.size() == 3);

    const auto gain = parameters.find("GAIN");
    REQUIRE(gain.has_value());
    REQUIRE(parameters.kind(*gain) == livewdf::compiler::ParameterKind::PotPosition);
    REQUIRE(parameters.value(*gain) == 0.5);

    const auto mid = parameters.find("MID");
    REQUIRE(mid.has_value());
    REQUIRE(parameters.kind(*mid) == livewdf::compiler::ParameterKind::VarResPosition);

    const auto r1 = parameters.find("R1");
    REQUIRE(r1.has_value());
    REQUIRE(parameters.kind(*r1) == livewdf::compiler::ParameterKind::Resistance);
    REQUIRE(parameters.value(*r1) == 10e3);

    // The pot expands into GAIN_1/GAIN_2 and the rheostat into MID; neither is
    // a second handle for the same control.
    REQUIRE_FALSE(parameters.find("GAIN_1").has_value());
    REQUIRE_FALSE(parameters.find("GAIN_2").has_value());

    // Handles are indices and stay stable across value changes.
    for (int i = 0; i < static_cast<int>(parameters.size()); ++i)
        REQUIRE(parameters.handles()[static_cast<std::size_t>(i)].value == i);
}

TEST_CASE("compiler: revision is stable, and changes with the netlist", "[compiler]") {
    const wdf::Circuit circuit = potAndRheostatCircuit();
    const auto first = livewdf::compiler::Compiler{}.compile(circuit);
    const auto second = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE(first.circuit->revision() == second.circuit->revision());

    wdf::Circuit changed = circuit;
    changed.res.front().value = 11e3;
    const auto third = livewdf::compiler::Compiler{}.compile(changed);
    REQUIRE(third.ok());
    REQUIRE(third.circuit->revision() != first.circuit->revision());

    livewdf::compiler::CompileOptions options;
    options.revision = "document-sha-abc";
    const auto supplied = livewdf::compiler::Compiler{}.compile(circuit, options);
    REQUIRE(supplied.ok());
    REQUIRE(supplied.circuit->revision() == "document-sha-abc");
}

TEST_CASE("compiler: invalid values are errors in strict mode", "[compiler]") {
    wdf::Circuit circuit = potAndRheostatCircuit();
    circuit.res.front().value = 0.0;  // R1

    livewdf::compiler::CompileOptions strictOptions;
    strictOptions.strict = true;
    const auto strict = livewdf::compiler::Compiler{}.compile(circuit, strictOptions);
    REQUIRE_FALSE(strict.ok());
    REQUIRE(std::any_of(strict.diagnostics.begin(), strict.diagnostics.end(),
                        [](const auto& d) { return d.code == "element.invalid-value"; }));

    livewdf::compiler::CompileOptions lenientOptions;
    lenientOptions.strict = false;
    const auto lenient = livewdf::compiler::Compiler{}.compile(circuit, lenientOptions);
    REQUIRE(lenient.ok());
    REQUIRE(std::any_of(lenient.diagnostics.begin(), lenient.diagnostics.end(),
                        [](const auto& d) {
                            return d.code == "element.invalid-value" &&
                                   d.severity == livewdf::compiler::Severity::Warning;
                        }));
}

TEST_CASE("compiler: a pot without backing resistors is refused", "[compiler]") {
    wdf::Circuit circuit;
    circuit.addSrc("in", "0", 1.0);
    circuit.addRes("in", "0", 1e3, "R1");
    wdf::Pot pot;
    pot.n1 = "in";
    pot.wiper = "w";
    pot.n2 = "0";
    pot.name = "BROKEN";
    circuit.pots.push_back(pot);

    const auto result = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE_FALSE(result.ok());
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto& d) { return d.code == "pot.missing-resistor"; }));
}

TEST_CASE("compiler: topology candidates reference real devices and ports", "[compiler]") {
    const wdf::Circuit circuit = lwdf_test::sharedCathodePair();
    const auto result = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(result.ok());

    const auto& plan = result.circuit->topology();
    const auto& layout = plan.rootLayout();
    REQUIRE(plan.hasCandidateStages());

    std::set<int> seenPorts;
    for (const auto& group : plan.candidateGroups()) {
        REQUIRE_FALSE(group.deviceIndices.empty());
        for (int deviceIndex : group.deviceIndices) {
            REQUIRE(deviceIndex >= 0);
            REQUIRE(deviceIndex < static_cast<int>(layout.devices.size()));
        }
        for (int port : group.rootPorts) {
            REQUIRE(port >= 0);
            REQUIRE(port < layout.K);
            REQUIRE(seenPorts.insert(port).second);  // every port appears once
        }
        if (group.deviceIndices.size() > 1)
            REQUIRE_FALSE(group.scalarClosedForm);
    }
    REQUIRE(seenPorts.size() == static_cast<std::size_t>(layout.K));
}

TEST_CASE("compiler: root layout is self-consistent", "[compiler]") {
    for (const auto& testCase : lwdf_test::circuitCases()) {
        const wdf::Circuit circuit = testCase.build();
        const auto result = livewdf::compiler::Compiler{}.compile(circuit);
        INFO(testCase.name);
        REQUIRE(result.ok());

        const auto& layout = result.circuit->topology().rootLayout();
        int ports = 0;
        int active = 0;
        for (const auto& device : layout.devices) {
            REQUIRE(device.portBase == ports);
            REQUIRE(device.numPorts > 0);
            ports += device.numPorts;
            active += device.numActiveRows;
        }
        REQUIRE(ports == layout.K);
        REQUIRE(active == layout.nRed);
        REQUIRE(static_cast<int>(layout.ports.size()) == layout.K);
        REQUIRE(static_cast<int>(layout.redCol.size()) == layout.nRed);
        REQUIRE(static_cast<int>(layout.redRow.size()) == layout.K);
        for (int port : layout.redCol) {
            REQUIRE(port >= 0);
            REQUIRE(port < layout.K);
            REQUIRE(layout.redRow[static_cast<std::size_t>(port)] >= 0);
        }
    }
}
