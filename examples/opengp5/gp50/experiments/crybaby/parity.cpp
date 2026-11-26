#include "format/PortImpedance.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

#define GP5_FAST
#include "crybaby.h"

int main(int argc, char** argv)
{
    try {
        if (argc != 2) throw std::runtime_error("expected the Dunlop Cry Baby GCB-95 schematic path");
        wdf::LiveWDFDocument doc;
        std::string error;
        if (!wdf::LiveWDFDocument::loadFromFile(argv[1], doc, error)) throw std::runtime_error(error);
        const auto circuit = wdf::circuitWithPortImpedances(doc);
        const auto input = std::find_if(circuit.src.begin(), circuit.src.end(),
                                        [&](const auto& source) { return source.name == doc.inputId(); });
        if (input == circuit.src.end()) throw std::runtime_error("missing Cry Baby input");

        bool passed = true;
        for (float amplitude : {0.02f, CRYBABY_INPUT_VOLTS, 2.0f}) {
            wdf::Engine reference(circuit, 44100.0);
            auto sources = reference.dcValues();
            const auto [output, outputOffset] = reference.compiled().info.at(doc.outputNet());
            const int outputIndex = reference.nodeIndex(output);
            CryBabyState state{};
            double maxError = 0.0, sumError = 0.0, sumSignal = 0.0;
            unsigned rejected = 0, maxBlock = 0, iterations = 0, totalIterations = 0;
            unsigned position = 0;
            for (unsigned i = 0; i < 65536; ++i) {
                if (i % 256 == 0) {
                    const unsigned motion = (i / 256) % (2 * (CRYBABY_POSITIONS - 1));
                    position = std::min(motion, 2u * (CRYBABY_POSITIONS - 1u) - motion);
                    if (!reference.setPotPosition("POT1", static_cast<double>(position) / (CRYBABY_POSITIONS - 1)))
                        throw std::runtime_error("could not adjust reference treadle");
                }
                const float x = static_cast<float>(amplitude * (0.62 * std::sin(i * 0.056945) +
                    0.27 * std::sin(i * 0.151773) + 0.11 * std::sin(i * 0.262841)));
                sources[static_cast<size_t>(input - circuit.src.begin())] = x;
                const auto& nodes = reference.step(sources);
                double expected = nodes.at(outputIndex) + outputOffset.at(0);
                for (size_t j = 0; j < sources.size(); ++j) expected += outputOffset.at(1 + j) * sources[j];

                float actual = 0.0f;
                const int accepted = crybaby_step(&state, x, position, &actual);
                if (!accepted) ++rejected;
                iterations += accepted ? accepted : CRYBABY_MAX_ITERATIONS;
                if (i % 64 == 63) {
                    maxBlock = std::max(maxBlock, iterations);
                    totalIterations += iterations;
                    iterations = 0;
                }
                double difference = expected - actual;
                if (!std::isfinite(difference)) throw std::runtime_error("non-finite Cry Baby output");
                maxError = std::max(maxError, std::abs(difference));
                sumError += difference * difference;
                sumSignal += expected * expected;
            }
            const double relative = 10.0 * std::log10(sumError / sumSignal);
            std::cout << amplitude << " V peak input, max error " << maxError << " V, relative RMS "
                << relative << " dB, rejected " << rejected << ", max block evaluations " << maxBlock
                << ", mean " << totalIterations / 65536.0 << "\n";
            if (amplitude <= CRYBABY_INPUT_VOLTS && (rejected || maxError > 0.0005 || relative > -60.0))
                passed = false;
        }
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
