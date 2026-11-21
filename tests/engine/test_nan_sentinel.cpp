#include <catch2.hpp>

#include <limits>

#include "support/CircuitCases.h"
#include "wdf/Engine.h"

// The NaN residual is an intentional sentinel of the closed-form path, not a
// solver failure. A test-only derived class reaches the protected contract
// directly; no production API is widened just to make the contract testable.

namespace {
class ProbeEngine : public wdf::Engine {
public:
    using wdf::Engine::Engine;

    void setResidual(double value, bool converged) {
        lastResidual_ = value;
        lastConverged_ = converged;
    }
    bool acceptable() const { return stateAcceptable(); }
};
}  // namespace

TEST_CASE("NaN residual is accepted only with explicit roots and convergence", "[nancontract]") {
    ProbeEngine e(lwdf_test::commonCathodeBypassed(), 48000.0, false);
    REQUIRE(e.explicitRoots());

    const double nan = std::numeric_limits<double>::quiet_NaN();
    e.setResidual(nan, true);
    REQUIRE(e.acceptable());

    e.setResidual(nan, false);
    REQUIRE_FALSE(e.acceptable());

    e.setResidual(0.0, true);
    REQUIRE(e.acceptable());

    e.setResidual(1e13, true);
    REQUIRE_FALSE(e.acceptable());
}

TEST_CASE("NaN residual cannot pass the joint-Newton path", "[nancontract]") {
    ProbeEngine e(lwdf_test::sharedCathodePair(), 48000.0, false);
    REQUIRE_FALSE(e.explicitRoots());
    e.setResidual(std::numeric_limits<double>::quiet_NaN(), true);
    REQUIRE_FALSE(e.acceptable());
}
