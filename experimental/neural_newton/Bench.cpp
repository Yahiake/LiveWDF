#include "neural_newton/Hook.h"

#include "format/Document.h"
#include "format/Json.h"
#include "format/PortImpedance.h"
#include "wdf/Engine.h"

#ifdef LIVEWDF_BENCH_NAM_A2
#include "NAM/dsp.h"
#include "NAM/wavenet/a2_fast.h"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
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

enum class SeedMode { Residual, EarlyVoltage, Current, CarriedCurrent, Plate };

struct PlateSeed {
    std::array<std::vector<int>, 2> plates;
    std::vector<float> predictions;
    size_t sample = 0;

    explicit PlateSeed(const BenchEngine& engine) {
        std::array<int, 2> nodes{-1, -1};
        for (const auto& device : engine.rootLayout().devices) {
            if (device.kind != wdf::RootKind::Pentode)
                continue;
            const int port = device.portBase;
            const int node = engine.rootPorts()[port][0];
            const size_t side = nodes[0] < 0 || nodes[0] == node ? 0 : 1;
            if (node < 0 || (nodes[side] >= 0 && nodes[side] != node))
                throw std::runtime_error("Expected two power-tube plate nodes");
            nodes[side] = node;
            plates[side].push_back(port);
        }
        if (plates[0].size() != 2 || plates[1].size() != 2)
            throw std::runtime_error("Expected two pairs of power-tube plate ports");
        std::cout << "Plate ports";
        for (const auto& side : plates) {
            std::cout << " [";
            for (const int port : side)
                std::cout << ' ' << port;
            std::cout << " ]";
        }
        std::cout << '\n';
    }

    double differential(std::span<const double> voltages) const {
        const auto mean = [&](size_t side) {
            double sum = 0;
            for (int port : plates[side])
                sum += voltages[port];
            return sum / plates[side].size();
        };
        return 0.5 * (mean(0) - mean(1));
    }

    bool apply(wdf::solver::NewtonContext& ctx) const noexcept {
        const double correction = static_cast<double>(predictions[sample]) - differential(ctx.vv);
        for (size_t side = 0; side < plates.size(); ++side) {
            const double offset = side == 0 ? correction : -correction;
            for (const int port : plates[side]) {
                const double move = ctx.vv[port] + offset - ctx.vvPrev[port];
                if (!std::isfinite(move) || move < -ctx.stepDown[port] || move > ctx.stepUp[port])
                    return false;
            }
        }
        for (int port : plates[0])
            ctx.vv[port] += correction;
        for (int port : plates[1])
            ctx.vv[port] -= correction;
        return true;
    }
};

struct SmallPlateModel {
    static constexpr size_t historySize = 8;
    static constexpr size_t hiddenSize = 14;
    float plateScale;
    float guitarScale;
    Layer hidden;
    std::array<float, hiddenSize> outputWeights;
    float outputBias;

    explicit SmallPlateModel(const std::string& path) {
        std::ifstream stream(path);
        const std::string contents((std::istreambuf_iterator<char>(stream)), {});
        wdf::json::Value parsed;
        std::string error;
        if (!wdf::json::Value::parse(contents, parsed, error))
            throw std::runtime_error("Invalid small plate model JSON: " + error);
        const auto& model = parsed;
        if (model["plate_history"].asDouble() != historySize || !model.objVal.contains("guitar_scale"))
            throw std::runtime_error("Expected an eight-sample plate history model");

        const auto number = [](const wdf::json::Value& value) {
            const auto f = static_cast<float>(value.asDouble());
            if (!value.isNumber() || !std::isfinite(f))
                throw std::runtime_error("Invalid small plate model number");
            return f;
        };
        const auto read = [&](const wdf::json::Value& value, size_t count, auto dest) {
            if (!value.isArray() || value.arrVal.size() != count)
                throw std::runtime_error("Invalid small plate model shape");
            for (const auto& item : value.arrVal)
                *dest++ = number(item);
        };
        plateScale = number(model["plate_scale_volts"]);
        const auto& guitar = model["guitar_scale"];
        const bool conditioned = guitar.type != wdf::json::Type::Null;
        guitarScale = conditioned ? number(guitar) : 0.f;
        if (plateScale <= 0.f || (conditioned && guitarScale <= 0.f))
            throw std::runtime_error("Invalid plate or guitar scale");

        hidden.inputs = historySize + conditioned;
        const auto& rows = model["input_weights"];
        if (!rows.isArray() || rows.arrVal.size() != hiddenSize)
            throw std::runtime_error("Expected a fourteen-unit hidden layer");
        for (const auto& row : rows.arrVal)
            read(row, hidden.inputs, std::back_inserter(hidden.weights));
        read(model["input_bias"], hiddenSize, std::back_inserter(hidden.bias));
        read(model["output_weights"], hiddenSize, outputWeights.begin());
        hidden.output.resize(hiddenSize);
        outputBias = number(model["output_bias"]);
    }

    float predict(const std::array<float, historySize>& history, float guitar) {
        std::array<float, historySize + 1> input;
        for (size_t i = 0; i < historySize; ++i)
            input[i] = history[i] / plateScale;
        if (guitarScale > 0.f)
            input.back() = guitar / guitarScale;
        hidden.run(std::span(input).first(hidden.inputs), true);
        float estimate = outputBias;
        for (size_t i = 0; i < hiddenSize; ++i)
            estimate += hidden.output[i] * outputWeights[i];
        return estimate * plateScale;
    }
};

SeedMode seedMode(std::string_view model, std::string_view trace) {
    if (model.starts_with("a2:") || model.starts_with("a2-history:") ||
        model.starts_with("small-plate:") || model.starts_with("plate:") || trace.starts_with("plate:"))
        return SeedMode::Plate;
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
    PlateSeed* plates = nullptr;
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
    if (r.mode == SeedMode::Plate) {
        if (r.previousWork < r.gate)
            return false;
        ++r.attempts;
        return r.plates->apply(ctx);
    }
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
    const double voltsPerFullScale = doc.inputVoltsPerFullScale();
    const double drive = std::stod(argv[5]) * voltsPerFullScale / peak;
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
    const bool plateTrace = tracePath.starts_with("plate:");
    const bool historyA2 = modelPath.starts_with("a2-history:");
    const bool liveA2 = historyA2 || modelPath.starts_with("a2:");
    const auto seedFile = [&] { return std::string(modelPath.substr(modelPath.find(':') + 1)); };
    std::optional<SmallPlateModel> smallPlate;
    if (modelPath.starts_with("small-plate:"))
        smallPlate.emplace(seedFile());
    std::array<float, SmallPlateModel::historySize> plateHistory{};
#ifdef LIVEWDF_BENCH_NAM_A2
    std::unique_ptr<nam::DSP> a2;
    std::array<NAM_SAMPLE, blockSize> a2Input, a2Output;
    double a2Cpu = 0;
    float historyScale = 1.f;
    if (liveA2) {
        std::ifstream stream(seedFile());
        nlohmann::json model;
        if (!(stream >> model) || model.at("sample_rate") != sampleRate)
            throw std::runtime_error("Expected a 48 kHz NAM model");
        if (historyA2) {
            historyScale = model.at("metadata").at("plate_history_input_scale_volts").get<float>();
            if (!(historyScale > 0 && std::isfinite(historyScale)))
                throw std::runtime_error("Invalid A2 Lite plate history scale");
        } else if (model.value("metadata", nlohmann::json::object()).contains("plate_history_input_scale_volts")) {
            throw std::runtime_error("Use a2-history: for a plate history model");
        }
        const auto& config = model.at("config");
        int channels = 0;
        auto weights = model.at("weights").get<std::vector<float>>();
        if (!nam::wavenet::a2_fast::is_a2_shape(config, &channels) || channels != 3 || weights.size() != 1871)
            throw std::runtime_error("Expected the stock A2 Lite architecture");
        a2 = nam::wavenet::a2_fast::create_a2_fast_config(config, sampleRate)->create(std::move(weights), sampleRate);
        a2->Reset(sampleRate, historyA2 ? 1 : blockSize);
    }
#else
    if (liveA2)
        throw std::runtime_error("Build the benchmark with Release NAM Core for live A2 Lite inference");
#endif
    std::optional<PlateSeed> plates;
    std::vector<float> plateTargets;
    if (mode == SeedMode::Plate) {
        plates.emplace(engine);
        if (plateTrace) {
            if (modelPath != "-")
                throw std::runtime_error("Plate targets require the stock solver");
            plateTargets.resize(audio.size());
        } else {
            plates->predictions.resize(audio.size());
            if (!liveA2 && !smallPlate) {
                std::ifstream estimate(seedFile(), std::ios::binary);
                estimate.read(reinterpret_cast<char*>(plates->predictions.data()),
                              static_cast<std::streamsize>(audio.size() * sizeof(float)));
                if (!estimate || estimate.peek() != std::ifstream::traits_type::eof())
                    throw std::runtime_error("Expected one plate prediction per sample");
            }
        }
    }
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
    if (plates)
        state.plates = &*plates;
    std::optional<Model> model;
    if (modelPath != "-" && mode != SeedMode::CarriedCurrent && mode != SeedMode::Plate)
        state.model = &model.emplace(path(modelPath), state.target.size(), circuitId);
    std::ofstream trace;
    if (tracePath != "-" && !plateTrace) {
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
    if (!plateTrace && (state.collecting || modelPath != "-" || profile))
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
#ifdef LIVEWDF_BENCH_NAM_A2
        if (a2 && !historyA2) {
            for (size_t i = 0; i < blockSize; ++i)
                a2Input[i] = static_cast<float>(audio[start + i] * drive / voltsPerFullScale);
            auto* input = a2Input.data();
            auto* output = a2Output.data();
            a2->process(&input, &output, blockSize);
            std::copy(a2Output.begin(), a2Output.end(), plates->predictions.begin() + start);
            a2Cpu += threadSeconds() - cpu;
        }
#endif
        for (size_t i = start; i < start + blockSize; ++i) {
            state.seen = false;
            if (plates)
                plates->sample = i;
            if (smallPlate && state.previousWork >= state.gate)
                plates->predictions[i] = smallPlate->predict(
                    plateHistory, static_cast<float>(audio[i] * drive / voltsPerFullScale));
#ifdef LIVEWDF_BENCH_NAM_A2
            if (historyA2) {
                NAM_SAMPLE inputValue = plateHistory.front() / historyScale;
                NAM_SAMPLE outputValue;
                auto* input = &inputValue;
                auto* output = &outputValue;
                a2->process(&input, &output, 1);
                plates->predictions[i] = static_cast<float>(outputValue);
            }
#endif
            sources[inputIndex] = in->dc + audio[i] * drive;
            const auto& nodes = engine.step(sources, BenchEngine::kDefaultMaxIterations, BenchEngine::kFastLiveTolerance);
            rendered[i] = nodes[output];
            if (plateTrace)
                plateTargets[i] = static_cast<float>(plates->differential(engine.solvedPorts()));
            if (smallPlate || historyA2) {
                std::copy_backward(plateHistory.begin(), plateHistory.end() - 1, plateHistory.end());
                plateHistory.front() = static_cast<float>(plates->differential(engine.solvedPorts()));
            }
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
    if (plateTrace) {
        const std::string prefix(tracePath.substr(6));
        const auto writeFloats = [](const std::string& name, const std::vector<float>& samples) {
            std::ofstream file(name, std::ios::binary);
            file.write(reinterpret_cast<const char*>(samples.data()),
                       static_cast<std::streamsize>(samples.size() * sizeof(float)));
            if (!file)
                throw std::runtime_error("Plate trace write failed");
        };
        for (float& v : audio)
            v = static_cast<float>(v * drive / voltsPerFullScale);
        writeFloats(prefix + ".x.f32", audio);
        writeFloats(prefix + ".y.f32", plateTargets);
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
#ifdef LIVEWDF_BENCH_NAM_A2
    if (a2 && !historyA2)
        std::cout << "A2 Lite mean CPU ms/block " << a2Cpu * 1000 / times.cpu.size() << '\n';
#endif
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
