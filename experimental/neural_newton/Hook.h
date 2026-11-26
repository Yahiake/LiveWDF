#pragma once

#include <cstddef>
#include <span>

namespace wdf::solver {
struct NewtonContext;
}

namespace wdf::experimental {

struct NewtonSeed {
    std::span<double> guess;
    std::span<const double> previous;
    std::span<const double> older;
    std::span<const double> residual;
};

struct SeedExperiment {
    bool (*propose)(const NewtonSeed&) noexcept;
    std::size_t selected = 0;
    std::size_t retries = 0;
};
inline thread_local SeedExperiment* seedExperiment = nullptr;

struct LuFactors {
    const double* matrix = nullptr;
    const int* pivots = nullptr;
};

struct LuExperiment {
    LuFactors (*choose)(const solver::NewtonContext&) noexcept;
    std::size_t attempted = 0;
    std::size_t accepted = 0;
};
inline thread_local LuExperiment* luExperiment = nullptr;

}
