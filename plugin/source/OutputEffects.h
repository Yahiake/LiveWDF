#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>

namespace livewdf {

struct FactoryImpulse {
    const char* id;
    const char* name;
    const char* file;
    float makeupDb = 0.0f;
};

class OutputEffects {
public:
    OutputEffects();
    ~OutputEffects();

    void prepare(const juce::dsp::ProcessSpec&);
    void release();
    void reclaim();

    bool loadImpulse(bool reverb, const juce::File&);
    bool selectFactoryImpulse(bool reverb, juce::StringRef id);
    void resetImpulse(bool reverb);
    juce::String impulseName(bool reverb) const;
    juce::String impulseId(bool reverb) const;
    static std::span<const FactoryImpulse> factoryImpulses(bool reverb) noexcept;
    static juce::String suggestedCabinet(std::string_view circuit);
    static juce::String factoryCredits();
    void save(juce::ValueTree&) const;
    void restore(juce::ValueTree&);

    void process(juce::dsp::AudioBlock<float>, bool cabinet, float echo, float reverb, bool offline) noexcept;
    double tailSeconds(bool cabinet, bool echo, bool reverb) const noexcept;

private:
    struct Impulse {
        juce::MemoryBlock wave;
        juce::String name;
        juce::AudioBuffer<float> samples;
        double sampleRate = 0.0;
        juce::String factoryId{};
        float makeupDb = 0.0f;
    };
    struct Prepared;

    static Impulse decode(juce::MemoryBlock, const juce::String&, bool flac = false);
    static Impulse factoryImpulse(bool reverb, juce::StringRef id);
    static juce::AudioBuffer<float> atRate(const Impulse&, double rate);
    void publish(std::unique_ptr<Prepared>);
    void setImpulse(bool reverb, Impulse);
    void updateTailLengths();
    void releasePrepared();

    mutable std::mutex setupMutex_;
    Impulse cabinet_, reverb_;
    juce::dsp::ProcessSpec spec_{};
    std::unique_ptr<Prepared> active_;
    std::atomic<Prepared*> pending_{nullptr}, retired_{nullptr};
    std::atomic<unsigned> cabinetMillis_{2}, reverbMillis_{2500};
};

} // namespace livewdf
