#pragma once

#include <array>
#include <cstddef>
#include <vector>
#include "circuit/RootLayout.h"
#include "devices/Diode.h"
#include "devices/Pentode.h"
#include "devices/Triode.h"
#include "spice/Dense.h"

namespace wdf {

enum class DeviceKind {
    Triode,
    Diode,
    ControlledRes,
    Pentode,
    Jfet,
    Mosfet,
    Bjt,
    OpAmp
};

struct RootDevice {
    DeviceKind kind = DeviceKind::Triode;
    int portBase = 0;
    int numPorts = 0;
    int activeRowBase = 0;
    int numActiveRows = 0;
    size_t compIndex = 0;
};

inline DeviceKind deviceKind(RootKind kind) noexcept {
    switch (kind) {
    case RootKind::Triode: return DeviceKind::Triode;
    case RootKind::Diode: return DeviceKind::Diode;
    case RootKind::ControlledRes: return DeviceKind::ControlledRes;
    case RootKind::Pentode: return DeviceKind::Pentode;
    case RootKind::Jfet: return DeviceKind::Jfet;
    case RootKind::Mosfet: return DeviceKind::Mosfet;
    case RootKind::Bjt: return DeviceKind::Bjt;
    case RootKind::OpAmp: return DeviceKind::OpAmp;
    }
    return DeviceKind::Triode;
}

inline int activePortCount(const RootDevice& dev) noexcept {
    switch (dev.kind) {
    case DeviceKind::Triode:
        return dev.numActiveRows;
    case DeviceKind::Diode:
    case DeviceKind::ControlledRes:
    case DeviceKind::Jfet:
    case DeviceKind::Mosfet:
    case DeviceKind::OpAmp:
        return 1;
    case DeviceKind::Bjt:
        return 2;
    case DeviceKind::Pentode:
        return 3;
    default:
        return 0;
    }
}

inline int activePort(const RootDevice& dev, int rr) noexcept {
    return dev.portBase + (dev.kind == DeviceKind::Triode ? 2 * rr : rr);
}

struct JunctionLimiter {
    int port = 0;
    const DiodeModel* model = nullptr;
    // PNP junctions conduct when the port voltage is negative. The diode
    // limiter is written for a positive junction voltage, so the caller
    // mirrors the port into the limiter and mirrors the result back.
    bool negate = false;
};

/// A pentode or triode grid is a conducting junction whose Newton steps
/// overshoot without logarithmic damping (`pnjlim`). This holds the device
/// and calls `limitGrid` on it.
struct GridJunctionLimiter {
    int port = 0;
    const Pentode* pentode = nullptr;
    const TriodeModel* triode = nullptr;

    double limitGrid(double vold, double vnew, bool& limited) const noexcept {
        if (pentode)
            return pentode->limitGrid(vold, vnew, limited);
        if (triode)
            return triode->limitGrid(vold, vnew, limited);
        return vnew;
    }
};

/// Predecessor port coupling for fast closed-form and block folding.
struct PredecessorPort {
    int col = 0;
    double z = 0.0;
};

struct NewtonBlock {
    std::vector<int> devices;             ///< device-group indices, in root-device order
    std::vector<int> ports;               ///< live root ports, one per active row
    std::vector<int> portDevice;          ///< device group that owns each port
    bool isAntiParallelDiodePair = false; ///< two reversed diode ports solved as one scalar equation
    AntiParallelDiodeModel diodePair;
    std::vector<double> blockZ;           ///< Pre-gathered submatrix of Z: totalDevicePorts x n
    std::vector<std::vector<PredecessorPort>> predFold; ///< Per local device port: non-zero predecessor couplings
    std::vector<std::vector<PredecessorPort>> allFold;  ///< Per local device port: non-zero couplings from all other blocks
    std::vector<int> laterGrid;           ///< Grid columns of later blocks that `predFold` reads

    // ---- General linear/nonlinear decomposition -------------------------
    //
    // A block is one strongly-connected set of root devices.  Within that set,
    // devices whose port laws are affine in the port voltages (op-amps in their
    // linear region, controlled resistors) do not need to be Newton unknowns:
    // their currents can be eliminated algebraically.  What remains is a
    // smaller nonlinear root over `reducedPorts`.
    //
    // This is the same Schur elimination the anti-parallel pair performs by
    // hand, but for an arbitrary mix of linear devices and nonlinear devices.
    // `ports` still lists every active port: it is the block's interface to the
    // rest of the network (folding, residual checks, node reconstruction).  The
    // reduced system is an internal solve detail.
    bool linearReduction = false;
    unsigned linearSlopeMask = ~0u;
    std::vector<NewtonBlock> alternateLinearSlopes;
    std::vector<int> linearDevices;      ///< device indices eliminated algebraically
    std::vector<int> linearPorts;        ///< their active ports, in linearDevices order
    std::vector<double> linearG;         ///< nominal d(current)/d(active voltage) per linear device
    std::vector<double> linearA;         ///< mL x mL: I + G*Z on the linear columns
    std::vector<double> linearLU;
    std::vector<int> linearPivots;
    std::vector<double> linearALN;       ///< mL x nN: G*Z from linear rows to nonlinear columns
    std::vector<double> linearK;         ///< mL x nN: i_L = K0 + linearK * i_N

    std::vector<int> reducedDevices;     ///< nonlinear device indices, in solve order
    std::vector<int> reducedPorts;       ///< nonlinear active ports, the reduced unknowns
    std::vector<int> reducedRowPorts;    ///< every port of every reduced device, in row order
    std::vector<int> reducedRowOfPort;   ///< root port -> reduced row, or -1
    std::vector<double> reducedZ;        ///< reducedRowPorts.size() x nN

    bool reducedAntiParallel = false;    ///< two reversed diode ports in the reduced core
    AntiParallelDiodeModel reducedPair;
    bool reducedScalarDiode = false;     ///< one-diode reduced block; absorb Z' into its Rs
    int reducedScalarPort = -1;
    DiodeModel reducedScalarModel;

    void clearLinearReduction() {
        linearReduction = false;
        linearSlopeMask = ~0u;
        alternateLinearSlopes.clear();
        linearDevices.clear();
        linearPorts.clear();
        linearG.clear();
        linearA.clear();
        linearALN.clear();
        linearLU.clear();
        linearPivots.clear();
        linearK.clear();
        reducedDevices.clear();
        reducedPorts.clear();
        reducedRowPorts.clear();
        reducedRowOfPort.clear();
        reducedZ.clear();
        reducedAntiParallel = false;
        reducedScalarDiode = false;
        reducedScalarPort = -1;
    }
};

/// A singleton block with a scalar closed form: one live port, or a triode's
/// active rows.
inline bool isScalarBlock(const NewtonBlock& block, const RootDevice* devices) noexcept {
    if (block.devices.size() != 1)
        return false;
    const RootDevice& dev = devices[block.devices[0]];
    return static_cast<int>(block.ports.size()) == (dev.kind == DeviceKind::Triode ? activePortCount(dev) : 1);
}

/// One precomputed runtime partition. The vector-of-blocks is built at
/// topology time, so selecting a partition in the audio loop is a pointer
/// assignment, not a graph traversal.
struct RuntimePartition {
    std::vector<NewtonBlock> blocks;
    std::vector<int> order;
    int count = 0;
};

/// A block list plus the order it is solved in.
struct BlockView {
    const std::vector<NewtonBlock>* blocks = nullptr;
    const std::vector<int>* order = nullptr;
    int count = 0;
};

/// High-resolution diagnostic timestamps for the nonlinear solve.
struct PhaseProfile {
    double offsets = 0.0;    // eoff/roff/b: affine source maps
    double network = 0.0;    // u = Xe*(b-eoff), vOc
    double solve = 0.0;      // whole root dispatch; device/jacobian/lu/line are inside
    double predict = 0.0;    // extrapolated warm start
    double device = 0.0;     // currentsInto + residualInto, fused
    double jacobian = 0.0;   // reducedAssemble
    double lu = 0.0;         // reducedFinish: solve + step expansion
    double linesearch = 0.0; // halving trials, including their evals
    double post = 0.0;       // node reconstruction + capacitor states
    long long samples = 0;

    double total() const noexcept {
        return offsets + network + predict + device + jacobian + lu +
               linesearch + post;
    }
};

inline double acrossVec(const std::array<int, 2>& p, const std::vector<double>& v) noexcept {
    return (p[0] >= 0 ? v[static_cast<size_t>(p[0])] : 0.0) -
           (p[1] >= 0 ? v[static_cast<size_t>(p[1])] : 0.0);
}

inline double acrossMat(const std::array<int, 2>& p, const Dense& M, int c) noexcept {
    return (p[0] >= 0 ? M[p[0]][c] : 0.0) - (p[1] >= 0 ? M[p[1]][c] : 0.0);
}

inline double acrossMat(const std::array<int, 2>& p, const double* M, int c, int K) noexcept {
    return (p[0] >= 0 ? M[static_cast<size_t>(p[0]) * K + c] : 0.0) -
           (p[1] >= 0 ? M[static_cast<size_t>(p[1]) * K + c] : 0.0);
}

} // namespace wdf
