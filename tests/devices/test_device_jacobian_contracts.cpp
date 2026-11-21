#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include "devices/Bjt.h"
#include "devices/Jfet.h"
#include "devices/Mosfet.h"
#include "devices/OpAmp.h"
#include "devices/Triode.h"
#include "support/CircuitCases.h"

// Property contract for the non-pentode device Jacobians. Deterministic
// operating points inside each model's smooth domain, central differences with
// a magnitude-scaled step. Piecewise boundaries are deliberately avoided here:
// continuity at a boundary needs its own dedicated test.

namespace {
template <class F>
void checkDerivative(double analytic, double x, double h, F current, const char* what) {
    const double fd = (current(x + h) - current(x - h)) / (2.0 * h);
    INFO(what << " x=" << x << " analytic=" << analytic << " fd=" << fd);
    REQUIRE(std::isfinite(fd));
    REQUIRE(std::abs(fd - analytic) <= 2e-4 * (1.0 + std::abs(analytic)));
}
}  // namespace

TEST_CASE("triode quadric Jacobian matches central differences", "[devicejac]") {
    const auto m = lwdf_test::gridlessQuadric();
    for (double vgk : {-1.0, -0.3, 0.2}) {
        for (double vpk : {50.0, 150.0, 250.0}) {
            const auto c = m.evaluateQuadric(vgk, vpk);
            const double hv = 1e-4 * std::max(1.0, std::abs(vgk));
            const double hp = 1e-4 * std::max(1.0, std::abs(vpk));
            checkDerivative(c.dvgk, vgk, hv,
                            [&](double v) { return m.evaluateQuadric(v, vpk).ip; }, "triode dIp/dVgk");
            checkDerivative(c.dvpk, vpk, hp,
                            [&](double v) { return m.evaluateQuadric(vgk, v).ip; }, "triode dIp/dVpk");
        }
    }
}

TEST_CASE("JFET Jacobian matches central differences", "[devicejac]") {
    const auto& m = wdf::k2N5952;
    for (double vgs : {-1.0, -0.5, -0.1}) {
        for (double vds : {0.5, 2.0, 5.0}) {
            const auto e = m.evaluate(vgs, vds);
            REQUIRE(e.valid);
            const double hg = 1e-4 * std::max(1.0, std::abs(vgs));
            const double hd = 1e-4 * std::max(1.0, std::abs(vds));
            checkDerivative(e.did_gs, vgs, hg,
                            [&](double v) { return m.evaluate(v, vds).id; }, "jfet dId/dVgs");
            checkDerivative(e.did_ds, vds, hd,
                            [&](double v) { return m.evaluate(vgs, v).id; }, "jfet dId/dVds");
        }
    }
}

TEST_CASE("MOSFET Jacobian matches central differences", "[devicejac]") {
    const auto m = wdf::MosfetModel::fromK(0.02, 1.5, 0.01, false);
    for (double vgs : {2.0, 3.0, 5.0}) {
        for (double vds : {0.5, 2.0, 4.0}) {
            const auto e = m.evaluate(vgs, vds);
            REQUIRE(e.valid);
            const double hg = 1e-4 * std::max(1.0, std::abs(vgs));
            const double hd = 1e-4 * std::max(1.0, std::abs(vds));
            checkDerivative(e.did_gs, vgs, hg,
                            [&](double v) { return m.evaluate(v, vds).id; }, "mosfet dId/dVgs");
            checkDerivative(e.did_ds, vds, hd,
                            [&](double v) { return m.evaluate(vgs, v).id; }, "mosfet dId/dVds");
        }
    }
}

TEST_CASE("BJT transport Jacobian matches central differences", "[devicejac]") {
    const auto* card = wdf::findBjtByName("2N3904");
    REQUIRE(card != nullptr);
    const auto& m = card->model;
    for (double vbe : {0.55, 0.65, 0.72}) {
        for (double vbc : {-2.0, -0.5, 0.0}) {
            const auto e = m.evaluateEM(vbe, vbc);
            REQUIRE(e.valid);
            const double hb = 1e-5;
            const double hc = 1e-5;
            checkDerivative(e.dib_dvbe, vbe, hb,
                            [&](double v) { return m.evaluateEM(v, vbc).ib; }, "bjt dIb/dVbe");
            checkDerivative(e.dib_dvbc, vbc, hc,
                            [&](double v) { return m.evaluateEM(vbe, v).ib; }, "bjt dIb/dVbc");
            checkDerivative(e.dic_dvbe, vbe, hb,
                            [&](double v) { return m.evaluateEM(v, vbc).ic; }, "bjt dIc/dVbe");
            checkDerivative(e.dic_dvbc, vbc, hc,
                            [&](double v) { return m.evaluateEM(vbe, v).ic; }, "bjt dIc/dVbc");
        }
    }
}

TEST_CASE("op-amp transient source derivative matches central differences", "[devicejac]") {
    const wdf::OpAmpModel m(wdf::OpAmpType::Tl072);
    const double alpha = 0.02;
    const double vPrev = 1.0;
    const double maxDv = std::numeric_limits<double>::infinity();
    auto source = [&](double vDiff, double* d = nullptr) {
        return wdf::OpAmpEngineLaw::sourceVoltage(m, vDiff, vPrev, alpha, maxDv, false, d);
    };
    for (double vDiff : {-1e-4, 0.0, 3e-4}) {
        double d = 0.0;
        const double v = source(vDiff, &d);
        REQUIRE(std::isfinite(v));
        const double h = 1e-7;
        const double fd = (source(vDiff + h) - source(vDiff - h)) / (2.0 * h);
        INFO("vDiff=" << vDiff << " d=" << d << " fd=" << fd);
        REQUIRE(std::abs(fd - d) <= 1e-4 * (1.0 + std::abs(d)));
    }
}
