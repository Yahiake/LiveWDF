// Run from the repository root. With no arguments, exercise a passive ladder
// and three factory circuits. --screen uses the plugin worker selection policy.
// Additional arguments select .livewdf files.
#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int rate = 48000, frames = 128, warmup = 32, blocks = 375;
constexpr double tolerance = 1e-7;

struct Fixture {
    wdf::Circuit circuit;
    size_t input = 0;
    std::string output;
};

Fixture load(const std::string& path) {
    Fixture f;
    if (path == "rc-ladder") {
        f.circuit.addSrc("in", "0", 0.0);
        f.circuit.addRes("in", "a", 12000.0);
        f.circuit.addCap("a", "0", 10e-9);
        f.circuit.addRes("a", "out", 68000.0);
        f.circuit.addCap("out", "0", 2.2e-9);
        f.circuit.addRes("out", "0", 33000.0);
        f.output = "out";
        return f;
    }
    wdf::LiveWDFDocument doc;
    std::string error;
    if (!wdf::LiveWDFDocument::loadFromFile(path, doc, error))
        throw std::runtime_error(error);
    f.circuit = doc.toCircuit();
    const auto id = doc.inputId();
    const auto source = std::find_if(f.circuit.src.begin(), f.circuit.src.end(),
                                     [&](const auto& s) { return s.name == id; });
    if (source == f.circuit.src.end())
        throw std::runtime_error("missing input: " + id);
    f.input = static_cast<size_t>(source - f.circuit.src.begin());
    f.output = doc.outputNet();
    return f;
}

// On Linux measure the audio work even if another process preempts this one.
double timeMs() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        throw std::runtime_error("cannot read thread CPU clock");
    return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) * 1e-6;
#else
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

struct Times {
    std::vector<double> ms;
    double quantile(double q) const { return ms[static_cast<size_t>(std::ceil(q * ms.size())) - 1]; }
    size_t missed() const {
        return std::count_if(ms.begin(), ms.end(), [](double t) { return t > 1000.0 * frames / rate; });
    }
};

void run(const std::string& name, bool screen) {
    const auto f = load(name);
    wdf::Engine reference(f.circuit, rate), candidate(f.circuit, rate);
    const int output = reference.nodeIndex(f.output);
    if (output < 0)
        throw std::runtime_error("missing output: " + f.output);
    // Both paths have the same Fast Live tolerance and available slope plans.
    reference.prepareMixedAffineReduction(f.input);
    candidate.prepareMixedAffineReduction(f.input);
    const char* policy = "forced";
    if (screen) {
        wdf::Engine::LiveTransferSetup left(f.circuit), right(f.circuit);
        if (!candidate.captureLiveTransferSetup(left, true) || !reference.captureLiveTransferSetup(right, true))
            throw std::runtime_error("cannot capture circuit plan: " + name);
        left.build(f.input, false);
        right.build(f.input, false);
        policy = left.screenCircuitPlan(right, f.input, output) ? "selected" : "declined";
        if (!candidate.installLiveTransferSetup(left))
            throw std::runtime_error("cannot install screened circuit plan: " + name);
    } else {
        candidate.prepareCircuitPlan();
    }
    candidate.useCircuitPlan(true);
    const bool drive = candidate.hasLinearDrivePlan();
    std::array<wdf::Engine*, 2> engines{&reference, &candidate};
    std::array<Times, 2> times;
    std::array<long long, 2> rejects{};
    std::array<std::array<double, frames>, 2> audio{};
    std::array<std::array<bool, frames>, 2> rejected{};
    int rejectionMismatches = 0;
    std::vector<double> sources = reference.dcValues();
    const double inputBias = sources.at(f.input);
    std::array<double, frames> stimulus{};
    double peakError = 0.0, squaredError = 0.0, peak = 0.0;
    long long attempts = 0, hits = 0;
    long long affineAttempts = 0, affineHits = 0;
    for (int b = -warmup; b < blocks; ++b) {
        // Quiet passage, 360 mV hard attack, release and silence. Identical input
        // for each engine. Sample generation and comparison are outside timing.
        for (int i = 0; i < frames; ++i) {
            const int n = std::max(0, b * frames + i);
            const double t = static_cast<double>(n) / rate;
            const int phase = n % 12000;
            const double gain = phase < 3000 ? 0.02 : phase < 7500 ? 0.36 :
                                phase < 10500 ? 0.36 * std::exp(-double(phase - 7500) / 500.0) : 0.0;
            stimulus[i] = gain * (0.7 * std::sin(2 * std::numbers::pi * 110 * t) +
                                  0.3 * std::sin(2 * std::numbers::pi * 1729 * t));
        }
        if (b == 0) {
            attempts = candidate.circuitPlanAttempts();
            hits = candidate.circuitPlanHits();
            affineAttempts = candidate.affineRootAttempts();
            affineHits = candidate.affineRootHits();
            for (int m = 0; m < 2; ++m)
                rejects[m] = engines[m]->stateRejectCount();
        }
        for (int order = 0; order < 2; ++order) {
            const int m = (b + warmup + order) % 2;
            auto& e = *engines[m];
            const double start = timeMs();
            for (int i = 0; i < frames; ++i) {
                sources[f.input] = inputBias + stimulus[i];
                audio[m][i] = e.step(sources, 60, tolerance)[static_cast<size_t>(output)];
                rejected[m][i] = e.lastStateRejected();
            }
            const double ms = timeMs() - start;
            if (b >= 0)
                times[m].ms.push_back(ms);
        }
        if (b >= 0) {
            for (int i = 0; i < frames; ++i) {
                rejectionMismatches += rejected[0][i] != rejected[1][i];
                const double delta = std::abs(audio[1][i] - audio[0][i]);
                if (!std::isfinite(delta))
                    throw std::runtime_error("nonfinite audio: " + name);
                peak = std::max(peak, std::abs(audio[0][i]));
                peakError = std::max(peakError, delta);
                squaredError += delta * delta;
            }
        }
    }
    for (auto& side : times)
        std::sort(side.ms.begin(), side.ms.end());
    std::printf("%s | %s | %d | %.4f %.4f | %.4f %.4f | %zu %zu | %lld/%lld | %lld %lld %d | %.7g %.7g %.7g | %lld/%lld\n",
                name.c_str(), policy, int(drive), times[0].quantile(.5), times[1].quantile(.5),
                times[0].quantile(.99), times[1].quantile(.99), times[0].missed(), times[1].missed(),
                candidate.circuitPlanHits() - hits, candidate.circuitPlanAttempts() - attempts,
                reference.stateRejectCount() - rejects[0], candidate.stateRejectCount() - rejects[1],
                rejectionMismatches,
                peakError, std::sqrt(squaredError / (blocks * frames)), peak,
                candidate.affineRootHits() - affineHits, candidate.affineRootAttempts() - affineAttempts);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv) {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    std::puts("Thread CPU time (engine only)");
#else
    std::puts("Wall clock time (engine only)");
#endif
    std::puts("48 kHz mono 1x | 128 frames | 375 paired blocks | times in ms, reference then compiled");
    std::puts("fixture | policy | drive plan | median | p99 | missed | root hits/attempts | rejected reference/compiled/mismatched | peak error / RMS error / reference peak (V) | affine hits/attempts");
    try {
        const bool screen = argc > 1 && std::string(argv[1]) == "--screen";
        const int first = screen ? 2 : 1;
        if (argc > first) {
            for (int i = first; i < argc; ++i)
                run(argv[i], screen);
        } else {
            for (const char* fixture : {"rc-ladder",
                     "schematics/pedals_fx/EHX Big Muff Pi Ram's Head Violet.livewdf",
                     "schematics/studio/eq/Manley Massive Passive Standard Circuit Model.livewdf",
                     "schematics/studio/eq/Neve 1081 Gold Label Equalizer.livewdf"})
                run(fixture, screen);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
