// Manual instrument: does the runtime facade cost anything on the hot path?
//
// Absolute ns/sample numbers move with machine load, so this benchmark does not
// report them as a regression bar. It runs `Engine` and `DenseRuntime` in the
// same process, with the same precomputed input, alternating the two, and
// reports the ratio. That ratio is the adapter's cost; the rest cancels.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "compiler/Compiler.h"
#include "runtime/Runtime.h"
#include "runtime/dense/DenseRuntime.h"
#include "support/CircuitCases.h"
#include "wdf/Engine.h"

namespace {

constexpr double kFs = 48000.0;
constexpr int kDriveSamples = 48000;
constexpr int kWarmup = 4096;
constexpr int kRepeats = 5;

struct NamedCase {
    const char* name;
    wdf::Circuit (*build)();
};

double measureEngine(wdf::Engine& engine, const std::vector<std::vector<double>>& drive) {
    const auto start = std::chrono::steady_clock::now();
    for (const auto& sv : drive)
        (void)engine.step(sv);
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    return seconds * 1.0e9 / static_cast<double>(drive.size());
}

double measureRuntime(livewdf::runtime::Runtime& runtime,
                      const std::vector<std::vector<double>>& drive) {
    const auto start = std::chrono::steady_clock::now();
    for (const auto& sv : drive)
        (void)runtime.step(sv);
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    return seconds * 1.0e9 / static_cast<double>(drive.size());
}

double measureConcrete(livewdf::runtime::dense::DenseRuntime& runtime,
                       const std::vector<std::vector<double>>& drive) {
    const auto start = std::chrono::steady_clock::now();
    for (const auto& sv : drive)
        (void)runtime.step(sv);
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    return seconds * 1.0e9 / static_cast<double>(drive.size());
}

}  // namespace

int main() {
    const NamedCase cases[] = {
        {"rc_lowpass", &lwdf_test::rcLowpass},
        {"common_cathode_bypassed", &lwdf_test::commonCathodeBypassed},
        {"four_stage_preamp", &lwdf_test::fourStagePreamp},
        {"push_pull_power_stage", &lwdf_test::pushPullPowerStage},
    };

    std::printf("runtime facade A/B   fs=%.0f Hz   %d samples   min of %d alternating runs\n",
                kFs, kDriveSamples, kRepeats);
    std::printf("  %-26s %12s %12s %12s %8s %9s\n", "circuit", "engine ns/s", "dense ns/s",
                "virtual ns/s", "d/e", "v/e");
    std::printf("  --------------------------------------------------------------------------------\n");

    for (const auto& testCase : cases) {
        const wdf::Circuit circuit = testCase.build();
        const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
        if (!compiled.ok()) {
            std::printf("  %-26s COMPILE FAILED\n", testCase.name);
            continue;
        }

        wdf::Engine engine(circuit, kFs);
        livewdf::runtime::RuntimeConfig config;
        config.sampleRate = kFs;
        livewdf::runtime::dense::DenseRuntime concrete(compiled.circuit, config);
        livewdf::runtime::Runtime& runtime = concrete;

        std::vector<std::vector<double>> drive(static_cast<std::size_t>(kDriveSamples));
        for (int n = 0; n < kDriveSamples; ++n) {
            auto& sv = drive[static_cast<std::size_t>(n)];
            sv.resize(circuit.src.size());
            for (std::size_t i = 0; i < circuit.src.size(); ++i)
                sv[i] = circuit.src[i].dc;
            sv[0] = 0.2 * std::sin(2.0 * wdf::kPi * 440.0 * static_cast<double>(n) / kFs);
        }

        const std::vector<std::vector<double>> warmup(drive.begin(), drive.begin() + kWarmup);
        (void)measureEngine(engine, warmup);
        (void)measureConcrete(concrete, warmup);
        (void)measureRuntime(runtime, warmup);

        std::vector<double> engineRuns;
        std::vector<double> concreteRuns;
        std::vector<double> runtimeRuns;
        for (int repeat = 0; repeat < kRepeats; ++repeat) {
            engineRuns.push_back(measureEngine(engine, drive));
            concreteRuns.push_back(measureConcrete(concrete, drive));
            runtimeRuns.push_back(measureRuntime(runtime, drive));
        }

        std::sort(engineRuns.begin(), engineRuns.end());
        std::sort(concreteRuns.begin(), concreteRuns.end());
        std::sort(runtimeRuns.begin(), runtimeRuns.end());
        const double engineNs = engineRuns.front();
        const double concreteNs = concreteRuns.front();
        const double runtimeNs = runtimeRuns.front();
        std::printf("  %-26s %12.2f %12.2f %12.2f %7.3fx %8.3fx\n", testCase.name, engineNs,
                    concreteNs, runtimeNs,
                    engineNs > 0.0 ? concreteNs / engineNs : 0.0,
                    engineNs > 0.0 ? runtimeNs / engineNs : 0.0);
    }

    return 0;
}
