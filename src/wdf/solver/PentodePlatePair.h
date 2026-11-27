#pragma once

#include "devices/Pentode.h"
#include "devices/PolynomialRoots.h"

#include <algorithm>
#include <cmath>

namespace wdf::solver {

// At fixed control-grid and screen voltages, a quadric plate has
// Ip(v) = v * (conductance + saturation / (v + knee)) on its conducting branch.
struct DerkDPlate {
    double knee, conductance, saturation, screenBase;

    bool at(const Pentode& model, const double* voltage, double screenCurrent) noexcept {
        const auto& m = model.reefman;
        const auto q = m.split();
        if (model.modelKind != PentodeModelKind::Reefman || !model.useReefman ||
            (!m.quadric && (m.knee != PentodeKneeType::DerkD || m.secondaryEmission)) ||
            !(q.beta > 0. && q.plate >= 0. && q.lambda >= 0. && q.rho > 0. && q.sigma >= 0. &&
              m.count > 0. && m.isSupported(voltage[1], voltage[2], 0.)))
            return false;
        double plate = voltage[0];
        if (plate < 0.) {
            // Reverse cutoff zeros both terminal currents. Recover emission
            // from the same device at the edge of its conducting branch.
            double grid, current;
            plate = 0.;
            m.evalCurrents(voltage[1], voltage[2], plate, grid, screenCurrent, current);
        }
        knee = 1. / q.beta;
        screenBase = screenCurrent / q.screenShare(plate);
        conductance = screenBase * q.lambda;
        saturation = screenBase * q.plate;
        return knee > 0. && std::isfinite(knee) && conductance >= 0. && std::isfinite(conductance) &&
               saturation >= 0. && std::isfinite(saturation) && std::isfinite(screenBase);
    }

    double current(double v) const noexcept {
        return v <= 0. ? 0. : v * (conductance + saturation / (knee + v));
    }

    double isolatedVoltage(double drive, double resistance) const noexcept {
        if (drive <= 0.)
            return drive;
        const double a = 1. + resistance * conductance;
        const double b = a * knee + resistance * saturation - drive;
        const double c = knee * drive;
        const double discriminant = std::sqrt(b * b + 4. * a * c);
        return b > 0. ? 2. * c / (b + discriminant) : (discriminant - b) / (2. * a);
    }
};

inline bool solvePentodePlatePair(const DerkDPlate& left, const DerkDPlate& right,
                                  double drive1, double drive2, double z11, double z22, double mutual,
                                  double& plate1, double& plate2) noexcept {
    const double determinant = z11 * z22 - mutual * mutual;
    if (!(z11 > 0. && z22 > 0. && mutual != 0. && determinant > 0.) ||
        !std::isfinite(determinant) || !std::isfinite(drive1) || !std::isfinite(drive2))
        return false;
    const auto satisfiesLoad = [&] {
        const double i1 = left.current(plate1), i2 = right.current(plate2);
        return std::abs(plate1 - drive1 + z11 * i1 + mutual * i2) < 1e-6 &&
               std::abs(plate2 - drive2 + mutual * i1 + z22 * i2) < 1e-6;
    };
    // Positive definite load and nondecreasing plate currents give a unique
    // solution. If either plate cuts off, one scalar quadratic suffices.
    plate1 = left.isolatedVoltage(drive1, z11);
    plate2 = drive2 - mutual * left.current(plate1);
    if (plate2 <= 0.)
        return satisfiesLoad();
    plate2 = right.isolatedVoltage(drive2, z22);
    plate1 = drive1 - mutual * right.current(plate2);
    // A plate rounded just above zero is still at cutoff when the load holds.
    if (plate1 <= 0. || satisfiesLoad())
        return satisfiesLoad();

    // In admittance coordinates with u = Vp1 + knee1 and v = Vp2 + knee2,
    // each conducting plate contributes a conic:
    //   A*u^2 + B*u + D + k*u*v = 0,   E*v^2 + F*v + H + k*u*v = 0.
    // Substituting v = -(A*u+B+D/u)/k in the second gives the monic quartic.
    const double y11 = z22 / determinant, y22 = z11 / determinant, k = -mutual / determinant;
    const double A = y11 + left.conductance, E = y22 + right.conductance;
    const double B = left.saturation - left.conductance * left.knee -
                     y11 * (drive1 + left.knee) - k * (drive2 + right.knee);
    const double F = right.saturation - right.conductance * right.knee -
                     y22 * (drive2 + right.knee) - k * (drive1 + left.knee);
    const double D = -left.saturation * left.knee, H = -right.saturation * right.knee;
    const double leading = A * (E * A - k * k);
    const double a = (2. * E * A * B - F * k * A - k * k * B) / leading;
    const double b = (E * (B * B + 2. * A * D) - F * k * B - k * k * D + H * k * k) / leading;
    const double c = (2. * E * B * D - F * k * D) / leading;
    const double d = E * D * D / leading;

    // Ferrari's resolvent for the depressed quartic. Scale the resolvent
    // variable so voltage powers do not mimic a degenerate leading term.
    const double a2 = a * a;
    const double p = b - 3. * a2 / 8.;
    const double q = c - a * b / 2. + a2 * a / 8.;
    const double r = d - a * c / 4. + a2 * b / 16. - 3. * a2 * a2 / 256.;
    const double scale = std::max({1., std::abs(p), std::sqrt(std::abs(r))});
    const double ps = p / scale, qs = q / (scale * std::sqrt(scale)), rs = r / (scale * scale);
    double roots[3];
    const int count = poly::realCubicRoots(1., 2. * ps, ps * ps - 4. * rs, -qs * qs, roots);
    if (!count)
        return false;
    const double z = scale * *std::max_element(roots, roots + count);
    if (!(z > 0.))
        return false;
    const double m = std::sqrt(z);
    for (double sign : {1., -1.}) {
        const double discriminant = -(2. * p + z + sign * 2. * q / m);
        if (!(discriminant >= 0.))
            continue;
        const double squareRoot = std::sqrt(discriminant);
        for (double u : {(sign * m + squareRoot) / 2. - a / 4., (sign * m - squareRoot) / 2. - a / 4.}) {
            if (!(u >= left.knee))
                continue;
            const double v = -(A * u + B + D / u) / k;
            if (!(v >= right.knee))
                continue;
            plate1 = u - left.knee;
            plate2 = v - right.knee;
            if (satisfiesLoad())
                return true;
        }
    }
    return false;
}

} // namespace wdf::solver
