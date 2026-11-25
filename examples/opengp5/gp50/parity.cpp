#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

#define GP5_FAST
#include "params.h"
#include "dsp/wdf_solver.h"

int main(int argc, char** argv)
{
    try {
        if (argc != 2)
            throw std::runtime_error("expected the DOD 250 schematic path");
        wdf::LiveWDFDocument doc;
        std::string error;
        if (!wdf::LiveWDFDocument::loadFromFile(argv[1], doc, error))
            throw std::runtime_error(error);
        const auto circuit = doc.toCircuit();
        for (double amplitude : {0.36, 1.2, 5.0}) {
            wdf::Engine reference(circuit, 44100);
            auto sources = reference.dcValues();
            const int output = reference.nodeIndex("out");
            wdf_reset();
            double peakError = 0, errorEnergy = 0, outputEnergy = 0;
            unsigned gain = 0;
            unsigned level = DOD250_LEVEL_DEFAULT;
            for (unsigned frame = 0; frame < 65536; ++frame) {
                if (frame % 512 == 0) {
                    const unsigned block = frame / 512;
                    gain = block % WDF_GAINS;
                    level = block % (DOD250_LEVEL_MAX + 1);
                    if (!reference.setPotPosition("VR7", static_cast<double>(gain) / DOD250_GAIN_MAX) ||
                        !reference.setPotPosition("VR10", static_cast<double>(level) / DOD250_LEVEL_MAX))
                        throw std::runtime_error("could not set DOD 250 pots in the reference engine");
                }
                const float input = static_cast<float>(amplitude * (0.63 * std::sin(frame * 0.062689377) +
                    0.29 * std::sin(frame * 0.03632) + 0.08 * std::sin(frame * 0.4123)));
                sources[0] = input;
                double expected = reference.step(sources).at(output);
                double actual = wdf_step(input, gain, level_scales[level]);
                double difference = actual - expected;
                if (!std::isfinite(difference))
                    throw std::runtime_error("non-finite DOD 250 sample");
                peakError = std::max(peakError, std::abs(difference));
                errorEnergy += difference * difference;
                outputEnergy += expected * expected;
            }
            const double relativeErrorDb = 10.0 * std::log10(errorEnergy / outputEnergy);
            std::cout << amplitude << " V input: peak error " << peakError
                      << " V, relative RMS error " << relativeErrorDb << " dB\n";
            if (!(peakError < 50e-6 && relativeErrorDb < -85.0))
                throw std::runtime_error("DOD 250 recurrence diverged from the reference with moving pots");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
