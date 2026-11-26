#include "OutputEffects.h"
#include "SplitReverb.h"
#include "FactoryImpulseData.h"

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
constexpr auto cabinetFactory = "cabinet_ir_factory";
constexpr auto reverbWave = "reverb_ir_wave";
constexpr auto reverbName = "reverb_ir_name";
constexpr auto reverbFactory = "reverb_ir_factory";

constexpr FactoryImpulse factoryCabinets[] = {
    {"synthetic-cab", "Studio Cab (Synthetic)", nullptr, 2.9f},
    {"jensen-sm57", "Jensen SM57", "jensen-sm57.wav", 4.6f},
    {"jensen-e606", "Jensen e606", "jensen-e606.wav", 6.9f},
    {"hk-celestion-sm57", "H&K Celestion SM57", "hk-celestion-sm57.wav", 5.9f},
    {"hk-celestion-c414", "H&K Celestion C414", "hk-celestion-c414.wav", 7.3f},
    {"engl-v30-sm57", "ENGL V30 SM57", "engl-v30-sm57.wav", 6.7f},
    {"engl-v30-e606", "ENGL V30 e606", "engl-v30-e606.wav", 9.4f},
    {"bg412s-v30-sm57", "BG412S V30 SM57", "bg412s-v30-sm57.wav", 5.4f},
    {"bg412s-dv77-sm57", "BG412S DV-77 SM57", "bg412s-dv77-sm57.wav", 4.7f},
    {"bg412s-rockdriver-e606", "BG412S Rockdriver e606", "bg412s-rockdriver-e606.wav", 5.9f},
    {"bg412s-v30-dv77-sm57", "BG412S V30 + DV-77", "bg412s-v30-dv77-sm57.wav", 5.0f},
    {"orange-ppc112-v30-sm57", "Orange 1x12 V30", "orange-ppc112-v30-sm57.wav", 7.7f},
    {"orange-ppc212v-neocreamback", "Orange 2x12 Neo Creamback", "orange-ppc212v-neocreamback.wav", 5.5f},
    {"mesa-412-os-v30", "Mesa 412 OS V30", "mesa-412-os-v30.wav", 5.7f},
    {"celestion-g12-vintage", "Celestion G12 Vintage", "celestion-g12-vintage.wav", 6.1f},
    {"eminence-governor", "Eminence Governor", "eminence-governor.wav", 5.4f},
    {"ampeg-810", "Ampeg 8x10", "ampeg-810.wav", 12.0f},
};

constexpr FactoryImpulse factoryReverbs[] = {
    {"studio-hall", "Studio Hall", nullptr},
    {"plate-studio", "Studio Plate", "plate-studio.flac"},
    {"plate-dark", "Dark Plate", "plate-dark.flac"},
    {"spring-combo", "Combo Spring", "spring-combo.flac"},
    {"spring-tank", "Long Tank Spring", "spring-tank.flac"},
    {"hall-small", "Small Hall", "hall-small.flac"},
    {"hall-concert", "Concert Hall", "hall-concert.flac"},
    {"room-genesis-live", "Genesis 6 Live Room", "room-genesis-live.flac"},
    {"room-maida-vale", "Maida Vale Studio 5", "room-maida-vale.flac"},
    {"hall-jack-lyons", "Jack Lyons Concert Hall", "hall-jack-lyons.flac"},
    {"hall-usina-del-arte", "Usina del Arte Symphony Hall", "hall-usina-del-arte.flac"},
    {"church-st-albans", "St Albans Lady Chapel", "church-st-albans.flac"},
    {"church-york-minster", "York Minster", "church-york-minster.flac"},
    {"outdoor-trollers-gill", "Troller's Gill Gorge", "outdoor-trollers-gill.flac"},
    {"outdoor-koli-forest", "Koli Forest in Winter", "outdoor-koli-forest.flac"},
};

// Audition suggestions include alternatives where the bank lacks a capture
// of the original combo speaker or the amp is a head or preamp.
constexpr std::pair<std::string_view, const char*> cabinetSuggestions[] = {
    {"Ampeg B-15N", "ampeg-810"},
    {"ENGL E530", "engl-v30-sm57"},
    {"EVH 5150 III", "bg412s-dv77-sm57"},
    {"Fender Bassman", "jensen-e606"},
    {"Fender Deluxe Reverb", "jensen-sm57"},
    {"Fender Princeton Reverb", "jensen-sm57"},
    {"Fender Tweed Champ", "jensen-sm57"},
    {"Fender Tweed Deluxe", "jensen-e606"},
    {"Fender Twin Reverb", "jensen-sm57"},
    {"Hiwatt DR103", "hk-celestion-c414"},
    {"Laney GH100L", "hk-celestion-sm57"},
    {"Marshall 1959", "hk-celestion-sm57"},
    {"Marshall 2555", "hk-celestion-sm57"},
    {"Marshall JCM800", "hk-celestion-sm57"},
    {"Marshall Valvestate", "hk-celestion-sm57"},
    {"Mesa Boogie Dual Rectifier", "mesa-412-os-v30"},
    {"Mesa Boogie Mark IIC+", "mesa-412-os-v30"},
    {"Orange OR120", "orange-ppc112-v30-sm57"},
    {"Orange Rockerverb 50w - Clean", "orange-ppc212v-neocreamback"},
    {"Orange Rockerverb 50w - Dirty", "orange-ppc112-v30-sm57"},
    {"Peavey 5150", "bg412s-dv77-sm57"},
    {"Roland JC-120", "jensen-sm57"},
    {"Soldano SLO-100", "mesa-412-os-v30"},
    {"Vox AC15", "orange-ppc212v-neocreamback"},
    {"Vox AC30", "orange-ppc212v-neocreamback"},
};

std::span<const char> factoryResource(std::string_view filename) {
    for (int i = 0; i < FactoryImpulseData::namedResourceListSize; ++i) {
        if (filename != FactoryImpulseData::originalFilenames[i])
            continue;
        int size = 0;
        const auto* data = FactoryImpulseData::getNamedResource(FactoryImpulseData::namedResourceList[i], size);
        if (data != nullptr && size > 0)
            return {data, static_cast<size_t>(size)};
        break;
    }
    return {};
}

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
        const bool hasCabinet = cab.samples.getNumSamples() > 0;
        auto cabinetResponse = hasCabinet ? OutputEffects::atRate(cab, spec.sampleRate) : cabinetImpulse();
        const auto makeupDb = cab.name.isEmpty() ? factoryCabinets[0].makeupDb : cab.makeupDb;
        cabinetResponse.applyGain(juce::Decibels::decibelsToGain(makeupDb));
        cabinet.loadImpulseResponse(std::move(cabinetResponse), hasCabinet ? spec.sampleRate : cabinetRate,
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

OutputEffects::Impulse OutputEffects::decode(juce::MemoryBlock wave, const juce::String& name, bool flac) {
    if (wave.isEmpty())
        return {};
    juce::WavAudioFormat wavFormat;
    juce::FlacAudioFormat flacFormat;
    juce::AudioFormat& format = flac ? static_cast<juce::AudioFormat&>(flacFormat) : wavFormat;
    const std::unique_ptr<juce::AudioFormatReader> reader(
        format.createReaderFor(new juce::MemoryInputStream(wave, false), true));
    if (!reader || reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples <= 0
        || reader->lengthInSamples > std::numeric_limits<int>::max()
        || !std::isfinite(reader->sampleRate) || reader->sampleRate <= 0.0)
        return {};
    const size_t bytesPerFrame = ((reader->bitsPerSample + 7u) / 8u) * reader->numChannels;
    if (!flac && (bytesPerFrame == 0 || static_cast<size_t>(reader->lengthInSamples) > wave.getSize() / bytesPerFrame))
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

std::span<const FactoryImpulse> OutputEffects::factoryImpulses(bool room) noexcept {
    return room ? std::span<const FactoryImpulse>(factoryReverbs) : std::span<const FactoryImpulse>(factoryCabinets);
}

juce::String OutputEffects::suggestedCabinet(std::string_view circuit) {
    for (const auto& [prefix, cabinet] : cabinetSuggestions)
        if (circuit.starts_with(prefix)) return cabinet;
    return {};
}

juce::String OutputEffects::factoryCredits() {
    juce::String credits;
    for (const auto* name : {"CABINET-IR-SOURCES.md", "LICENSE-TONE3000.txt", "REVERB-IR-SOURCES.md"}) {
        const auto data = factoryResource(name);
        if (!data.empty())
            credits << juce::String::fromUTF8(data.data(), static_cast<int>(data.size())) << "\n\n";
    }
    return credits;
}

OutputEffects::Impulse OutputEffects::factoryImpulse(bool room, juce::StringRef id) {
    const auto entries = factoryImpulses(room);
    const auto entry = std::find_if(entries.begin(), entries.end(),
        [id](const auto& item) { return id == juce::StringRef(item.id); });
    if (entry == entries.end())
        return {};

    Impulse result;
    if (entry->file != nullptr) {
        const std::string_view file(entry->file);
        const auto data = factoryResource(file);
        if (!data.empty())
            result = decode(juce::MemoryBlock(data.data(), data.size()), entry->name, file.ends_with(".flac"));
        if (result.samples.getNumSamples() == 0)
            return {};
        result.wave.reset();
    }
    result.name = entry->name;
    result.factoryId = entry->id;
    result.makeupDb = entry->makeupDb;
    return result;
}

bool OutputEffects::selectFactoryImpulse(bool room, juce::StringRef id) {
    try {
        auto impulse = factoryImpulse(room, id);
        if (impulse.factoryId.isEmpty())
            return false;
        setImpulse(room, std::move(impulse));
        return true;
    } catch (const std::exception&) {
        return false;
    }
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
    return impulse.name.isEmpty() ? juce::String(factoryImpulses(room).front().name) : impulse.name;
}

juce::String OutputEffects::impulseId(bool room) const {
    std::lock_guard lock(setupMutex_);
    const auto& impulse = room ? reverb_ : cabinet_;
    return impulse.name.isEmpty() ? juce::String(factoryImpulses(room).front().id) : impulse.factoryId;
}

void OutputEffects::save(juce::ValueTree& state) const {
    std::lock_guard lock(setupMutex_);
    if (!cabinet_.wave.isEmpty()) {
        state.setProperty(cabinetWave, cabinet_.wave, nullptr);
        state.setProperty(cabinetName, cabinet_.name, nullptr);
    } else if (cabinet_.factoryId.isNotEmpty()) {
        state.setProperty(cabinetFactory, cabinet_.factoryId, nullptr);
    }
    if (!reverb_.wave.isEmpty()) {
        state.setProperty(reverbWave, reverb_.wave, nullptr);
        state.setProperty(reverbName, reverb_.name, nullptr);
    } else if (reverb_.factoryId.isNotEmpty()) {
        state.setProperty(reverbFactory, reverb_.factoryId, nullptr);
    }
}

void OutputEffects::restore(juce::ValueTree& state) {
    const auto read = [&state](bool room, const char* waveKey, const char* nameKey, const char* factoryKey) {
        const auto data = state[waveKey];
        const auto name = state[nameKey].toString();
        const auto factory = state[factoryKey].toString();
        auto impulse = data.getBinaryData() ? decode(*data.getBinaryData(), name) : factoryImpulse(room, factory);
        state.removeProperty(waveKey, nullptr);
        state.removeProperty(nameKey, nullptr);
        state.removeProperty(factoryKey, nullptr);
        return impulse;
    };
    auto cabinet = read(false, cabinetWave, cabinetName, cabinetFactory);
    auto reverb = read(true, reverbWave, reverbName, reverbFactory);
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
