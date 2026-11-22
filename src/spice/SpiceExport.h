#pragma once

// ============================================================================
// SPICE netlist export for the LiveWDF core circuit.
//
// `spiceExport(circuit)` writes the flat `wdf::Circuit` (the exact thing the
// engine solves) as an ngspice-compatible netlist. It is JUCE-free and lives
// in the core library so any host (plugin, editor, CLI, tests) can generate a
// SPICE deck from a `.livewdf` document and compare against ngspice.
//
// Supported elements:
//   Res, Cap, Ind, MutualInd (transformers), Src, Isrc, VCVS (op-amps),
//   Triode (quadric DAFx-23), Diode (extended Shockley, incl. Rp subcircuit),
//   Pentode (Reefman DerkD/DerkE and Koren, via behavioural subcircuits),
//   Jfet, Mosfet, Bjt (Ebers-Moll via behavioural subcircuits).
//
// Devices without a single SPICE primitive are exported as `.subckt`s built
// from behavioural current sources (B elements). Self-referential `I(Bx)`
// terms are resolved by ngspice's Newton iteration, exactly as ATTILA's
// extendedSchockleydiode subckt does.
//
// Notes:
//   * Pots / VarRes / ControlledRes are already expanded into Res at parse
//     time by Circuit::addPot / addVarRes / addControlledRes, so nothing
//     special is needed here.
//   * Centre-tapped transformers are already expanded into coupled inductors
//     by Circuit::addCenterTapTransformerElement, so MutualInd export covers
//     them.
//   * Devices the exporter cannot represent silently get a comment line
//     instead of a netlist entry; the caller can check the returned diagnostics.
// ============================================================================

#include <string>

#include "spice/Circuit.h"

namespace wdf {

/// Result of a SPICE export run.
struct SpiceExportResult {
    std::string netlist;   // the generated .cir text
    std::string diagnostics; // human-readable notes / warnings
    bool ok = false;
};

/// Export a `wdf::Circuit` to a SPICE netlist string.
///
/// `fs` is the sample rate used for the transient `.tran` card and for
/// inductor DC resistance (`kIndDcR` from Circuit.h).
/// `driveVolts` sets the input source amplitude for the transient.
/// `durationSeconds` sets the transient stop time.
SpiceExportResult spiceExport(const Circuit& c,
                              double fs = 48000.0,
                              double driveVolts = 0.25,
                              double durationSeconds = 0.2);

}  // namespace wdf
