#include "recurrence_fixture.h"
#include "wdf/Engine.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

extern "C" void test_c99_reset(void);
extern "C" float test_c99_step(float input);

#define CircuitStep GeneratedRLC
#include "generated_rlc.h"
#undef CircuitStep
#define CircuitStep GeneratedDiode
#include "generated_diode.h"
#undef CircuitStep
#define CircuitStep GeneratedExternal
#include "generated_external.h"
#undef CircuitStep

#define CircuitStep GeneratedAmp
#include "generated_amp.h"
#undef CircuitStep

template<class Model>
void compare(int count) {
    Model model;
    wdf::Engine reference(recurrenceFixture(count), 48000.);
    const auto& [rep, offset] = reference.compiled().info.at("out");
    auto source = Model::dcSources;
    for (int n = 0; n < 4096; ++n) {
        source[0] = .1 + 2. * std::sin(.039 * n) + .6 * std::sin(.41 * n);
        const auto& v = reference.step(source.data(), source.size(), 60, 1e-10);
        if (reference.lastStateRejected()) throw std::runtime_error("reference rejected sample");
        double expected = (rep.empty() ? 0. : v.at(reference.nodeIndex(rep))) +
                          wdf::evalOff(offset, source.data());
        double actual;
        if constexpr (std::tuple_size_v<typename Model::Ports> == 2) {
            const auto saved = model.state;
            const auto bad = model.step(source, [](const auto&, auto& r) {
                r.voltage[0] = std::numeric_limits<double>::quiet_NaN();
                return true;
            });
            if (bad || saved != model.state) throw std::runtime_error("failed step advanced history");
            auto y = model.step(source, [&](const auto&, auto& result) {
                const auto& ports = reference.rootLayout().ports;
                for (std::size_t r = 0; r < ports.size(); ++r) {
                    result.current[r] = reference.rootCurrents()[r];
                    result.voltage[r] = reference.compiled().across(ports[r], v) +
                                        wdf::evalOff(ports[r].off, source.data());
                }
                return true;
            });
            if (!y) throw std::runtime_error("valid loaded port solution rejected");
            actual = *y;
        } else actual = model.step(source);
        if (!(std::abs(actual - expected) < 1e-8))
            throw std::runtime_error("generated sample disagrees with reference");
    }
}
void compareC99() {
    test_c99_reset();
    wdf::Engine reference(recurrenceFixture(0), 44100.);
    const auto& [rep, offset] = reference.compiled().info.at("out");
    auto source = reference.dcValues();
    for (int n = 0; n < 4096; ++n) {
        const float input = static_cast<float>(.1 + 2. * std::sin(.039 * n) + .6 * std::sin(.41 * n));
        source[0] = input;
        const auto& v = reference.step(source.data(), source.size(), 60, 1e-10);
        const double expected = (rep.empty() ? 0. : v.at(reference.nodeIndex(rep))) +
                                wdf::evalOff(offset, source.data());
        if (reference.lastStateRejected() || !(std::abs(test_c99_step(input) - expected) < 1e-5))
            throw std::runtime_error("generated C99 sample disagrees with reference");
    }
}

int main() {
    try {
        compare<GeneratedRLC>(0);
        compare<GeneratedDiode>(1);
        compare<GeneratedExternal>(2);
        compare<GeneratedAmp>(3);
        compareC99();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
