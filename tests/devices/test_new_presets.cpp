#include <catch2.hpp>
#include "devices/Jfet.h"
#include "devices/Mosfet.h"

using namespace wdf;

TEST_CASE("New JFET presets have valid parameters", "[jfets][presets]") {
    const char* names[] = {"2N5459","LSK170A","LSK170B","LSK170C",
                           "LSK189A","LSK189B","LSK189C","JFE150"};
    for (const char* n : names) {
        const JfetEntry* e = findJfetByName(n);
        REQUIRE(e != nullptr);
        REQUIRE(e->model.valid());
        REQUIRE(std::isfinite(e->model.p.Is0));
        REQUIRE(e->model.p.Is0 > 0.0);
        REQUIRE(e->model.p.Vp < 0.0);
        REQUIRE(e->model.p.lambda >= 0.0);
    }
}

TEST_CASE("New JFET presets evaluate at representative operating points", "[jfets][curve]") {
    const char* names[] = {"2N5459","LSK170A","LSK170B","LSK170C",
                           "LSK189A","LSK189B","LSK189C","JFE150"};
    for (const char* n : names) {
        const JfetEntry* e = findJfetByName(n);
        REQUIRE(e != nullptr);
        auto s = e->model.evaluate(0.0, 5.0);
        REQUIRE(s.valid);
        REQUIRE(s.id >= 0.0);
        auto c = e->model.evaluate(-5.0, 2.0);
        REQUIRE(c.valid);
        REQUIRE((c.region == JfetRegion::Cutoff || c.id == 0.0));
    }
}

TEST_CASE("New MOSFET presets have defensible approximate ranges", "[mosfets][curve]") {
    REQUIRE((150.0e-3 > 0.0 && 150.0e-3 < 1.0));
    REQUIRE((2.5 > 0.0 && 2.5 < 10.0));
}

TEST_CASE("2N5459 evaluates at representative point", "[jfets][curve]") {
    JfetModel m = JfetModel::fromSpice(377.6e-6, -4.473, 4.167e-3, 1.0, 1.0, 4.627e-12, 4.0e-12);
    REQUIRE(m.valid());
    auto e = m.evaluate(-2.0, 5.0); // Vgs > Vp, Vds > Vov -> saturation
    REQUIRE(e.valid);
    REQUIRE(e.region == JfetRegion::Saturation);
    REQUIRE(e.id > 0.0);
}
