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
    bool (*early)(solver::NewtonContext&) noexcept = nullptr;
    std::size_t selected = 0;
    std::size_t retries = 0;
    void (*inspect)(solver::NewtonContext&, double) noexcept = nullptr;
    std::size_t rootRetries = 0;
    double retrySeconds = 0.0;
    int iterationCap = 0;
    bool acceptUnconverged = false;
    bool cappedSample = false;
};
inline thread_local SeedExperiment* seedExperiment = nullptr;

}
