#include <catch2.hpp>

#include <cmath>

#include "spice/Dense.h"

// The dense setup solver reports only pivot success. This contract test pins
// an independent backward-error check on unfactored systems, including the
// multi-RHS case the network transfer solve actually uses.

TEST_CASE("dense solve is backward stable on a well-conditioned system", "[densediag]") {
    wdf::Dense A{{4.0, 1.0}, {1.0, 3.0}};
    wdf::Dense B{{1.0}, {2.0}};
    REQUIRE(wdf::solveInPlace(A, B));

    wdf::Dense original{{4.0, 1.0}, {1.0, 3.0}};
    wdf::Dense rhs{{1.0}, {2.0}};
    REQUIRE(wdf::maxBackwardError(original, rhs, B) < 1e-12);
}

TEST_CASE("dense backward error covers every right-hand column", "[densediag]") {
    wdf::Dense A{{2.0, -1.0, 0.0}, {-1.0, 2.0, -1.0}, {0.0, -1.0, 2.0}};
    wdf::Dense B{{1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}};
    REQUIRE(wdf::solveInPlace(A, B));

    wdf::Dense original{{2.0, -1.0, 0.0}, {-1.0, 2.0, -1.0}, {0.0, -1.0, 2.0}};
    wdf::Dense rhs{{1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}};
    REQUIRE(wdf::maxBackwardError(original, rhs, B) < 1e-12);
}

TEST_CASE("zero-pivot systems are rejected rather than solved", "[densediag]") {
    wdf::Dense A{{1.0, 2.0}, {2.0, 4.0}};
    wdf::Dense B{{1.0}, {2.0}};
    REQUIRE_FALSE(wdf::solveInPlace(A, B));
}
