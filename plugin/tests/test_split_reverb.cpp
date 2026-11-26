#include "../source/SplitReverb.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Buffer = juce::AudioBuffer<float>;
using Tap = std::pair<int, float>;
using Taps = std::span<const Tap>;

constexpr Tap leftTaps[]{{0, 0.31f}, {37, -0.17f}, {1023, -0.08f}, {1024, 0.13f},
                         {1025, -0.09f}, {2047, 0.27f}, {2048, -0.19f}, {2049, 0.11f},
                         {4095, 0.18f}, {4096, -0.13f}, {4097, 0.06f},
                         {8193, -0.23f}, {8959, 0.07f}, {8960, -0.11f}, {8961, 0.09f},
                         {16381, 0.12f}};
constexpr Tap rightTaps[]{{0, -0.41f}, {73, 0.15f}, {2048, 0.26f}, {2561, 0.12f},
                          {4096, -0.21f}, {8271, 0.29f}, {16401, -0.09f}};
constexpr float tolerance = 2e-5f;

Buffer makeResponse(Taps left, Taps right = {}) {
    const int size = 1 + (right.empty() ? left.back().first : std::max(left.back().first, right.back().first));
    Buffer response(right.empty() ? 1 : 2, size);
    response.clear();
    for (auto [offset, gain] : left)
        response.setSample(0, offset, gain);
    for (auto [offset, gain] : right)
        response.setSample(1, offset, gain);
    return response;
}

Buffer makeInput(int channels, int size) {
    Buffer input(channels, size);
    std::uint32_t random = 23;
    for (int channel = 0; channel < channels; ++channel) {
        for (int frame = 0; frame < size; ++frame) {
            random = random * 1664525u + 1013904223u;
            const float value = static_cast<float>(random >> 16) / 65536.0f - 0.5f;
            input.setSample(channel, frame, frame < size / 2 ? value * 0.3f : 0.0f);
        }
    }
    return input;
}

Buffer direct(const Buffer& input, Taps left, Taps right = {}) {
    const int frames = input.getNumSamples();
    Buffer output(input.getNumChannels(), frames);
    output.clear();
    for (int channel = 0; channel < output.getNumChannels(); ++channel) {
        const auto taps = channel == 1 && !right.empty() ? right : left;
        for (auto [offset, gain] : taps)
            if (offset < frames)
                output.addFrom(channel, offset, input.getReadPointer(channel), frames - offset, gain);
    }
    return output;
}

enum class RenderMode { offline, live, burst };

void render(livewdf::SplitReverb& effect, Buffer& output, std::span<const int> blocks,
            double sampleRate, RenderMode mode, int begin = 0, int end = -1) {
    using Clock = std::chrono::steady_clock;
    if (end < 0)
        end = output.getNumSamples();
    auto due = Clock::now();
    std::size_t blockIndex = 0;
    for (int pos = begin; pos < end; ++blockIndex) {
        const int frames = std::min(blocks[blockIndex % blocks.size()], end - pos);
        if (mode == RenderMode::live) {
            const auto period = std::chrono::duration<double>(frames / sampleRate);
            due = std::max(due, Clock::now()) + std::chrono::duration_cast<Clock::duration>(period);
            std::this_thread::sleep_until(due);
        }
        effect.process(juce::dsp::AudioBlock<float>(output).getSubBlock(pos, frames), mode == RenderMode::offline);
        pos += frames;
    }
}

bool match(const char* context, const Buffer& actual, const Buffer& expected, int begin = 0) {
    for (int channel = 0; channel < actual.getNumChannels(); ++channel) {
        for (int frame = begin; frame < actual.getNumSamples(); ++frame) {
            const float got = actual.getSample(channel, frame);
            const float want = expected.getSample(channel, frame);
            if (!(std::abs(got - want) < tolerance)) {
                std::cerr << context << ", channel " << channel << ", frame " << frame
                          << ": got " << got << ", expected " << want << '\n';
                return false;
            }
        }
    }
    return true;
}

bool offlineAlignment() {
    constexpr std::array blocks{1, 41, 64, 513, 257, 1001, 9, 129};
    for (const auto [channels, irChannels, sampleRate] :
         {std::array{1, 1, 44100}, std::array{2, 1, 48000}, std::array{2, 2, 96000}}) {
        const auto right = irChannels == 2 ? Taps(rightTaps) : Taps{};
        auto audio = makeInput(channels, 65539);
        const auto expected = direct(audio, leftTaps, right);
        livewdf::SplitReverb effect(makeResponse(leftTaps, right),
                                    {static_cast<double>(sampleRate), 64, static_cast<juce::uint32>(channels)});
        render(effect, audio, blocks, sampleRate, RenderMode::offline);
        if (!match("offline direct convolution", audio, expected))
            return false;
    }
    return true;
}

bool liveAlignment() {
    constexpr double sampleRate = 48000;
    constexpr std::array blocks{61, 67, 128, 35, 89, 257};
    auto audio = makeInput(2, 48137);
    const auto expected = direct(audio, leftTaps, rightTaps);
    livewdf::SplitReverb effect(makeResponse(leftTaps, rightTaps), {sampleRate, 256, 2});
    render(effect, audio, blocks, sampleRate, RenderMode::live, 0, 19103);
    render(effect, audio, blocks, sampleRate, RenderMode::offline, 19103, 27319);
    render(effect, audio, blocks, sampleRate, RenderMode::live, 27319);
    return match("direct convolution across live/offline transitions", audio, expected);
}

bool largeLiveAlignment() {
    constexpr double sampleRate = 48000;
    constexpr std::array blocks{4096, 8192, 1024};
    auto audio = makeInput(1, 26719);
    const auto expected = direct(audio, leftTaps);
    livewdf::SplitReverb effect(makeResponse(leftTaps), {sampleRate, 8192, 1});
    render(effect, audio, blocks, sampleRate, RenderMode::live);
    return match("large live callbacks", audio, expected);
}

bool overloadRecovery() {
    constexpr double sampleRate = 48000;
    constexpr int burstFrames = 131072;
    constexpr int recoveredImpulse = burstFrames + 8192;
    constexpr Tap ir[]{{0, 0.43f}, {8193, -0.24f}, {16381, 0.19f}};
    constexpr std::array blocks{64};
    Buffer audio(1, recoveredImpulse + ir[2].first + 1024);
    audio.clear();
    for (int pos = 0; pos + ir[2].first < burstFrames; pos += 9973)
        audio.setSample(0, pos, 0.2f + static_cast<float>(pos) / burstFrames);
    audio.setSample(0, recoveredImpulse, 0.5f);
    const auto expected = direct(audio, ir);
    const auto early = direct(audio, Taps(ir, 1));
    livewdf::SplitReverb effect(makeResponse(ir), {sampleRate, 64, 1});
    render(effect, audio, blocks, sampleRate, RenderMode::burst, 0, burstFrames);
    render(effect, audio, blocks, sampleRate, RenderMode::live, burstFrames);
    for (int frame = 0; frame < recoveredImpulse; ++frame) {
        const float dryTail = early.getSample(0, frame);
        const float fullTail = expected.getSample(0, frame);
        const float got = audio.getSample(0, frame);
        if (!(got > std::min(dryTail, fullTail) - tolerance && got < std::max(dryTail, fullTail) + tolerance)) {
            std::cerr << "overloaded live worker changed the head or replayed a stale tail at frame "
                      << frame << ": got " << got << '\n';
            return false;
        }
    }
    return match("recovered live tail", audio, expected, recoveredImpulse);
}

} // namespace

int main() {
    return offlineAlignment() && liveAlignment() && overloadRecovery() && largeLiveAlignment() ? 0 : 1;
}
