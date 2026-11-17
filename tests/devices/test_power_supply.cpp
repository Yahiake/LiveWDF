#include <catch2.hpp>

#include "devices/PowerSupply.h"
#include "devices/OpAmp.h"
#include "spice/Circuit.h"
#include "wdf/Engine.h"

TEST_CASE("power supply no load returns nominal minus static drop", "[psu]") {
    // Solid state rectifier has ~1.4V static drop
    wdf::PowerSupply psuSs(9.0, 10.0, 100e-6, wdf::RectifierType::SolidState, 48000.0);
    for (int i = 0; i < 1000; ++i) {
        psuSs.tick(0.0);
    }
    const double vSs = psuSs.tick(0.0);
    REQUIRE(vSs == Approx(9.0 - 1.4).margin(0.01));

    // Tube rectifier has ~10V static drop
    wdf::PowerSupply psuTube(480.0, 150.0, 40e-6, wdf::RectifierType::Tube, 48000.0);
    for (int i = 0; i < 1000; ++i) {
        psuTube.tick(0.0);
    }
    const double vTube = psuTube.tick(0.0);
    REQUIRE(vTube == Approx(480.0 - 10.0).margin(0.1));
}

TEST_CASE("power supply steady-state matches first tick", "[psu]") {
    wdf::PowerSupply psu(350.0, 180.0, 16e-6, wdf::RectifierType::Tube, 48000.0);
    const double steady = psu.steadyStateVoltage();
    const double firstTick = psu.tick(0.0);
    REQUIRE(steady == Approx(firstTick).margin(0.01));
}

TEST_CASE("power supply sags under load", "[psu]") {
    wdf::PowerSupply psu(480.0, 150.0, 40e-6, wdf::RectifierType::Tube, 48000.0);
    for (int i = 0; i < 1000; ++i) {
        psu.tick(0.0);
    }
    const double vNoLoad = psu.tick(0.0);

    // Draw 50 mA
    for (int i = 0; i < 2000; ++i) {
        psu.tick(0.050);
    }
    const double vLoaded = psu.tick(0.050);
    REQUIRE(vLoaded < vNoLoad);

    // With 150 ohm and 50 mA, steady-state drop is I * Z = 7.5V
    const double actualDrop = vNoLoad - vLoaded;
    REQUIRE(actualDrop == Approx(7.5).margin(0.5));
}

TEST_CASE("power supply tube sags more than solid state", "[psu]") {
    wdf::PowerSupply psuTube(480.0, 150.0, 40e-6, wdf::RectifierType::Tube, 48000.0);
    wdf::PowerSupply psuSs(480.0, 5.0, 220e-6, wdf::RectifierType::SolidState, 48000.0);

    for (int i = 0; i < 5000; ++i) {
        psuTube.tick(0.100);
        psuSs.tick(0.100);
    }
    const double vTube = psuTube.tick(0.100);
    const double vSs = psuSs.tick(0.100);

    REQUIRE(vTube < vSs);
    REQUIRE(vSs > 470.0);  // Stiff supply stays near nominal
}

TEST_CASE("power supply recovers after transient load", "[psu]") {
    wdf::PowerSupply psu(480.0, 150.0, 40e-6, wdf::RectifierType::Tube, 48000.0);
    for (int i = 0; i < 2000; ++i) {
        psu.tick(0.0);
    }
    const double vStable = psu.tick(0.0);

    // Heavy 200 mA transient
    for (int i = 0; i < 1000; ++i) {
        psu.tick(0.200);
    }
    const double vSagged = psu.tick(0.200);
    REQUIRE(vSagged < vStable - 15.0);

    // Remove load, let it recover
    for (int i = 0; i < 30000; ++i) {
        psu.tick(0.0);
    }
    const double vRecovered = psu.tick(0.0);
    REQUIRE(vRecovered == Approx(vStable).margin(0.5));
}

TEST_CASE("power supply voltage never goes negative on extreme load", "[psu]") {
    wdf::PowerSupply psu(9.0, 50.0, 47e-6, wdf::RectifierType::SolidState, 48000.0);
    for (int i = 0; i < 10000; ++i) {
        const double v = psu.tick(10.0);  // 10 Amps load on 9V pedal
        REQUIRE(v >= 0.0);
    }
}

TEST_CASE("power supply reset restores nominal voltage", "[psu]") {
    wdf::PowerSupply psu(9.0, 50.0, 47e-6, wdf::RectifierType::SolidState, 48000.0);
    for (int i = 0; i < 5000; ++i) {
        psu.tick(0.200);
    }
    REQUIRE(psu.voltage() < 8.0);
    psu.reset();
    REQUIRE(psu.voltage() == Approx(9.0));
}

TEST_CASE("opamp asymmetric soft rails provide smooth saturation and continuous derivative", "[opamp][rails]") {
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.setVRails(4.5, 3.0);  // Asymmetric rails

    // Default: hard rails
    REQUIRE_FALSE(model.softRails);
    REQUIRE(model.applyRails(10.0) == Approx(4.5));
    REQUIRE(model.applyRails(-10.0) == Approx(-3.0));
    REQUIRE(model.railDerivative(10.0) == 0.0);
    REQUIRE(model.railDerivative(2.0) == 1.0);

    // Enable soft rails
    model.enableSoftRails(true);
    REQUIRE(model.softRails);

    // Under soft rails: output is smooth tanh: vRail * tanh(v / vRail)
    const double outPos = model.applyRails(4.5);
    REQUIRE(outPos == Approx(4.5 * std::tanh(1.0)));
    REQUIRE(outPos < 4.5);

    const double outNeg = model.applyRails(-3.0);
    REQUIRE(outNeg == Approx(-3.0 * std::tanh(1.0)));
    REQUIRE(outNeg > -3.0);

    // Large signals approach the rail asymptotically without hard corners
    REQUIRE(model.applyRails(50.0) < 4.5);
    REQUIRE(model.applyRails(50.0) > 4.49);

    // Derivative is continuous: 1 - tanh^2
    const double dPos = model.railDerivative(4.5);
    const double expectedD = 1.0 - std::pow(std::tanh(1.0), 2);
    REQUIRE(dPos == Approx(expectedD));
    REQUIRE(dPos > 0.0);  // Non-zero everywhere on finite input!
}

TEST_CASE("opamp root dynamic power rail sag", "[opamp][psu]") {
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.setVRails(9.0, 9.0);
    wdf::OpAmpRoot root(model);
    root.setSampleRate(48000.0);
    root.enablePowerSag(100.0, 47e-6, wdf::RectifierType::SolidState);

    // Feed steady DC signal drawing current
    root.setVp(1.0);
    const double initialV = root.process(0.0, 100.0);
    REQUIRE(initialV > 0.0);

    // Run for several thousand samples under load
    for (int i = 0; i < 5000; ++i) {
        root.process(0.0, 100.0);
    }
    // Supply rails should have sagged
    REQUIRE(root.model.vRailPos < 9.0);
}

TEST_CASE("engine opamp dynamic power rail sag and soft rails", "[engine][opamp][psu]") {
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.setVRails(9.0, 9.0);

    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 1.0, {}, "V1");
    c.addRes("in", "inv", 10000.0, "Ri");
    c.addRes("inv", "out", 10000.0, "Rf");
    c.addRes("pos", "0", 100000.0, "Rpos");
    c.addRes("out", "0", 100.0, "Rload");  // Heavy 100-ohm load drawing current
    c.addOpAmp("out", "inv", "pos", "0", "0", model, "U1");

    wdf::Engine engine(c, 48000.0);
    REQUIRE(engine.opAmpCount() == 1);

    // Enable soft rails and dynamic power rail sag
    engine.setAllOpAmpSoftRails(true);
    REQUIRE(engine.isOpAmpSoftRails(0));

    engine.enableAllOpAmpRailSag(100.0, 47e-6, wdf::RectifierType::SolidState);
    REQUIRE(engine.isOpAmpRailSagEnabled(0));

    const auto* psuPos = engine.opAmpPowerSupplyPos(0);
    REQUIRE(psuPos != nullptr);
    REQUIRE(psuPos->voltage() == Approx(9.0));

    // Step engine with positive input causing current to flow into load
    for (int i = 0; i < 5000; ++i) {
        engine.step({1.0});
    }

    // Supply rail should have sagged below 9.0V
    REQUIRE(psuPos->voltage() < 8.9);

    // Let input go to zero and run until recovered
    for (int i = 0; i < 30000; ++i) {
        engine.step({0.0});
    }
    REQUIRE(psuPos->voltage() > 8.99);

    // Test disable
    engine.disableAllOpAmpRailSag();
    REQUIRE_FALSE(engine.isOpAmpRailSagEnabled(0));
}
