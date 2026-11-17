#include <catch2.hpp>

TEST_CASE("test harness works", "[smoke]") {
    // Phase 0 skeleton sanity: proves Catch2 + build + CTest wiring.
    REQUIRE(1 + 1 == 2);
}
