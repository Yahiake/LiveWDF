#include <catch2.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include "compiler/Compiler.h"
#include "runtime/Runtime.h"
#include "support/CircuitCases.h"

// Phase 1 audio-thread budget: step() and setParameters() must not allocate
// once the runtime is warm.
//
// Counting happens in the global allocator because that is the only place that
// sees every allocation, including ones inside the engine. Counting is armed
// only around the measured loop, so Catch2's own machinery is not included.

namespace {

std::atomic<bool> g_counting{false};
std::atomic<long long> g_allocations{0};

void countAllocation() noexcept {
    if (g_counting.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
}

struct AllocationScope {
    AllocationScope() noexcept {
        g_allocations.store(0, std::memory_order_relaxed);
        g_counting.store(true, std::memory_order_relaxed);
    }
    ~AllocationScope() { g_counting.store(false, std::memory_order_relaxed); }
    long long count() const noexcept { return g_allocations.load(std::memory_order_relaxed); }
};

wdf::Circuit controlledLinearCircuit() {
    wdf::Circuit c;
    c.addSrc("in", "0", 1.0);
    c.addRes("in", "a", 10e3, "R1");
    c.addPot("a", "b", "0", 100e3, 0.5, wdf::PotTaper::Linear, "GAIN");
    c.addVarRes("b", "0", 50e3, 0.5, wdf::PotTaper::Linear, "MID");
    return c;
}

}  // namespace

// --- global allocation hooks -------------------------------------------------

void* operator new(std::size_t size) {
    countAllocation();
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}

void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

#if defined(_MSC_VER)

void* operator new(std::size_t size, std::align_val_t alignment) {
    countAllocation();
    if (void* p = _aligned_malloc(size == 0 ? 1 : size,
                                  static_cast<std::size_t>(alignment)))
        return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }

#else

void* operator new(std::size_t size, std::align_val_t alignment) {
    countAllocation();
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = size == 0 ? align : ((size + align - 1) / align) * align;
    if (void* p = std::aligned_alloc(align, rounded))
        return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

#endif

// --- tests ------------------------------------------------------------------

TEST_CASE("runtime: steady-state step does not allocate", "[runtime][allocation]") {
    const wdf::Circuit circuit = lwdf_test::fourStagePreamp();
    const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(compiled.ok());

    livewdf::runtime::RuntimeConfig config;
    config.sampleRate = 48000.0;
    auto runtime = livewdf::runtime::makeDenseRuntime(compiled.circuit, config);

    std::vector<double> sources(circuit.src.size(), 0.0);

    // Warm every buffer: the solver, the transfer and the per-sample scratch.
    for (int n = 0; n < 128; ++n) {
        sources[0] = 0.2 * std::sin(0.05 * static_cast<double>(n));
        (void)runtime->step(sources);
    }

    long long allocations = 0;
    {
        AllocationScope scope;
        for (int n = 0; n < 256; ++n) {
            sources[0] = 0.2 * std::sin(0.05 * static_cast<double>(n));
            (void)runtime->step(sources);
        }
        allocations = scope.count();
    }
    REQUIRE(allocations == 0);
}

TEST_CASE("runtime: a parameter batch does not allocate", "[runtime][allocation]") {
    const wdf::Circuit circuit = controlledLinearCircuit();
    const auto compiled = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(compiled.ok());

    livewdf::runtime::RuntimeConfig config;
    config.sampleRate = 48000.0;
    auto runtime = livewdf::runtime::makeDenseRuntime(compiled.circuit, config);

    const auto gain = runtime->parameters().find("GAIN");
    const auto mid = runtime->parameters().find("MID");
    REQUIRE(gain.has_value());
    REQUIRE(mid.has_value());

    // Warm the rebuild path: the first rebuild after construction may grow a
    // scratch buffer, and that one growth is allowed. Steady state is not.
    const livewdf::compiler::ParameterValue warmup[] = {
        {*gain, 0.4},
        {*mid, 0.6},
    };
    REQUIRE(runtime->setParameters(warmup));

    long long allocations = 0;
    {
        AllocationScope scope;
        for (int i = 0; i < 32; ++i) {
            const livewdf::compiler::ParameterValue updates[] = {
                {*gain, 0.2 + 0.01 * static_cast<double>(i)},
                {*mid, 0.8 - 0.01 * static_cast<double>(i)},
            };
            (void)runtime->setParameters(updates);
        }
        allocations = scope.count();
    }
    REQUIRE(allocations == 0);
}
