#include <catch2.hpp>

#include <array>
#include <cmath>

#include "devices/Triode.h"

namespace {

constexpr double kGridOn0 = -0.0745;
constexpr double kGridOn1 =  0.0941;
constexpr double kGridOn2 =  0.3000;

constexpr double kGridR0 = 3494.0;
constexpr double kGridR1 = 1550.0;
constexpr double kGridR2 = 1100.0;

constexpr double kFirstTransition =
    (kGridR0 * kGridOn1 - kGridR1 * kGridOn0) /
    (kGridR0 - kGridR1);

constexpr double kSecondTransition =
    (kGridR1 * kGridOn2 - kGridR2 * kGridOn1) /
    (kGridR1 - kGridR2);

struct GridResult {
    double current;
    double slope;
};

GridResult evaluateGrid(const wdf::TriodeModel& model, double vgk) {
    double slope = -1.0;
    const double current = model.gridCurrent(vgk, slope);
    return {current, slope};
}

} // namespace

TEST_CASE("Triode grid current selects the correct upper-envelope branch",
          "[triode][grid]") {
    const wdf::TriodeModel model = wdf::k12ax7Quadric();

    struct Expected {
        double voltage;
        double current;
        double slope;
    };

    const std::array<Expected, 5> cases{{
        {-0.1000, 0.0, 0.0},
        { kGridOn0, 0.0, 0.0},
        { 0.0000, (0.0000 - kGridOn0) / kGridR0, 1.0 / kGridR0},
        { 0.5000, (0.5000 - kGridOn1) / kGridR1, 1.0 / kGridR1},
        { 1.2000, (1.2000 - kGridOn2) / kGridR2, 1.0 / kGridR2}
    }};

    for (const Expected& test : cases) {
        INFO("Vgk = " << test.voltage);

        const GridResult result = evaluateGrid(model, test.voltage);

        REQUIRE(result.current ==
                Approx(test.current).epsilon(1e-12).margin(1e-15));

        REQUIRE(result.slope ==
                Approx(test.slope).epsilon(1e-12).margin(1e-15));
    }
}

TEST_CASE("Triode grid current is continuous at branch transitions",
          "[triode][grid]") {
    const wdf::TriodeModel model = wdf::k12ax7Quadric();

    constexpr double h = 1e-7;

    SECTION("first transition: 3494 ohms to 1550 ohms") {
        const GridResult left =
            evaluateGrid(model, kFirstTransition - h);
        const GridResult at =
            evaluateGrid(model, kFirstTransition);
        const GridResult right =
            evaluateGrid(model, kFirstTransition + h);

        const double currentLeft =
            (kFirstTransition - kGridOn0) / kGridR0;
        const double currentRight =
            (kFirstTransition - kGridOn1) / kGridR1;

        REQUIRE(currentLeft == Approx(currentRight).margin(1e-14));
        REQUIRE(at.current == Approx(currentLeft).margin(1e-14));
        REQUIRE(at.current == Approx(currentRight).margin(1e-14));

        REQUIRE(left.slope == Approx(1.0 / kGridR0));
        REQUIRE(right.slope == Approx(1.0 / kGridR1));

        REQUIRE(left.current ==
                Approx((kFirstTransition - h - kGridOn0) / kGridR0).margin(1e-14));
        REQUIRE(right.current ==
                Approx((kFirstTransition + h - kGridOn1) / kGridR1).margin(1e-14));
    }

    SECTION("second transition: 1550 ohms to 1100 ohms") {
        const GridResult left =
            evaluateGrid(model, kSecondTransition - h);
        const GridResult at =
            evaluateGrid(model, kSecondTransition);
        const GridResult right =
            evaluateGrid(model, kSecondTransition + h);

        const double currentLeft =
            (kSecondTransition - kGridOn1) / kGridR1;
        const double currentRight =
            (kSecondTransition - kGridOn2) / kGridR2;

        REQUIRE(currentLeft == Approx(currentRight).margin(1e-14));
        REQUIRE(at.current == Approx(currentLeft).margin(1e-14));
        REQUIRE(at.current == Approx(currentRight).margin(1e-14));

        REQUIRE(left.slope == Approx(1.0 / kGridR1));
        REQUIRE(right.slope == Approx(1.0 / kGridR2));

        REQUIRE(left.current ==
                Approx((kSecondTransition - h - kGridOn1) / kGridR1).margin(1e-14));
        REQUIRE(right.current ==
                Approx((kSecondTransition + h - kGridOn2) / kGridR2).margin(1e-14));
    }
}

TEST_CASE("Triode grid-current analytical slope matches finite differences",
          "[triode][grid][jacobian]") {
    const wdf::TriodeModel model = wdf::k12ax7Quadric();

    constexpr double h = 1e-6;

    constexpr std::array<double, 3> voltages{{
        0.0,
        0.5,
        1.2
    }};

    for (const double voltage : voltages) {
        INFO("Vgk = " << voltage);

        const GridResult center = evaluateGrid(model, voltage);
        const double currentPlus =
            evaluateGrid(model, voltage + h).current;
        const double currentMinus =
            evaluateGrid(model, voltage - h).current;

        const double numericalSlope =
            (currentPlus - currentMinus) / (2.0 * h);

        REQUIRE(center.slope ==
                Approx(numericalSlope).epsilon(1e-7).margin(1e-12));
    }
}

TEST_CASE("Disabling triode grid conduction zeros current and slope",
          "[triode][grid]") {
    wdf::TriodeModel model = wdf::k12ax7Quadric();
    model.gridConducts = false;

    constexpr std::array<double, 4> voltages{{
        -0.1, 0.0, 0.5, 1.2
    }};

    for (const double voltage : voltages) {
        INFO("Vgk = " << voltage);

        const GridResult result = evaluateGrid(model, voltage);

        REQUIRE(result.current == 0.0);
        REQUIRE(result.slope == 0.0);
    }
}