#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "spice/SpiceExport.h"
#include "wdf/Engine.h"

namespace {

constexpr double sampleRate = 48000;
constexpr double kelvinOffset = 273.15;
constexpr double voltsPerKelvin = 8.617333262e-5;
constexpr int frames = 48000;
constexpr int warmup = 4800;
constexpr int passes = 7;

enum class Kind { bjt, jfet, diode };

struct Part {
    std::string id, card;
    Kind kind;
    double supply, start, stop, step;
    double audioBias, audioAmplitude;
    double reverseStop = 0;
    bool testClipper = false;
};

const std::vector<Part> parts{
    {"2n3904-mister", "2N3904-MiSTer", Kind::bjt, 1, .44, .80, .002, .61, .07},
    {"2n3906-mister", "2N3906-MiSTer", Kind::bjt, -1, -.44, -.82, -.002, -.645, .07},
    {"2n3904-mister-ikf", "2N3904-MiSTer-IKF", Kind::bjt, 1, .44, .80, .002, .61, .07},
    {"2n3906-mister-ikf", "2N3906-MiSTer-IKF", Kind::bjt, -1, -.44, -.82, -.002, -.645, .07},
    {"bc109c", "BC109C", Kind::bjt, 5, .44, .76, .002, .61, .07},
    {"2sc2240", "2SC2240", Kind::bjt, 6, .44, .80, .002, .64, .07},
    {"2n270", "2N270", Kind::bjt, -1, -.02, -.21, -.001, -.10, .055},
    {"2sk184-gr", "2SK184-GR", Kind::jfet, 10, -1.55, 0, .005, -.233, .23},
    {"2n5458", "2N5458", Kind::jfet, 15, -7, 0, .025, -1.35, 1.05},
    {"1n914-nexperia", "1N914-Nexperia", Kind::diode, 0, 0, 1.1, .005, 0, 1.8, -20, true},
    {"1n914-nexperia-lowleakage", "1N914-Nexperia-LowLeakage", Kind::diode, 0, 0, 1.1, .005, 0, 1.8, -20, true},
    {"1s1588-proxy", "1S1588-Proxy", Kind::diode, 0, 0, 1.1, .005, 0, 1.8, -20},
    {"ma150-proxy", "MA150-Proxy", Kind::diode, 0, 0, 1.1, .005, 0, 1.8, -20},
    {"1s2075", "1S2075", Kind::diode, 0, 0, 1.05, .005, 0, 1.8, -30, true},
    {"1s2075-lowleakage", "1S2075-LowLeakage", Kind::diode, 0, 0, 1.05, .005, 0, 1.8, -30, true},
    {"1n34a-mister", "1N34A-MiSTer", Kind::diode, 0, 0, .6, .003, 0, 1.8, -50},
    {"1n4002", "1N4002", Kind::diode, 0, 0, 1.2, .005, 0, 1.8, -100, true},
    {"1n4002-lowleakage", "1N4002-LowLeakage", Kind::diode, 0, 0, 1.2, .005, 0, 1.8, -100, true},
    {"1ss133", "1SS133", Kind::diode, 0, 0, 1.1, .005, 0, 1.8, -80},
};

struct Fixture {
    wdf::Circuit circuit;
    double meter = .1;
    double temperature = 27;
    std::string observed = "sense";
};

std::string kindName(Kind kind) {
    switch (kind) {
        case Kind::bjt: return "bjt";
        case Kind::jfet: return "jfet";
        case Kind::diode: return "diode";
    }
    throw std::logic_error("invalid fixture kind");
}

Fixture makeFixture(const Part& part, bool timing, bool clipper = false) {
    Fixture fixture;
    auto& circuit = fixture.circuit;
    circuit.addSrc("drive", "0", part.audioBias, 0, {}, "input");
    if (part.kind == Kind::diode) {
        const auto* card = wdf::findDiodeByName(part.card);
        if (card == nullptr) throw std::runtime_error("unknown diode " + part.card);
        if (timing) fixture.meter = 1000;
        circuit.addRes("drive", fixture.observed, fixture.meter, "meter");
        circuit.addDiode(fixture.observed, "0", card->model(), "device");
        if (clipper) {
            circuit.addDiode("0", fixture.observed, card->model(), "opposite");
            circuit.addRes(fixture.observed, "0", 100e3, "load");
        }
        return fixture;
    }
    const double supply = timing ? std::copysign(9.0, part.supply) : part.supply;
    circuit.addSrc("supply", "0", supply, 0, {}, "fixture");
    if (timing) fixture.meter = part.kind == Kind::jfet ? 560.0 : 2200.0;
    circuit.addRes("supply", fixture.observed, fixture.meter, "meter");
    circuit.addRes("drive", "bias", timing && part.kind == Kind::jfet ? 10000 : 1, "control");
    if (part.kind == Kind::bjt) {
        const auto* card = wdf::findBjtByName(part.card);
        if (card == nullptr) throw std::runtime_error("unknown BJT " + part.card);
        fixture.temperature = card->model.Vt / voltsPerKelvin - kelvinOffset;
        circuit.addBjt(fixture.observed, "bias", "0", card->model, "device");
    } else {
        const auto* card = wdf::findJfetByName(part.card);
        if (card == nullptr) throw std::runtime_error("unknown JFET " + part.card);
        auto model = card->model;
        // A static DC sweep should not retain charge from one bias point to
        // the next. The timing fixture still uses the full card.
        if (!timing) model.p.cgs = model.p.cgd = model.p.cds = 0;
        circuit.addJfet(fixture.observed, "bias", "0", model, "device");
    }
    return fixture;
}

std::vector<double> sourceValues(const wdf::Circuit& circuit) {
    std::vector<double> values;
    for (const auto& source : circuit.src) values.push_back(source.dc);
    return values;
}

void sweep(const Part& part, const Fixture& fixture, std::ostream& index,
           const std::string& id, double start, double stop, double step) {
    auto circuit = fixture.circuit;
    circuit.src[0].dc = start;
    wdf::Engine engine(circuit, sampleRate);
    const bool diode = part.kind == Kind::diode;
    const int output = engine.nodeIndex(fixture.observed);
    const int bias = diode ? output : engine.nodeIndex("bias");
    if (output < 0 || bias < 0) throw std::runtime_error("missing fixture node for " + id);
    auto values = sourceValues(circuit);
    std::ofstream outputFile(id + "-native.csv");
    outputFile << "drive_V,terminal_V,bias_V,current_A,bias_current_A\n" << std::setprecision(15);
    const double sign = part.supply < 0 ? -1.0 : 1.0;
    const int points = std::lround((stop - start) / step) + 1;
    int unconverged = 0;
    for (int point = 0; point < points; ++point) {
        values[0] = start + step * point;
        const auto& v = engine.step(values);
        if (!engine.lastConverged()) ++unconverged;
        const double drive = diode ? values[0] : values[1];
        outputFile << values[0] << ',' << v[output] << ',' << v[bias] << ','
                   << sign * (drive - v[output]) / fixture.meter << ','
                   << (diode ? 0 : sign * (values[0] - v[bias])) << '\n';
    }
    const auto exported = wdf::spiceExport(circuit, sampleRate, 0);
    if (!exported.ok || !outputFile)
        throw std::runtime_error("failed to export " + id + ": " + exported.diagnostics);
    const auto control = exported.netlist.find("\n.control\n");
    if (control == std::string::npos) throw std::runtime_error("missing SPICE control block");
    std::ofstream deck(id + "-export.cir");
    deck << exported.netlist.substr(0, control) << '\n' << std::setprecision(15)
         << ".options tnom=" << fixture.temperature << " temp=" << fixture.temperature
         << " reltol=1e-10 abstol=1e-15 vntol=1e-11 gmin=1e-15\n"
         << ".control\nset wr_singlescale\nset numdgt=15\n"
         // Count the requested final point even when accumulated ngspice
         // roundoff puts it just beyond the written sweep boundary.
         << "dc V_input " << start << ' ' << stop + 0.25 * step << ' ' << step << '\n'
         << "wrdata " << id << "-ngspice.txt v(sense) v(" << (diode ? "sense" : "bias")
         << ")\nquit\n.endc\n.end\n";
    if (!deck) throw std::runtime_error("failed to write SPICE deck for " + id);
    index << id << ',' << part.id << ',' << part.card << ',' << kindName(part.kind) << ','
          << part.supply << ',' << fixture.meter << ',' << fixture.temperature << ','
          << engine.stateRejectCount() << ',' << unconverged << '\n';
}

struct Timing {
    std::array<double, passes> time{};
    long long rejected = 0, fallback = 0, unconverged = 0, iterations = 0, evaluations = 0;
    double checksum = 0;
};

void measure(const Part& part, Timing& result, int pass, bool clipper) {
    const auto fixture = makeFixture(part, true, clipper);
    wdf::Engine engine(fixture.circuit, sampleRate);
    const int output = engine.nodeIndex(fixture.observed);
    if (output < 0) throw std::runtime_error("missing timing output for " + part.id);
    auto values = sourceValues(fixture.circuit);
    std::vector<double> input(frames + warmup);
    for (int i = 0; i < frames + warmup; ++i)
        input[i] = part.audioBias + part.audioAmplitude * std::sin(2 * wdf::kPi * 440.0 * i / sampleRate);
    const auto drive = [&](int i) {
        values[0] = input[i];
        return engine.step(values)[output];
    };
    for (int i = 0; i < warmup; ++i) drive(i);
    double sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = warmup; i < frames + warmup; ++i) sum += std::abs(drive(i));
    result.time[pass] = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - start).count() / frames;
    if (!std::isfinite(sum)) throw std::runtime_error("nonfinite timing output for " + part.id);
    result.checksum = sum / frames;
    if (pass != 0) return;

    engine.reset();
    for (int i = 0; i < warmup; ++i) drive(i);
    const auto initialRejects = engine.stateRejectCount();
    const auto initialFallbacks = engine.blockFallbackCount();
    for (int i = warmup; i < frames + warmup; ++i) {
        drive(i);
        result.iterations += engine.newtonIterations();
        result.evaluations += engine.deviceEvaluations();
        if (!engine.lastConverged()) ++result.unconverged;
    }
    result.rejected = engine.stateRejectCount() - initialRejects;
    result.fallback = engine.blockFallbackCount() - initialFallbacks;
}

void writeTiming(const std::vector<Part>& parts, const char* filename, bool clipper) {
    std::vector<Timing> results(parts.size());
    for (int pass = 0; pass < passes; ++pass)
        for (size_t i = 0; i < parts.size(); ++i)
            measure(parts[i], results[i], pass, clipper);

    std::ofstream timing(filename);
    timing << "id,median_ns_frame,min_ns_frame,max_ns_frame,mean_iterations,"
              "mean_evaluations,fallbacks,rejected,unconverged,mean_abs_V\n" << std::setprecision(8);
    for (size_t i = 0; i < parts.size(); ++i) {
        auto& m = results[i];
        std::sort(m.time.begin(), m.time.end());
        timing << parts[i].id << ',' << m.time[passes / 2] << ',' << m.time.front() << ','
               << m.time.back() << ',' << static_cast<double>(m.iterations) / frames << ','
               << static_cast<double>(m.evaluations) / frames << ',' << m.fallback << ','
               << m.rejected << ',' << m.unconverged << ',' << m.checksum << '\n';
    }
    if (!timing) throw std::runtime_error(std::string("failed to write ") + filename);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::runtime_error("usage: livewdf_bench_semiconductors [card-id]");
        std::vector<Part> selected;
        for (const auto& part : parts)
            if (argc == 1 || part.id == argv[1]) selected.push_back(part);
        if (selected.empty()) throw std::runtime_error("unknown card ID");
        std::ofstream index("sweeps.csv");
        index << "sweep,id,card,kind,supply_V,meter_ohms,temperature_C,rejected,unconverged\n"
              << std::setprecision(15);
        for (const auto& part : selected) {
            const auto fixture = makeFixture(part, false);
            sweep(part, fixture, index, part.id, part.start, part.stop, part.step);
            if (part.reverseStop != 0)
                sweep(part, fixture, index, part.id + "-reverse", 0, part.reverseStop, -.1);
        }
        if (!index) throw std::runtime_error("failed to write sweep index");
        writeTiming(selected, "timing.csv", false);
        std::vector<Part> clippers;
        for (const auto& part : selected)
            if (part.testClipper) clippers.push_back(part);
        if (!clippers.empty()) writeTiming(clippers, "clipping-timing.csv", true);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
