#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

#define GP5_FAST
#include "dsp/microamp.h"

int main(int argc, char** argv)
{
    try {
        if (argc != 2) throw std::runtime_error("expected the MXR Micro Amp schematic path");
        wdf::LiveWDFDocument doc;
        std::string error;
        if (!wdf::LiveWDFDocument::loadFromFile(argv[1], doc, error))
            throw std::runtime_error(error);
        const auto circuit = doc.toCircuit();
        const auto source = std::find_if(circuit.src.begin(), circuit.src.end(),
                                        [&](const auto& value) { return value.name == doc.inputId(); });
        if (source == circuit.src.end()) throw std::runtime_error("missing Micro Amp input");
        const size_t input = static_cast<size_t>(source - circuit.src.begin());
        unsigned clipped = 0;
        for (double amplitude : {0.1, 0.87059607, 5.0}) {
            wdf::Engine reference(circuit, 44100.0);
            auto sources = reference.dcValues();
            const auto [outNet, outOffset] = reference.compiled().info.at(doc.outputNet());
            const int outIndex = reference.nodeIndex(outNet);
            const auto& amp = reference.opAmpModel(0);
            microamp_reset();
            double peakError = 0.0, errorEnergy = 0.0, outputEnergy = 0.0;
            unsigned gain = 0;
            for (unsigned frame = 0; frame < 65536; ++frame) {
                if (frame % 512 == 0) {
                    gain = frame / 512 % MICROAMP_GAIN_COUNT;
                    if (!reference.setPotPosition("R5", static_cast<double>(gain) / (MICROAMP_GAIN_COUNT - 1)))
                        throw std::runtime_error("could not set Micro Amp Gain in the reference");
                }
                const float x = static_cast<float>(amplitude * (0.65 * std::sin(frame * 0.062689377) +
                    0.35 * std::sin(frame * 0.03632)));
                sources[input] = x;
                const auto& nodes = reference.step(sources);
                double expected = outOffset.at(0);
                if (outIndex >= 0) expected += nodes.at(outIndex);
                for (size_t i = 0; i < sources.size(); ++i)
                    expected += outOffset.at(i + 1) * sources[i];
                const double pole = reference.opAmpPole(0);
                if (pole == amp.railLow() || pole == amp.railHigh()) ++clipped;
                double difference = microamp_step(x, gain, 1.0f) - expected;
                if (!std::isfinite(difference))
                    throw std::runtime_error("non-finite Micro Amp sample");
                peakError = std::max(peakError, std::abs(difference));
                errorEnergy += difference * difference;
                outputEnergy += expected * expected;
            }
            const double relativeDb = 10.0 * std::log10(errorEnergy / outputEnergy);
            std::cout << amplitude << " V input: peak error " << peakError
                      << " V, relative RMS error " << relativeDb << " dB\n";
            if (!(peakError < 50e-6 && relativeDb < -85.0))
                throw std::runtime_error("Micro Amp recurrence diverged from desktop LiveWDF");
        }
        if (!clipped) throw std::runtime_error("expected the overdriven test to hit the amplifier's rails");
        std::cout << clipped << " reference samples reached the TL061 rails\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
