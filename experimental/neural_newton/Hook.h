#pragma once

#include <cstddef>
#include <span>

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

}
