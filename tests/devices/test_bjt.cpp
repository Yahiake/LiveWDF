#include <catch2.hpp>
#include "devices/Bjt.h"
#include "spice/Circuit.h"
#include "format/LiveWDFFile.h"
#include "spice/SpiceExport.h"
#include "support/NgspiceOracle.h"
#include <cmath>
#include <iomanip>
#include <sstream>

using namespace wdf;

TEST_CASE("BJT presets valid and evaluate at representative points", "[bjt][presets]") {
    const char* names[] = {"2N3904","2N2222A","2N2222","BC547B","2N5088","BC109C","2SC2240",
                           "2N3906","2N2907","BC557B","2N5087","2N3904-MiSTer","2N3906-MiSTer"};
    for (const char* n : names) {
        const BjtEntry* e = findBjtByName(n);
        REQUIRE(e != nullptr);
        REQUIRE(e->model.valid());
        BjtModel m = e->model;
        m.prepare();
        // Forward-active bias follows the card's polarity: Q1's terminal
        // currents are positive for NPN and negative for PNP.
        const double sign = m.pnp ? -1.0 : 1.0;
        auto res = m.evaluateTerminals(sign * 5.0, sign * 0.65);
        REQUIRE(res.valid);
        CHECK(sign * res.iC > 0.0);
        CHECK(sign * res.iB > 0.0);
        CHECK(sign * res.iE < 0.0);
        CHECK(std::isfinite(res.iC));
        CHECK(std::isfinite(res.iB));
        CHECK(std::isfinite(res.iE));
    }
}

TEST_CASE("Germanium junction caps add linear stamps without changing transport",
          "[bjt][presets]") {
    using namespace wdf;
    const auto* oc44 = findBjtByName("OC44");
    const auto* withCaps = findBjtByName("OC44-DAFx-Cap");
    REQUIRE(oc44 != nullptr);
    REQUIRE(withCaps != nullptr);
    const auto original = oc44->model.evaluateTerminals(-2.0, -0.15);
    const auto revised = withCaps->model.evaluateTerminals(-2.0, -0.15);
    REQUIRE(original.valid);
    REQUIRE(revised.valid);
    CHECK(original.iC == revised.iC);
    CHECK(original.iB == revised.iB);

    Circuit circuit;
    circuit.addBjt("c", "b", "e", withCaps->model, "Q1");
    REQUIRE(circuit.cap.size() == 2);
    CHECK(circuit.cap[0].a == "b");
    CHECK(circuit.cap[0].b == "e");
    CHECK(circuit.cap[0].value == Approx(410e-12).epsilon(1e-12));
    CHECK(circuit.cap[1].a == "b");
    CHECK(circuit.cap[1].b == "c");
    CHECK(circuit.cap[1].value == Approx(10e-12).epsilon(1e-12));
}

TEST_CASE("MiSTer BJT corrections affect gain and retain an exact Jacobian", "[bjt][presets]") {
    const auto* preset = findBjtByName("2N3904-MiSTer");
    REQUIRE(preset != nullptr);
    const auto& model = preset->model;
    const auto atOneVolt = model.evaluateTerminals(1.0, 0.60);
    const auto atFiveVolts = model.evaluateTerminals(5.0, 0.60);
    REQUIRE(atOneVolt.valid);
    REQUIRE(atFiveVolts.valid);
    CHECK(atFiveVolts.iC > atOneVolt.iC);
    CHECK(atFiveVolts.iB > atFiveVolts.iC / model.Bf);

    constexpr double h = 1e-6;
    const auto minus = model.evaluateTerminals(5.0, 0.60 - h);
    const auto plus = model.evaluateTerminals(5.0, 0.60 + h);
    CHECK(atFiveVolts.diC_dVbe == Approx((plus.iC - minus.iC) / (2.0 * h)).epsilon(1e-7));
    CHECK(atFiveVolts.diB_dVbe == Approx((plus.iB - minus.iB) / (2.0 * h)).epsilon(1e-7));
    const auto collectorMinus = model.evaluateTerminals(5.0 - h, 0.60);
    const auto collectorPlus = model.evaluateTerminals(5.0 + h, 0.60);
    CHECK(atFiveVolts.diC_dVce == Approx((collectorPlus.iC - collectorMinus.iC) / (2.0 * h))
                                     .epsilon(1e-7));
    CHECK_FALSE(findBjtByName("2N3904")->model.hasLowCurrentCorrections());
}

TEST_CASE("Opt-in MiSTer BJT forward roll-off retains exact terminal derivatives",
          "[bjt][ikf]") {
    struct Card {
        const char* unchanged;
        const char* corrected;
        double ikf;
        double forwardDrive;
    };
    for (const Card card : {Card{"2N3904-MiSTer", "2N3904-MiSTer-IKF", 66.78e-3, .78},
                            Card{"2N3906-MiSTer", "2N3906-MiSTer-IKF", 80e-3, .82}}) {
        INFO(card.corrected);
        const auto* original = findBjtByName(card.unchanged);
        const auto* rolled = findBjtByName(card.corrected);
        REQUIRE(original != nullptr);
        REQUIRE(rolled != nullptr);
        CHECK(original->model.Ikf == 0.0);
        CHECK(rolled->model.Ikf == card.ikf);
        const auto& model = rolled->model;
        const double sign = model.pnp ? -1.0 : 1.0;
        const double lightDrive = model.pnp ? .61 : .57;
        const auto light = model.evaluateTerminals(sign, sign * lightDrive);
        const auto originalLight = original->model.evaluateTerminals(sign, sign * lightDrive);
        REQUIRE(light.valid);
        CHECK(light.iC == Approx(originalLight.iC).epsilon(0.001));
        const auto strong = model.evaluateTerminals(sign, sign * card.forwardDrive);
        const auto originalStrong = original->model.evaluateTerminals(sign, sign * card.forwardDrive);
        REQUIRE(strong.valid);
        CHECK(std::abs(strong.iC) < 0.7 * std::abs(originalStrong.iC));
        CHECK(strong.iB == originalStrong.iB);

        constexpr double h = 1e-6;
        for (double vce : {sign, sign * .2}) {
            const double vbe = sign * card.forwardDrive;
            const auto center = model.evaluateTerminals(vce, vbe);
            auto derivative = [&](double dVce, double dVbe, bool baseCurrent) {
                const auto left = model.evaluateTerminals(vce - dVce, vbe - dVbe);
                const auto right = model.evaluateTerminals(vce + dVce, vbe + dVbe);
                return (baseCurrent ? right.iB - left.iB : right.iC - left.iC) / (2 * h);
            };
            CHECK(center.diC_dVce == Approx(derivative(h, 0, false)).epsilon(1e-6).margin(1e-9));
            CHECK(center.diC_dVbe == Approx(derivative(0, h, false)).epsilon(1e-7));
            CHECK(center.diB_dVce == Approx(derivative(h, 0, true)).epsilon(1e-6).margin(1e-9));
            CHECK(center.diB_dVbe == Approx(derivative(0, h, true)).epsilon(1e-7));
        }
    }
}

TEST_CASE("Exported IKF BJT matches native transport at elevated current",
          "[bjt][ikf][ngspice]") {
    if (!lwdf_test::requireNgspice("IKF BJT transport")) return;

    for (const char* name : {"2N3904-MiSTer-IKF", "2N3906-MiSTer-IKF"}) {
        INFO(name);
        const auto& model = findBjtByName(name)->model;
        const double sign = model.pnp ? -1.0 : 1.0;
        const double bias = sign * (model.pnp ? .82 : .78);
        Circuit circuit;
        circuit.addSrc("c", "0", sign, 0, {}, "fixture");
        circuit.addSrc("b", "0", bias, 0, {}, "drive");
        circuit.addBjt("c", "b", "0", model, "tested");
        const auto exported = spiceExport(circuit, 48000, 0);
        REQUIRE(exported.ok);
        REQUIRE(exported.netlist.find("IKF=") != std::string::npos);
        const auto control = exported.netlist.find("\n.control\n");
        REQUIRE(control != std::string::npos);

        const double temperature = model.Vt / 8.617333262e-5 - 273.15;
        const std::string tag = model.pnp ? "wdf_ikf_pnp" : "wdf_ikf_npn";
        std::ostringstream deck;
        deck << exported.netlist.substr(0, control) << '\n' << std::setprecision(15)
             << ".options tnom=" << temperature << " temp=" << temperature
             << " abstol=1e-15 reltol=1e-10 gmin=1e-15\n"
             << lwdf_test::controlBlock({"op"}, {"-i(V_fixture)"}, tag) << ".end\n";
        REQUIRE(lwdf_test::runDeck(deck.str(), tag).ok);
        const auto result = lwdf_test::parseOpFile(lwdf_test::vecFile(tag, 0));
        REQUIRE(result.size() == 1);
        CHECK(result.front() == Approx(model.evaluateTerminals(sign, bias).iC).epsilon(.0005));
    }
}

TEST_CASE("a PNP BJT is the exact mirror of the NPN Ebers-Moll law",
          "[bjt][pnp][equation]") {
    BjtModel npn = findBjtByName("2N3906")->model;  // same card numbers
    npn.pnp = false;
    npn.prepare();
    BjtModel pnp = npn;
    pnp.pnp = true;
    pnp.prepare();

    for (double vce : {-5.0, -1.0, -0.1, 0.0, 0.5, 4.0}) {
        for (double vbe : {-0.8, -0.65, -0.3, 0.0, 0.4, 0.75}) {
            const auto n = npn.evaluateTerminals(vce, vbe);
            const auto p = pnp.evaluateTerminals(-vce, -vbe);
            INFO("vce " << vce << " vbe " << vbe);
            REQUIRE(n.valid);
            REQUIRE(p.valid);
            CHECK(p.iC == Approx(-n.iC).margin(1e-15));
            CHECK(p.iB == Approx(-n.iB).margin(1e-15));
            CHECK(p.iE == Approx(-n.iE).margin(1e-15));
            CHECK(p.diC_dVce == Approx(n.diC_dVce).margin(1e-12));
            CHECK(p.diC_dVbe == Approx(n.diC_dVbe).margin(1e-12));
            CHECK(p.diB_dVce == Approx(n.diB_dVce).margin(1e-12));
            CHECK(p.diB_dVbe == Approx(n.diB_dVbe).margin(1e-12));
        }
    }

    // Junction compensation is applied to the physical junction voltage, not
    // to the NPN-mirrored internal one.
    for (double v : {-0.9, -0.4, 0.0, 0.4, 0.9}) {
        CHECK(pnp.compensateBe(v) == Approx(-npn.compensateBe(-v)).margin(1e-15));
        CHECK(pnp.compensateBc(v) == Approx(-npn.compensateBc(-v)).margin(1e-15));
    }
}

TEST_CASE("a .livewdf PNP card converts with its own polarity",
          "[bjt][pnp][document]") {
    LiveWDFDocument doc;
    SchematicComponent q;
    q.id = "Q1";
    q.type = "bjt";
    q.pins = {{"collector", "c"}, {"base", "b"}, {"emitter", "0"}};
    q.properties = {{"model", "2N3906"}};

    // Loads keep the transistor out of `isolatedParts`; the converter only
    // emits devices that are actually wired into the circuit.
    SchematicComponent rc;
    rc.id = "R1";
    rc.type = "res";
    rc.pins = {{"a", "c"}, {"b", "0"}};
    rc.values = {{"value", 1000.0}};
    SchematicComponent rb;
    rb.id = "R2";
    rb.type = "res";
    rb.pins = {{"a", "b"}, {"b", "0"}};
    rb.values = {{"value", 1000000.0}};
    doc.components = {q, rc, rb};

    const Circuit c = doc.toCircuit();
    REQUIRE(c.bjt.size() == 1);
    CHECK(c.bjt[0].model.pnp);
    CHECK(c.bjt[0].model.Bf == Approx(200.0));

    // An unknown card is still an error, not a silent NPN substitution.
    doc.components.front().properties["model"] = "NotARealBjt";
    REQUIRE_THROWS_AS(doc.toCircuit(), std::runtime_error);
}

TEST_CASE("BJT model basic Ebers-Moll reference", "[bjt][equation]") {
    BjtModel m;
    m.Is = 1e-14; m.Bf = 100.0; m.Br = 1.0; m.Nf = 1.0; m.Nr = 1.0;
    m.prepare();
    auto res = m.evaluateTerminals(5.0, 0.7);
    REQUIRE(res.valid);
    REQUIRE(res.iC > 0.0); // NPN, forward active
    REQUIRE(res.iB > 0.0);
    REQUIRE(std::abs(res.iC + res.iB + res.iE) < 1e-6); // KCL
}

TEST_CASE("AC128 card is a PNP with the fitted transport values",
          "[bjt][ac128][presets]") {
    const BjtEntry* e = findBjtByName("AC128");
    REQUIRE(e != nullptr);
    const BjtModel& m = e->model;
    CHECK(m.pnp);
    CHECK(m.Is == Approx(22.61e-6));
    CHECK(m.Bf == Approx(1000.0));
    CHECK(m.Br == Approx(3.76));
    CHECK(m.Nf == Approx(1.04));
    CHECK(m.Nr == Approx(1.07));
    CHECK(m.valid());

    // Forward-active PNP: collector and base currents are negative.
    const auto q = m.evaluateTerminals(-5.0, -0.1);
    REQUIRE(q.valid);
    CHECK(q.iC < 0.0);
    CHECK(q.iB < 0.0);
    CHECK(q.iE > 0.0);
    CHECK(std::abs(q.iC + q.iB + q.iE) < 1e-15);
}

TEST_CASE("2N5089 preset matches DAFx-23 Table 1 parameters",
          "[bjt][2n5089][presets][dafx23]") {
    const BjtEntry* e = findBjtByName("2N5089");
    REQUIRE(e != nullptr);
    const BjtModel& m = e->model;
    CHECK_FALSE(m.pnp);
    CHECK(m.Is == Approx(5.915e-15).margin(1e-18));
    CHECK(m.Bf == Approx(1427.57).margin(0.1));
    CHECK(m.Br == Approx(1.262).margin(0.01));
    CHECK(m.Nf == Approx(1.0));
    CHECK(m.Nr == Approx(1.0));
    CHECK(m.valid());

    BjtModel prep = m;
    prep.prepare();
    CHECK(prep.alphaF == Approx(0.9993).margin(1e-4));
    CHECK(prep.alphaR == Approx(0.5579).margin(1e-4));

    // Evaluates correctly in forward-active region
    const auto res = prep.evaluateTerminals(5.0, 0.65);
    REQUIRE(res.valid);
    CHECK(res.iC > 0.0);
    CHECK(res.iB > 0.0);
    CHECK(res.iE < 0.0);
}

TEST_CASE("DAFx-23 logarithmic step damping and MNR compensation",
          "[bjt][damping][mnr][dafx23]") {
    const BjtEntry* e = findBjtByName("2N3904");
    REQUIRE(e != nullptr);
    BjtModel m = e->model;
    m.prepare();

    const double nVt = m.nfVt;
    const double limit = 2.0 * nVt;

    // 1. Small step is untouched (linear region)
    CHECK(BjtModel::dampStep(0.01, nVt) == Approx(0.01).margin(1e-15));
    CHECK(BjtModel::dampStep(-0.01, nVt) == Approx(-0.01).margin(1e-15));
    CHECK(BjtModel::dampStep(limit, nVt) == Approx(limit).margin(1e-15));
    CHECK(BjtModel::dampStep(-limit, nVt) == Approx(-limit).margin(1e-15));

    // 2. Symmetry: dampStep(-x) == -dampStep(x)
    for (double step : {0.05, 0.1, 0.5, 1.0, 5.0, 10.0}) {
        CHECK(BjtModel::dampStep(-step, nVt) == Approx(-BjtModel::dampStep(step, nVt)).margin(1e-15));
    }

    // 3. Continuity at limit: step just above limit matches closely
    const double eps = 1e-7;
    CHECK(BjtModel::dampStep(limit + eps, nVt) == Approx(limit).margin(1e-6));

    // 4. Large wild step (e.g. 5V) is safely compressed
    const double damped5V = BjtModel::dampStep(5.0, nVt);
    CHECK(damped5V < 0.25); // Well within safe range (< 250 mV)
    CHECK(damped5V > limit);

    // 5. applyMNRStep preserves small linear steps
    double vceNext = 0.0, vbeNext = 0.0;
    m.applyMNRStep(4.0, 0.6, 0.01, 0.005, vceNext, vbeNext);
    CHECK(vbeNext == Approx(0.605).margin(1e-9));
    CHECK(vceNext == Approx(4.01).margin(1e-9));

    // 6. applyMNRStep under hard overdrive (e.g. proposed +2V step) prevents explosion
    double vceNextHard = 0.0, vbeNextHard = 0.0;
    m.applyMNRStep(4.0, 0.65, -1.0, 2.0, vceNextHard, vbeNextHard);
    CHECK(vbeNextHard < 1.0); // Never exceeds 1V exponential boundary
    CHECK(vbeNextHard > 0.65);
    CHECK(std::isfinite(vceNextHard));
    CHECK(std::isfinite(vbeNextHard));

    // Evaluation at damped point is finite and valid
    const auto cur = m.evaluateTerminals(vceNextHard, vbeNextHard);
    CHECK(cur.valid);
    CHECK(std::isfinite(cur.iC));
    CHECK(std::isfinite(cur.iB));
}
