#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"
#include "livewdf/PreviewFingerprint.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Samples = std::vector<double>;

namespace {

void save(const fs::path& path, const Samples& samples) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(samples.size() * sizeof(double)));
    if (!out)
        throw std::runtime_error("Cannot write " + path.string());
}

Samples excitation(int rate, double level) {
    Samples input(rate);
    double peak = 0.0;
    const auto sine = [](double cycles) { return std::sin(2.0 * std::numbers::pi * cycles); };
    for (size_t i = 0; i < input.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double phase = std::fmod(t, 0.15);
        const double envelope = std::min(1.0, phase / 0.001) * std::exp(-phase / 0.045);
        input[i] = envelope * (0.41 * sine(81 * t) + 0.34 * sine(333 * t + 47 * t * t) +
                               0.16 * sine(2146 * t + 1001 * t * t) + 0.09 * sine(4913 * t));
        peak = std::max(peak, std::abs(input[i]));
    }
    for (double& value : input)
        value *= level / peak;
    return input;
}

void measure(const fs::path& file, int rate, const fs::path& destination) {
    if (rate < 8000 || rate > 384000)
        throw std::runtime_error("Supported sample rates are 8000 through 384000 Hz");
    wdf::LiveWDFDocument document;
    std::string error;
    if (!document.loadFromFile(file.string(), document, error))
        throw std::runtime_error(error);
    const auto circuit = document.toCircuit();
    const auto inputId = document.inputId();
    const auto source = std::find_if(circuit.src.begin(), circuit.src.end(),
                                     [&](const auto& part) { return part.name == inputId; });
    if (source == circuit.src.end())
        throw std::runtime_error("Missing circuit input");
    const size_t port = static_cast<size_t>(source - circuit.src.begin());

    fs::create_directories(destination);
    std::ofstream fingerprint(destination / "engine.sha256");
    fingerprint << LIVEWDF_PREVIEW_ENGINE_SHA256;
    fingerprint.close();
    if (!fingerprint)
        throw std::runtime_error("Cannot write engine fingerprint");
    std::ofstream canonical(destination / "document.json", std::ios::binary);
    canonical << document.toJson();
    canonical.close();
    if (!canonical)
        throw std::runtime_error("Cannot write canonical circuit document");

    const auto render = [&](const Samples& input) {
        wdf::Engine engine(circuit, rate);
        engine.prepareMixedAffineReduction(port);
        engine.useMixedAffineReduction(true);
        auto sources = engine.dcValues();
        const double inputBias = sources[port];
        const auto output = engine.nodeIndex(document.outputNet());
        if (output < 0)
            throw std::runtime_error("Missing circuit output");
        for (int i = 0; i < 8192; ++i)
            engine.step(sources, wdf::Engine::kDefaultMaxIterations, wdf::Engine::kFastLiveTolerance);
        Samples response;
        response.reserve(input.size());
        for (const double volts : input) {
            sources[port] = inputBias + volts;
            const double sample = engine.step(sources, wdf::Engine::kDefaultMaxIterations,
                                              wdf::Engine::kFastLiveTolerance)[output];
            if (!std::isfinite(sample) || engine.lastStateRejected())
                throw std::runtime_error("Reference solver failed during qualification");
            response.push_back(sample);
        }
        return response;
    };
    Samples impulse(static_cast<size_t>(std::lround(32768.0 * rate / 48000.0)));
    save(destination / "zero.f64", render(impulse));
    for (double level : {0.01, 0.7}) {
        impulse[0] = level;
        auto positive = render(impulse);
        impulse[0] = -level;
        const auto negative = render(impulse);
        for (size_t i = 0; i < positive.size(); ++i)
            positive[i] = (positive[i] - negative[i]) / (2 * level);
        save(destination / (level < 0.1 ? "small.f64" : "large.f64"), positive);
    }
    for (const int millivolts : {35, 360, 1200}) {
        const std::string stem = std::to_string(millivolts);
        const auto input = excitation(rate, millivolts / 1000.0);
        save(destination / (stem + ".input.f64"), input);
        save(destination / (stem + ".wet.f64"), render(input));
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: livewdf_preview_probe FILE.livewdf SAMPLE_RATE DIRECTORY\n";
        return 2;
    }
    try {
        measure(argv[1], std::stoi(argv[2]), argv[3]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
