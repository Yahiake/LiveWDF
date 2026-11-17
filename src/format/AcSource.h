#pragma once

// A source's value over time: the DC level, plus an AC amplitude times a sine.
//
// **Why this is not on the source.** The compiled `wdf::Circuit` is a *topology*
// description -- what is connected to what, and what each element's static
// parameters are. The engine reads a source's `dc` for the DC operating point
// and nothing else. "How this source moves over time" belongs to the thing that
// owns the sample counter, which is the driver: the editor's session when the
// editor is simulating, the plugin's processor when a host is.
//
// Putting the frequency on `wdf::Src` therefore changed the layout of a struct
// that a third of a million assertions depend on, for a value the engine never
// reads. That is a change that buys nothing and risks everything, and it is
// exactly what broke the netlist importer when it was tried.
//
// **Where the numbers live instead.** In the document, next to the DC level: a
// source part carries `dc`, `ac` and `freq` in its `values` map, the inspector
// shows all three, and the driver reads them from there. That is also what the
// user asked for -- "a voltage source AC of 1 Hz" is a thing you type on the
// source, not a thing the network carries.
//
// **Why one function.** Three readers need the same answer: the editor's
// session, the plugin's processor, and the small-signal `.ac` path. One
// function, so they cannot disagree about the phase -- and the phase is the
// thing that is easy to get wrong. A source that starts halfway through a cycle
// looks like a bug in the circuit.

#include <cmath>

namespace wdf {

/// A source's value at sample `sampleIndex`: its DC level plus its AC amplitude
/// times a sine at its frequency.
///
/// `sampleIndex` counts samples since the circuit started. The phase starts at
/// zero, so the signal begins at a zero crossing and rises -- the same as every
/// other signal generator, and the reason a scope trace does not appear to start
/// halfway through a cycle.
///
/// A source with no AC amplitude is simply its DC level, and a source with an
/// amplitude but no frequency is *also* just its DC level: a frequency of zero
/// means no oscillation, not an infinitely fast one.
inline double sourceValueAt(double dc, double ac, double freq, double sampleRate,
                            long long sampleIndex) noexcept {
    if (ac == 0.0 || freq == 0.0)
        return dc;

    const double phase = 2.0 * 3.14159265358979323846 * freq *
                         static_cast<double>(sampleIndex) / sampleRate;
    return dc + ac * std::sin(phase);
}

}  // namespace wdf
