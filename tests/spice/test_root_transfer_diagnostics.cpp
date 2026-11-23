#include <catch2.hpp>

#include <array>
#include <vector>

#include "spice/RootTransfer.h"

// Setup-only diagnostic: the multi-column transfer solve reports its own
// backward error so an ill-conditioned reduction can be detected without
// guessing from pivot finiteness. The default path pays nothing for it.

TEST_CASE("root transfer reports backward error and keeps the sign convention", "[rootdiag]") {
    std::vector<wdf::ElemPort> elements{{0, 1, 1.0}, {1, -1, 2.0}};
    std::vector<std::array<int, 2>> roots{{0, -1}};
    wdf::Dense Xe, Xr;
    double backward = -1.0;

    REQUIRE(wdf::rootTransfer(elements, roots, 2, Xe, Xr, {}, {}, &backward));
    REQUIRE(backward >= 0.0);
    REQUIRE(backward < 1e-12);
    REQUIRE(Xe.size() == 2);
    REQUIRE(Xr.size() == 2);
    REQUIRE(Xe[0].size() == 2);
    REQUIRE(Xr[0].size() == 1);

    // v = Xe*b - Xr*i: a unit current injected at node 0 must appear in Xr.
    REQUIRE(std::abs(Xr[0][0]) > 0.0);
}

TEST_CASE("singular transfer fails without writing a backward error", "[rootdiag]") {
    std::vector<wdf::ElemPort> elements{{0, -1, 0.0}};
    std::vector<std::array<int, 2>> roots{{0, -1}};
    wdf::Dense Xe, Xr;
    double backward = -1.0;
    REQUIRE_FALSE(wdf::rootTransfer(elements, roots, 1, Xe, Xr, {}, {}, &backward));
    REQUIRE(backward == -1.0);
}

TEST_CASE("factored transfer preserves node order, signed mutual stamps and pivoting",
          "[rootdiag][transfer]") {
    std::vector<wdf::ElemPort> elements{
        {0, -1, 1.0}, {1, -1, 2.0}, {2, -1, 3.0}, {3, -1, 4.0},
        {0, 1, 0.1}, {0, 2, 0.2}, {0, 3, 0.3}
    };
    const std::vector<int> live{4, 0, 2};
    const std::vector<std::array<int, 2>> roots{{1, 3}, {0, -1}};
    const std::vector<wdf::MutualPortPair> mutual{{0, 2, 0.13}, {2, 0, 0.13}};
    const std::vector<wdf::VCVSTransconductance> vcvs{{0, -1, 1, -1, 31.0}};
    wdf::FactoredTransfer plan;

    for (double contact : {0.1, 1.0e-9, 100.0}) {
        elements[4].g = contact;
        wdf::Dense expectedXe, expectedXr;
        REQUIRE(wdf::rootTransfer(elements, roots, 4, expectedXe, expectedXr, mutual, vcvs));
        wdf::Dense y, rhs, xe, xr;
        double residual = 1.0;
        REQUIRE(wdf::rootTransferInto(elements, roots, 4, y, rhs, xe, xr, mutual, vcvs,
                                      &residual, &plan, live));
        CHECK(residual < 1e-12);
        const double waves[]{-0.3, 0.2, 0.75};
        double nodes[4]{};
        plan.apply(waves, nodes);
        for (int n = 0; n < 4; ++n) {
            double expected = 0.0;
            for (int t = 0; t < 3; ++t)
                expected += expectedXe[n][live[t]] * waves[t];
            CHECK(nodes[n] == Approx(expected).margin(1e-12));
            for (int r = 0; r < 2; ++r)
                CHECK(xr[n][r] == Approx(expectedXr[n][r]).margin(1e-12));
        }
    }
}
