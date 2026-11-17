#include <catch2.hpp>
#include "devices/Bjt.h"
#include "format/LiveWDFFile.h"
#include <cmath>

using namespace wdf;

TEST_CASE("BJT presets valid and evaluate at representative points", "[bjt][presets]") {
    const char* names[] = {"2N3904","2N2222A","2N2222","BC547B","2N5088",
                           "2N3906","2N2907","BC557B","2N5087"};
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

TEST_CASE("AC128 card is an Ebers-Moll PNP with the extracted transport values",
          "[bjt][ac128][presets]") {
    const BjtEntry* e = findBjtByName("AC128");
    REQUIRE(e != nullptr);
    const BjtModel& m = e->model;
    CHECK(m.pnp);
    CHECK(m.Is == Approx(20.66e-6));
    CHECK(m.Bf == Approx(229.6));
    CHECK(m.Br == Approx(14.66));
    CHECK(m.Nf == Approx(1.133));
    CHECK(m.Nr == Approx(1.140));
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
