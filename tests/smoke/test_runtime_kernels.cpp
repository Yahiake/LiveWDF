#include <catch2.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "wdf/DenseSolve.h"
#include "wdf/RuntimeKernelsX86.h"
#include "wdf/solver/SharedNewtonColumns.h"

TEST_CASE("Dispatch selects the expected DSP kernels", "[cpu-kernels]") {
    using namespace wdf::detail;
    const char* expected = std::getenv("LIVEWDF_TEST_EXPECT_AVX2");
    if (expected == nullptr)
        return;

#if defined(LIVEWDF_HAS_AVX2_KERNELS)
    const bool accelerated = &runtimeKernels() == &avx2Kernels;
#else
    const bool accelerated = false;
#endif
    REQUIRE(accelerated == (std::string_view(expected) == "1"));
}

TEST_CASE("Dispatched dense solvers preserve pivoting and solve results", "[cpu-kernels]") {
    using namespace wdf::detail;
    const int size = GENERATE(1, 2, 3, 4, 18, 25);
    std::vector<double> matrix(size * size, 0.125);
    for (int row = 0; row < size; ++row)
        matrix[row * size + row] = 2.0;
    if (size > 1)
        std::swap_ranges(matrix.begin(), matrix.begin() + size, matrix.end() - size);

    std::vector<double> rhs(size, 0.0);
    for (int row = 0; row < size; ++row)
        for (int col = 0; col < size; ++col)
            rhs[row] += matrix[row * size + col] * (col + 1.0);

    auto directMatrix = matrix;
    auto direct = rhs;
    REQUIRE(solveData(directMatrix.data(), direct.data(), size));

    std::vector<int> pivots(size);
    REQUIRE(factorLUData(matrix.data(), pivots.data(), size));
    REQUIRE(pivots[0] == size - 1);
    solveLUData(matrix.data(), pivots.data(), rhs.data(), size);

    for (int col = 0; col < size; ++col) {
        CHECK(direct[col] == Approx(col + 1.0));
        CHECK(rhs[col] == Approx(col + 1.0));
    }
}

TEST_CASE("Newton factors refresh when an omitted identity row acquires a correction", "[cpu-kernels]") {
    using namespace wdf::detail;
    const std::array<double, 25> original{
        4, 7, 0, 1, 0,
        0, 1, 0, 0, 0,
        0, 0, 1, 0, 0,
        2, 0, 0, 3, 11,
        0, 0, 0, 0, 1};
    auto matrix = original;
    CorrectionLU factors;
    std::array<double, 5> rhs{6, 0, 0, 8, 0};
    REQUIRE(factors.factorAndSolve(matrix.data(), rhs.data(), 5));
    CHECK(rhs[0] == Approx(1.));
    CHECK(rhs[3] == Approx(2.));
    rhs = {11, 0, 0, 12, 0};
    REQUIRE(factors.solve(matrix.data(), rhs.data()));
    CHECK(rhs[0] == Approx(2.1));
    CHECK(rhs[3] == Approx(2.6));

    rhs = {11, 1, 0, 12, 0};
    CHECK_FALSE(factors.solve(matrix.data(), rhs.data()));
    matrix = original;
    rhs = {11, 1, 0, 12, 0};
    REQUIRE(factors.factorAndSolve(matrix.data(), rhs.data(), 5));
    CHECK(rhs[1] == Approx(1.));
}

TEST_CASE("Grouped Newton factors solve a second residual without overwriting the matrix", "[cpu-kernels]") {
    constexpr int ports = 12;
    std::vector<double> transfer(ports * ports), jacobian(ports * ports);
    std::vector<wdf::RootDevice> devices;
    std::array<int, ports> active;
    for (int i = 0; i < ports; ++i) {
        transfer[(i % 8) * ports + i] = .1 * (1 + i % 8);
        jacobian[i * ports + i] = .2 * (1 + i);
        devices.push_back({wdf::DeviceKind::ControlledRes, i, 1, i, 1});
        active[i] = i;
    }
    wdf::solver::SharedNewtonColumns cached, fresh;
    cached.prepare(transfer, ports, ports, devices);
    fresh.prepare(transfer, ports, ports, devices);
    REQUIRE(cached.available());
    std::array<double, ports> residual, actual, expected;
    residual.fill(.5);
    wdf::detail::CorrectionLU factors;
    cached.assemble(jacobian.data(), residual.data(), ports, active.data(), devices);
    REQUIRE(cached.finish(residual.data(), ports, actual.data(), &factors));

    for (int i = 0; i < ports; ++i)
        residual[i] = .1 * (i - 5);
    cached.assemble<false>(jacobian.data(), residual.data(), ports, active.data(), devices);
    fresh.assemble(jacobian.data(), residual.data(), ports, active.data(), devices);
    REQUIRE(cached.finish(residual.data(), ports, actual.data(), &factors, true));
    REQUIRE(fresh.finish(residual.data(), ports, expected.data()));
    for (int i = 0; i < ports; ++i)
        CHECK(actual[i] == Approx(expected[i]).margin(1.e-15));
}

TEST_CASE("Dispatched panel update subtracts the root current contribution", "[cpu-kernels]") {
    using namespace wdf::detail;
    const TransferPanel panel{1, 0, 2, 0};
    double values[2 * transferPanelWidth];
    double nodes[transferPanelWidth + 2];
    const int columns[]{1, 0};
    const double currents[]{2.0, -0.5};
    for (int i = 0; i < transferPanelWidth; ++i) {
        values[i] = i + 1.0;
        values[i + transferPanelWidth] = i * 0.5;
    }
    std::fill_n(nodes, transferPanelWidth + 2, 5.0);

    runtimeKernels().subtractTransferPanels(&panel, 1, values, columns, currents, nodes);

    CHECK(nodes[0] == 5.0);
    for (int i = 0; i < transferPanelWidth; ++i)
        CHECK(nodes[i + 1] == Approx(5.0 - (-0.5 * (i + 1.0) + i)));
    CHECK(nodes[transferPanelWidth + 1] == 5.0);
}
