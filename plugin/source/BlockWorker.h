#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <thread>
#include <type_traits>
#if !JUCE_WINDOWS
#include <pthread.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

namespace livewdf {

inline void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

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
        if (!post(theirs)) {
            mine();
            theirs();
            return;
        }
        mine();
        if (takeBack())
            theirs();
        join();
    }

    // Audio thread: hand `job` over; false when there is no worker to take it.
    template <class Job>
    bool post(Job& job) noexcept {
        if (!started_.load(std::memory_order_acquire))
            return false;
        if (!priorityMatched_) {
            priorityMatched_ = true;
            realtime_ = matchCallerPriority();
        }
        job_ = [](void* context) { (*static_cast<Job*>(context))(); };
        context_ = &job;
        state_.store(posted, std::memory_order_release);
        state_.notify_one();
        return true;
    }

    bool claimed() const noexcept { return state_.load(std::memory_order_acquire) != posted; }

    // True when the worker had not started the job: the caller runs it.
    bool takeBack() noexcept {
        int state = posted;
        return state_.compare_exchange_strong(state, callerRuns, std::memory_order_acquire);
    }

    void join() noexcept {
        int state;
        while ((state = state_.load(std::memory_order_acquire)) == running)
            state_.wait(running, std::memory_order_acquire);
        state_.store(idle, std::memory_order_relaxed);
    }

    // The worker runs at realtime priority just below the audio thread.
    bool realtime() const noexcept { return realtime_; }

private:
    enum : int { idle, posted, running, callerRuns, done, stopping };

    // Just below the audio thread when it is realtime and the system allows
    // it; otherwise the worker stays at normal priority and the claim covers it.
    bool matchCallerPriority() noexcept {
#if !JUCE_WINDOWS
        int policy = 0;
        sched_param param{};
        if (pthread_getschedparam(pthread_self(), &policy, &param) == 0 &&
            (policy == SCHED_FIFO || policy == SCHED_RR) &&
            param.sched_priority > sched_get_priority_min(policy)) {
            --param.sched_priority;
            return pthread_setschedparam(thread_.native_handle(), policy, &param) == 0;
        }
#endif
        return false;
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
    bool priorityMatched_ = false, realtime_ = false;
    void (*job_)(void*) = nullptr;
    void* context_ = nullptr;
    std::thread thread_;
};

} // namespace livewdf
