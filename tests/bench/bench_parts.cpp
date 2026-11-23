// Manual release benchmark. Timings include Engine::step and a checksum.
// Setup, warm-up, signal generation and diagnostic collection are untimed.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "format/Document.h"
#include "wdf/Engine.h"

namespace {

constexpr double sampleRate = 48000.0;
constexpr int frames = 48000;
constexpr int warmup = 4800;
constexpr int passes = 7;

struct Case {
    std::string name;
    wdf::Circuit circuit;
    std::string output = "out";
    size_t input = 0;
    double amplitude = 0.2;
    double controlAmplitude = 0.0;
    bool partition = true;
};

Case passive() {
    Case row{"Passive divider", {}};
    row.circuit.addSrc("in", "0");
    row.circuit.addRes("in", "out", 10000.0);
    row.circuit.addRes("out", "0", 10000.0);
    return row;
}

Case bbd(double clockHz) {
    Case row{"MN3005 at " + std::to_string(static_cast<int>(clockHz)) + " Hz", {}};
    wdf::BucketBrigadeModel model;
    model.clockHz = clockHz;
    model.inputBias = model.outputBias = 0.0;
    row.circuit.addSrc("in", "0");
    row.circuit.addBucketBrigade("in", "out", "0", "0", model);
    row.circuit.addRes("out", "0", 100000.0);
    return row;
}

Case photoresistor() {
    Case row = passive();
    row.name = "Photoresistor";
    row.controlAmplitude = 0.5;
    row.circuit.addSrc("ctl", "0", 0.5);
    wdf::PhotoresistorControl model;
    model.sensePos = "ctl";
    model.senseNeg = "0";
    row.circuit.addPhotoresistor("out", "0", 100000.0, model, "LDR");
    return row;
}

Case vca(bool blackmer) {
    Case row{blackmer ? "THAT2180" : "Linear VCA", {}};
    row.controlAmplitude = blackmer ? 0.12 : 0.5;
    row.circuit.addSrc("in", "0");
    row.circuit.addSrc("ctl", "0", blackmer ? 0.0 : 0.5);
    if (blackmer) {
        row.circuit.addRes("in", "sum", 20000.0);
        row.circuit.addThat2180("sum", "out", "ctl", "0", "0", 25.0, "VCA1");
        row.circuit.addRes("out", "0", 20000.0);
    } else {
        row.circuit.addLinearVca("in", "out", 2200.0, {"ctl", "0", 1.0}, "VCA1");
        row.circuit.addRes("out", "0", 4400.0);
    }
    return row;
}

Case dbxVca() {
    Case row{"dbx 200 linked controls", {}};
    row.controlAmplitude = 0.06;
    auto& c = row.circuit;
    c.addSrc("in", "0");
    c.addSrc("ctl", "0");
    c.addRes("in", "sum", 20000.0);
    c.addDbx200("sum", "out", "ctl", "0", "0", "ctl", "0", "M1");
    c.addRes("out", "0", 20000.0);
    return row;
}

Case rmsDetector(bool dbx = false) {
    Case row{dbx ? "dbx 209 RMS detector" : "Log RMS detector", {}};
    row.circuit.addSrc("in", "0");
    const auto model = dbx ? wdf::RmsDetectorModel::dbx209(23.34e-6, 22e-6)
                           : wdf::RmsDetectorModel{};
    if (dbx)
        row.amplitude = model.referenceVolts * 2.0;
    row.circuit.addRmsDetector("in", "out", "0", model, "RMS1");
    row.circuit.addRes("out", "0", 10000.0);
    return row;
}

Case ota(int clippers, bool partition) {
    Case row{"CA3080", {}};
    if (clippers)
        row.name += partition ? " with 8 clippers (default)" : " with 8 clippers (joint)";
    row.partition = partition;
    row.amplitude = 0.8;
    auto& c = row.circuit;
    c.addSrc("in", "0");
    c.addSrc("vcc", "0", 9.0);
    c.addSrc("vee", "0", -9.0);
    c.addIsrc("0", "bias", 120e-6);
    c.addRes("out", "0", 10000.0);
    c.addCa3080("out", "0", "in", "vcc", "vee", "bias", "OTA1");
    for (int i = 1; i <= clippers; ++i) {
        const std::string node = "clip" + std::to_string(i);
        c.addRes("in", node, 1000.0 * i);
        c.addDiode(node, "0", {1e-9, 1.6, 1.0});
    }
    return row;
}

Case amplifier(const char* name, wdf::OpAmpType part) {
    Case row{name, {}};
    auto& c = row.circuit;
    c.addSrc("in", "0");
    c.addRes("in", "fb", 10000.0);
    c.addRes("out", "fb", 100000.0);
    c.addRes("out", "0", 2000.0);
    c.addOpAmp("out", "fb", "0", "0", "0", wdf::OpAmpModel::fromType(part), "U1");
    return row;
}

Case fromFile(const char* path) {
    wdf::LiveWDFDocument doc;
    std::string error;
    if (!doc.loadFromFile(path, error) || !doc.loadWarnings.empty())
        throw std::runtime_error(std::string(path) + ": " +
            (error.empty() ? doc.loadWarnings.front() : error));
    Case row{doc.name, doc.toCircuit(), doc.outputNet()};
    const auto src = std::find_if(row.circuit.src.begin(), row.circuit.src.end(),
        [&](const auto& s) { return s.name == doc.inputId(); });
    if (src == row.circuit.src.end())
        throw std::runtime_error(std::string(path) + ": no input source");
    row.input = static_cast<size_t>(src - row.circuit.src.begin());
    row.amplitude = 0.1;
    return row;
}

struct Counts {
    long long rejected = 0;
    long long fallback = 0;
    long long iterations = 0;
    long long evaluations = 0;
};

struct Result {
    std::array<double, passes> times{};
    Counts counts;
    double meanOutput = 0.0;
};

void measure(const Case& row, Result& result, int pass) {
    wdf::Engine engine(row.circuit, sampleRate);
    engine.setExplicitRoots(row.partition);
    const int out = engine.nodeIndex(row.output);
    if (out < 0)
        throw std::runtime_error(row.name + ": invalid output");
    std::vector<double> dc, roots;
    if (!engine.solveDc(dc, roots))
        throw std::runtime_error(row.name + ": DC did not converge");

    std::vector<double> signal, control, sources;
    for (int n = 0; n < frames + warmup; ++n) {
        const double t = n / sampleRate;
        signal.push_back(row.amplitude * std::sin(2.0 * wdf::kPi * 440.0 * t));
        control.push_back(row.controlAmplitude * std::sin(2.0 * wdf::kPi * 17.0 * t));
    }
    for (const auto& source : row.circuit.src)
        sources.push_back(source.dc);
    const auto dcInput = sources[row.input];
    const auto dcControl = row.controlAmplitude != 0.0 ? sources[1] : 0.0;
    const auto drive = [&](int n) {
        sources[row.input] = dcInput + signal[n];
        if (row.controlAmplitude != 0.0)
            sources[1] = dcControl + control[n];
        return engine.step(sources)[out];
    };

    engine.reset();
    for (int n = 0; n < warmup; ++n)
        drive(n);
    double sum = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (int n = warmup; n < frames + warmup; ++n)
        sum += std::abs(drive(n));
    result.times[pass] = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - start).count() / frames;
    result.meanOutput = sum / frames;

    if (pass == 0) {
        engine.reset();
        for (int n = 0; n < warmup; ++n)
            drive(n);
        const auto rejectedBefore = engine.stateRejectCount();
        const auto fallbackBefore = engine.blockFallbackCount();
        for (int n = warmup; n < frames + warmup; ++n) {
            drive(n);
            result.counts.iterations += engine.newtonIterations();
            result.counts.evaluations += engine.deviceEvaluations();
        }
        result.counts.rejected = engine.stateRejectCount() - rejectedBefore;
        result.counts.fallback = engine.blockFallbackCount() - fallbackBefore;
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<Case> cases{
            passive(), bbd(10000), bbd(100000), photoresistor(), vca(false), vca(true),
            rmsDetector(), dbxVca(), rmsDetector(true), ota(0, true), ota(8, true), ota(8, false),
            amplifier("RC4558", wdf::OpAmpType::Rc4558),
            amplifier("RC4558 Class AB", wdf::OpAmpType::Rc4558ClassAB),
            amplifier("OP275", wdf::OpAmpType::Op275)
        };
        for (int i = 1; i < argc; ++i)
            cases.push_back(fromFile(argv[i]));
        std::vector<Result> results(cases.size());
        for (int pass = 0; pass < passes; ++pass)
            for (size_t i = 0; i < cases.size(); ++i)
                measure(cases[i], results[i], pass);
        std::fputs("48 kHz mono, 48,000 timed frames per pass, 7 interleaved passes\n", stderr);
        std::puts("case,median_ns/frame,min_ns/frame,max_ns/frame,mean_iterations,"
                  "mean_evaluations,fallbacks,rejected,mean_abs_V");
        for (size_t i = 0; i < cases.size(); ++i) {
            auto& result = results[i];
            std::sort(result.times.begin(), result.times.end());
            std::printf("\"%s\",%.1f,%.1f,%.1f,%.3f,%.3f,%lld,%lld,%.9f\n",
                        cases[i].name.c_str(), result.times[passes / 2], result.times.front(),
                        result.times.back(), static_cast<double>(result.counts.iterations) / frames,
                        static_cast<double>(result.counts.evaluations) / frames,
                        result.counts.fallback, result.counts.rejected, result.meanOutput);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
