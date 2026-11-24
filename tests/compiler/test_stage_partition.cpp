#include <catch2.hpp>

#include <limits>
#include <random>
#include "compiler/StagePartition.h"

namespace {
wdf::RootLayout diodePorts(int n) {
    wdf::RootLayout layout;
    layout.K = n;
    for (int i = 0; i < n; ++i)
        layout.devices.push_back({wdf::RootKind::Diode, i, i, 1, i, 1, {0, 0, 0}});
    return layout;
}
}

TEST_CASE("Stage discovery recovers both directions of a loaded split", "[compiler][stage-partition]") {
    constexpr int n = 12;
    auto layout = diodePorts(n);
    std::mt19937 rng(41);
    std::uniform_real_distribution<double> random(10., 20.);
    std::vector<double> z(n*n);
    // Interleave the physical groups. Each has an arbitrary internal matrix.
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            z[i*n+j] = i%2 == j%2 ? random(rng) : (i+1.)*(j+1.)*(i%2 ? -1e-3 : 2e-3);
    const auto plan = livewdf::compiler::discoverStagePartition(layout, z, 96000., 7);
    REQUIRE(plan);
    REQUIRE(plan->sampleRate == 96000.);
    REQUIRE(plan->linearRevision == 7);
    for (int side = 0; side < 2; ++side) {
        const auto& c = plan->toStage[side];
        REQUIRE(plan->devices[side].size() == 6);
        REQUIRE(c.rank == 1);
        for (size_t i = 0; i < c.voltagePorts.size(); ++i)
            for (size_t j = 0; j < c.currentPorts.size(); ++j) {
                REQUIRE(c.voltagePorts[i]%2 == side);
                REQUIRE(c.currentPorts[j]%2 == 1-side);
                REQUIRE(c.leftFactor[i]*c.rightFactor[j] ==
                        Approx(z[c.voltagePorts[i]*n+c.currentPorts[j]]).margin(1e-13));
            }
    }
}

TEST_CASE("Stage discovery declines dense coupling and invalid transfers", "[compiler][stage-partition]") {
    constexpr int n = 12;
    auto layout = diodePorts(n);
    std::mt19937 rng(53);
    std::uniform_real_distribution<double> random(-1., 1.);
    std::vector<double> z(n*n);
    for (auto& x : z) x = random(rng);
    REQUIRE_FALSE(livewdf::compiler::discoverStagePartition(layout, z, 48000., 2));
    std::fill(z.begin(), z.end(), 0.0);
    REQUIRE(livewdf::compiler::discoverStagePartition(layout, z, 48000., 2));
    REQUIRE_FALSE(livewdf::compiler::discoverStagePartition(layout, z, 0., 2));
    REQUIRE_FALSE(livewdf::compiler::discoverStagePartition(layout, z, 48000., -1));
    z.back() = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(livewdf::compiler::discoverStagePartition(layout, z, 48000., 2));
    z.back() = 0.0;
    layout.devices.back().portBase = n;
    REQUIRE_FALSE(livewdf::compiler::discoverStagePartition(layout, z, 48000., 2));
}

TEST_CASE("Stage interfaces preserve physical BJT, FET, and op-amp terminal loading", "[compiler][stage-partition]") {
    wdf::RootLayout layout;
    for (const auto& [kind, width] : std::array{
            std::pair{wdf::RootKind::Bjt, 3}, std::pair{wdf::RootKind::OpAmp, 3},
            std::pair{wdf::RootKind::Mosfet, 2}, std::pair{wdf::RootKind::Jfet, 2}}) {
        layout.devices.push_back({kind, 0, layout.K, width});
        layout.K += width;
    }
    const std::vector<double> zero(static_cast<size_t>(layout.K*layout.K));
    const auto p = livewdf::compiler::discoverStagePartition(layout, zero, 48000., 0);
    REQUIRE(p);
    std::vector<int> inputs, currents;
    for (const auto& c : p->toStage) {
        inputs.insert(inputs.end(), c.voltagePorts.begin(), c.voltagePorts.end());
        currents.insert(currents.end(), c.currentPorts.begin(), c.currentPorts.end());
    }
    std::sort(inputs.begin(), inputs.end());
    std::sort(currents.begin(), currents.end());
    const std::vector<int> expectedInputs{0, 1, 3, 4, 5, 6, 7, 8, 9};
    const std::vector<int> expectedCurrents{0, 1, 3, 6, 8};
    REQUIRE(inputs == expectedInputs);
    REQUIRE(currents == expectedCurrents);
}
