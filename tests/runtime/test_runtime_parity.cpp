#include <catch2.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "compiler/Compiler.h"
#include "runtime/Runtime.h"
#include "support/CircuitCases.h"
#include "wdf/Engine.h"

// Phase 1 runtime parity: the facade must be invisible. Every comparison here
// is bit-exact, including the diagnostics that describe how the solve went.

namespace {

constexpr double kFs = 48000.0;

std::uint64_t bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

void requireSameNodes(const std::vector<double>& engine,
                      std::span<const double> runtime) {
    REQUIRE(engine.size() == runtime.size());
    for (std::size_t i = 0; i < engine.size(); ++i) {
        INFO("node " << i);
        REQUIRE(bits(engine[i]) == bits(runtime[i]));
    }
}

std::unique_ptr<livewdf::runtime::Runtime> makeRuntime(
    const std::shared_ptr<const livewdf::compiler::CompiledCircuit>& compiled) {
    livewdf::runtime::RuntimeConfig config;
    config.sampleRate = kFs;
    return livewdf::runtime::makeDenseRuntime(compiled, config);
}

wdf::Circuit controlledLinearCircuit() {
    wdf::Circuit c;
    c.addSrc("in", "0", 1.0);
    c.addRes("in", "a", 10e3, "R1");
    c.addPot("a", "b", "0", 100e3, 0.5, wdf::PotTaper::Linear, "GAIN");
    c.addVarRes("b", "0", 50e3, 0.5, wdf::PotTaper::Linear, "MID");
    return c;
}

}  // namespace

TEST_CASE("runtime: DenseRuntime is bit-identical to Engine on the corpus", "[runtime]") {
    for (const auto& testCase : lwdf_test::circuitCases()) {
        INFO(testCase.name);
        const wdf::Circuit circuit = testCase.build();

        const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
        REQUIRE(compiled.ok());

        wdf::Engine engine(circuit, kFs);
        auto runtime = makeRuntime(compiled.circuit);
        REQUIRE(runtime != nullptr);

        REQUIRE(engine.sourceCount() == compiled.circuit->sourceCount());
        REQUIRE(engine.nodeNames() ==
                std::vector<std::string>(runtime->nodes().names().begin(),
                                         runtime->nodes().names().end()));

        for (int n = 0; n < 1024; ++n) {
            const std::vector<double> sources = lwdf_test::sampleSources(circuit, testCase, n);
            const auto& engineNodes = engine.step(sources);
            const auto runtimeNodes = runtime->step(sources);

            requireSameNodes(engineNodes, runtimeNodes);

            const auto diagnostics = runtime->diagnostics();
            REQUIRE(engine.lastConverged());
            REQUIRE(engine.lastConverged() == diagnostics.lastConverged);
            REQUIRE(engine.newtonIterations() == diagnostics.lastIterations);
            REQUIRE(bits(engine.lastResidual()) == bits(diagnostics.lastResidual));
            REQUIRE(engine.lastBlockFallback() == diagnostics.lastBlockFallback);
            REQUIRE(engine.lastKeptBest() == diagnostics.lastKeptBest);
            REQUIRE(engine.lastStateRejected() == diagnostics.lastStateRejected);
        }

        REQUIRE(engine.blockFallbackCount() == runtime->diagnostics().blockFallbacks);
        REQUIRE(engine.stateRejectCount() == runtime->diagnostics().stateRejections);
        REQUIRE(engine.linearRebuilds() == runtime->diagnostics().linearRebuilds);
    }
}

TEST_CASE("runtime: reset reproduces sample 1 exactly", "[runtime]") {
    const auto testCase = lwdf_test::circuitCases().front();  // rc_lowpass
    const wdf::Circuit circuit = testCase.build();

    const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(compiled.ok());
    wdf::Engine engine(circuit, kFs);
    auto runtime = makeRuntime(compiled.circuit);

    const std::vector<double> sources = lwdf_test::sampleSources(circuit, testCase, 0);
    for (int n = 0; n < 200; ++n)
        (void)engine.step(sources);
    (void)runtime->step(sources);

    engine.reset();
    runtime->reset();

    const std::vector<double> afterReset = lwdf_test::sampleSources(circuit, testCase, 0);
    requireSameNodes(engine.step(afterReset), runtime->step(afterReset));
}

TEST_CASE("runtime: step returns a view, not a copy", "[runtime]") {
    const wdf::Circuit circuit = lwdf_test::commonCathodeBypassed();
    const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(compiled.ok());
    auto runtime = makeRuntime(compiled.circuit);

    std::vector<double> sources(circuit.src.size(), 0.0);
    const auto first = runtime->step(sources);
    const auto second = runtime->step(sources);
    REQUIRE(first.data() == second.data());  // same engine buffer every sample
}

TEST_CASE("runtime: parameter batch matches the engine's setters", "[runtime]") {
    const wdf::Circuit circuit = controlledLinearCircuit();
    const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(compiled.ok());

    wdf::Engine engine(circuit, kFs);
    auto runtime = makeRuntime(compiled.circuit);

    const auto& table = runtime->parameters();
    const auto gain = table.find("GAIN");
    const auto mid = table.find("MID");
    const auto r1 = table.find("R1");
    REQUIRE(gain.has_value());
    REQUIRE(mid.has_value());
    REQUIRE(r1.has_value());

    // The engine's separate calls rebuild three times; the batch rebuilds once.
    // Both must end at the same values, which is what the samples prove.
    REQUIRE(engine.setPotPosition("GAIN", 0.8));
    REQUIRE(engine.setVarResPosition("MID", 0.2));
    REQUIRE(engine.setResistances({{"R1", 22e3}}));

    const livewdf::compiler::ParameterValue updates[] = {
        {*gain, 0.8},
        {*mid, 0.2},
        {*r1, 22e3},
    };
    REQUIRE(runtime->setParameters(updates));

    REQUIRE(runtime->parameterValue(*gain) == 0.8);
    REQUIRE(runtime->parameterValue(*mid) == 0.2);
    REQUIRE(runtime->parameterValue(*r1) == 22e3);
    REQUIRE(runtime->diagnostics().linearRebuilds ==
            engine.linearRebuilds() - 2);  // one batch vs three setters

    std::vector<double> sources(circuit.src.size(), 1.0);
    for (int n = 0; n < 128; ++n) {
        sources[0] = 0.3 * std::sin(0.05 * n);
        requireSameNodes(engine.step(sources), runtime->step(sources));
        const auto diagnostics = runtime->diagnostics();
        REQUIRE(engine.lastConverged() == diagnostics.lastConverged);
    }
}

TEST_CASE("runtime: an invalid batch is rejected without moving anything", "[runtime]") {
    const wdf::Circuit circuit = controlledLinearCircuit();
    const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(compiled.ok());

    wdf::Engine engine(circuit, kFs);
    auto runtime = makeRuntime(compiled.circuit);

    const auto& table = runtime->parameters();
    const auto gain = table.find("GAIN");
    const auto r1 = table.find("R1");
    REQUIRE(gain.has_value());
    REQUIRE(r1.has_value());

    const double before = runtime->parameterValue(*gain);
    const auto rebuildsBefore = runtime->diagnostics().linearRebuilds;

    const livewdf::compiler::ParameterValue badValue[] = {{*r1, -1.0}};
    REQUIRE_FALSE(runtime->setParameters(badValue));
    REQUIRE(runtime->parameterValue(*gain) == before);
    REQUIRE(runtime->diagnostics().linearRebuilds == rebuildsBefore);

    const livewdf::compiler::ParameterValue badHandle[] = {
        {livewdf::compiler::ParameterHandle{-1}, 1.0}};
    REQUIRE_FALSE(runtime->setParameters(badHandle));

    const livewdf::compiler::ParameterValue nanPosition[] = {
        {*gain, std::numeric_limits<double>::quiet_NaN()}};
    REQUIRE_FALSE(runtime->setParameters(nanPosition));

    std::vector<double> sources(circuit.src.size(), 1.0);
    requireSameNodes(engine.step(sources), runtime->step(sources));
}

TEST_CASE("runtime: operating point matches the engine's DC solve", "[runtime]") {
    const wdf::Circuit circuit = lwdf_test::twoStageCascade();
    const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(compiled.ok());
    wdf::Engine engine(circuit, kFs);
    auto runtime = makeRuntime(compiled.circuit);

    std::vector<double> engineNode, engineRoot;
    REQUIRE(engine.solveDc(engineNode, engineRoot));
    const auto point = runtime->operatingPoint();
    REQUIRE(point.converged);
    REQUIRE(point.nodeVolts.size() == engineNode.size());
    REQUIRE(point.rootCurrents.size() == static_cast<std::size_t>(engine.rootCount()));

    for (std::size_t i = 0; i < engineNode.size(); ++i)
        REQUIRE(bits(engineNode[i]) == bits(point.nodeVolts[i]));
}

TEST_CASE("runtime: the experimental block solver is opt-in and finite", "[runtime]") {
    const wdf::Circuit circuit = lwdf_test::commonCathodeBypassed();
    const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(compiled.ok());

    livewdf::runtime::RuntimeConfig plain;
    plain.sampleRate = kFs;
    auto defaultRuntime = livewdf::runtime::makeDenseRuntime(compiled.circuit, plain);

    livewdf::runtime::RuntimeConfig experimental;
    experimental.sampleRate = kFs;
    experimental.enableRuntimeBlockSolver = true;
    auto blockRuntime = livewdf::runtime::makeDenseRuntime(compiled.circuit, experimental);

    std::vector<double> sources(circuit.src.size(), 0.0);
    for (int n = 0; n < 64; ++n) {
        sources[0] = 0.25 * std::sin(0.05 * n);
        const auto defaultNodes = defaultRuntime->step(sources);
        const auto blockNodes = blockRuntime->step(sources);

        REQUIRE_FALSE(defaultRuntime->diagnostics().lastBlockUsed);
        REQUIRE(blockNodes.size() == defaultNodes.size());
        for (double value : blockNodes) {
            REQUIRE(std::isfinite(value));
        }
    }
}
