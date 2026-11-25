#include <catch2.hpp>

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "wdf/DenseSolve.h"
#include "wdf/RuntimeKernelsX86.h"

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

TEST_CASE("Dispatched panel update subtracts the root current contribution", "[cpu-kernels]") {
    using namespace wdf::detail;
    const RootPanel panel{1, 0, 2, 0};
    double values[2 * rootPanelWidth];
    double nodes[rootPanelWidth + 2];
    const int columns[]{1, 0};
    const double currents[]{2.0, -0.5};
    for (int i = 0; i < rootPanelWidth; ++i) {
        values[i] = i + 1.0;
        values[i + rootPanelWidth] = i * 0.5;
    }
    std::fill_n(nodes, rootPanelWidth + 2, 5.0);

    runtimeKernels().applyRootPanels(&panel, 1, values, columns, currents, nodes);

    CHECK(nodes[0] == 5.0);
    for (int i = 0; i < rootPanelWidth; ++i)
        CHECK(nodes[i + 1] == Approx(5.0 - (-0.5 * (i + 1.0) + i)));
    CHECK(nodes[rootPanelWidth + 1] == 5.0);
}
