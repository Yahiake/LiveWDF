#include "format/Document.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

double threadMicros()
{
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return now.tv_sec * 1e6 + now.tv_nsec * 1e-3;
}

int main(int argc, char** argv)
{
    if (argc != 4)
        return 2;
    const double rate = std::stod(argv[2]);
    const int repeatCount = std::stoi(argv[3]);
    wdf::LiveWDFDocument doc;
    std::string error;
    if (!doc.loadFromFile(argv[1], error)) {
        std::cerr << error << '\n';
        return 3;
    }
    const auto circuit = doc.toCircuit();
    std::vector<double> sources;
    for (const auto& s : circuit.src)
        sources.push_back(s.dc);
    for (const auto& s : circuit.isrc)
        sources.push_back(s.dc);
    const auto input = std::find_if(circuit.src.begin(), circuit.src.end(),
                       [&](const auto& s) { return s.name == doc.inputId(); });
    const int audioIn = input == circuit.src.end() ? -1 : static_cast<int>(input - circuit.src.begin());

    for (int run = 0; run < repeatCount; ++run) {
        std::unique_ptr<wdf::Engine> engines[2];
        double elapsed[2]{};
        for (int offset = 0; offset < 2; ++offset) {
            const bool legacy = (run + offset) % 2 == 0;
            const auto start = threadMicros();
            auto engine = std::make_unique<wdf::Engine>(circuit, rate);
            if (legacy) {
                std::vector<double> nodes, roots;
                engine->solveDc(nodes, roots);
                engine->reset();
            }
            for (int sample = 0; sample < 64; ++sample)
                engine->step(sources);
            elapsed[!legacy] = threadMicros() - start;
            engines[!legacy] = std::move(engine);
        }

        std::size_t different = 0;
        double maxError = 0;
        auto drive = sources;
        for (int sample = 0; sample < 1024; ++sample) {
            if (audioIn >= 0)
                drive[static_cast<std::size_t>(audioIn)] = 0.12 * std::sin(
                    2.0 * std::numbers::pi * 440.0 * sample / rate);
            const auto& a = engines[0]->step(drive);
            const auto& b = engines[1]->step(drive);
            if (a.size() != b.size())
                return 4;
            for (std::size_t n = 0; n < a.size(); ++n) {
                if (std::memcmp(&a[n], &b[n], sizeof(double)) != 0)
                    ++different;
                maxError = std::max(maxError, std::abs(a[n] - b[n]));
            }
        }
        std::cout << rate << ',' << run << ',' << std::setprecision(10)
                  << elapsed[0] << ',' << elapsed[1] << ',' << different << ','
                  << maxError << ',' << engines[0]->stateRejectCount() << ','
                  << engines[1]->stateRejectCount() << '\n';
    }
}
