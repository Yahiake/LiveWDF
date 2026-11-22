#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "devices/Diode.h"

// The anti-parallel pair's port solve, against the port equation's own root.
//
// The pair solve (eqn (39) of the Werner paper as its start, then Newton on
// the port residual with the exact branch laws) claims a port voltage and two
// branch currents per sample. What it must be is fixed by the law alone: with
// the port impedance R and the open-circuit port voltage vo, the WDF solution
// is the single root of
//     v - vo + R*(i1(v) - i2(-v)) = 0,
// whose left side is strictly increasing (its derivative is
// 1 + R*(di1 + di2) >= 1). A bisection on the same branch laws lands on that
// root with no opinion of its own, so it is a second opinion on the solve
// rather than a copy of it, and how close the two land is what the pair path
// is allowed to claim.
//
// The sweep covers both the branch-current and port-impedance ratios a
// clipping pair hits (the impedance from a lone pair's Thevenin cut to a
// stiff, far-away one) and both signal signs, plus an asymmetric pair -- two
// different branch models across the same node pair, which the start's
// equal-and-opposite-branch assumption does not hold for, so its whole job
// there is to be near enough for the steps to finish.
TEST_CASE("AntiParallelDiodeModel port solve vs the port equation's root", "[diode][pair]") {
    struct Port {
        double vo;
        double impedance;
    };
    const Port ports[] = {
        {0.01, 1.0},    {0.01, 100.0},   {0.01, 1e4},    {0.01, 1e6},
        {0.1, 1.0},     {0.1, 100.0},    {0.1, 1e4},     {0.1, 1e6},
        {1.0, 1.0},     {1.0, 100.0},    {1.0, 1e4},     {1.0, 1e6},
        {5.0, 1.0},     {5.0, 100.0},    {5.0, 1e4},     {5.0, 1e6},
        {20.0, 1.0},    {20.0, 100.0},   {20.0, 1e4},    {20.0, 1e6},
        {-3.0, 1.0},    {-3.0, 100.0},   {-3.0, 1e4},    {-3.0, 1e6},
        {-0.2, 1.0},    {-0.2, 100.0},   {-0.2, 1e4},    {-0.2, 1e6},
    };

    const wdf::DiodeModel a(4.352e-9, 1.905, 0.56, 1e6);  // 1N914-class extended branch
    const wdf::DiodeModel b(3.0e-9, 1.905, 0.40, 1e6);    // a deliberately different one

    for (int pairCase = 0; pairCase < 2; ++pairCase) {
        const bool symmetric = (pairCase == 0);
        const wdf::AntiParallelDiodeModel p(
            a, symmetric ? a : b);
        if (symmetric)
            REQUIRE(p.symmetrical);
        else
            REQUIRE_FALSE(p.symmetrical);

        double worst = 0.0;
        for (const Port& port : ports) {
            double vOut = 0.0, i1 = 0.0, i2 = 0.0;
            p.solvePort(port.vo, port.impedance, vOut, i1, i2);

            // The same port equation, read straight off the branch laws.
            auto portResidual = [&](double x) {
                double up = 0.0, down = 0.0, d1 = 0.0, d2 = 0.0;
                p.d1.eval(x, up, d1);
                p.d2.eval(-x, down, d2);
                return x - port.vo + port.impedance * (up - down);
            };

            double lo = port.vo - 50.0, hi = port.vo + 50.0;
            while (portResidual(lo) > 0.0)
                lo -= 50.0;
            while (portResidual(hi) < 0.0)
                hi += 50.0;
            for (int it = 0; it < 200; ++it) {
                const double mid = 0.5 * (lo + hi);
                if (portResidual(mid) < 0.0)
                    lo = mid;
                else
                    hi = mid;
            }
            const double root = 0.5 * (lo + hi);
            const double err = std::abs(vOut - root);
            worst = std::max(worst, err);

            // The currents must be the branch laws at the voltage that was
            // solved for, not at some earlier iterate.
            double law1 = 0.0, law2 = 0.0, drop = 0.0;
            p.d1.eval(vOut, law1, drop);
            p.d2.eval(-vOut, law2, drop);
            REQUIRE(std::abs(i1 - law1) <= 1e-18 + 1e-9 * std::abs(law1));
            REQUIRE(std::abs(i2 - law2) <= 1e-18 + 1e-9 * std::abs(law2));

            std::cout << "vo=" << port.vo << " R=" << port.impedance
                      << " vOut=" << vOut << " root=" << root
                      << " err=" << err << "\n";
        }
        INFO("pairCase=" << pairCase << " worst port-solve error: " << worst);
        REQUIRE(worst < 1e-9);
    }
}
