#include "neural_newton/Hook.h"

#include "format/Document.h"
#include "format/PortImpedance.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

constexpr int sampleRate = 48000;
constexpr size_t blockSize = 128;

class BenchEngine : public wdf::Engine {
public:
    using Engine::Engine;
    const std::vector<double>& solvedPorts() const { return vv_; }
};

struct Layer {
    size_t inputs;
    std::vector<float> weights, bias, output;

    void run(std::span<const float> input, bool activate) {
        for (size_t i = 0; i < output.size(); ++i) {
            const float* row = weights.data() + i * inputs;
            float sum = bias[i];
            for (size_t j = 0; j < inputs; ++j)
                sum += row[j] * input[j];
            output[i] = activate && sum < 0 ? sum * 0.01f : sum;
        }
    }
};

struct Model {
    std::vector<float> center, invScale, correctionScale;
    std::vector<Layer> layers;

    explicit Model(const std::string& path, size_t ports, uint64_t circuitId) {
        std::ifstream file(path);
        const auto read = [&file](auto& values) {
            for (auto& value : values)
                if (!(file >> value) || !std::isfinite(value))
                    throw std::runtime_error("Invalid weights");
        };
        size_t depth = 0, width = 0;
        uint64_t modelCircuitId = 0;
        if (!(file >> modelCircuitId >> depth >> width) || modelCircuitId != circuitId ||
            depth < 1 || depth > 3 || width != 3 * ports + 1)
            throw std::runtime_error("Weights do not match the circuit, control order or port count");
        center.resize(width);
        invScale.resize(width);
        correctionScale.resize(ports);
        read(center);
        read(invScale);
        read(correctionScale);
        for (size_t i = 0; i < depth; ++i) {
            size_t output = 0;
            if (!(file >> output) || output == 0 || output > 64 ||
                (i + 1 == depth && output != ports))
                throw std::runtime_error("Wrong layer shape");
            Layer layer{width, std::vector<float>(width * output),
                        std::vector<float>(output), std::vector<float>(output)};
            read(layer.weights);
            read(layer.bias);
            layers.push_back(std::move(layer));
            width = output;
        }
    }

    bool predict(std::span<float> input, std::span<double> guess) {
        for (size_t i = 0; i < input.size(); ++i) {
            input[i] = (input[i] - center[i]) * invScale[i];
            if (!(std::abs(input[i]) <= 12.f))
                return false;
        }
        std::span<const float> values = input;
        for (auto& layer : layers) {
            layer.run(values, &layer != &layers.back());
            values = layer.output;
        }
        for (float v : values)
            if (!(std::abs(v) <= 12.f))
                return false;
        for (size_t i = 0; i < values.size(); ++i)
            guess[i] += values[i] * correctionScale[i];
        return true;
    }
};

enum class SeedMode { Residual, EarlyVoltage, Current, CarriedCurrent };

SeedMode seedMode(std::string_view model, std::string_view trace) {
    if (model == "carry-current")
        return SeedMode::CarriedCurrent;
    if (model.starts_with("voltage:") || trace.starts_with("voltage:"))
        return SeedMode::EarlyVoltage;
    if (model.starts_with("current:") || trace.starts_with("current:"))
        return SeedMode::Current;
    return SeedMode::Residual;
}

struct Run {
    Model* model = nullptr;
    std::vector<float> input, target;
    std::vector<double> stock, priorDrive;
    SeedMode mode;
    bool driveReady = false;
    bool seen = false;
    bool collecting = false;
    double control = 0.0;
    int gate = 0;
    long long previousWork = 0;
    size_t calls = 0, attempts = 0, rows = 0;

    Run(size_t ports, SeedMode seed)
        : input(3 * ports + 1), target(ports), stock(ports), priorDrive(ports), mode(seed) {}

    void record(std::ofstream& file, const BenchEngine& engine, int work) {
        if (!seen || (mode != SeedMode::Residual && calls == 1) ||
            !engine.lastConverged() || engine.lastStateRejected())
            return;
        const auto& solved = mode == SeedMode::Current ? engine.rootCurrents() : engine.solvedPorts();
        for (size_t i = 0; i < target.size(); ++i)
            target[i] = static_cast<float>(solved[i] - stock[i]);
        const float cost = static_cast<float>(work);
        const auto write = [&file](std::span<const float> values) {
            file.write(reinterpret_cast<const char*>(values.data()),
                       static_cast<std::streamsize>(values.size_bytes()));
        };
        write(input);
        write(target);
        write(std::span(&cost, 1));
        ++rows;
    }
};

Run* run = nullptr;

bool seedHook(const wdf::experimental::NewtonSeed& seed) noexcept {
    auto& r = *run;
    if (r.seen)
        return false;
    r.seen = true;
    ++r.calls;
    if (!r.collecting && (!r.model || r.previousWork < r.gate))
        return false;
    const size_t n = seed.guess.size();
    for (size_t i = 0; i < n; ++i) {
        r.input[i] = static_cast<float>(seed.guess[i]);
        r.input[n + i] = static_cast<float>(seed.previous[i] - seed.older[i]);
        r.input[2 * n + i] = static_cast<float>(seed.residual[i]);
        r.stock[i] = seed.guess[i];
    }
    r.input.back() = static_cast<float>(r.control);
    if (!r.model || !r.model->predict(r.input, seed.guess))
        return false;
    ++r.attempts;
    return true;
}

bool earlySeed(wdf::solver::NewtonContext& ctx) noexcept {
    auto& r = *run;
    if (r.seen)
        return false;
    r.seen = true;
    ++r.calls;
    const bool carry = r.mode == SeedMode::CarriedCurrent;
    const bool current = r.mode != SeedMode::EarlyVoltage;
    const auto& seed = current ? ctx.ii : ctx.vv;
    if (!carry) {
        const bool features = r.driveReady && (r.collecting || r.previousWork >= r.gate);
        r.driveReady = true;
        for (int i = 0; i < ctx.K; ++i) {
            if (features) {
                r.input[i] = static_cast<float>(ctx.vvPrev[i]);
                r.input[ctx.K + i] = static_cast<float>(ctx.vvPrev[i] - ctx.vvPrev2[i]);
                r.input[2 * ctx.K + i] = static_cast<float>(ctx.vOc[i] - r.priorDrive[i]);
                r.stock[i] = seed[i];
            }
            r.priorDrive[i] = ctx.vOc[i];
        }
        r.input.back() = static_cast<float>(r.control);
        if (!r.model || !features)
            return false;
    }

    auto& proposal = current ? ctx.iv : ctx.vt;
    std::copy(seed.begin(), seed.end(), proposal.begin());
    if (!carry && !r.model->predict(r.input, proposal))
        return false;
    ++r.attempts;
    if (current) {
        wdf::solver::RootNewton::residualInto(ctx, ctx.vOc, ctx.vOc, ctx.Z, proposal, ctx.F);
        for (int i = 0; i < ctx.K; ++i)
            ctx.vt[i] = ctx.vOc[i] - ctx.F[i];
    }
    for (int i = 0; i < ctx.K; ++i) {
        const double move = ctx.vt[i] - ctx.vvPrev[i];
        if (!std::isfinite(move) || move < -ctx.stepDown[i] || move > ctx.stepUp[i])
            return false;
    }
    std::copy(ctx.vt.begin(), ctx.vt.end(), ctx.vv.begin());
    return true;
}

uint64_t circuitFingerprint(const char* path, const std::vector<std::pair<int, double>>& controls) {
    std::ifstream file(path, std::ios::binary);
    uint64_t hash = 14695981039346656037ull;
    for (char byte; file.get(byte);)
        hash = (hash ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
    if (!file.eof())
        throw std::runtime_error("Cannot hash circuit");
    for (const auto& setting : controls)
        hash = (hash ^ static_cast<uint64_t>(setting.first)) * 1099511628211ull;
    return hash;
}

double threadSeconds() {
#ifdef CLOCK_THREAD_CPUTIME_ID
    timespec tm{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tm);
    return tm.tv_sec + tm.tv_nsec * 1e-9;
#else
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}

struct BlockTimes {
    std::vector<double> cpu, wall;

    static void summarize(const char* name, std::vector<double>& times) {
        std::sort(times.begin(), times.end());
        const auto percentile = [&](double q) { return times[static_cast<size_t>(q * times.size())]; };
        const size_t late = times.end() - std::upper_bound(times.begin(), times.end(), blockSize * 1000.0 / sampleRate);
        std::cout << name << " p50/p99/p99.9/max ms " << percentile(.50) << '/' << percentile(.99)
                  << '/' << percentile(.999) << '/' << times.back() << " late " << late << '/' << times.size() << '\n';
    }
};

int bench(int argc, char** argv) {
    if (argc != 11)
        throw std::runtime_error("Usage: livewdf_neural_newton circuit di.f32 startSec seconds peakFraction model|- trace|- output|- gate control-pots");
    wdf::LiveWDFDocument doc;
    std::string error;
    if (!doc.loadFromFile(argv[1], error))
        throw std::runtime_error(error);
    wdf::Circuit circuit = wdf::circuitWithPortImpedances(doc);
    const auto in = std::find_if(circuit.src.begin(), circuit.src.end(),
        [&](const auto& source) { return source.name == doc.inputId(); });
    if (in == circuit.src.end())
        throw std::runtime_error("No named circuit input");
    std::vector<double> sources;
    for (const auto& source : circuit.src)
        sources.push_back(source.dc);
    const size_t inputIndex = static_cast<size_t>(in - circuit.src.begin());

    const size_t wanted = static_cast<size_t>(std::stod(argv[4]) * sampleRate);
    std::vector<float> audio(wanted);
    std::ifstream di(argv[2], std::ios::binary);
    di.seekg(static_cast<std::streamoff>(std::stod(argv[3]) * sampleRate) * sizeof(float));
    di.read(reinterpret_cast<char*>(audio.data()), static_cast<std::streamsize>(wanted * sizeof(float)));
    audio.resize(static_cast<size_t>(di.gcount()) / sizeof(float) / blockSize * blockSize);
    if (audio.empty())
        throw std::runtime_error("No complete DI blocks");
    double peak = 0;
    for (float v : audio)
        peak = std::max(peak, std::abs(static_cast<double>(v)));
    if (!(peak > 0))
        throw std::runtime_error("Silent DI");
    const double drive = std::stod(argv[5]) * doc.inputVoltsPerFullScale() / peak;
    std::vector<std::pair<int, double>> controls;
    for (std::string_view settings = argv[10]; !settings.empty();) {
        const auto end = settings.find(',');
        const auto field = settings.substr(0, end);
        const auto eq = field.find('=');
        if (eq == std::string_view::npos)
            throw std::runtime_error("Expected pot=value");
        auto pot = std::find_if(circuit.pots.begin(), circuit.pots.end(),
            [&](const auto& p) { return p.name == field.substr(0, eq); });
        if (pot == circuit.pots.end())
            throw std::runtime_error("Unknown pot " + std::string(field.substr(0, eq)));
        controls.emplace_back(static_cast<int>(pot - circuit.pots.begin()),
                              std::stod(std::string(field.substr(eq + 1))));
        settings = end == std::string_view::npos ? std::string_view{} : settings.substr(end + 1);
    }

    BenchEngine engine(circuit, sampleRate);
    engine.prepareLiveControls();
    const bool preview = !circuit.pent.empty();
    engine.useTriodePreview(preview);
    engine.prepareCircuitPlan();
    engine.useCircuitPlan(true);
    auto settle = [&] {
        for (int i = 0; i < sampleRate / 2; ++i)
            engine.step(sources, BenchEngine::kDefaultMaxIterations, BenchEngine::kFastLiveTolerance);
    };
    settle();
    if (!engine.setLiveControls(controls, {}))
        throw std::runtime_error("Transfer update failed");
    settle();
    const int output = engine.nodeIndex(doc.outputNet());
    if (output < 0)
        throw std::runtime_error("No circuit output");

    const std::string_view modelPath = argv[6], tracePath = argv[7];
    const SeedMode mode = seedMode(modelPath, tracePath);
    const auto path = [](std::string_view spec) {
        if (spec.starts_with("voltage:") || spec.starts_with("current:"))
            spec.remove_prefix(spec.find(':') + 1);
        return std::string(spec);
    };
    uint64_t circuitId = circuitFingerprint(argv[1], controls);
    if (mode != SeedMode::Residual) {
        circuitId = (circuitId ^ static_cast<uint64_t>(mode)) * 1099511628211ull;
        for (const auto& [_, position] : controls)
            circuitId = (circuitId ^ std::bit_cast<uint64_t>(position)) * 1099511628211ull;
    }
    Run state(static_cast<size_t>(engine.rootCount()), mode);
    std::optional<Model> model;
    if (modelPath != "-" && mode != SeedMode::CarriedCurrent)
        state.model = &model.emplace(path(modelPath), state.target.size(), circuitId);
    std::ofstream trace;
    if (tracePath != "-") {
        trace.open(path(tracePath), std::ios::binary);
        if (!trace || modelPath != "-")
            throw std::runtime_error("Training trace requires stock solver");
        const uint32_t ports = static_cast<uint32_t>(state.target.size());
        trace.write(reinterpret_cast<const char*>(&circuitId), sizeof(circuitId));
        trace.write(reinterpret_cast<const char*>(&ports), sizeof(ports));
        state.collecting = true;
    }
    state.gate = std::stoi(argv[9]);
    state.control = controls.empty() ? 0.0 : controls.front().second;
    run = &state;
    const bool profile = state.gate < 0;
    engine.setProfile(profile);
    wdf::experimental::SeedExperiment experiment{&seedHook};
    if (mode != SeedMode::Residual) {
        experiment.propose = nullptr;
        experiment.early = &earlySeed;
    }
    if (state.collecting || modelPath != "-" || profile)
        wdf::experimental::seedExperiment = &experiment;

    BlockTimes times;
    times.cpu.reserve(audio.size() / blockSize);
    times.wall.reserve(audio.size() / blockSize);
    std::vector<double> rendered(audio.size());
    long long iterations = 0, trials = 0, evals = 0, rejected = 0, unconverged = 0;
    std::vector<int> costs;
    costs.reserve(audio.size());
    using Clock = std::chrono::steady_clock;
    for (size_t start = 0; start < audio.size(); start += blockSize) {
        const auto wall = Clock::now();
        const double cpu = threadSeconds();
        for (size_t i = start; i < start + blockSize; ++i) {
            state.seen = false;
            sources[inputIndex] = in->dc + audio[i] * drive;
            const auto& nodes = engine.step(sources, BenchEngine::kDefaultMaxIterations, BenchEngine::kFastLiveTolerance);
            rendered[i] = nodes[output];
            iterations += engine.newtonIterations();
            trials += engine.lineSearchTrials();
            evals += engine.deviceEvaluations();
            rejected += engine.lastStateRejected();
            unconverged += !engine.lastConverged();
            state.previousWork = engine.newtonIterations() + engine.lineSearchTrials();
            costs.push_back(static_cast<int>(state.previousWork));
            if (trace.is_open())
                state.record(trace, engine, costs.back());
        }
        times.cpu.push_back((threadSeconds() - cpu) * 1000);
        times.wall.push_back(std::chrono::duration<double, std::milli>(Clock::now() - wall).count());
    }
    wdf::experimental::seedExperiment = nullptr;
    if (std::string_view(argv[8]) != "-") {
        std::ofstream file(argv[8], std::ios::binary);
        file.write(reinterpret_cast<const char*>(rendered.data()),
                   static_cast<std::streamsize>(rendered.size() * sizeof(double)));
        if (!file)
            throw std::runtime_error("Output write failed");
    }
    if (trace.is_open() && !trace)
        throw std::runtime_error("Trace write failed");
    std::sort(costs.begin(), costs.end());
    const auto& phases = engine.phaseProfile();
    std::cout << std::fixed << std::setprecision(3) << "ports " << state.target.size()
              << " samples " << audio.size() << " peak V " << peak * drive
              << " adjusted " << engine.hasAdjustedLiveTransfer() << " Preview " << preview
              << " calls " << state.calls << " attempts " << state.attempts
              << " selected " << experiment.selected << " retries " << experiment.retries << " rows " << state.rows << '\n'
              << "rejected " << rejected << " unconverged " << unconverged
              << " iterations " << iterations << " trials " << trials << " device-evals " << evals
              << " work p50/p99/max " << costs[costs.size()/2] << '/' << costs[costs.size()*99/100]
              << '/' << costs.back() << '\n';
    if (profile)
        std::cout << "phase ms solve/device/jacobian/LU/line "
                  << phases.solve * 1000 << '/' << phases.device * 1000 << '/' << phases.jacobian * 1000
                  << '/' << phases.lu * 1000 << '/' << phases.linesearch * 1000 << '\n';
    times.summarize("CPU ", times.cpu);
    times.summarize("wall", times.wall);
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        return bench(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
