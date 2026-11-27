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
constexpr double earlySeconds = 0.08;
constexpr auto reverbSizeParam = "reverb_size";

float validRoomSize(float ratio) noexcept {
    return std::isfinite(ratio) ? std::clamp(ratio, 0.5f, 1.5f) : 1.0f;
}
constexpr auto cabinetWave = "cabinet_ir_wave";
constexpr auto cabinetName = "cabinet_ir_name";
constexpr auto cabinetFactory = "cabinet_ir_factory";
constexpr auto reverbWave = "reverb_ir_wave";
constexpr auto reverbName = "reverb_ir_name";
constexpr auto reverbFactory = "reverb_ir_factory";

constexpr FactoryImpulse factoryCabinets[] = {
    {"synthetic-cab", "Studio Cab (Synthetic)", nullptr, 2.9f},
    {"air-deluxe-p12q-sm57", "Deluxe P12Q", "air-deluxe-p12q-sm57.wav", 5.06f},
    {"air-princeton-p10r-sm57", "Princeton P10R", "air-princeton-p10r-sm57.wav", 5.45f},
    {"air-twin-oxford-sm57", "Twin Oxford", "air-twin-oxford-sm57.wav", 5.48f},
    {"air-bassman-c12na-sm57", "Bassman C12NA", "air-bassman-c12na-sm57.wav", 5.94f},
    {"air-champ-g8c-or57", "Champ G8C", "air-champ-g8c-or57.wav", 5.22f},
    {"air-marshall-greenback-sm57", "Marshall Greenback", "air-marshall-greenback-sm57.wav", 4.96f},
    {"air-hiwatt-fane-f25-sm57", "Fane F25", "air-hiwatt-fane-f25-sm57.wav", 5.77f},
    {"air-v30-212-sm57", "V30 2x12", "air-v30-212-sm57.wav", 6.10f},
    {"air-g12t75-212-sm57", "G12T-75 2x12", "air-g12t75-212-sm57.wav", 5.12f},
    {"air-evh-g12-sm57", "EVH G12", "air-evh-g12-sm57.wav", 4.82f},
    {"air-mesa-recto-v30-sm57", "Mesa Rectifier V30", "air-mesa-recto-v30-sm57.wav", 6.01f},
    {"air-engl-v30-sm57", "ENGL E412 V30", "air-engl-v30-sm57.wav", 5.93f},
    {"air-orange-ppc412-sm57", "Orange PPC412", "air-orange-ppc412-sm57.wav", 5.91f},
    {"air-vox-blue-sm57", "Vox Alnico Blue", "air-vox-blue-sm57.wav", 5.50f},
    {"air-roland-jc77-sm57", "Roland JC-77", "air-roland-jc77-sm57.wav", 6.08f},
    {"air-ampeg-810-md421", "Ampeg 8x10", "air-ampeg-810-md421.wav", 14.27f},
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
    {"Ampeg B-15N", "air-ampeg-810-md421"},
    {"ENGL E530", "air-engl-v30-sm57"},
    {"EVH 5150 III", "air-evh-g12-sm57"},
    {"Fender Bassman", "air-bassman-c12na-sm57"},
    {"Fender Deluxe Reverb", "air-deluxe-p12q-sm57"},
    {"Fender Princeton Reverb", "air-princeton-p10r-sm57"},
    {"Fender Tweed Champ", "air-champ-g8c-or57"},
    {"Fender Tweed Deluxe", "air-deluxe-p12q-sm57"},
    {"Fender Twin Reverb", "air-twin-oxford-sm57"},
    {"Hiwatt DR103", "air-hiwatt-fane-f25-sm57"},
    {"Laney GH100L", "air-marshall-greenback-sm57"},
    {"Marshall 1959", "air-marshall-greenback-sm57"},
    {"Marshall 2555", "air-v30-212-sm57"},
    {"Marshall JCM800", "air-marshall-greenback-sm57"},
    {"Marshall Valvestate", "air-g12t75-212-sm57"},
    {"Mesa Boogie Dual Rectifier", "air-mesa-recto-v30-sm57"},
    {"Mesa Boogie Mark IIC+", "air-mesa-recto-v30-sm57"},
    {"Orange OR120", "air-orange-ppc412-sm57"},
    {"Orange Rockerverb 50w - Clean", "air-orange-ppc412-sm57"},
    {"Orange Rockerverb 50w - Dirty", "air-orange-ppc412-sm57"},
    {"Peavey 5150", "air-v30-212-sm57"},
    {"Roland JC-120", "air-roland-jc77-sm57"},
    {"Soldano SLO-100", "air-v30-212-sm57"},
    {"Vox AC15", "air-vox-blue-sm57"},
    {"Vox AC30", "air-vox-blue-sm57"},
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

namespace {
int delaySamples(double rate, float milliseconds) noexcept {
    return std::isfinite(milliseconds) ? juce::roundToInt(rate * milliseconds * 0.001) : 0;
}

class Delay {
public:
    Delay(const juce::dsp::ProcessSpec& spec, float maxMs)
        : ring_(static_cast<int>(spec.numChannels), std::max(1, delaySamples(spec.sampleRate, maxMs) + 1)),
          rampLength_(std::max(1, delaySamples(spec.sampleRate, 15.0f))) {}

    void reset() noexcept { position_ = filled_ = 0; }

    void process(Block block, int delay, float feedback) noexcept {
        const int capacity = ring_.getNumSamples();
        delay = std::clamp(delay, 0, capacity - 1);
        if (filled_ == 0) {
            target_ = previous_ = delay;
            remaining_ = 0;
        } else if (delay != target_ && remaining_ == 0) {
            previous_ = target_;
            target_ = delay;
            remaining_ = rampLength_;
        }
        const auto read = [&](const float* ring, int offset, float input) {
            if (offset == 0) return input;
            if (filled_ < offset) return 0.0f;
            const int index = position_ - offset;
            return ring[index < 0 ? index + capacity : index];
        };
        for (size_t i = 0; i < block.getNumSamples(); ++i) {
            const bool moving = remaining_ != 0;
            const float fraction = 1.0f - static_cast<float>(remaining_) / static_cast<float>(rampLength_);
            for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
                auto* audio = block.getChannelPointer(ch);
                auto* ring = ring_.getWritePointer(static_cast<int>(ch));
                const float input = audio[i];
                const float newSample = read(ring, target_, input);
                const float delayed = moving ? std::lerp(read(ring, previous_, input), newSample, fraction) : newSample;
                ring[position_] = input + feedback * delayed;
                audio[i] = delayed;
            }
            if (++position_ == capacity) position_ = 0;
            if (filled_ < capacity) ++filled_;
            if (moving) --remaining_;
        }
    }

private:
    Buffer ring_;
    int rampLength_, position_ = 0, filled_ = 0;
    int previous_ = 0, target_ = 0, remaining_ = 0;
};

int retainedEarlySamples(const Buffer& room, double rate) {
    const int frames = room.getNumSamples();
    const float audible = std::max(1.e-9f, room.getMagnitude(0, frames) * 0.001f);
    int onset = frames;
    for (int ch = 0; ch < room.getNumChannels(); ++ch) {
        const auto* samples = room.getReadPointer(ch);
        const auto* first = std::find_if(samples, samples + onset,
            [audible](float sample) { return std::abs(sample) > audible; });
        onset = static_cast<int>(first - samples);
    }
    return onset + std::min(frames - onset, juce::roundToInt(earlySeconds * rate));
}

Buffer retimeTail(Buffer room, double rate, float size) {
    const int originalFrames = room.getNumSamples();
    if (size == 1.0f) return room;
    const int earlyFrames = retainedEarlySamples(room, rate);
    if (earlyFrames == originalFrames) return room;
    const int tailFrames = originalFrames - earlyFrames;
    const double framesScaled = earlyFrames + std::round(static_cast<double>(size) * tailFrames);
    if (framesScaled > std::numeric_limits<int>::max())
        throw std::length_error("Resized IR is too large");
    const int frames = static_cast<int>(framesScaled);
    Buffer result(room.getNumChannels(), frames);
    for (int ch = 0; ch < room.getNumChannels(); ++ch) {
        result.copyFrom(ch, 0, room, ch, 0, earlyFrames);
        const auto* source = room.getReadPointer(ch);
        auto* out = result.getWritePointer(ch);
        // Blend the time map into the unchanged onset over the first 10ms of
        // the tail. Reshape that tail only, with unity level near the splice.
        const int transition = std::max(1, juce::roundToInt(rate * 0.01));
        for (int i = earlyFrames; i < frames; ++i) {
            const double progress = std::min(1.0, static_cast<double>(i - earlyFrames) / transition);
            const double position = std::min(static_cast<double>(originalFrames - 1),
                earlyFrames + (i - earlyFrames) * std::lerp(1.0, 1.0 / size, progress));
            const int index = static_cast<int>(position);
            out[i] = std::lerp(source[index], source[std::min(index + 1, originalFrames - 1)],
                               static_cast<float>(position - index));
        }
    }
    return result;
}
} // namespace

struct OutputEffects::PreparedRoom {
    PreparedRoom(const juce::dsp::ProcessSpec& spec, const Impulse& room, float size, std::uint64_t revision)
        : impulseRevision(revision) {
        auto response = room.samples.getNumSamples() > 0
            ? OutputEffects::atRate(room, spec.sampleRate) : hallImpulse(spec.sampleRate);
        response = retimeTail(std::move(response), spec.sampleRate, size);
        frames = response.getNumSamples();
        history = static_cast<size_t>(frames) + spec.maximumBlockSize;
        reverb = std::make_unique<SplitReverb>(std::move(response), spec);
    }
    const std::uint64_t impulseRevision;
    std::unique_ptr<SplitReverb> reverb;
    int frames = 0;
    size_t history = 0, drain = 0;
};

struct OutputEffects::Prepared {
    Prepared(const juce::dsp::ProcessSpec& spec, const Impulse& cab, const Impulse& room,
             float size, std::uint64_t revision)
        : sampleRate(spec.sampleRate), echo(spec, maxEchoDelayMs), preDelay(spec, maxPreDelayMs),
          scratch(static_cast<int>(spec.numChannels), static_cast<int>(std::min(spec.maximumBlockSize, 256u))),
          fadeScratch(scratch.getNumChannels(), scratch.getNumSamples()),
          reverb(std::make_unique<PreparedRoom>(spec, room, size, revision)) {
        const auto stereo = Convolution::Stereo::yes;
        const bool hasCabinet = cab.samples.getNumSamples() > 0;
        auto cabinetResponse = hasCabinet ? OutputEffects::atRate(cab, spec.sampleRate) : cabinetImpulse();
        const auto makeupDb = cab.name.isEmpty() ? factoryCabinets[0].makeupDb : cab.makeupDb;
        cabinetResponse.applyGain(juce::Decibels::decibelsToGain(makeupDb));
        cabinet.loadImpulseResponse(std::move(cabinetResponse), hasCabinet ? spec.sampleRate : cabinetRate,
                                    stereo, Convolution::Trim::no, Convolution::Normalise::no);
        cabinet.prepare({spec.sampleRate, static_cast<juce::uint32>(scratch.getNumSamples()), spec.numChannels});
        cabinetHistory = static_cast<size_t>(cabinet.getCurrentIRSize()) + spec.maximumBlockSize;
    }

    void beginRoomSwap(PreparedRoom* room, bool offline) noexcept {
        fading = std::move(reverb);
        reverb.reset(room);
        fadeRemaining = offline ? 0 : std::max(1, delaySamples(sampleRate, 30.0f));
    }

    void process(Block audio, const Settings& settings, bool offline) noexcept {
        if (!settings.cabinet && settings.echoLevel == 0.0f && settings.reverbLevel == 0.0f
            && cabDrain == 0 && reverb->drain == 0) {
            echo.reset();
            preDelay.reset();
            fadeRemaining = 0;
            return;
        }
        const int echoDelay = std::max(1, delaySamples(sampleRate, std::clamp(settings.echoDelayMs, 0.0f, maxEchoDelayMs)));
        const int reverbDelay = delaySamples(sampleRate, std::clamp(settings.preDelayMs, 0.0f, maxPreDelayMs));
        auto scratchBlock = Block(scratch).getSubsetChannelBlock(0, audio.getNumChannels());
        for (size_t start = 0; start < audio.getNumSamples(); start += scratchBlock.getNumSamples()) {
            const size_t frames = std::min(scratchBlock.getNumSamples(), audio.getNumSamples() - start);
            auto block = audio.getSubBlock(start, frames);
            auto wet = scratchBlock.getSubBlock(0, frames);
            if (settings.cabinet) {
                cabinet.process(juce::dsp::ProcessContextReplacing<float>(block));
                cabDrain = cabinetHistory;
            } else if (cabDrain > 0) {
                wet.clear();
                cabinet.process(juce::dsp::ProcessContextReplacing<float>(wet));
                cabDrain -= std::min(cabDrain, frames);
            }
            if (settings.echoLevel > 0.0f) {
                wet.copyFrom(block);
                echo.process(wet, echoDelay, settings.echoFeedback);
                block.add(wet.multiplyBy(settings.echoLevel));
            } else {
                echo.reset();
            }

            if (settings.reverbLevel > 0.0f) {
                wet.copyFrom(block);
                preDelay.process(wet, reverbDelay, 0.0f);
                if (fadeRemaining > 0) {
                    auto oldWet = Block(fadeScratch).getSubsetChannelBlock(0, wet.getNumChannels()).getSubBlock(0, frames);
                    oldWet.copyFrom(wet);
                    fading->reverb->process(oldWet, offline);
                    reverb->reverb->process(wet, offline);
                    const int fadeLength = std::max(1, delaySamples(sampleRate, 30.0f));
                    for (size_t ch = 0; ch < wet.getNumChannels(); ++ch) {
                        const auto* oldSamples = oldWet.getChannelPointer(ch);
                        auto* newSamples = wet.getChannelPointer(ch);
                        for (size_t i = 0; i < frames; ++i) {
                            const float weight = static_cast<float>(std::max(0, fadeRemaining - static_cast<int>(i)))
                                               / static_cast<float>(fadeLength);
                            newSamples[i] = std::lerp(newSamples[i], oldSamples[i], weight);
                        }
                    }
                    fadeRemaining = std::max(0, fadeRemaining - static_cast<int>(frames));
                } else {
                    reverb->reverb->process(wet, offline);
                }
                block.add(wet.multiplyBy(settings.reverbLevel));
                reverb->drain = reverb->history;
            } else {
                preDelay.reset();
                fadeRemaining = 0;
                if (reverb->drain > 0) {
                    wet.clear();
                    reverb->reverb->process(wet, offline);
                    reverb->drain -= std::min(reverb->drain, frames);
                }
            }
        }
    }

    double sampleRate;
    Convolution cabinet;
    Delay echo, preDelay;
    Buffer scratch, fadeScratch;
    std::unique_ptr<PreparedRoom> reverb, fading;
    size_t cabinetHistory = 0, cabDrain = 0;
    int fadeRemaining = 0;
};

OutputEffects::OutputEffects() = default;
OutputEffects::~OutputEffects() { releasePrepared(); }

void OutputEffects::prepare(const juce::dsp::ProcessSpec& spec, float reverbSize) {
    std::lock_guard lock(setupMutex_);
    releasePrepared();
    spec_ = spec;
    reverbSize_ = requestedReverbSize_ = validRoomSize(reverbSize);
    reverbSizeFailed_ = false;
    active_ = std::make_unique<Prepared>(spec_, cabinet_, reverb_, reverbSize_, impulseRevision_);
    updateTailLengths(active_->reverb->frames);
}

void OutputEffects::release() {
    std::lock_guard lock(setupMutex_);
    spec_ = {};
    releasePrepared();
}

void OutputEffects::releasePrepared() {
    delete pending_.exchange(nullptr);
    delete pendingRoom_.exchange(nullptr);
    reclaim();
    active_.reset();
}

void OutputEffects::reclaim() {
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    delete retiredRoom_.exchange(nullptr, std::memory_order_acq_rel);
}

void OutputEffects::publish(std::unique_ptr<Prepared> next) {
    delete pendingRoom_.exchange(nullptr, std::memory_order_acq_rel);
    if (next) {
        impulseRevision_ = next->reverb->impulseRevision;
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
    for (const auto* name : {"REVERB-IR-SOURCES.md"}) {
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
        next = std::make_unique<Prepared>(spec_, room ? cabinet_ : impulse, room ? impulse : reverb_, reverbSize_, impulseRevision_ + 1);
    (room ? reverb_ : cabinet_) = std::move(impulse);
    if (room) reverbSizeFailed_ = false;
    updateTailLengths(next ? next->reverb->frames : 0);
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
    const auto param = state.getChildWithProperty("id", reverbSizeParam);
    const float size = param.isValid() ? validRoomSize(static_cast<float>(param["value"]) * 0.01f) : 1.0f;
    std::lock_guard lock(setupMutex_);
    std::unique_ptr<Prepared> next;
    if (spec_.sampleRate > 0.0)
        next = std::make_unique<Prepared>(spec_, cabinet, reverb, size, impulseRevision_ + 1);
    cabinet_ = std::move(cabinet);
    reverb_ = std::move(reverb);
    reverbSize_ = requestedReverbSize_ = size;
    reverbSizeFailed_ = false;
    updateTailLengths(next ? next->reverb->frames : 0);
    publish(std::move(next));
}

void OutputEffects::updateTailLengths(int roomFrames) {
    const auto millis = [](const Impulse& ir, double fallback) {
        const double value = ir.sampleRate > 0.0 ? 1000.0 * ir.samples.getNumSamples() / ir.sampleRate : fallback;
        return static_cast<unsigned>(std::min(std::ceil(value), static_cast<double>(std::numeric_limits<unsigned>::max())));
    };
    cabinetMillis_.store(millis(cabinet_, 1000.0 * 48 / cabinetRate), std::memory_order_relaxed);
    const double base = millis(reverb_, 1000.0 * hallSeconds);
    const double early = std::min(base, 1000.0 * earlySeconds);
    const double roomMs = roomFrames > 0 ? 1000.0 * roomFrames / spec_.sampleRate
                                        : early + (base - early) * reverbSize_;
    reverbMillis_.store(static_cast<unsigned>(std::min(std::ceil(roomMs),
                            static_cast<double>(std::numeric_limits<unsigned>::max()))), std::memory_order_relaxed);
}

void OutputEffects::applyReverbSize(float ratio, bool immediate) {
    ratio = validRoomSize(ratio);
    std::lock_guard lock(setupMutex_);
    if (requestedReverbSize_ != ratio) {
        requestedReverbSize_ = ratio;
        reverbSizeFailed_ = false;
        reverbSizeChangedAt_ = juce::Time::getMillisecondCounter();
    }
    if (reverbSize_ == ratio || reverbSizeFailed_
        || (!immediate && juce::Time::getMillisecondCounter() - reverbSizeChangedAt_ < 150u))
        return;
    std::unique_ptr<PreparedRoom> room;
    if (spec_.sampleRate > 0.0) {
        try {
            room = std::make_unique<PreparedRoom>(spec_, reverb_, ratio, impulseRevision_);
        } catch (const std::exception&) {
            reverbSizeFailed_ = true;
            return;
        }
    }
    reverbSize_ = ratio;
    updateTailLengths(room ? room->frames : 0);
    delete pendingRoom_.exchange(room.release(), std::memory_order_acq_rel);
}

double OutputEffects::tailSeconds(const Settings& settings) const noexcept {
    double seconds = 0.0;
    if (settings.cabinet) seconds += cabinetMillis_.load(std::memory_order_relaxed) * 0.001;
    if (settings.echoLevel > 0.0f) {
        const double repeats = settings.echoFeedback > 0.0f
            ? 1.0 + std::ceil(std::log(1.e-6) / std::log(settings.echoFeedback)) : 1.0;
        seconds += repeats * settings.echoDelayMs * 0.001;
    }
    if (settings.reverbLevel > 0.0f)
        seconds += (reverbMillis_.load(std::memory_order_relaxed) + settings.preDelayMs) * 0.001;
    return seconds;
}

void OutputEffects::process(Block block, const Settings& settings, bool offline) noexcept {
    // Retired objects are reclaimed away from the audio callback.
    if (retired_.load(std::memory_order_acquire) == nullptr) {
        if (auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            retired_.store(active_.release(), std::memory_order_release);
            active_.reset(next);
        }
    }
    if (!active_) return;
    if (!active_->fading && retiredRoom_.load(std::memory_order_acquire) == nullptr) {
        if (auto* room = pendingRoom_.exchange(nullptr, std::memory_order_acq_rel)) {
            const auto revision = active_->reverb->impulseRevision;
            if (room->impulseRevision == revision) {
                active_->beginRoomSwap(room, offline);
            } else {
                // Keep a room prepared for an impulse swap that the audio
                // thread has yet to consume. A newer request takes priority.
                PreparedRoom* empty = nullptr;
                if (room->impulseRevision < revision ||
                    !pendingRoom_.compare_exchange_strong(empty, room, std::memory_order_acq_rel))
                    retiredRoom_.store(room, std::memory_order_release);
            }
        }
    }
    active_->process(block, settings, offline);
    if (active_->fading && active_->fadeRemaining == 0)
        retiredRoom_.store(active_->fading.release(), std::memory_order_release);
}

} // namespace livewdf
