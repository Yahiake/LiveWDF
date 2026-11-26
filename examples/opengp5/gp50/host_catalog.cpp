#include "catalog_config.h"
#include "format/ControlGroups.h"
#include "format/PortImpedance.h"
#include "wdf/Engine.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>

namespace {
constexpr struct { const char* name; const char* json; } schematics[] = {
#include "catalog_schematics.inc"
};
static_assert(std::size(schematics) == CATALOG_MODELS);

struct Control {
    double initial = 0.5, position = 0.5;
    int target = -1;
    unsigned rampSamples = 0, samplesLeft = 0;
    std::vector<int> pots, rheostats;
};

struct Model {
    explicit Model(const char* json, unsigned rate)
        : dcBlend(1.0 - std::exp(-2.0 * std::numbers::pi * 3.0 / rate)) {
        wdf::LiveWDFDocument doc;
        std::string error;
        if (!wdf::LiveWDFDocument::fromJson(json, doc, error))
            throw std::runtime_error(error);
        auto circuit = wdf::circuitWithPortImpedances(doc);
        const auto source = std::find_if(circuit.src.begin(), circuit.src.end(),
            [&](const auto& src) { return src.name == doc.inputId(); });
        if (source == circuit.src.end())
            throw std::runtime_error("the preset needs an audio input");
        input = static_cast<size_t>(source - circuit.src.begin());
        inputVolts = doc.inputVoltsPerFullScale();
        outputVolts = doc.outputFullScaleVolts();

        engine = std::make_unique<wdf::Engine>(circuit, rate);
        output = engine->nodeIndex(doc.outputNet());
        if (output < 0)
            throw std::runtime_error("the preset needs an audio output");
        sources = engine->dcValues();
        sources[input] = 0.0;
        for (unsigned i = 0; i < 64; ++i)
            dc = engine->step(sources)[output];
        for (const auto& group : wdf::potControlGroups(doc)) {
            Control control;
            // Bias changes move the operating point and can charge the coupling capacitors.
            control.rampSamples = static_cast<unsigned>(rate * (group.name == "Bias" ? 0.25 : 0.015));
            for (const auto& member : group.members) {
                if (member.type == "pot") {
                    const int index = engine->potIndexByName(member.id);
                    if (index < 0) throw std::runtime_error("missing pot " + member.id);
                    if (control.pots.empty() && control.rheostats.empty())
                        control.initial = circuit.pots[index].position;
                    control.pots.push_back(index);
                } else {
                    const int index = engine->varResIndexByName(member.id);
                    if (index < 0) throw std::runtime_error("missing rheostat " + member.id);
                    if (control.pots.empty() && control.rheostats.empty())
                        control.initial = circuit.varRes[index].position;
                    control.rheostats.push_back(index);
                }
            }
            control.position = control.initial;
            controls.push_back(std::move(control));
        }
        potMoves.reserve(circuit.pots.size());
        rheostatMoves.reserve(circuit.varRes.size());
        engine->prepareLiveControls();
    }

    void update(const int32_t* values, unsigned frames) {
        potMoves.clear();
        rheostatMoves.clear();
        std::array<double, CATALOG_CONTROLS> next;
        for (size_t i = 0; i < controls.size(); ++i) {
            auto& control = controls[i];
            if (values[i] != control.target) {
                control.target = values[i];
                control.samplesLeft = control.rampSamples;
            }
            if (control.samplesLeft == 0)
                continue;
            const double target = control.target < 0 ? control.initial : control.target * 0.001;
            next[i] = std::lerp(control.position, target,
                                static_cast<double>(std::min(frames, control.samplesLeft)) / control.samplesLeft);
            for (int index : control.pots) potMoves.emplace_back(index, next[i]);
            for (int index : control.rheostats) rheostatMoves.emplace_back(index, next[i]);
        }
        if (engine->setLiveControls(potMoves, rheostatMoves))
            for (size_t i = 0; i < controls.size(); ++i) {
                auto& control = controls[i];
                if (control.samplesLeft == 0)
                    continue;
                control.position = next[i];
                control.samplesLeft -= std::min(frames, control.samplesLeft);
            }
    }

    double step(double sample) {
        sources[input] = sample * inputVolts;
        const double signal = engine->step(sources)[output];
        const double ac = signal - dc;
        dc += dcBlend * ac;
        return ac / outputVolts;
    }

    size_t input = 0;
    int output = 0;
    double inputVolts = 1.0, outputVolts = 1.0, dc = 0.0;
    const double dcBlend;
    std::unique_ptr<wdf::Engine> engine;
    std::vector<double> sources;
    std::vector<Control> controls;
    std::vector<std::pair<int, double>> potMoves, rheostatMoves;
};

std::vector<Model> models;
unsigned active = CATALOG_DEFAULT;
double previousOutput = 0.0;
}

extern "C" void livewdf_catalog_init(unsigned rate) {
    models.clear();
    models.reserve(CATALOG_MODELS);
    for (const auto& preset : schematics) {
        try {
            models.emplace_back(preset.json, rate);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: %s\n", preset.name, error.what());
            std::abort();
        }
    }
    active = CATALOG_DEFAULT;
    previousOutput = 0.0;
}

extern "C" void livewdf_catalog_block(unsigned model, unsigned level, unsigned frames,
                                      const int32_t* controls, const int32_t* input, int32_t* output) {
    constexpr double fullScale = 2147483648.0;
    const bool changed = model != active;
    const double previous = previousOutput;
    active = model;
    auto& circuit = models[model];
    circuit.update(controls, frames);
    for (unsigned i = 0; i < frames; ++i) {
        double sample = circuit.step(input[i] / fullScale) * (level * 0.01);
        if (changed) sample = std::lerp(previous, sample, static_cast<double>(i + 1) / frames);
        previousOutput = std::isfinite(sample) ? std::clamp(sample, -1.0, 1.0 - 1.0 / fullScale) : 0.0;
        output[i] = static_cast<int32_t>(previousOutput * fullScale);
    }
}
