#include <catch2.hpp>

#include <cmath>
#include <vector>

#include "devices/WrightOmega.h"

// Correctness and precision tests for the MIT-derived Wright Omega implementation.
// Equation: w + ln(w) = z for w > 0.

TEST_CASE("wrightOmega evaluates known mathematical anchor points", "[omega][devices]") {
    // At z = 1: 1 + ln(1) = 1, so w(1) must be exactly 1.0
    const double w1 = wdf::wrightOmega(1.0);
    CHECK(w1 == Approx(1.0).margin(1e-14));

    // At z = 0: w(0) = Omega constant ~ 0.5671432904097838729999686622
    constexpr double kOmega = 0.5671432904097838729999686622;
    const double w0 = wdf::wrightOmega(0.0);
    CHECK(w0 == Approx(kOmega).margin(1e-14));

    // At z = -1: w(-1) ~ 0.2784645427610738
    constexpr double wNeg1 = 0.2784645427610738;
    CHECK(wdf::wrightOmega(-1.0) == Approx(wNeg1).margin(1e-14));
}

TEST_CASE("wrightOmega satisfies w + ln(w) = z to machine precision across wide domain",
          "[omega][devices]") {
    constexpr int kPoints = 2000;
    constexpr double zMin = -35.0;
    constexpr double zMax = 40.0;
    constexpr double dz = (zMax - zMin) / kPoints;

    double maxResidual = 0.0;
    for (int i = 0; i <= kPoints; ++i) {
        const double z = zMin + i * dz;
        const double w = wdf::wrightOmega(z);

        REQUIRE(std::isfinite(w));
        REQUIRE(w > 0.0);

        const double residual = std::abs(w + std::log(w) - z);
        maxResidual = std::max(maxResidual, residual);
        CHECK(residual < 1e-12);
    }
    CHECK(maxResidual < 1e-13);
}

TEST_CASE("wrightOmega handles far-negative asymptote without underflow", "[omega][devices]") {
    const double farNegative[] = {-36.0, -40.0, -50.0, -100.0, -300.0};
    for (double z : farNegative) {
        CAPTURE(z);
        const double w = wdf::wrightOmega(z);
        CHECK(std::isfinite(w));
        CHECK(w >= 0.0);
        CHECK(w == Approx(std::exp(z)).margin(1e-16));
    }
}

TEST_CASE("wrightOmega is strictly monotonic", "[omega][devices]") {
    double prevW = 0.0;
    for (int i = -300; i <= 300; ++i) {
        const double z = 0.1 * static_cast<double>(i);
        const double w = wdf::wrightOmega(z);
        if (i > -300) {
            CHECK(w > prevW);
        }
        prevW = w;
    }
}
