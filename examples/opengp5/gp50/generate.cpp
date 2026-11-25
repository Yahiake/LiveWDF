#include "format/LiveWDFFile.h"
#include "firmware/params.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <vector>

namespace {
constexpr int nStates = 6;
constexpr int nGain = DOD250_GAIN_MAX + 1;
constexpr int nPorts = 5;
constexpr int input = nStates + 1;
constexpr int ampCurrent = input + 1;
constexpr int diodeCurrent = input + 2;
constexpr int amp = 2;
constexpr int diode = 0;
constexpr int ticksPerVolt = 512;
using Row = std::array<double, nStates + 4>;
using Bias = std::array<double, nStates>;

void add(Row& row, const Row& term, double scale)
{
    for (size_t j = 0; j < row.size(); ++j)
        row[j] += scale * term[j];
}

template <typename Values>
void emitRow(std::ostream& out, const Values& values)
{
    out << '{';
    for (double value : values) {
        float narrowed = static_cast<float>(value);
        if (!std::isfinite(narrowed))
            throw std::runtime_error("invalid recurrence coefficient");
        out << std::hexfloat << narrowed << "f,";
    }
    out << '}';
}

void validate(const wdf::Circuit& circuit, const wdf::LiveWDFDocument& document)
{
    struct Connection { std::string_view name, a, b; };
    constexpr std::array resistors = {
        Connection{"R001", "in", "0"}, Connection{"R005", "n9", "n6"},
        Connection{"R012", "n1", "n2"}, Connection{"R002", "n2", "n5"},
        Connection{"R003", "n5", "0"}, Connection{"R004", "n5", "n6"},
        Connection{"R008", "n3", "n4"}, Connection{"R006", "n10", "n11"},
        Connection{"VR7_1", "n10", "0"}, Connection{"VR7_2", "0", "0"},
        Connection{"R009", "n7", "n8"}, Connection{"VR10_1", "n8", "out"},
        Connection{"VR10_2", "out", "0"},
    };
    constexpr std::array capacitors = {
        Connection{"C003", "in", "0"}, Connection{"C002", "in", "n9"},
        Connection{"C001", "n5", "0"}, Connection{"C004", "n11", "n3"},
        Connection{"C006", "n4", "n7"}, Connection{"C007", "n8", "0"},
    };
    const auto connectionsMatch = [](const auto& expected, const auto& actual) {
        return expected.size() == actual.size() &&
            std::equal(expected.begin(), expected.end(), actual.begin(), [](auto a, const auto& b) {
                return a.name == b.name && a.a == b.a && a.b == b.b && b.value > 0 && std::isfinite(b.value);
            });
    };
    if (document.inputId() != "IN1" || document.outputNet() != "out" ||
        !connectionsMatch(resistors, circuit.res) || !connectionsMatch(capacitors, circuit.cap) ||
        circuit.src.size() != 2 || circuit.pots.size() != 2 || circuit.opamp.size() != 1 || circuit.diode.size() != 2)
        throw std::runtime_error("expected the factory DOD 250 topology");
    const auto& in = circuit.src[0];
    const auto& supply = circuit.src[1];
    if (in.name != "IN1" || in.a != "in" || in.b != "0" || in.dc != 0 || !in.sin.empty() ||
        supply.name != "V1" || supply.a != "n1" || supply.b != "0" || supply.dc != 9 || !supply.sin.empty())
        throw std::runtime_error("unexpected DOD 250 sources");
    const auto& gain = circuit.pots[0];
    const auto& level = circuit.pots[1];
    if (gain.name != "VR7" || gain.taper != wdf::PotTaper::RevLog ||
        level.name != "VR10" || level.taper != wdf::PotTaper::Log ||
        level.position != static_cast<double>(DOD250_LEVEL_DEFAULT) / DOD250_LEVEL_MAX)
        throw std::runtime_error("unexpected DOD 250 controls");
    const auto& first = circuit.diode[0];
    const auto& second = circuit.diode[1];
    const auto& oa = circuit.opamp.front();
    if (first.a != "n8" || first.b != "0" || second.a != "0" || second.b != "n8" ||
        oa.model.type != wdf::OpAmpType::Lm741 || oa.out != "n4" || oa.neg != "n3" || oa.pos != "n6" ||
        oa.vcc != "n2" || oa.vee != "0" || oa.model.softRails || oa.model.hasClassABOutput())
        throw std::runtime_error("expected the DOD 250's hard-railed LM741 and antiparallel diodes");
    const auto is4148 = [](const wdf::DiodeModel& model) {
        return model.isat == 4.352e-9 && model.n == 1.905 && model.rs == 0.56 && model.rp == 1e6 && model.bv == 0;
    };
    if (!is4148(first.model) || !is4148(second.model))
        throw std::runtime_error("expected two identical 1N4148 cards");
}

void emitModel(std::ostream& out, const wdf::Engine& engine, const Bias& bias)
{
    if (engine.sourceCount() != 2 || engine.capCount() != nStates || engine.inductorCount() ||
        engine.currentSourceCount() || engine.rootCount() != nPorts || engine.opAmpCount() != 1 ||
        engine.rootDeviceKind(0) != wdf::DeviceKind::Diode || engine.rootDeviceKind(1) != wdf::DeviceKind::Diode ||
        engine.rootDeviceKind(2) != wdf::DeviceKind::OpAmp)
        throw std::runtime_error("unexpected DOD 250 root layout");

    const auto offset = [&](const wdf::AffineOffset& source) {
        Row row{};
        row[0] = source.c0;
        for (auto [index, scale] : source.coef) {
            if (index == 0)
                row[input] += scale;
            else
                row[0] += scale * engine.dcValues().at(index);
        }
        return row;
    };
    std::vector<Row> nodes(engine.nodeCount());
    const auto& ports = engine.liveElementPorts();
    for (size_t j = 0; j < ports.size(); ++j) {
        int port = ports[j];
        Row wave{};
        add(wave, offset(engine.elementOffsets()[port]), -1.0);
        int cap = engine.capOf()[port];
        if (cap >= 0) {
            wave[0] += bias[cap];
            wave[1 + cap] = 1.0;
        }
        for (size_t node = 0; node < nodes.size(); ++node)
            add(nodes[node], wave, engine.Xe()[node * ports.size() + j]);
    }
    const auto across = [&](int a, int b, Row row) {
        if (a >= 0) add(row, nodes.at(a), 1.0);
        if (b >= 0) add(row, nodes.at(b), -1.0);
        return row;
    };
    const auto open = [&](int port) {
        const auto [a, b] = engine.rootPorts()[port];
        return across(a, b, offset(engine.rootOffsets()[port]));
    };
    const auto z = [&](int row, int col) { return engine.rootImpedance()[row * nPorts + col]; };
    for (int row = 0; row < nPorts; ++row) {
        if (std::abs(z(row, 0) + z(row, 1)) > 1e-6 || std::abs(z(0, row) + z(1, row)) > 1e-6)
            throw std::runtime_error("diode ports must be opposite orientations of one branch");
    }

    // Eliminate the amplifier output current. The two resulting Thevenin
    // impedances are for the linear and rail-clamped amplifier branches.
    const auto& oa = engine.opAmpModel(0);
    const double weight = oa.outputVoltageWeight();
    const double alpha = std::clamp(wdf::OpAmpEngineLaw::alpha(oa, engine.fs()), 0.0, 1.0);
    const double gain = alpha * oa.openLoopGain / (1.0 + alpha);
    const double feedbackZ = z(amp + 1, amp) - z(amp + 2, amp);
    const double diodeFeedbackZ = z(amp + 1, diode) - z(amp + 2, diode);
    const double load = oa.outputImpedance + weight * z(amp, amp);
    const double feedback = 1.0 - gain * feedbackZ / load;
    const double poleFromCurrent = (gain * feedbackZ * weight * z(amp, diode) / load - gain * diodeFeedbackZ) / feedback;
    const double sourceToDiode = z(diode, amp) / load;
    const double zClamped = z(diode, diode) - sourceToDiode * weight * z(amp, diode);
    const double zLinear = zClamped - sourceToDiode * poleFromCurrent;
    if (!(load > 0 && feedback > 0 && zClamped > 0 && zLinear > 0 && oa.railLow() < oa.railHigh()))
        throw std::runtime_error("DOD 250 root reduction requires a monotone load");
    Row ampDrive{};
    add(ampDrive, open(amp), weight);
    Row ampSource{};
    add(ampSource, open(amp + 1), gain / feedback);
    add(ampSource, open(amp + 2), -gain / feedback);
    add(ampSource, ampDrive, -gain * feedbackZ / (load * feedback));
    Row diodeDrive = open(diode);
    add(diodeDrive, ampDrive, -sourceToDiode);

    for (size_t node = 0; node < nodes.size(); ++node) {
        nodes[node][ampCurrent] = -engine.Xr()[node * nPorts + amp];
        nodes[node][diodeCurrent] = -engine.Xr()[node * nPorts + diode];
    }
    const auto& [outNet, outOffset] = engine.compiled().info.at("out");
    Row output = nodes.at(engine.nodeIndex(outNet));
    output[0] += outOffset.at(0) + outOffset.at(2) * engine.dcValues().at(1);
    output[input] += outOffset.at(1);
    std::array<Row, nStates> next{};
    for (int port = 0; port < engine.elementCount(); ++port) {
        int cap = engine.capOf()[port];
        if (cap < 0) continue;
        next[cap][0] = -2.0 * bias[cap];
        next[cap][1 + cap] = -1.0;
        add(next[cap], across(engine.elementPortA()[port], engine.elementPortB()[port],
                             offset(engine.elementOffsets()[port])), 2.0);
    }
    out << "{\n";
    for (const Row& row : {ampDrive, ampSource, diodeDrive, output}) {
        emitRow(out, row);
        out << ",\n";
    }
    out << "{\n";
    for (const Row& row : next) {
        emitRow(out, row);
        out << ",\n";
    }
    out << "},\n";
    emitRow(out, std::array{1.0 / ((1.0 + alpha) * feedback), poleFromCurrent, sourceToDiode,
                           zClamped, zLinear, 1.0 / load, weight * z(amp, diode),
                           wdf::OpAmpEngineLaw::maxDv(oa, engine.fs()), oa.railLow(), oa.railHigh()});
    out << ",\n},\n";
}

void generate(std::ostream& out, const wdf::Circuit& circuit)
{
    wdf::Engine engine(circuit, 44100);
    Bias bias{};
    if (engine.capacitorStates().size() != bias.size())
        throw std::runtime_error("unexpected DOD 250 state size");
    std::copy(engine.capacitorStates().begin(), engine.capacitorStates().end(), bias.begin());
    out << "/* Generated from the LiveWDF DOD 250 preset at 44.1 kHz. */\n"
        << "#define WDF_STATES " << nStates << "\n#define WDF_GAINS " << nGain
        << "\n#define WDF_TICKS_PER_VOLT " << ticksPerVolt << "\n"
        << "struct WdfModel {\n"
        << "    float amp_drive[WDF_STATES+4], amp_source[WDF_STATES+4], diode_drive[WDF_STATES+4];\n"
        << "    float output[WDF_STATES+4], next[WDF_STATES][WDF_STATES+4];\n"
        << "    struct {\n"
        << "        float pole_feedback, diode_to_pole, source_to_diode, impedance_clamped, impedance_linear;\n"
        << "        float inv_amp_load, diode_to_amp, slew, rail_low, rail_high;\n"
        << "    } load;\n"
        << "};\nstatic const float initial_pole = ";
    out << std::hexfloat << static_cast<float>(engine.opAmpPole(0)) << "f;\n";
    const auto& level = circuit.pots[1];
    const auto wiperResistance = [&](double position) {
        return wdf::Pot::resistances(level.totalResistance, position, level.taper).second;
    };
    const double levelDefault = wiperResistance(level.position);
    std::array<double, DOD250_LEVEL_MAX + 1> levelScales{};
    for (size_t percent = 0; percent < levelScales.size(); ++percent)
        levelScales[percent] = wiperResistance(static_cast<double>(percent) / DOD250_LEVEL_MAX) / levelDefault;
    out << "static const float level_scales[] = ";
    emitRow(out, levelScales);
    out << ";\n";
    const auto& junction = engine.compiled().diode.front().second;
    std::array<double, 2 * ticksPerVolt + 1> diodeTable{};
    for (size_t tick = 0; tick < diodeTable.size(); ++tick) {
        double forward, reverse, slope;
        junction.eval(static_cast<double>(tick) / ticksPerVolt, forward, slope);
        junction.eval(-static_cast<double>(tick) / ticksPerVolt, reverse, slope);
        diodeTable[tick] = forward - reverse;
        if (tick && !(diodeTable[tick] > diodeTable[tick - 1]))
            throw std::runtime_error("diode pair lookup must be strictly monotone");
    }
    out << "static const float diode_lut[] WDF_TABLE = ";
    emitRow(out, diodeTable);
    out << ";\nstatic const struct WdfModel models[WDF_GAINS] WDF_TABLE = {\n";
    for (int position = 0; position < nGain; ++position) {
        wdf::Engine adjusted(circuit, 44100);
        if (!adjusted.setPotPosition("VR7", static_cast<double>(position) / (nGain - 1)))
            throw std::runtime_error("could not set DOD 250 Gain");
        emitModel(out, adjusted, bias);
    }
    out << "};\n";
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 3)
            throw std::runtime_error("usage: livewdf_codegen_dod250 <preset.livewdf> <circuit.h>");
        wdf::LiveWDFDocument doc;
        std::string error;
        if (!wdf::LiveWDFDocument::loadFromFile(argv[1], doc, error))
            throw std::runtime_error(error);
        const auto circuit = doc.toCircuit();
        validate(circuit, doc);
        std::ostringstream header;
        header.imbue(std::locale::classic());
        generate(header, circuit);
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        out << header.str();
        if (!out)
            throw std::runtime_error("could not write circuit header");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
