#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <thread>
#include <type_traits>
#if !JUCE_WINDOWS
#include <pthread.h>
#endif

namespace livewdf {

// One persistent thread that runs a job beside the audio thread. The audio
// thread posts the job, does its own part and then joins; when the worker has
// not claimed the job by then the audio thread runs it itself, so a late,
// descheduled or missing worker never makes the callback slower than serial.
class BlockWorker {
public:
    ~BlockWorker() {
        if (!thread_.joinable()) return;
        state_.store(stopping, std::memory_order_release);
        state_.notify_one();
        thread_.join();
    }

    // Message thread only.
    void start() {
        if (started_.load(std::memory_order_relaxed)) return;
        thread_ = std::thread([this] { loop(); });
        started_.store(true, std::memory_order_release);
    }

    template <class Mine, class Theirs>
    void runBeside(Mine&& mine, Theirs&& theirs) noexcept {
        if (!started_.load(std::memory_order_acquire)) {
            mine();
            theirs();
            return;
        }
        if (!priorityMatched_) {
            priorityMatched_ = true;
            matchCallerPriority();
        }
        job_ = [](void* job) { (*static_cast<std::remove_reference_t<Theirs>*>(job))(); };
        context_ = &theirs;
        state_.store(posted, std::memory_order_release);
        state_.notify_one();
        mine();
        int state = posted;
        if (state_.compare_exchange_strong(state, callerRuns, std::memory_order_acquire))
            theirs();
        else
            while ((state = state_.load(std::memory_order_acquire)) == running)
                state_.wait(running, std::memory_order_acquire);
        state_.store(idle, std::memory_order_relaxed);
    }

private:
    enum : int { idle, posted, running, callerRuns, done, stopping };

    // Just below the audio thread when it is realtime and the system allows
    // it; otherwise the worker stays at normal priority and the claim covers it.
    void matchCallerPriority() noexcept {
#if !JUCE_WINDOWS
        int policy = 0;
        sched_param param{};
        if (pthread_getschedparam(pthread_self(), &policy, &param) == 0 &&
            (policy == SCHED_FIFO || policy == SCHED_RR) &&
            param.sched_priority > sched_get_priority_min(policy)) {
            --param.sched_priority;
            pthread_setschedparam(thread_.native_handle(), policy, &param);
        }
#endif
    }

    void loop() noexcept {
        juce::ScopedNoDenormals noDenormals;
        for (int state = state_.load(std::memory_order_acquire); state != stopping;
             state = state_.load(std::memory_order_acquire)) {
            if (state == posted && state_.compare_exchange_strong(state, running, std::memory_order_acquire)) {
                job_(context_);
                state_.store(done, std::memory_order_release);
                state_.notify_one();
            } else {
                state_.wait(state, std::memory_order_acquire);
            }
        }
    }

    std::atomic<int> state_{idle};
    std::atomic<bool> started_{false};
    bool priorityMatched_ = false;
    void (*job_)(void*) = nullptr;
    void* context_ = nullptr;
    std::thread thread_;
};

} // namespace livewdf
