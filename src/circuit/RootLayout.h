#pragma once

// The root-port layout, in one place.
//
// A root device owns a contiguous run of R-type ports, and only some of those
// ports are independent unknowns in the reduced Newton system. Before this
// header those two facts were spelled out separately in `Network::buildNetwork`,
// `EngineCore::allocateBuffers`, `EngineCompile::solveDc` and `EngineSolve`'s
// group helpers. A new root device therefore had to be added in four places,
// and the compiler could not tell us when one of them was missed.
//
// `buildRootLayout` is the single description. It is deliberately data, not a
// solver: the device laws and the group solve still live in the engine, but the
// port order, the active rows/columns and the reduced-system mapping do not.
//
// Order is part of the contract because the existing closed forms and tests
// address ports by offset. It is:
//
//   triodes (3 ports each: plate, external grid, internal grid)
//   diodes (1 port each)
//   controlled resistors (1 port each)
//   pentodes (3 ports each: plate, control grid, screen (passive))
//   JFETs (2 ports each: drain-source, gate-source)
//   MOSFETs (2 ports each: drain-source, gate-source)
//
// New devices are appended, never inserted, so an existing circuit's offsets do
// not move.

#include <array>
#include <cstddef>
#include <vector>

#include "spice/Compiled.h"

namespace wdf {

enum class RootKind {
    Triode,
    Diode,
    ControlledRes,
    Pentode,
    Jfet,
    Mosfet,
    Bjt,
    OpAmp
};

struct RootDeviceLayout {
    RootKind kind = RootKind::Triode;
    int compIndex = 0;      // index into the matching `Compiled` vector
    int portBase = 0;       // first root port
    int numPorts = 0;       // ports owned by this device
    int activeRowBase = 0;  // first reduced row/column
    int numActiveRows = 0;  // independent unknowns
    // Port offsets (relative to `portBase`) that carry the active rows. Only
    // the first `numActiveRows` entries are meaningful. The same offsets are
    // used for columns because every current root device is symmetric in that
    // respect; a future device that is not can grow this into two arrays.
    std::array<int, 3> activePorts{0, 0, 0};
};

struct RootLayout {
    std::vector<RootDeviceLayout> devices;
    std::vector<Port> ports;  // `ports[portBase + q]`, in root order
    int K = 0;                // number of device root ports
    int nRed = 0;             // size of the reduced Newton system
    std::vector<int> redCol;  // reduced column -> root port
    std::vector<int> redRow;  // root port -> reduced row, or -1
    // Root ports whose currents physically enter the linear network and hence
    // belong in node reconstruction. This is deliberately not `redCol`: the
    // BJT's e0 port is dependent (`iE = -(iC + iB)`) already represented by
    // the ce/be returns.
    std::vector<int> nodeCol;
};

inline RootLayout buildRootLayout(const Compiled& c) {
    RootLayout layout;

    auto add = [&](RootKind kind, int compIndex, const Port* ports, int numPorts,
                   int numActiveRows, std::array<int, 3> activePorts) {
        RootDeviceLayout dev;
        dev.kind = kind;
        dev.compIndex = compIndex;
        dev.portBase = layout.K;
        dev.numPorts = numPorts;
        dev.activeRowBase = layout.nRed;
        dev.numActiveRows = numActiveRows;
        dev.activePorts = activePorts;
        layout.devices.push_back(dev);

        for (int q = 0; q < numPorts; ++q)
            layout.ports.push_back(ports[q]);

        layout.K += numPorts;
        layout.nRed += numActiveRows;
    };

    for (int t = 0; t < static_cast<int>(c.tri.size()); ++t) {
        const auto& ct = c.tri[static_cast<size_t>(t)];
        const Port ports[3] = {ct.pk, ct.gk, ct.gik};
        // The plate conducts, and the internal grid too when grid current is
        // on; the external grid is passive.
        add(RootKind::Triode, t, ports, 3, ct.model.gridConducts ? 2 : 1, {0, 2, 0});
    }

    for (int q = 0; q < static_cast<int>(c.diode.size()); ++q) {
        const Port ports[1] = {c.diode[static_cast<size_t>(q)].first};
        add(RootKind::Diode, q, ports, 1, 1, {0, 0, 0});
    }

    for (int q = 0; q < static_cast<int>(c.ctrlRes.size()); ++q) {
        const Port ports[1] = {c.ctrlRes[static_cast<size_t>(q)].first};
        add(RootKind::ControlledRes, q, ports, 1, 1, {0, 0, 0});
    }

    for (int q = 0; q < static_cast<int>(c.pent.size()); ++q) {
        const auto& cp = c.pent[static_cast<size_t>(q)];
        // The imported pedalkernel three-port law references the screen voltage
        // through the model table (`vg2`), not through a port: its screen current
        // and screen derivatives are identically zero, so the screen port is not
        // an unknown. The active rows are the contiguous pair (plate, control
        // grid), following the precedent of the triode's passive grid ports.
        const Port ports[3] = {cp.pk, cp.gk, cp.sk};
        add(RootKind::Pentode, q, ports, 3, 2, {0, 1, 0});
    }

    for (int q = 0; q < static_cast<int>(c.jfet.size()); ++q) {
        const auto& cj = c.jfet[static_cast<size_t>(q)];
        const Port ports[2] = {cj.ds, cj.gs};
        add(RootKind::Jfet, q, ports, 2, 2, {0, 1, 0});
    }

    for (int q = 0; q < static_cast<int>(c.mosfet.size()); ++q) {
        const auto& cm = c.mosfet[static_cast<size_t>(q)];
        const Port ports[2] = {cm.ds, cm.gs};
        add(RootKind::Mosfet, q, ports, 2, 2, {0, 1, 0});
    }

    for (int q = 0; q < static_cast<int>(c.bjt.size()); ++q) {
        const auto& bc = c.bjt[static_cast<size_t>(q)];
        const Port ports[3] = {bc.ce, bc.be, bc.e0};
        add(RootKind::Bjt, q, ports, 3, 2, {0, 1, 0});
    }

    for (int q = 0; q < static_cast<int>(c.opamp.size()); ++q) {
        const auto& ca = c.opamp[static_cast<size_t>(q)];
        // Three-port VCVS: output, non-inverting input, inverting input,
        // all referenced to the negative supply. All three currents are
        // solved: the output current drives the feedback network, while the
        // two input currents are identically zero and their equations pin the
        // input port voltages to the passive network.
        const Port ports[3] = {ca.out, ca.pos, ca.neg};
        add(RootKind::OpAmp, q, ports, 3, 3, {0, 1, 2});
    }

    layout.redCol.resize(static_cast<size_t>(layout.nRed), 0);
    layout.redRow.assign(static_cast<size_t>(layout.K), -1);
    for (const auto& dev : layout.devices) {
        for (int q = 0; q < dev.numActiveRows; ++q) {
            const int port = dev.portBase + dev.activePorts[static_cast<size_t>(q)];
            layout.redCol[static_cast<size_t>(dev.activeRowBase + q)] = port;
            layout.redRow[static_cast<size_t>(port)] = dev.activeRowBase + q;
        }
    }

    // Node reconstruction uses terminal currents, not the reduced Newton
    // columns. Keep every port except the BJT's dependent emitter port: its
    // current is already the return path of `ce` and `be`, so summing it a
    // second time doubles the emitter voltage drop. The remaining non-active
    // ports are identically zero (triode external grid, JFET/MOSFET gate), so
    // they are harmless and preserve the existing node response.
    layout.nodeCol.clear();
    for (const auto& dev : layout.devices) {
        for (int q = 0; q < dev.numPorts; ++q) {
            if (dev.kind == RootKind::Bjt && q == 2)
                continue;
            layout.nodeCol.push_back(dev.portBase + q);
        }
    }

    return layout;
}

}  // namespace wdf
