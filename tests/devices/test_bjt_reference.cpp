#include <catch2.hpp>
#include <cmath>
#include "devices/Bjt.h"

namespace {

// Independent evaluation of the SPICE-compatible Ebers-Moll transport form.
// This intentionally does not call any BjtModel helper other than the public
// parameter fields, so it cannot agree with a sign or scaling mistake in
// `evaluate`.
struct ReferenceCurrents {
    double iC = 0.0;
    double iB = 0.0;
    double iE = 0.0;
    double diC_dVce = 0.0;
    double diC_dVbe = 0.0;
    double diB_dVce = 0.0;
    double diB_dVbe = 0.0;
};

ReferenceCurrents referenceTerminals(const wdf::BjtModel& m, double vce, double vbe) {
    const double vbc = vbe - vce;
    const double eBe = std::exp(vbe / m.nfVt);
    const double eBc = std::exp(vbc / m.nrVt);
    const double iF = m.Is * (eBe - 1.0);
    const double iR = m.Is * (eBc - 1.0);
    const double gBe = m.Is * eBe / m.nfVt;
    const double gBc = m.Is * eBc / m.nrVt;

    ReferenceCurrents out;
    out.iB = iF / m.Bf + iR / m.Br;
    out.iC = iF - iR / m.alphaR;
    out.iE = -(out.iC + out.iB);

    // iC(vce, vbe) with vbc = vbe - vce
    out.diC_dVce = gBc / m.alphaR;
    out.diC_dVbe = gBe - gBc / m.alphaR;
    out.diB_dVce = -gBc / m.Br;
    out.diB_dVbe = gBe / m.Bf + gBc / m.Br;
    return out;
}

}  // namespace

TEST_CASE("BJT device independent reference", "[bjt][equation][independent]") {
    using namespace wdf;
    const BjtEntry* e = findBjtByName("2N3904");
    REQUIRE(e != nullptr);
    const BjtModel& m = e->model;
    REQUIRE(m.valid());

    // Representative forward-active point: vBE=0.65, vCE=2.0 => vBC=-1.35.
    const double vbe = 0.65;
    const double vce = 2.0;
    const auto cur = m.evaluateTerminals(vce, vbe);
    const auto ref = referenceTerminals(m, vce, vbe);
    REQUIRE(cur.valid);
    REQUIRE((std::isfinite(cur.iC) && std::isfinite(cur.iB) && std::isfinite(cur.iE)));

    REQUIRE(cur.iC == Approx(ref.iC).epsilon(1e-4).margin(1e-6));
    REQUIRE(cur.iB == Approx(ref.iB).epsilon(1e-4).margin(1e-6));
    REQUIRE(cur.iE == Approx(ref.iE).epsilon(1e-4).margin(1e-6));

    // Current balance.
    REQUIRE((std::abs(cur.iE - (-(cur.iC + cur.iB))) < 1e-12));

    // The terminal-current gain must be BF, not alphaF. This is the regression
    // for the transport-form bug where IS was scaled by alphaF as well.
    REQUIRE(cur.iC > 0.0);
    REQUIRE(cur.iB > 0.0);
    REQUIRE(cur.iC / cur.iB == Approx(m.Bf).epsilon(1e-6));
    REQUIRE(cur.iE < 0.0);  // current leaving emitter

    // Analytic Jacobian against a central finite difference at the same point.
    const double h = 1e-6;
    const auto curVceP = m.evaluateTerminals(vce + h, vbe);
    const auto curVceM = m.evaluateTerminals(vce - h, vbe);
    const auto curVbeP = m.evaluateTerminals(vce, vbe + h);
    const auto curVbeM = m.evaluateTerminals(vce, vbe - h);
    const double fd_diC_dVce = (curVceP.iC - curVceM.iC) / (2.0 * h);
    const double fd_diC_dVbe = (curVbeP.iC - curVbeM.iC) / (2.0 * h);
    const double fd_diB_dVce = (curVceP.iB - curVceM.iB) / (2.0 * h);
    const double fd_diB_dVbe = (curVbeP.iB - curVbeM.iB) / (2.0 * h);

    REQUIRE(cur.diC_dVce == Approx(fd_diC_dVce).margin(std::abs(fd_diC_dVce) * 1e-1 + 1e-3));
    REQUIRE(cur.diC_dVbe == Approx(fd_diC_dVbe).margin(std::abs(fd_diC_dVbe) * 1e-1 + 1e-3));
    REQUIRE(cur.diB_dVce == Approx(fd_diB_dVce).margin(std::abs(fd_diB_dVce) * 1e-1 + 1e-3));
    REQUIRE(cur.diB_dVbe == Approx(fd_diB_dVbe).margin(std::abs(fd_diB_dVbe) * 1e-1 + 1e-3));

    REQUIRE(cur.diC_dVce == Approx(ref.diC_dVce).epsilon(1e-4).margin(1e-6));
    REQUIRE(cur.diC_dVbe == Approx(ref.diC_dVbe).epsilon(1e-4).margin(1e-6));
    REQUIRE(cur.diB_dVce == Approx(ref.diB_dVce).epsilon(1e-4).margin(1e-6));
    REQUIRE(cur.diB_dVbe == Approx(ref.diB_dVbe).epsilon(1e-4).margin(1e-6));
}

TEST_CASE("PNP device currents match the mirrored independent reference",
          "[bjt][equation][independent][pnp]") {
    using namespace wdf;
    const BjtEntry* e = findBjtByName("2N3906");
    REQUIRE(e != nullptr);
    const BjtModel& m = e->model;
    REQUIRE(m.pnp);

    // Forward-active PNP: vBE = -0.65, vCE = -2.0. The independent reference is
    // the NPN Ebers-Moll form evaluated at the mirrored bias and negated. This
    // is the same transform the device implements, but it does not call the
    // device's `evaluateTerminals`, so a sign error cannot agree with itself.
    const double vbe = -0.65;
    const double vce = -2.0;
    const auto cur = m.evaluateTerminals(vce, vbe);
    const auto refN = referenceTerminals(m, -vce, -vbe);
    REQUIRE(cur.valid);
    CHECK(cur.iC == Approx(-refN.iC).epsilon(1e-4).margin(1e-6));
    CHECK(cur.iB == Approx(-refN.iB).epsilon(1e-4).margin(1e-6));
    CHECK(cur.iE == Approx(-refN.iE).epsilon(1e-4).margin(1e-6));
    CHECK(cur.iC < 0.0);
    CHECK(cur.iB < 0.0);
    CHECK(cur.iE > 0.0);
    CHECK(cur.iC / cur.iB == Approx(m.Bf).epsilon(1e-6));

    const double h = 1e-6;
    const auto curVceP = m.evaluateTerminals(vce + h, vbe);
    const auto curVceM = m.evaluateTerminals(vce - h, vbe);
    const auto curVbeP = m.evaluateTerminals(vce, vbe + h);
    const auto curVbeM = m.evaluateTerminals(vce, vbe - h);
    const double fd_diC_dVce = (curVceP.iC - curVceM.iC) / (2.0 * h);
    const double fd_diC_dVbe = (curVbeP.iC - curVbeM.iC) / (2.0 * h);
    const double fd_diB_dVce = (curVceP.iB - curVceM.iB) / (2.0 * h);
    const double fd_diB_dVbe = (curVbeP.iB - curVbeM.iB) / (2.0 * h);
    CHECK(cur.diC_dVce == Approx(fd_diC_dVce).margin(std::abs(fd_diC_dVce) * 1e-1 + 1e-3));
    CHECK(cur.diC_dVbe == Approx(fd_diC_dVbe).margin(std::abs(fd_diC_dVbe) * 1e-1 + 1e-3));
    CHECK(cur.diB_dVce == Approx(fd_diB_dVce).margin(std::abs(fd_diB_dVce) * 1e-1 + 1e-3));
    CHECK(cur.diB_dVbe == Approx(fd_diB_dVbe).margin(std::abs(fd_diB_dVbe) * 1e-1 + 1e-3));
}
