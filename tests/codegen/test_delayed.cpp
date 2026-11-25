#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <type_traits>

#define CircuitStep ForwardNetwork
#define DelayedCircuitStep Forward
#include "generated_delayed.h"
#undef CircuitStep
#undef DelayedCircuitStep
#define CircuitStep ReverseNetwork
#define DelayedCircuitStep Reverse
#include "generated_delayed-reverse.h"
#undef CircuitStep
#undef DelayedCircuitStep

static_assert(Forward::firstPorts == Reverse::secondPorts);
static_assert(Forward::secondPorts == Reverse::firstPorts);

template<class Network, class Model>
void compareDirections(const wdf::Circuit& circuit) {
    Model model;
    Network linear;
    auto previous = Network::initialCurrent;
    const auto first = [](std::size_t port) {
        return std::find(Model::firstPorts.begin(), Model::firstPorts.end(), port) != Model::firstPorts.end();
    };
    wdf::Engine native(circuit, Network::sampleRate);

    for (int n = 0; n < 384; ++n) {
        auto source = Network::dcSources;
        source[Network::inputIndex] += .36 * std::sin(.35 * n);
        native.step(source.data(), source.size(), 60, 1e-7);
        if (native.lastStateRejected()) throw std::runtime_error("fixture current trace rejected");
        typename Network::Ports current{};
        std::copy(native.rootCurrents().begin(), native.rootCurrents().end(), current.begin());
        const auto open = linear.openCircuit(source);
        const bool exact = n % 7 == 2;
        typename Network::Ports expectedVoltage = open;
        for (std::size_t r = 0; r < open.size(); ++r)
            for (std::size_t c = 0; c < open.size(); ++c)
                expectedVoltage[r] -= Network::impedance[r][c] *
                    ((!exact && first(r) && !first(c)) ? previous[c] : current[c]);

        // Replay realistic terminal currents to check the network independently
        // of any particular implementation of the local nonlinear solver.
        int solverCalls = 0;
        const auto solve = [&](const auto& ids, const auto& z, const auto& drive, const auto&) {
            constexpr std::size_t count = std::tuple_size_v<std::remove_cvref_t<decltype(ids)>>;
            typename Model::template StageSolution<count> solution{};
            ++solverCalls;
            for (std::size_t r = 0; r < count; ++r) {
                solution.current[r] = current[ids[r]];
                solution.voltage[r] = drive[r];
                for (std::size_t c = 0; c < count; ++c) solution.voltage[r] -= z[r][c] * current[ids[c]];
            }
            return std::optional{solution};
        };
        const auto validate = [&](const auto& voltage, const auto& actualCurrent) {
            if (actualCurrent != current) return false;
            for (std::size_t r = 0; r < voltage.size(); ++r)
                if (!(std::abs(voltage[r] - expectedVoltage[r]) < 1e-8)) return false;
            return true;
        };
        const auto saved = model.circuit().state;
        if (n % 31 == 0) {
            auto poison = [&](const auto&... args) {
                auto trial = solve(args...);
                trial->voltage.front() += 1.;
                return trial;
            };
            const auto refuseSecond = [&](const auto& ids, const auto&... args) {
                auto result = solve(ids, args...);
                if (ids.front() == Model::secondPorts.front()) result.reset();
                return result;
            };
            if (model.step(source, poison, validate) || model.step(source, refuseSecond, validate) ||
                model.step(source, solve, [](const auto&, const auto&) { return false; }) ||
                model.circuit().state != saved || model.previousCurrent() != previous)
                throw std::runtime_error("failed trial advanced delay or reactive history");
        }
        solverCalls = 0;
        const auto y = exact ? model.stepExact(source, solve, validate) : model.step(source, solve, validate);
        if (!y || solverCalls != (exact ? 1 : 2))
            throw std::runtime_error("unexpected stage drive, ordering or solve count");
        if (*y != linear.advance(source, current) || linear.state != model.circuit().state ||
            model.previousCurrent() != current)
            throw std::runtime_error("feedback and network histories out of sync");
        previous = current;
    }
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    try {
        wdf::LiveWDFDocument document;
        std::string error;
        if (!document.loadFromFile(argv[1], document, error)) throw std::runtime_error(error);
        const auto circuit = document.toCircuit();
        compareDirections<ForwardNetwork, Forward>(circuit);
        compareDirections<ReverseNetwork, Reverse>(circuit);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
