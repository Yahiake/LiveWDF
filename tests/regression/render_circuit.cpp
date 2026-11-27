#include "format/AcSource.h"
#include "format/ControlGroups.h"
#include "format/LiveWDFFile.h"
#include "format/MainsSupply.h"
#include "format/PortImpedance.h"
#include "spice/SpiceExport.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using wdf::json::Value;
constexpr double sampleRate = 48000.0;
constexpr size_t blockSize = 128;

Value readJson(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    Value result;
    std::string error;
    if (!Value::parse(text, result, error)) throw std::runtime_error(error);
    return result;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    if (!output || !(output << text)) throw std::runtime_error("Cannot write " + path.string());
}

void setDocumentControls(wdf::LiveWDFDocument& document, const Value& selections, bool switches) {
    const auto groups = switches ? wdf::switchControlGroups(document) : wdf::potControlGroups(document);
    for (const auto& [name, selection] : selections.objVal) {
        const auto group = std::ranges::find(groups, name, &wdf::LiveControlGroup::name);
        if (group == groups.end()) throw std::runtime_error("Unknown control " + name);
        for (const auto& member : group->members) {
            auto component = std::ranges::find(document.components, member.id, &wdf::SchematicComponent::id);
            if (component == document.components.end()) throw std::runtime_error("Missing control member " + member.id);
            if (switches)
                component->properties["select"] = selection.asInt() ? "1" : "0";
            else
                component->values["pos"] = selection.asDouble();
        }
    }
}

void moveControls(wdf::Engine& engine, const std::vector<wdf::LiveControlGroup>& groups,
                  const Value& positions) {
    std::vector<std::pair<int, double>> pots, rheostats;
    for (const auto& [name, position] : positions.objVal) {
        const auto group = std::ranges::find(groups, name, &wdf::LiveControlGroup::name);
        if (group == groups.end()) throw std::runtime_error("Unknown moving control " + name);
        for (const auto& member : group->members) {
            if (member.type == "pot") {
                const int index = engine.potIndexByName(member.id);
                // A member switched out by its enabledBy gate is not in the engine.
                if (index < 0) continue;
                pots.emplace_back(index, position.asDouble());
            } else {
                const int index = engine.varResIndexByName(member.id);
                if (index < 0) throw std::runtime_error("Missing live rheostat " + member.id);
                rheostats.emplace_back(index, position.asDouble());
            }
        }
    }
    if (!engine.setLiveControls(pots, rheostats)) throw std::runtime_error("Live control update failed");
}

void prepareFast(wdf::Engine& engine, const wdf::Circuit& circuit,
                 size_t input, int output, bool compiled, bool outputOnly,
                 double peak, bool allowPad) {
    wdf::Engine peer(circuit, sampleRate);
    peer.prepareLiveControls();
    peer.prepareMixedAffineReduction(input);
    wdf::Engine::LiveTransferSetup setup(circuit), peerSetup(circuit);
    const bool planOnly = !engine.hasAdjustedLiveTransfer();
    if (!engine.captureLiveTransferSetup(setup, planOnly) || !peer.captureLiveTransferSetup(peerSetup, planOnly))
        throw std::runtime_error("Cannot capture Preview preparation");
    setup.build(input, !planOnly);
    peerSetup.build(input, !planOnly);
    setup.screenCircuitPlan(peerSetup, input, output);
    if (compiled) setup.prepareCompiledPreview(input, output, outputOnly, peak, allowPad);
    if (!engine.installLiveTransferSetup(setup)) throw std::runtime_error("Cannot install Preview preparation");
    engine.useCircuitPlan(true);
    engine.useCompiledOptical(compiled);
    engine.useLoadedJunctionPreview(compiled);
}

void swapWaveBytes(std::vector<double>& wave) {
    if constexpr (std::endian::native != std::endian::little) {
        for (double& value : wave) {
            auto bytes = std::bit_cast<std::array<unsigned char, sizeof(double)>>(value);
            std::ranges::reverse(bytes);
            value = std::bit_cast<double>(bytes);
        }
    }
}

std::vector<double> readWave(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto bytes = static_cast<std::streamoff>(file.tellg());
    if (bytes <= 0 || bytes % sizeof(double)) throw std::runtime_error("Invalid stimulus " + path.string());
    file.seekg(0);
    std::vector<double> result(static_cast<size_t>(bytes) / sizeof(double));
    if (!file.read(reinterpret_cast<char*>(result.data()), bytes)) throw std::runtime_error("Cannot read stimulus");
    swapWaveBytes(result);
    return result;
}

void exportDeck(wdf::Circuit circuit, size_t input, const wdf::LiveWDFDocument& document,
                const Value& configuration, const std::filesystem::path& prefix) {
    circuit.src[input].sin.clear();
    circuit.src[input].ac = 1.0;
    const bool transient = configuration["analysis"].asString() == "transient";
    const double frequency = configuration["frequency"].asDouble(1000.0);
    if (transient) {
        circuit.src[input].ac = 0.0;
        circuit.src[input].sin = {circuit.src[input].dc, configuration["amplitude"].asDouble(), frequency};
    }
    const auto exported = wdf::spiceExport(circuit, sampleRate, 0.0);
    if (!exported.ok) throw std::runtime_error(exported.diagnostics);
    if (!circuit.bucketBrigades.empty() || !circuit.ctrlRes.empty() || !circuit.rmsDetectors.empty())
        throw std::runtime_error("A saved SPICE reference requires supported time-domain devices");
    const auto control = exported.netlist.find(".control");
    if (control == std::string::npos) throw std::runtime_error("Export has no analysis block");
    std::ostringstream command;
    command << std::setprecision(17) << ".control\nset numdgt=17\n";
    if (transient) {
        const double duration = (configuration["samples"].asInt() - 1) / sampleRate;
        command << "tran " << 1.0 / sampleRate << ' ' << duration << " 0 " << 1.0 / (8 * sampleRate)
                << "\nlinearize v(" << document.outputNet() << ")\nwrdata tran.txt v(" << document.outputNet() << ")\n";
    } else {
        command << "ac lin 1 " << frequency << ' ' << frequency
                << "\nwrdata ac.txt v(" << document.outputNet() << ")\n";
    }
    command << "quit\n.endc\n.end\n";
    writeText(prefix.string() + ".cir", exported.netlist.substr(0, control) + command.str());
}
} // namespace

int main(int argc, char** argv) try {
    if (argc != 5) throw std::runtime_error("render_circuit configuration.json accurate|fast|preview output-prefix render|export");
    const Value configuration = readJson(argv[1]);
    const std::string mode = argv[2];
    const bool accurate = mode == "accurate", preview = mode == "preview";
    if (!accurate && !preview && mode != "fast") throw std::runtime_error("Unknown mode");
    const std::filesystem::path prefix = argv[3];
    wdf::LiveWDFDocument document;
    std::string error;
    if (!document.loadFromFile(configuration["circuit"].asString(), error)) throw std::runtime_error(error);
    setDocumentControls(document, configuration["switches"], true);
    const auto defaults = document;
    setDocumentControls(document, configuration["positions"], false);
    auto circuit = wdf::circuitWithPortImpedances(document);
    if (!accurate)
        wdf::replaceMainsSupplies(circuit, defaults, sampleRate, preview);
    const auto source = std::ranges::find(circuit.src, document.inputId(), &wdf::Src::name);
    if (source == circuit.src.end()) throw std::runtime_error("Circuit has no audio input");
    const auto input = static_cast<size_t>(source - circuit.src.begin());
    if (std::string(argv[4]) == "export") {
        exportDeck(circuit, input, document, configuration, prefix);
        return 0;
    }
    if (std::string(argv[4]) != "render") throw std::runtime_error("Unknown action");
    auto wave = readWave(configuration["stimulus"].asString());
    if (configuration["normalized_input"].asBool(true)) {
        for (double& sample : wave) sample *= document.inputVoltsPerFullScale();
    }
    wdf::Engine engine(circuit, sampleRate);
    const int output = engine.nodeIndex(document.outputNet());
    if (output < 0) throw std::runtime_error("Circuit has no audio output");
    engine.prepareLiveControls();
    const auto settings = document.sourceAcSettings(circuit);
    const auto groups = wdf::potControlGroups(document);
    std::vector<size_t> oscillators;
    for (size_t i = 0; i < settings.size(); ++i)
        if (settings[i].ac != 0.0 && settings[i].freq != 0.0) oscillators.push_back(i);
    const bool outputOnly = oscillators.empty() && std::ranges::all_of(document.allProbes(), [&](const auto& probe) {
        const int node = engine.nodeIndex(probe.second);
        return node < 0 || node == output;
    });
    if (!accurate) {
        engine.prepareMixedAffineReduction(input);
        prepareFast(engine, circuit, input, output, preview, outputOnly,
                    document.previewPeakVolts(), document.allowPreviewInputPad);
        engine.useMixedAffineReduction(true);
    }
    auto initial = engine.dcValues();
    for (const auto& source : circuit.isrc)
        initial.push_back(source.dc);
    auto values = initial;
    const double tolerance = accurate ? wdf::Engine::kDefaultTolerance : wdf::Engine::kFastLiveTolerance;
    const bool bipolarAllowed = preview && outputOnly;
    long long iterations = 0, evaluations = 0, trials = 0, bipolar = 0;
    const auto step = [&](double value, long long sample, bool count) {
        for (const size_t i : oscillators)
            values[i] = wdf::sourceValueAt(initial[i], settings[i].ac, settings[i].freq, sampleRate, sample);
        values[input] = value;
        double result;
        if (bipolarAllowed && engine.stepBipolarPreview(value, result)) {
            if (count) ++bipolar;
            return result;
        }
        result = engine.step(values, wdf::Engine::kDefaultMaxIterations, tolerance)[output];
        if (count) {
            iterations += engine.newtonIterations();
            evaluations += engine.deviceEvaluations();
            trials += engine.lineSearchTrials();
        }
        return result;
    };
    const int warmup = configuration["warmup"].asInt(1024);
    for (int n = 0; n < warmup; ++n) step(0.0, n, false);
    const auto rejectedBefore = engine.stateRejectCount();
    const auto loadedBefore = engine.loadedJunctionHits(), affineBefore = engine.affineRootHits(), planBefore = engine.circuitPlanHits();
    const auto affineNodeBefore = engine.affineNodePreviewHits();
    const auto& moves = configuration["moves"].arrVal;
    size_t nextMove = 0;
    std::vector<double> rendered(wave.size());
    Value blocks(wdf::json::Type::Array);
    for (size_t start = 0; start < wave.size(); start += blockSize) {
        const auto wallStart = std::chrono::steady_clock::now();
        const auto cpuStart = std::clock();
        const auto count = std::min(blockSize, wave.size() - start);
        const auto rejected = engine.stateRejectCount();
        for (size_t n = start; n < start + count; ++n) {
            while (nextMove < moves.size() && moves[nextMove]["sample"].asInt() == static_cast<int>(n)) {
                moveControls(engine, groups, moves[nextMove]["positions"]);
                ++nextMove;
            }
            rendered[n] = step(wave[n], n + warmup, true);
            if (!std::isfinite(rendered[n])) throw std::runtime_error("Non-finite audio at sample " + std::to_string(n));
        }
        Value block(wdf::json::Type::Object);
        block["cpu_us"] = 1e6 * (std::clock() - cpuStart) / CLOCKS_PER_SEC;
        block["wall_us"] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - wallStart).count();
        block["rejected"] = double(engine.stateRejectCount() - rejected);
        blocks.arrVal.push_back(std::move(block));
    }
    if (nextMove != moves.size()) throw std::runtime_error("Control events were not consumed in sample order");
    Value report(wdf::json::Type::Object);
    report["mode"] = mode;
    report["samples"] = double(wave.size());
    report["newton"] = double(iterations);
    report["evaluations"] = double(evaluations);
    report["line_trials"] = double(trials);
    report["rejected"] = double(engine.stateRejectCount() - rejectedBefore);
    report["loaded_hits"] = double(engine.loadedJunctionHits() - loadedBefore);
    report["affine_hits"] = double(engine.affineRootHits() - affineBefore);
    report["affine_node_hits"] = double(engine.affineNodePreviewHits() - affineNodeBefore);
    report["plan_hits"] = double(engine.circuitPlanHits() - planBefore);
    report["bipolar_hits"] = double(bipolar);
    report["moves"] = double(nextMove);
    report["blocks"] = std::move(blocks);
    writeText(prefix.string() + ".json", report.serialize());
    std::ofstream audio(prefix.string() + ".f64", std::ios::binary);
    swapWaveBytes(rendered);
    audio.write(reinterpret_cast<const char*>(rendered.data()), rendered.size() * sizeof(double));
    if (!audio) throw std::runtime_error("Cannot write rendered waveform");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
