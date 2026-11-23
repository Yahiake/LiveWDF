#include "format/Document.h"
#include "wdf/Engine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

double cpuSeconds() {
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return now.tv_sec + 1e-9 * now.tv_nsec;
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        wdf::LiveWDFDocument document;
        std::string error;
        if (!document.loadFromFile(argv[1], error)) throw std::runtime_error(error);
        const double rate = std::stod(argv[2]);
        const double peak = std::stod(argv[3]);
        auto circuit = document.toCircuit();
        wdf::Engine engine(circuit, rate);
        const auto input = std::find_if(circuit.src.begin(), circuit.src.end(),
            [&](const auto& source) { return source.name == document.inputId(); });
        const auto output = engine.nodeIndex(document.outputNet());
        if (input == circuit.src.end() || output < 0) throw std::runtime_error("missing IO terminal");
        std::vector<double> sources;
        for (const auto& s : circuit.src) sources.push_back(s.dc);
        for (const auto& s : circuit.isrc) sources.push_back(s.dc);
        const auto inputIndex = static_cast<size_t>(input - circuit.src.begin());
        const int sampleCount = int(rate * .20);
        const int warmCount = int(rate * .12);
        engine.reset();
        int warmRejects = 0;
        for (int i = 0; i < warmCount; ++i) {
            engine.step(sources);
            warmRejects += engine.lastStateRejected();
        }
        double dc = engine.step(sources)[output];
        constexpr std::array<double, 4> tones{220., 997., 2100., 5900.};
        constexpr std::array<double, 4> weights{.34, .34, .20, .12};
        std::vector<double> wave(sampleCount);
        for (int i = 0; i < sampleCount; ++i) {
            const double t = 2.0 * std::numbers::pi * i / rate;
            const double env = std::min(1., std::min(double(i), double(sampleCount-1-i)) / (rate*.008));
            for (size_t j = 0; j < tones.size(); ++j)
                wave[i] += env * weights[j] * std::sin(tones[j] * t);
        }
        constexpr std::array<double, 4> levels{.1,.4,1.,2.};
        for (double level : levels) {
            int rejects = 0, nfinite = 0;
            double ss = 0, maxOut = 0, maxIn = 0;
            const auto start = cpuSeconds();
            for (int i = 0; i < sampleCount; ++i) {
                sources[inputIndex] = wave[i] * peak * level;
                const auto& nodes = engine.step(sources);
                const double v = nodes[output] - dc;
                rejects += engine.lastStateRejected();
                nfinite += !std::isfinite(v);
                ss += v*v;
                maxOut = std::max(maxOut, std::abs(v));
                maxIn = std::max(maxIn, std::abs(sources[inputIndex]));
            }
            const auto cpuUs = (cpuSeconds() - start)*1e6/sampleCount;
            std::printf("%.0f,%.1f,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%.4f,%zu\n",
                        rate,level,maxIn,maxOut,std::sqrt(ss/sampleCount),dc,warmRejects,rejects,nfinite,cpuUs,document.loadWarnings.size());
        }
    } catch (const std::exception& err) {
        std::fprintf(stderr, "%s\n", err.what());
        return 1;
    }
}
