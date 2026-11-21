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
