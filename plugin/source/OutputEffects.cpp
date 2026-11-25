#include "OutputEffects.h"
#include "SplitReverb.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace livewdf {
namespace {
using Buffer = juce::AudioBuffer<float>;
using Block = juce::dsp::AudioBlock<float>;
using Convolution = juce::dsp::Convolution;

constexpr double cabinetRate = 44100.0;
constexpr double hallSeconds = 2.5;
constexpr double echoSeconds = 0.25;
constexpr float echoFeedback = 0.42f;
constexpr auto cabinetWave = "cabinet_ir_wave";
constexpr auto cabinetName = "cabinet_ir_name";
constexpr auto reverbWave = "reverb_ir_wave";
constexpr auto reverbName = "reverb_ir_name";

Buffer cabinetImpulse() {
    // The same symmetric synthetic cabinet IR used by the GP-50 pack.
    constexpr std::array<float, 24> half = {
         0.00000000f,  0.00000535f, -0.00001630f, -0.00028957f, -0.00110638f, -0.00252006f,
        -0.00409955f, -0.00496814f, -0.00421586f, -0.00154458f,  0.00219604f,  0.00496724f,
         0.00420514f, -0.00193926f, -0.01326570f, -0.02675501f, -0.03690152f, -0.03727234f,
        -0.02287111f,  0.00755562f,  0.05024275f,  0.09683055f,  0.13652890f,  0.15936278f,
    };
    Buffer buffer(1, static_cast<int>(2 * half.size()));
    std::copy(half.begin(), half.end(), buffer.getWritePointer(0));
    std::reverse_copy(half.begin(), half.end(), buffer.getWritePointer(0, static_cast<int>(half.size())));
    return buffer;
}

Buffer hallImpulse(double rate) {
    juce::Reverb hall;
    juce::Reverb::Parameters params;
    params.roomSize = 0.8f;
    params.damping = 0.45f;
    params.dryLevel = 0.0f;
    params.wetLevel = 0.8f;
    hall.setParameters(params);
    hall.setSampleRate(rate);
    hall.reset();

    Buffer buffer(2, juce::roundToInt(rate * hallSeconds));
    buffer.clear();
    buffer.setSample(0, 0, 1.0f);
    buffer.setSample(1, 0, 1.0f);
    hall.processStereo(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples());
    const int fade = juce::roundToInt(rate * 0.05);
    buffer.applyGainRamp(buffer.getNumSamples() - fade, fade, 1.0f, 0.0f);
    return buffer;
}
}

struct OutputEffects::Prepared {
    Prepared(const juce::dsp::ProcessSpec& spec, const Impulse& cab, const Impulse& room)
        : echoLine(static_cast<int>(spec.numChannels), std::max(1, juce::roundToInt(spec.sampleRate * echoSeconds))),
          scratch(static_cast<int>(spec.numChannels), static_cast<int>(std::min(spec.maximumBlockSize, 256u))) {
        const auto stereo = Convolution::Stereo::yes;
        if (cab.samples.getNumSamples() > 0)
            cabinet.loadImpulseResponse(OutputEffects::atRate(cab, spec.sampleRate), spec.sampleRate,
                                        stereo, Convolution::Trim::no, Convolution::Normalise::no);
        else
            cabinet.loadImpulseResponse(cabinetImpulse(), cabinetRate,
                                        stereo, Convolution::Trim::no, Convolution::Normalise::no);
        cabinet.prepare({spec.sampleRate, static_cast<juce::uint32>(scratch.getNumSamples()), spec.numChannels});
        cabinetHistory = static_cast<size_t>(cabinet.getCurrentIRSize()) + spec.maximumBlockSize;

        auto response = room.samples.getNumSamples() > 0
            ? OutputEffects::atRate(room, spec.sampleRate) : hallImpulse(spec.sampleRate);
        reverbHistory = static_cast<size_t>(response.getNumSamples()) + spec.maximumBlockSize;
        reverb = std::make_unique<SplitReverb>(std::move(response), spec);
    }

    void process(Block audio, bool useCab, float echoMix, float reverbMix, bool offline) noexcept {
        if (!useCab && echoMix == 0.0f && reverbMix == 0.0f && cabDrain == 0 && reverbDrain == 0) {
            echoPos = echoFilled = 0;
            return;
        }
        auto scratchBlock = Block(scratch).getSubsetChannelBlock(0, audio.getNumChannels());
        for (size_t start = 0; start < audio.getNumSamples(); start += scratchBlock.getNumSamples()) {
            const size_t frames = std::min(scratchBlock.getNumSamples(), audio.getNumSamples() - start);
            auto block = audio.getSubBlock(start, frames);
            auto wet = scratchBlock.getSubBlock(0, frames);
            if (useCab) {
                cabinet.process(juce::dsp::ProcessContextReplacing<float>(block));
                cabDrain = cabinetHistory;
            } else if (cabDrain > 0) {
                wet.clear();
                cabinet.process(juce::dsp::ProcessContextReplacing<float>(wet));
                cabDrain -= std::min(cabDrain, frames);
            }
            if (echoMix > 0.0f)
                processEcho(block, echoMix);
            else
                echoPos = echoFilled = 0;

            if (reverbMix > 0.0f) {
                wet.copyFrom(block);
                reverb->process(wet, offline);
                block.multiplyBy(1.0f - reverbMix).add(wet.multiplyBy(reverbMix));
                reverbDrain = reverbHistory;
            } else if (reverbDrain > 0) {
                wet.clear();
                reverb->process(wet, offline);
                reverbDrain -= std::min(reverbDrain, frames);
            }
        }
    }

    void processEcho(Block block, float mix) noexcept {
        const int length = echoLine.getNumSamples();
        for (size_t i = 0; i < block.getNumSamples(); ++i) {
            for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
                auto* audio = block.getChannelPointer(ch);
                auto* ring = echoLine.getWritePointer(static_cast<int>(ch));
                const float previous = echoFilled == length ? ring[echoPos] : 0.0f;
                const float input = audio[i];
                ring[echoPos] = input + echoFeedback * previous;
                audio[i] = input + mix * (previous - input);
            }
            if (++echoPos == length) {
                echoPos = 0;
                echoFilled = length;
            }
        }
    }

    Convolution cabinet;
    Buffer echoLine, scratch;
    std::unique_ptr<SplitReverb> reverb;
    size_t cabinetHistory = 0, cabDrain = 0, reverbHistory = 0, reverbDrain = 0;
    int echoPos = 0, echoFilled = 0;
};

OutputEffects::OutputEffects() = default;
OutputEffects::~OutputEffects() { releasePrepared(); }

void OutputEffects::prepare(const juce::dsp::ProcessSpec& spec) {
    std::lock_guard lock(setupMutex_);
    releasePrepared();
    spec_ = spec;
    active_ = std::make_unique<Prepared>(spec_, cabinet_, reverb_);
}

void OutputEffects::release() {
    std::lock_guard lock(setupMutex_);
    spec_ = {};
    releasePrepared();
}

void OutputEffects::releasePrepared() {
    delete pending_.exchange(nullptr);
    reclaim();
    active_.reset();
}

void OutputEffects::reclaim() {
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
}

void OutputEffects::publish(std::unique_ptr<Prepared> next) {
    if (next) {
        reclaim();
        delete pending_.exchange(next.release(), std::memory_order_acq_rel);
    }
}

OutputEffects::Impulse OutputEffects::decode(juce::MemoryBlock wave, const juce::String& name) {
    if (wave.isEmpty())
        return {};
    juce::WavAudioFormat format;
    const std::unique_ptr<juce::AudioFormatReader> reader(
        format.createReaderFor(new juce::MemoryInputStream(wave, false), true));
    if (!reader || reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples <= 0
        || reader->lengthInSamples > std::numeric_limits<int>::max()
        || !std::isfinite(reader->sampleRate) || reader->sampleRate <= 0.0)
        return {};
    const size_t bytesPerFrame = ((reader->bitsPerSample + 7u) / 8u) * reader->numChannels;
    if (bytesPerFrame == 0 || static_cast<size_t>(reader->lengthInSamples) > wave.getSize() / bytesPerFrame)
        return {};
    Buffer samples(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    if (!reader->read(samples.getArrayOfWritePointers(), samples.getNumChannels(), 0, samples.getNumSamples()))
        return {};
    for (int channel = 0; channel < samples.getNumChannels(); ++channel) {
        const auto* start = samples.getReadPointer(channel);
        if (!std::all_of(start, start + samples.getNumSamples(), [](float v) { return std::isfinite(v); }))
            return {};
    }
    return { std::move(wave), name, std::move(samples), reader->sampleRate };
}

Buffer OutputEffects::atRate(const Impulse& impulse, double rate) {
    if (impulse.sampleRate == rate)
        return impulse.samples;
    const double ratio = impulse.sampleRate / rate;
    const double frames = std::max(1.0, std::round(impulse.samples.getNumSamples() / ratio));
    if (frames > std::numeric_limits<int>::max())
        throw std::length_error("Resampled IR is too large");
    auto original = impulse.samples;
    juce::MemoryAudioSource source(original, false);
    juce::ResamplingAudioSource resampler(&source, false, original.getNumChannels());
    resampler.setResamplingRatio(ratio);
    Buffer result(original.getNumChannels(), static_cast<int>(frames));
    resampler.prepareToPlay(result.getNumSamples(), rate);
    resampler.getNextAudioBlock({ &result, 0, result.getNumSamples() });
    // The sample sum is the FIR's DC gain. Preserve it across sample rates.
    result.applyGain(static_cast<float>(ratio));
    return result;
}

void OutputEffects::setImpulse(bool room, Impulse impulse) {
    std::lock_guard lock(setupMutex_);
    std::unique_ptr<Prepared> next;
    if (spec_.sampleRate > 0.0)
        next = std::make_unique<Prepared>(spec_, room ? cabinet_ : impulse, room ? impulse : reverb_);
    (room ? reverb_ : cabinet_) = std::move(impulse);
    updateTailLengths();
    publish(std::move(next));
}

bool OutputEffects::loadImpulse(bool room, const juce::File& path) {
    try {
        juce::MemoryBlock wave;
        if (!path.loadFileAsData(wave))
            return false;
        auto impulse = decode(std::move(wave), path.getFileName());
        if (impulse.wave.isEmpty())
            return false;
        setImpulse(room, std::move(impulse));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void OutputEffects::resetImpulse(bool room) { setImpulse(room, {}); }

juce::String OutputEffects::impulseName(bool room) const {
    std::lock_guard lock(setupMutex_);
    const auto& impulse = room ? reverb_ : cabinet_;
    return impulse.wave.isEmpty() ? juce::String("Built-in") : impulse.name;
}

void OutputEffects::save(juce::ValueTree& state) const {
    std::lock_guard lock(setupMutex_);
    if (!cabinet_.wave.isEmpty()) {
        state.setProperty(cabinetWave, cabinet_.wave, nullptr);
        state.setProperty(cabinetName, cabinet_.name, nullptr);
    }
    if (!reverb_.wave.isEmpty()) {
        state.setProperty(reverbWave, reverb_.wave, nullptr);
        state.setProperty(reverbName, reverb_.name, nullptr);
    }
}

void OutputEffects::restore(juce::ValueTree& state) {
    const auto read = [&state](const char* waveKey, const char* nameKey) {
        const auto data = state[waveKey];
        const auto name = state[nameKey].toString();
        auto impulse = data.getBinaryData() ? decode(*data.getBinaryData(), name) : Impulse{};
        state.removeProperty(waveKey, nullptr);
        state.removeProperty(nameKey, nullptr);
        return impulse;
    };
    auto cabinet = read(cabinetWave, cabinetName);
    auto reverb = read(reverbWave, reverbName);
    std::lock_guard lock(setupMutex_);
    std::unique_ptr<Prepared> next;
    if (spec_.sampleRate > 0.0)
        next = std::make_unique<Prepared>(spec_, cabinet, reverb);
    cabinet_ = std::move(cabinet);
    reverb_ = std::move(reverb);
    updateTailLengths();
    publish(std::move(next));
}

void OutputEffects::updateTailLengths() {
    const auto millis = [](const Impulse& ir, double fallback) {
        const double value = ir.sampleRate > 0.0 ? 1000.0 * ir.samples.getNumSamples() / ir.sampleRate : fallback;
        return static_cast<unsigned>(std::min(std::ceil(value), static_cast<double>(std::numeric_limits<unsigned>::max())));
    };
    cabinetMillis_.store(millis(cabinet_, 1000.0 * 48 / cabinetRate), std::memory_order_relaxed);
    reverbMillis_.store(millis(reverb_, 1000.0 * hallSeconds), std::memory_order_relaxed);
}

double OutputEffects::tailSeconds(bool cabinet, bool echo, bool reverb) const noexcept {
    double seconds = 0.0;
    if (cabinet) seconds += cabinetMillis_.load(std::memory_order_relaxed) * 0.001;
    if (echo) seconds += 16 * echoSeconds; // Feedback is below −120 dB after sixteen repeats.
    if (reverb) seconds += reverbMillis_.load(std::memory_order_relaxed) * 0.001;
    return seconds;
}

void OutputEffects::process(Block block, bool cabinet, float echo, float reverb, bool offline) noexcept {
    // Only the audio thread owns the active chain. Destruction and worker joins
    // happen when the control thread reclaims a retired chain.
    if (retired_.load(std::memory_order_acquire) == nullptr) {
        if (auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            retired_.store(active_.release(), std::memory_order_release);
            active_.reset(next);
        }
    }
    if (active_)
        active_->process(block, cabinet, echo, reverb, offline);
}

} // namespace livewdf
