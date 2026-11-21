#include <catch2.hpp>

#include <cmath>
#include <initializer_list>

#include "spice/Circuit.h"

// Pot tapers are a front-end contract: end points must not create zero
// conductances, the two legs must conserve the declared resistance, and the
// wiper must move monotonically. Deterministic sweep, no random data.

TEST_CASE("pot tapers clamp endpoints and conserve total resistance", "[potcontract]") {
    constexpr double kTotal = 100e3;
    for (wdf::PotTaper taper : {wdf::PotTaper::Linear, wdf::PotTaper::Log, wdf::PotTaper::RevLog}) {
        double previous = -1.0;
        for (int i = 0; i <= 100; ++i) {
            const double position = static_cast<double>(i) / 100.0;
            const auto legs = wdf::Pot::resistances(kTotal, position, taper);
            REQUIRE(std::isfinite(legs.first));
            REQUIRE(std::isfinite(legs.second));
            REQUIRE(legs.first >= wdf::Pot::kEps);
            REQUIRE(legs.second >= wdf::Pot::kEps);
            REQUIRE(legs.first + legs.second == Approx(kTotal).epsilon(1e-12));
            REQUIRE(legs.second >= previous - 1e-6);
            previous = legs.second;
        }
    }
}
