#include "format/Document.h"
#include "format/PortImpedance.h"
#include "spice/MatchedPentodes.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr double relaxedTolerance = 1e-3;

std::vector<double> loadDi(const char* path, int rate) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff bytes = file.tellg();
    if (bytes <= 0 || bytes % sizeof(double) != 0)
        throw std::runtime_error("Expected mono raw float64 DI");
    const int padding = 2048 * (rate / 48000);
    std::vector<double> di(bytes / sizeof(double) + 3 * padding);
    if (!file.seekg(0).read(reinterpret_cast<char*>(di.data() + padding), bytes))
        throw std::runtime_error("Failed to read DI");
    double peak = 0.;
    for (double sample : di) {
        if (!std::isfinite(sample))
            throw std::runtime_error("Non-finite DI sample");
        peak = std::max(peak, std::abs(sample));
    }
    if (peak == 0.)
        throw std::runtime_error("Silent DI");
    const double gain = std::pow(10., -6. / 20.) / peak;
    for (auto& sample : di)
        sample *= gain;
    return di;
}

double cpuMicros() {
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return 1e6 * now.tv_sec + 1e-3 * now.tv_nsec;
}

struct Render {
    std::vector<double> volts;
    std::uint64_t rejects = 0;
};

Render run(const wdf::LiveWDFDocument& doc, const wdf::Circuit& circuit, size_t audio,
           const std::vector<double>& di, int rate, double tolerance, bool profile) {
    wdf::Engine engine(circuit, rate), peer(circuit, rate);
    const int output = engine.nodeIndex(doc.outputNet());
    if (output < 0)
        throw std::runtime_error("Output node unavailable");
    engine.prepareLiveControls();
    peer.prepareLiveControls();
    wdf::Engine::LiveTransferSetup setup(circuit), peerSetup(circuit);
    if (!engine.captureLiveTransferSetup(setup, true) ||
        !peer.captureLiveTransferSetup(peerSetup, true))
        throw std::runtime_error("Failed to capture Preview state");
    setup.build(audio, true);
    peerSetup.build(audio, true);
    setup.screenCircuitPlan(peerSetup, audio, output);
    setup.prepareCompiledPreview(audio, output, true, doc.previewPeakVolts(), doc.allowPreviewInputPad);
    if (!engine.installLiveTransferSetup(setup) || !engine.hasPackedDrivePlan())
        throw std::runtime_error("Packed Preview unavailable");
    engine.useCircuitPlan(true);
    engine.useMixedAffineReduction(true);
    engine.useLoadedJunctionPreview(true);
    engine.useCompiledOptical(true);
    auto sources = engine.dcValues();
    for (int i = 0; i < 4096; ++i)
        engine.step(sources);

    const size_t frames = 128 * (rate / 48000);
    const double inputVolts = doc.inputVoltsPerFullScale();
    std::vector<double> blocks(di.size() / frames);
    Render render{std::vector<double>(blocks.size() * frames)};
    const auto rejects = engine.stateRejectCount(), fallbacks = engine.blockFallbackCount();
    long long iterations = 0, trials = 0;
    double residual = 0.;
    engine.resetPhaseProfile();
    engine.setProfile(profile);
    for (size_t block = 0; block < blocks.size(); ++block) {
        const double start = cpuMicros();
        for (size_t n = block * frames; n < (block + 1) * frames; ++n) {
            sources[audio] = di[n] * inputVolts;
            if (!engine.stepBipolarPreview(sources[audio], render.volts[n]))
                render.volts[n] = engine.step(sources, wdf::Engine::kDefaultMaxIterations,
                                               tolerance)[output];
            if (profile) {
                iterations += engine.newtonIterations();
                trials += engine.lineSearchTrials();
                residual = std::max(residual, engine.lastResidual());
            }
        }
        blocks[block] = cpuMicros() - start;
    }
    std::ranges::sort(blocks);
    const double p99 = blocks[static_cast<size_t>(.99 * (blocks.size() - 1))] / 1000.;
    render.rejects = engine.stateRejectCount() - rejects;
    std::cout << "tol=" << tolerance << " blocks=" << blocks.size() << " ports=" << engine.rootCount()
              << " mean_ms=" << std::accumulate(blocks.begin(), blocks.end(), 0.) / (1000. * blocks.size())
              << " p99_ms=" << p99 << " rejects=" << render.rejects
              << " fallbacks=" << engine.blockFallbackCount() - fallbacks << '\n';
    if (profile) {
        const auto& timing = engine.phaseProfile();
        std::cout << "iterations=" << iterations << " line_trials=" << trials
                  << " max_residual=" << residual << " phase_seconds solve=" << timing.solve
                  << " network=" << timing.network << " offsets=" << timing.offsets
                  << " post=" << timing.post << " lu=" << timing.lu
                  << " line_search=" << timing.linesearch << '\n';
    }
    return render;
}

void compare(const Render& baseline, const Render& candidate) {
    double energy = 0., error = 0., peak = 0.;
    for (size_t n = 0; n < baseline.volts.size(); ++n) {
        const double delta = candidate.volts[n] - baseline.volts[n];
        if (!std::isfinite(delta))
            throw std::runtime_error("Non-finite output");
        energy += baseline.volts[n] * baseline.volts[n];
        error += delta * delta;
        peak = std::max(peak, std::abs(delta));
    }
    if (energy == 0.)
        throw std::runtime_error("Silent output");
    std::cout << "delta_db=" << 10. * std::log10(error / energy)
              << " delta_rms_v=" << std::sqrt(error / baseline.volts.size())
              << " delta_peak_v=" << peak << '\n';
    if (baseline.rejects || candidate.rejects)
        throw std::runtime_error("Rejected solver state");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 || (std::string_view(argv[4]) != "clean" && std::string_view(argv[4]) != "profile")) {
        std::cerr << "Usage: bench_pentode_preview circuit.livewdf mono.f64 48000|96000 clean|profile\n";
        return 2;
    }
    try {
        const int rate = std::stoi(argv[3]);
        if (rate != 48000 && rate != 96000)
            throw std::runtime_error("Expected 48000 or 96000 Hz");
        const bool profile = std::string_view(argv[4]) == "profile";
        const auto di = loadDi(argv[2], rate);
        wdf::LiveWDFDocument doc;
        std::string error;
        if (!doc.loadFromFile(argv[1], error))
            throw std::runtime_error(error);
        auto circuit = wdf::circuitWithPortImpedances(doc);
        const size_t audio = std::ranges::find(circuit.src, doc.inputId(), &wdf::Src::name) - circuit.src.begin();
        if (audio >= circuit.src.size() || circuit.pent.size() != 4 ||
            wdf::collapseMatchedPentodes(circuit, std::span(&audio, 1)) != 2)
            throw std::runtime_error("Expected one audio source and two eligible pentode pairs");
        for (int repeat = 0; repeat < (profile ? 1 : 3); ++repeat) {
            const bool reverse = repeat % 2 != 0;
            const double stock = wdf::Engine::kFastLiveTolerance;
            const auto first = run(doc, circuit, audio, di, rate, reverse ? relaxedTolerance : stock, profile);
            const auto second = run(doc, circuit, audio, di, rate, reverse ? stock : relaxedTolerance, profile);
            compare(reverse ? second : first, reverse ? first : second);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
