#include "format/PortImpedance.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
constexpr int states = 8, junctions = 6, positions = 17;
constexpr int inputColumn = states, currentColumn = states + 1;
using Row = std::array<double, currentColumn + junctions>;

void add(Row& row, const Row& term, double weight)
{
    for (size_t column = 0; column < row.size(); ++column)
        row[column] += weight * term[column];
}

void constant(double value)
{
    const auto narrowed = static_cast<float>(value);
    if (!std::isfinite(narrowed)) throw std::runtime_error("invalid Cry Baby coefficient");
    std::cout << std::hexfloat << narrowed << 'f';
}

template<typename Values>
void emit(const Values& values, size_t count)
{
    std::cout << '{';
    for (size_t i = 0; i < count; ++i) {
        constant(values.at(i));
        std::cout << ',';
    }
    std::cout << "},\n";
}

void emitPosition(wdf::Engine& engine, int input, const std::string& outputNet,
                  const std::vector<double>& baselineStates)
{
    if (engine.capCount() != 7 || engine.inductorCount() != 1 || engine.currentSourceCount() ||
        engine.rootCount() != 9 || engine.bjtCount() != 3 || engine.rootLayout().nRed != junctions)
        throw std::runtime_error("unexpected Cry Baby root or reactive count");

    const auto& layout = engine.rootLayout();
    std::array<double, junctions> biasArgument{};
    std::vector<double> biasNodes, v;
    if (!engine.solveDc(biasNodes, v)) throw std::runtime_error("could not solve Cry Baby bias");
    for (int device = 0; device < 3; ++device) {
        const auto& root = layout.devices.at(device);
        if (root.kind != wdf::RootKind::Bjt || root.portBase != 3 * device)
            throw std::runtime_error("unexpected Cry Baby root order");
        const auto& bjt = engine.compiled().bjt.at(root.compIndex).model;
        if (bjt.Is != 1e-14 || bjt.Bf != 300.0 || bjt.Br != 4.0 || bjt.pnp || bjt.Nf != 1.0 ||
            bjt.Nr != 1.0 || bjt.Vt != 25.85e-3 || bjt.Vaf || bjt.Var || bjt.Ikf || bjt.Ise || bjt.cbe || bjt.cbc)
            throw std::runtime_error("expected the original three 2N3904 Ebers–Moll cards");
        biasArgument[device * 2] = v.at(root.portBase + 1) / bjt.Vt;
        biasArgument[device * 2 + 1] = (v.at(root.portBase + 1) - v.at(root.portBase)) / bjt.Vt;
    }

    // I_C = I_BE - 1.25 I_BC; I_B = I_BE/300 + I_BC/4.
    // The third current returns through the emitter and is already implicit.
    const auto& xe = engine.Xe();
    const auto& xr = engine.Xr();
    const auto& z = engine.rootImpedance();
    const auto& ports = engine.liveElementPorts();
    const auto& initial = engine.capacitorStates();
    for (int i = 0; i < states; ++i) {
        const double current = i == 7 ? engine.inductorStates().front() : initial.at(i);
        if (std::abs(current - baselineStates.at(i)) > 1e-7)
            throw std::runtime_error("treadle changed the Cry Baby DC operating point");
    }
    const auto waveIndex = [&](int port) {
        return engine.capOf()[port] >= 0 ? engine.capOf()[port] : 7 + engine.indOf()[port];
    };
    const auto source = [&](const wdf::AffineOffset& offset) {
        double gain = 0.0;
        for (auto [index, value] : offset.coef) if (index == input) gain += value;
        return gain;
    };
    std::vector<Row> nodes(engine.nodeCount());
    for (size_t node = 0; node < nodes.size(); ++node) {
        auto& row = nodes[node];
        for (size_t j = 0; j < ports.size(); ++j) {
            const auto port = ports[j];
            const double gain = xe[node * ports.size() + j];
            row[inputColumn] -= gain * source(engine.elementOffsets()[port]);
            if (engine.capOf()[port] >= 0 || engine.indOf()[port] >= 0)
                row[waveIndex(port)] += engine.capOf()[port] >= 0 ? gain : -gain;
        }
        for (int device = 0; device < 3; ++device) {
            const double c = -xr[node * 9 + 3 * device], b = -xr[node * 9 + 3 * device + 1];
            row[currentColumn + 2 * device] = c + b / 300.0;
            row[currentColumn + 2 * device + 1] = -1.25 * c + 0.25 * b;
        }
    }
    const auto open = [&](int port) {
        const auto [a, b] = engine.rootPorts().at(port);
        Row row{};
        row[inputColumn] = source(engine.rootOffsets().at(port));
        if (a >= 0) add(row, nodes.at(a), 1.0);
        if (b >= 0) add(row, nodes.at(b), -1.0);
        return row;
    };
    std::cout << "{\n{";
    for (int row = 0; row < junctions; ++row) {
        const int root = 3 * (row / 2);
        Row coeff = open(root + 1);
        if (row % 2) add(coeff, open(root), -1.0);
        emit(coeff, currentColumn);
    }
    std::cout << "},{\n";
    for (int row = 0; row < junctions; ++row) {
        const int root = 3 * (row / 2);
        std::array<double, junctions> values{};
        for (int device = 0; device < 3; ++device) {
            const auto coefficient = [&](int localPort) {
                const int column = 3 * device + localPort;
                return z[(root + 1) * 9 + column] - (row % 2 ? z[root * 9 + column] : 0.0);
            };
            const double c = coefficient(0), b = coefficient(1);
            values[2 * device] = c + b / 300.0;
            values[2 * device + 1] = -1.25 * c + 0.25 * b;
        }
        emit(values, values.size());
    }
    std::array<Row, states + 1> next{};
    for (int port = 0; port < engine.elementCount(); ++port) {
        if (engine.capOf()[port] < 0 && engine.indOf()[port] < 0) continue;
        const int state = waveIndex(port);
        Row& row = next[state];
        row[state] = engine.capOf()[port] >= 0 ? -1.0 : 1.0;
        row[inputColumn] = 2.0 * source(engine.elementOffsets()[port]);
        const int a = engine.elementPortA()[port], b = engine.elementPortB()[port];
        if (a >= 0) add(row, nodes.at(a), 2.0);
        if (b >= 0) add(row, nodes.at(b), -2.0);
    }
    const auto& [net, offset] = engine.compiled().info.at(outputNet);
    if (!net.empty()) next[states] = nodes.at(engine.nodeIndex(net));
    next[states][inputColumn] += offset.at(1 + input);
    std::cout << "},{\n";
    for (const auto& row : next) emit(row, row.size());
    std::cout << "},\n";
    emit(biasArgument, biasArgument.size());
    for (auto& argument : biasArgument) argument = 1e-14 * std::exp(argument);
    emit(biasArgument, biasArgument.size());
    std::cout << "},\n";
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) throw std::runtime_error("expected the Dunlop Cry Baby GCB-95 schematic path");
        wdf::LiveWDFDocument doc;
        std::string error;
        if (!wdf::LiveWDFDocument::loadFromFile(argv[1], doc, error)) throw std::runtime_error(error);
        const auto circuit = wdf::circuitWithPortImpedances(doc);
        if (circuit.pots.size() != 1 || circuit.pots.front().name != "POT1" ||
            circuit.pots.front().totalResistance != 100000 || circuit.pots.front().taper != wdf::PotTaper::Log)
            throw std::runtime_error("expected the factory Cry Baby treadle");
        const auto input = std::find_if(circuit.src.begin(), circuit.src.end(),
                                        [&](const auto& source) { return source.name == doc.inputId(); });
        if (input == circuit.src.end()) throw std::runtime_error("missing Cry Baby input");

        wdf::Engine engine(circuit, 44100.0);
        if (engine.capCount() != 7 || engine.inductorCount() != 1)
            throw std::runtime_error("expected the factory Cry Baby reactive components");
        auto state = engine.capacitorStates();
        state.push_back(engine.inductorStates().front());
        std::cout << "/* Generated for the factory LiveWDF Cry Baby at 44.1 kHz. */\n"
            << "#define CRYBABY_STATES " << states << "\n#define CRYBABY_JUNCTIONS " << junctions
            << "\n#define CRYBABY_POSITIONS " << positions << "\n#define CRYBABY_INPUT_VOLTS ";
        constant(doc.inputVoltsPerFullScale());
        std::cout << "\n#define CRYBABY_OUTPUT_VOLTS ";
        constant(doc.outputFullScaleVolts());
        std::cout << "\nstruct CryBabyTransfer {\n"
            "    float drive[6][9], impedance[6][6], advance[9][15];\n"
            "    float bias_argument[6], bias_current[6];\n"
            "};\nstatic const struct CryBabyTransfer crybaby_transfer[CRYBABY_POSITIONS] = {\n";
        for (int i = 0; i < positions; ++i) {
            if (!engine.setPotPosition(0, static_cast<double>(i) / (positions - 1)))
                throw std::runtime_error("could not set Cry Baby treadle position");
            engine.reset();
            emitPosition(engine, static_cast<int>(input - circuit.src.begin()), doc.outputNet(), state);
        }
        std::cout << "};\n";
        if (!std::cout) throw std::runtime_error("could not write Cry Baby transfers");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
