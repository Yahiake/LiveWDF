#include <catch2.hpp>

#include <vector>

#include "circuit/RootLayout.h"
#include "spice/Compiled.h"

// Contract guard for the three pentode port orders documented in
// docs/03_NETWORK_COMPILER.md:
//   netlist        { plate, screen, grid, cathode }
//   compiled fields{ pk, sk, gk }          (screen before grid)
//   root ports     { pk, gk, sk }          (grid before screen)
// The last one is what every solver, Jacobian, DC seed and Z column sees.
// If this test fails, a port reorder has leaked into the numerical contract.

namespace {
wdf::Port makePort(int a, int b) { return wdf::Port{a, b, {}}; }
}  // namespace

TEST_CASE("pentode root layout is plate, control grid, screen grid", "[corecontracts]") {
    wdf::Compiled compiled;
    wdf::Compiled::CompiledPentode pentode;
    pentode.pk = makePort(0, 3);  // plate-cathode
    pentode.sk = makePort(1, 3);  // screen-cathode
    pentode.gk = makePort(2, 3);  // control-grid-cathode
    pentode.name = "V1";
    compiled.pent.push_back(pentode);

    const wdf::RootLayout layout = wdf::buildRootLayout(compiled);

    REQUIRE(layout.K == 3);
    REQUIRE(layout.devices.size() == 1);

    const auto& dev = layout.devices.front();
    REQUIRE(dev.kind == wdf::RootKind::Pentode);
    REQUIRE(dev.compIndex == 0);
    REQUIRE(dev.portBase == 0);
    REQUIRE(dev.numPorts == 3);
    REQUIRE(dev.numActiveRows == 3);
    REQUIRE(dev.activePorts[0] == 0);
    REQUIRE(dev.activePorts[1] == 1);
    REQUIRE(dev.activePorts[2] == 2);

    REQUIRE(layout.ports.size() == 3);
    REQUIRE(layout.ports[0].a == 0);  // plate
    REQUIRE(layout.ports[0].b == 3);  // cathode
    REQUIRE(layout.ports[1].a == 2);  // control grid
    REQUIRE(layout.ports[1].b == 3);
    REQUIRE(layout.ports[2].a == 1);  // screen grid
    REQUIRE(layout.ports[2].b == 3);

    REQUIRE(layout.redCol == std::vector<int>{0, 1, 2});
    REQUIRE(layout.redRow == std::vector<int>{0, 1, 2});
    REQUIRE(layout.nodeCol == std::vector<int>{0, 1, 2});
}
