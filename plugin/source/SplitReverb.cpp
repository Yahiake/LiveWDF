#include "SplitReverb.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <thread>
#include <vector>

namespace livewdf {
namespace {

using Block = juce::dsp::AudioBlock<float>;

// A short direct FIR keeps the live callback out of JUCE's IR swap lock and
// portable FFT lock. Independent accumulators let the compiler vectorize it.
class EarlyReverb {
public:
    EarlyReverb(const juce::AudioBuffer<float>& ir, int channels, int length)
        : taps_((length + 7) & ~7), coefficients_(channels, taps_), history_(channels, 2 * taps_)
    {
        coefficients_.clear();
        history_.clear();
        if (length != 0) {
            for (int channel = 0; channel < channels; ++channel)
                coefficients_.copyFrom(channel, 0, ir, std::min(channel, ir.getNumChannels() - 1), 0, length);
        }
    }

    void process(Block wet) noexcept
    {
        if (taps_ == 0) {
            wet.clear();
            return;
        }
        const auto samples = wet.getNumSamples();
        for (size_t channel = 0; channel < wet.getNumChannels(); ++channel) {
            auto* data = wet.getChannelPointer(channel);
            auto* history = history_.getWritePointer(static_cast<int>(channel));
            const auto* coefficients = coefficients_.getReadPointer(static_cast<int>(channel));
            int at = write_;
            for (size_t sample = 0; sample < samples; ++sample) {
                auto* window = history + at;
                window[0] = window[taps_] = data[sample];
                std::array<float, 8> sum{};
                for (int tap = 0; tap < taps_; tap += 8) {
                    for (int lane = 0; lane < 8; ++lane)
                        sum[lane] += coefficients[tap + lane] * window[tap + lane];
                }
                data[sample] = ((sum[0] + sum[1]) + (sum[2] + sum[3])) +
                               ((sum[4] + sum[5]) + (sum[6] + sum[7]));
                if (--at < 0) at = taps_ - 1;
            }
        }
        write_ = (write_ + taps_ - static_cast<int>(samples % static_cast<size_t>(taps_))) % taps_;
    }

private:
    const int taps_;
    juce::AudioBuffer<float> coefficients_, history_;
    int write_ = 0;
};

// Overlap-add for early heads that grow with the host's maximum block size.
// The first partition is rendered directly. Subsequent partitions can use a
// finished input block because their first tap is already one partition late.
class EarlyPartitions {
public:
    EarlyPartitions(const juce::AudioBuffer<float>& ir, int channels, int block, int end)
        : block_(block), fftSize_(2 * block), count_((end - 1) / block),
          permutation_(fftSize_), roots_(block_), dry_(channels, block_), next_(channels, block_),
          overlap_(channels, block_), sum_(fftSize_)
    {
        dry_.clear();
        next_.clear();
        overlap_.clear();
        for (int i = 1, bits = 0; i < fftSize_; ++i) {
            int bit = fftSize_ / 2;
            for (; (bits & bit) != 0; bit /= 2) bits ^= bit;
            permutation_[i] = bits ^= bit;
        }
        for (int i = 0; i < block_; ++i) {
            const float angle = -std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(block_);
            roots_[i] = {std::cos(angle), std::sin(angle)};
        }
        for (int channel = 0; channel < channels; ++channel) {
            auto& spectra = spectra_[channel];
            spectra.impulse.resize(static_cast<size_t>(count_) * fftSize_);
            spectra.input.resize(spectra.impulse.size());
            for (int part = 0; part < count_; ++part) {
                auto spectrum = partition(spectra.impulse, part);
                const int start = (part + 1) * block_;
                auto* taps = ir.getReadPointer(std::min(channel, ir.getNumChannels() - 1), start);
                for (int i = 0; i < std::min(block_, end - start); ++i) spectrum[i].real = taps[i];
                transform(spectrum, false);
            }
        }
    }

    size_t contiguous(size_t requested) const noexcept
    {
        return std::min(requested, static_cast<size_t>(block_ - fill_));
    }

    void capture(Block input) noexcept
    {
        for (int channel = 0; channel < dry_.getNumChannels(); ++channel)
            dry_.copyFrom(channel, fill_, input.getChannelPointer(channel), static_cast<int>(input.getNumSamples()));
    }

    void mix(Block wet) noexcept
    {
        for (int channel = 0; channel < next_.getNumChannels(); ++channel)
            juce::FloatVectorOperations::add(wet.getChannelPointer(channel), next_.getReadPointer(channel, fill_),
                                             wet.getNumSamples());
        fill_ += static_cast<int>(wet.getNumSamples());
        if (fill_ != block_) return;

        for (int channel = 0; channel < dry_.getNumChannels(); ++channel) {
            auto& spectra = spectra_[channel];
            auto current = partition(spectra.input, position_);
            for (int i = 0; i < block_; ++i) current[i] = {dry_.getSample(channel, i), 0.0f};
            std::fill(current.begin() + block_, current.end(), Complex{});
            transform(current, false);
            std::fill(sum_.begin(), sum_.end(), Complex{});
            int inputPart = position_;
            for (int part = 0; part < count_; ++part) {
                auto signal = partition(spectra.input, inputPart);
                auto impulse = partition(spectra.impulse, part);
                for (int i = 0; i < fftSize_; ++i) {
                    const auto product = multiply(signal[i], impulse[i]);
                    sum_[i].real += product.real;
                    sum_[i].imaginary += product.imaginary;
                }
                if (--inputPart < 0) inputPart = count_ - 1;
            }
            transform(sum_, true);
            auto* ready = next_.getWritePointer(channel);
            auto* overlap = overlap_.getWritePointer(channel);
            const float scale = 1.0f / static_cast<float>(fftSize_);
            for (int i = 0; i < block_; ++i) {
                ready[i] = overlap[i] + scale * sum_[i].real;
                overlap[i] = scale * sum_[block_ + i].real;
            }
        }
        if (++position_ == count_) position_ = 0;
        fill_ = 0;
    }

private:
    struct Complex { float real = 0.0f, imaginary = 0.0f; };
    struct Spectra { std::vector<Complex> impulse, input; };

    static Complex multiply(Complex a, Complex b) noexcept
    {
        return {a.real * b.real - a.imaginary * b.imaginary, a.real * b.imaginary + a.imaginary * b.real};
    }

    std::span<Complex> partition(std::vector<Complex>& storage, int part) const noexcept
    {
        return {storage.data() + static_cast<size_t>(part) * fftSize_, static_cast<size_t>(fftSize_)};
    }

    void transform(std::span<Complex> data, bool inverse) const noexcept
    {
        for (int i = 1; i < fftSize_; ++i)
            if (i < permutation_[i]) std::swap(data[i], data[permutation_[i]]);

        for (int group = 2; group <= fftSize_; group *= 2) {
            const int half = group / 2, step = fftSize_ / group;
            for (int first = 0; first < fftSize_; first += group) {
                for (int i = 0; i < half; ++i) {
                    auto root = roots_[i * step];
                    if (inverse) root.imaginary = -root.imaginary;
                    const auto a = data[first + i], b = multiply(data[first + i + half], root);
                    data[first + i] = {a.real + b.real, a.imaginary + b.imaginary};
                    data[first + i + half] = {a.real - b.real, a.imaginary - b.imaginary};
                }
            }
        }
    }

    const int block_, fftSize_, count_;
    std::vector<int> permutation_;
    std::vector<Complex> roots_;
    juce::AudioBuffer<float> dry_, next_, overlap_;
    std::vector<Complex> sum_;
    std::array<Spectra, 2> spectra_;
    int position_ = 0, fill_ = 0;
};

class ChunkQueue {
public:
    struct Frame {
        juce::AudioBuffer<float> samples;
        uint64_t first = 0;
    };

    ChunkQueue(int channels, int samples, unsigned slots) : frames_(slots)
    {
        for (auto& frame : frames_)
            frame.samples.setSize(channels, samples);
    }

    Frame* reserve() noexcept
    {
        auto write = write_.load(std::memory_order_relaxed);
        return next(write) == read_.load(std::memory_order_acquire) ? nullptr : &frames_[write];
    }

    void publish() noexcept
    {
        write_.store(next(write_.load(std::memory_order_relaxed)), std::memory_order_release);
    }

    const Frame* front() const noexcept
    {
        auto read = read_.load(std::memory_order_relaxed);
        return read == write_.load(std::memory_order_acquire) ? nullptr : &frames_[read];
    }

    void pop() noexcept
    {
        read_.store(next(read_.load(std::memory_order_relaxed)), std::memory_order_release);
    }

    unsigned size() const noexcept { return static_cast<unsigned>(frames_.size()); }

private:
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    unsigned next(unsigned at) const noexcept { return at + 1u == size() ? 0 : at + 1u; }

    std::vector<Frame> frames_;
    alignas(64) std::atomic<unsigned> read_{0};
    alignas(64) std::atomic<unsigned> write_{0};
};

class LateReverb {
public:
    LateReverb(const juce::AudioBuffer<float>& ir, const juce::dsp::ProcessSpec& spec, int chunk, int split)
        : channels_(static_cast<int>(spec.numChannels)), chunk_(chunk), split_(split),
          input_(channels_, chunk, queueSlots(split, chunk)), output_(channels_, chunk, queueSlots(split, chunk)),
          discard_(channels_, chunk),
          convolver_(juce::dsp::Convolution::NonUniform{chunk * 8})
    {
        const auto responseChannels = std::min(ir.getNumChannels(), channels_);
        juce::AudioBuffer<float> tail(responseChannels, ir.getNumSamples() - split);
        for (int channel = 0; channel < responseChannels; ++channel)
            tail.copyFrom(channel, 0, ir, channel, split, tail.getNumSamples());
        convolver_.loadImpulseResponse(std::move(tail), spec.sampleRate,
                                      juce::dsp::Convolution::Stereo::yes,
                                      juce::dsp::Convolution::Trim::no,
                                      juce::dsp::Convolution::Normalise::no);
        auto workerSpec = spec;
        workerSpec.maximumBlockSize = static_cast<juce::uint32>(chunk);
        convolver_.prepare(workerSpec);
        jassert(convolver_.getLatency() == 0);
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~LateReverb()
    {
        stopping_.store(true, std::memory_order_release);
        worker_.join();
    }

    void setOffline(bool offline) noexcept
    {
        if (offline == offline_) return;
        offline_ = offline;
        if (offline) {
            suspendWorker_.store(true, std::memory_order_release);
            // Only non-realtime processing waits for in-flight work.
            unsigned owner = idle;
            while (!owner_.compare_exchange_weak(owner, rendering, std::memory_order_acquire)) {
                owner = idle;
                std::this_thread::yield();
            }
        } else {
            owner_.store(idle, std::memory_order_release);
            suspendWorker_.store(false, std::memory_order_release);
        }
    }

    size_t contiguous(size_t requested) const noexcept
    {
        return std::min(requested, static_cast<size_t>(chunk_ - offset_));
    }

    void capture(Block dry) noexcept
    {
        if (offset_ == 0) {
            expirePastOutput();
            if (offline_) while (renderPending(position_)) {}
            write_ = input_.reserve();
            // Once the first sample of a chunk is due, either hold the
            // complete result for this chunk or drop it, even if it arrives later.
            const auto* candidate = output_.front();
            read_ = candidate != nullptr && candidate->first == position_ ? candidate : nullptr;
        }
        if (write_ != nullptr) {
            for (int channel = 0; channel < channels_; ++channel)
                write_->samples.copyFrom(channel, offset_, dry.getChannelPointer(channel),
                                         static_cast<int>(dry.getNumSamples()));
            if (offset_ + static_cast<int>(dry.getNumSamples()) == chunk_) {
                write_->first = position_;
                input_.publish();
            }
        }
    }

    void mix(Block wet) noexcept
    {
        const auto count = wet.getNumSamples();
        if (read_ != nullptr) {
            for (int channel = 0; channel < channels_; ++channel)
                juce::FloatVectorOperations::add(wet.getChannelPointer(channel),
                                                 read_->samples.getReadPointer(channel, offset_),
                                                 count);
        }
        offset_ += static_cast<int>(count);
        if (offset_ != chunk_) return;
        if (read_ != nullptr) output_.pop();
        offset_ = 0;
        position_ += static_cast<uint64_t>(chunk_);
    }

private:
    // Buffer one prepared callback in addition to the results awaiting their deadline.
    static unsigned queueSlots(int split, int chunk) noexcept
    {
        return static_cast<unsigned>(std::max(16, 2 * (split / chunk) + 1));
    }

    void expirePastOutput() noexcept
    {
        for (unsigned i = 0; i < output_.size(); ++i) {
            const auto* candidate = output_.front();
            if (candidate == nullptr || candidate->first >= position_) break;
            output_.pop();
        }
    }

    bool renderPending(uint64_t firstNeeded = 0) noexcept
    {
        const auto* pending = input_.front();
        if (pending == nullptr) return false;
        const auto first = pending->first;
        const auto due = first + static_cast<uint64_t>(split_);
        auto* result = due >= firstNeeded ? output_.reserve() : nullptr;
        auto& samples = result != nullptr ? result->samples : discard_;
        for (int channel = 0; channel < channels_; ++channel)
            samples.copyFrom(channel, 0, pending->samples, channel, 0, chunk_);
        input_.pop();
        // A dropped input chunk leaves a hole in the convolution state.
        if (first != expected_) convolver_.reset();
        Block block(samples);
        convolver_.process(juce::dsp::ProcessContextReplacing<float>(block));
        expected_ = first + static_cast<uint64_t>(chunk_);
        if (result != nullptr) {
            result->first = due;
            output_.publish();
        }
        return true;
    }

    void workerLoop() noexcept
    {
        juce::ScopedNoDenormals guard;
        while (!stopping_.load(std::memory_order_acquire)) {
            // The live callback only publishes samples. No wake-up system call
            // or worker takeover on that thread, including when we miss a chunk.
            if (!suspendWorker_.load(std::memory_order_acquire) && input_.front() != nullptr) {
                unsigned owner = idle;
                if (owner_.compare_exchange_strong(owner, worker, std::memory_order_acquire)) {
                    renderPending();
                    owner_.store(idle, std::memory_order_release);
                    continue;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    static_assert(std::atomic<bool>::is_always_lock_free);
    enum : unsigned { idle, worker, rendering };
    const int channels_, chunk_, split_;
    ChunkQueue input_, output_;
    juce::AudioBuffer<float> discard_;
    juce::dsp::Convolution convolver_;
    uint64_t position_ = 0, expected_ = 0;
    int offset_ = 0;
    bool offline_ = false;
    ChunkQueue::Frame* write_ = nullptr;
    const ChunkQueue::Frame* read_ = nullptr;
    std::atomic<unsigned> owner_{idle};
    std::atomic<bool> suspendWorker_{false}, stopping_{false};
    std::thread worker_;
};

} // namespace

struct SplitReverb::Impl {
    static constexpr int directLimit = 2048;

    static int earlyLength(const juce::AudioBuffer<float>& ir, uint32_t maximumBlockSize, int chunk) noexcept
    {
        const auto size = static_cast<uint64_t>(chunk);
        auto blocks = (static_cast<uint64_t>(maximumBlockSize) + size - 1) / size;
        return static_cast<int>(std::min((3 + std::max(uint64_t{1}, blocks)) * size,
                                         static_cast<uint64_t>(ir.getNumSamples())));
    }

    Impl(const juce::AudioBuffer<float>& ir, const juce::dsp::ProcessSpec& spec)
        : channels(spec.numChannels), chunk(spec.sampleRate > 48000.0 ? 512 : 256),
          split(ir.getNumChannels() == 0 ? 0 : earlyLength(ir, spec.maximumBlockSize, chunk)),
          early(ir, static_cast<int>(channels), split > directLimit ? chunk : split),
          partitions(split > directLimit ? std::make_unique<EarlyPartitions>(ir, static_cast<int>(channels), chunk, split) : nullptr),
          late(ir.getNumChannels() > 0 && ir.getNumSamples() > split
                   ? std::make_unique<LateReverb>(ir, spec, chunk, split)
                   : nullptr)
    {
        jassert(channels == 1 || channels == 2);
    }

    const size_t channels;
    const int chunk, split;
    EarlyReverb early;
    std::unique_ptr<EarlyPartitions> partitions;
    std::unique_ptr<LateReverb> late;
};

SplitReverb::SplitReverb(juce::AudioBuffer<float> responseAtHostRate, const juce::dsp::ProcessSpec& spec)
    : impl_(std::make_unique<Impl>(responseAtHostRate, spec)) {}

SplitReverb::~SplitReverb() = default;

void SplitReverb::process(Block wet, bool offline) noexcept
{
    jassert(wet.getNumChannels() == impl_->channels);
    if (wet.getNumChannels() != impl_->channels || wet.getNumSamples() == 0) return;
    juce::ScopedNoDenormals guard;
    auto* late = impl_->late.get();
    auto* partitions = impl_->partitions.get();
    if (late == nullptr && partitions == nullptr) {
        impl_->early.process(wet);
        return;
    }
    if (late != nullptr) late->setOffline(offline);
    const auto samples = wet.getNumSamples();
    for (size_t first = 0; first < samples;) {
        auto count = late != nullptr ? late->contiguous(samples - first) : partitions->contiguous(samples - first);
        auto part = wet.getSubBlock(first, count);
        if (late != nullptr) late->capture(part);
        if (partitions != nullptr) partitions->capture(part);
        impl_->early.process(part);
        if (partitions != nullptr) partitions->mix(part);
        if (late != nullptr) late->mix(part);
        first += count;
    }
}

} // namespace livewdf
