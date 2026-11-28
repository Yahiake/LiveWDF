#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
constexpr int states = 5;
constexpr int gainCount = 17;
constexpr int inputColumn = states + 1;
constexpr int poleColumn = states + 2;
using Row = std::array<double, poleColumn + 1>;

void add(Row& row, const Row& term, double weight)
{
    for (size_t column = 0; column < row.size(); ++column)
        row[column] += weight * term[column];
}

void constant(std::ostream& out, double value)
{
    float narrowed = static_cast<float>(value);
    if (!std::isfinite(narrowed))
        throw std::runtime_error("invalid Micro Amp recurrence coefficient");
    out << std::hexfloat << narrowed << 'f';
}

void emitRow(std::ostream& out, const Row& row)
{
    out << '{';
    for (double coefficient : row) {
        constant(out, coefficient);
        out << ',';
    }
    out << "},\n";
}

void emitModel(std::ostream& out, const wdf::Engine& engine, int input,
               const std::string& outputNet, const std::vector<double>& bias, double poleBias)
{
    if (engine.sourceCount() != 2 || engine.capCount() != states || engine.inductorCount() ||
        engine.currentSourceCount() || engine.rootCount() != 3 || engine.opAmpCount() != 1 ||
        engine.rootDeviceKind(0) != wdf::DeviceKind::OpAmp)
        throw std::runtime_error("expected a single loaded amplifier and five capacitors");
    const auto& amp = engine.opAmpModel(0);
    if (amp.type != wdf::OpAmpType::Tl061 || amp.softRails || amp.hasClassABOutput() ||
        engine.isOpAmpRailSagEnabled(0) || !(amp.railLow() < amp.railHigh()))
        throw std::runtime_error("expected the factory Micro Amp's hard-railed TL061");

    const auto affine = [&](const wdf::AffineOffset& offset) {
        Row row{};
        row[0] = offset.c0;
        for (auto [source, weight] : offset.coef) {
            if (source == input)
                row[inputColumn] += weight;
            else
                row[0] += weight * engine.dcValues().at(source);
        }
        return row;
    };
    std::vector<Row> nodes(engine.nodeCount());
    const auto& live = engine.liveElementPorts();
    for (size_t i = 0; i < live.size(); ++i) {
        const int port = live[i];
        Row wave{};
        add(wave, affine(engine.elementOffsets()[port]), -1.0);
        const int cap = engine.capOf()[port];
        if (cap >= 0) {
            wave[0] += bias.at(cap);
            wave[cap + 1] = 1.0;
        }
        for (size_t node = 0; node < nodes.size(); ++node)
            add(nodes[node], wave, engine.Xe()[node * live.size() + i]);
    }
    const auto across = [&](int a, int b, Row row) {
        if (a >= 0) add(row, nodes.at(a), 1.0);
        if (b >= 0) add(row, nodes.at(b), -1.0);
        return row;
    };
    const auto open = [&](int index) {
        const auto [a, b] = engine.rootPorts().at(index);
        return across(a, b, affine(engine.rootOffsets().at(index)));
    };
    const auto& z = engine.rootImpedance();
    const double weight = amp.outputVoltageWeight();
    const double alpha = std::clamp(wdf::OpAmpEngineLaw::alpha(amp, engine.fs()), 0.0, 1.0);
    const double gain = alpha * amp.openLoopGain / (1.0 + alpha);
    const double feedbackZ = z[3] - z[6];
    const double load = amp.outputImpedance + weight * z[0];
    const double feedback = 1.0 - gain * feedbackZ / load;
    if (!(feedbackZ <= 0.0 && load > 0.0 && feedback > 0.0))
        throw std::runtime_error("Micro Amp recurrence requires a monotone loaded output");

    // In the linear piece, solve the loaded output for the internal pole.
    // Each row uses deviations from the same DC state at every Gain setting.
    const Row openDrive = open(0);
    Row pole{};
    add(pole, open(1), gain / feedback);
    add(pole, open(2), -gain / feedback);
    add(pole, openDrive, -gain * feedbackZ * weight / (load * feedback));
    pole[poleColumn] = 1.0 / ((1.0 + alpha) * feedback);
    pole[0] += poleBias * (pole[poleColumn] - 1.0);

    // Substitute i = (weight * openDrive - pole) / load into each node.
    // The runtime can clamp the pole first and use the same recurrence.
    for (size_t node = 0; node < nodes.size(); ++node) {
        const double factor = engine.Xr()[node * 3] / load;
        add(nodes[node], openDrive, -factor * weight);
        nodes[node][0] += factor * poleBias;
        nodes[node][poleColumn] = factor;
    }
    const auto& [net, offset] = engine.compiled().info.at(outputNet);
    Row output = net.empty() ? Row{} : nodes.at(engine.nodeIndex(net));
    output[0] += offset.at(0);
    for (int source = 0; source < engine.sourceCount(); ++source) {
        if (source == input)
            output[inputColumn] += offset.at(1 + source);
        else
            output[0] += offset.at(1 + source) * engine.dcValues().at(source);
    }

    std::array<Row, states> next{};
    for (int port = 0; port < engine.elementCount(); ++port) {
        const int cap = engine.capOf()[port];
        if (cap < 0) continue;
        next[cap][0] = -2.0 * bias.at(cap);
        next[cap][cap + 1] = -1.0;
        add(next[cap], across(engine.elementPortA()[port], engine.elementPortB()[port],
                              affine(engine.elementOffsets()[port])), 2.0);
    }
    out << "{\n";
    emitRow(out, pole);
    emitRow(out, output);
    out << "{\n";
    for (const Row& row : next) emitRow(out, row);
    out << "},\n},\n";
}

void generate(std::ostream& out, const wdf::LiveWDFDocument& doc)
{
    auto circuit = doc.toCircuit();
    if (circuit.pots.size() != 1 || circuit.pots.front().name != "R5" ||
        circuit.pots.front().taper != wdf::PotTaper::RevLog || circuit.pots.front().position != 0.75)
        throw std::runtime_error("expected the factory MXR Micro Amp Gain control");
    const auto source = std::find_if(circuit.src.begin(), circuit.src.end(),
                                    [&](const auto& value) { return value.name == doc.inputId(); });
    if (source == circuit.src.end()) throw std::runtime_error("missing Micro Amp input");
    const int input = static_cast<int>(source - circuit.src.begin());
    wdf::Engine initial(circuit, 44100.0);
    const double poleBias = initial.opAmpPole(0);
    const auto& amp = initial.opAmpModel(0);
    out << "/* Generated from the LiveWDF MXR Micro Amp preset at 44.1 kHz. */\n"
        << "#define MICROAMP_STATES " << states << "u\n"
        << "#define MICROAMP_GAIN_COUNT " << gainCount << "u\n"
        << "#define MICROAMP_INPUT_VOLTS ";
    constant(out, doc.inputVoltsPerFullScale());
    out << "\n#define MICROAMP_OUTPUT_VOLTS ";
    constant(out, doc.outputFullScaleVolts());
    out << "\n#define MICROAMP_RAIL_LOW ";
    constant(out, amp.railLow() - poleBias);
    out << "\n#define MICROAMP_RAIL_HIGH ";
    constant(out, amp.railHigh() - poleBias);
    out << "\n#define MICROAMP_SLEW ";
    constant(out, wdf::OpAmpEngineLaw::maxDv(amp, initial.fs()));
    out << "\nstruct MicroampModel {\n"
        << "    float pole[MICROAMP_STATES + 3], output[MICROAMP_STATES + 3];\n"
        << "    float next[MICROAMP_STATES][MICROAMP_STATES + 3];\n"
        << "};\nstatic const struct MicroampModel microamp_models[MICROAMP_GAIN_COUNT] MICROAMP_TABLE = {\n";
    for (int position = 0; position < gainCount; ++position) {
        wdf::Engine adjusted(circuit, 44100.0);
        if (!adjusted.setPotPosition("R5", static_cast<double>(position) / (gainCount - 1)))
            throw std::runtime_error("could not set Micro Amp Gain");
        emitModel(out, adjusted, input, doc.outputNet(), initial.capacitorStates(), poleBias);
    }
    out << "};\n";
}
}

int main(int argc, char** argv)
{
    try {
        if (argc != 3)
            throw std::runtime_error("usage: livewdf_codegen_microamp <preset.livewdf> <microamp_coefficients.h>");
        wdf::LiveWDFDocument doc;
        std::string error;
        if (!wdf::LiveWDFDocument::loadFromFile(argv[1], doc, error))
            throw std::runtime_error(error);
        std::ostringstream header;
        header.imbue(std::locale::classic());
        generate(header, doc);
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        out << header.str();
        if (!out) throw std::runtime_error("could not write Micro Amp coefficients");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
