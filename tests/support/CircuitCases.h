#pragma once

// A small library of named circuit fixtures, shared by the tests and the
// benchmark instrument.
//
// Why this exists: `rc_lowpass` alone is a poor basis for deciding anything --
// it is N=1, P=2, K=0, so it exercises none of the paths the engine actually
// spends its time in (no device root, no Newton, no closed-form gate, no
// reactive state worth the name). The circuits here are built from the
// Cathodyne stage shapes (`SchematicBuilder::_buildCommonCathode`,
// `_buildCommonCathodeUnbypassed`, `_buildCathodeFollower`, and the two-stage
// Dual Rectifier chain) so that the engine is measured and checked on the
// topologies it is meant to run.
//
// These are built with `Circuit::add*` rather than parsed from a SPICE string
// on purpose. The string parser in `spice/SpiceNet.h` refuses what it does not
// understand (it *used* to skip it, so a supply written as `E1 ...` disappeared
// without a word -- see `tests/spice/test_spice_parser.cpp`), but it still only
// *understands* a subset (`R C L V I X D`, tubes only via `Tubes_12AX7`). A
// fixture built programmatically cannot name an element that is not in that
// subset, so the library stays a statement about topology rather than about the
// parser's vocabulary.

#include <cmath>
#include <string>
#include <vector>

#include "spice/Net.h"
#include "devices/Diode.h"
#include "devices/Jfet.h"
#include "devices/Pentode.h"
#include "devices/Triode.h"

namespace lwdf_test {

inline wdf::TriodeModel gridlessQuadric() {
    return wdf::k12ax7Quadric();
}

/// Single 12AX7 common cathode, cathode resistor bypassed (`Ck`). This is the
/// shape the three-arm benchmark uses, kept here so the test suite covers it.
inline wdf::Circuit commonCathodeBypassed() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    c.addCap("in", "i", 100e-9);   // Ci
    c.addRes("i", "0", 1e6);       // Ri
    c.addRes("i", "g", 20e3);      // Rg
    c.addRes("k", "0", 1e3);       // Rk
    c.addCap("k", "0", 10e-6);     // Ck
    c.addRes("e", "p", 100e3);     // Rp
    c.addCap("p", "o", 10e-9);     // Co
    c.addRes("o", "0", 1e6);       // Ro
    c.addTri("g", "p", "k", "g", gridlessQuadric());
    return c;
}

/// Single 12AX7 common cathode, **unbypassed** cathode: the same stage with the
/// bypass cap removed. Local current feedback, so it is a different transfer --
/// and structurally it drops one reactive element, which is exactly the kind of
/// change the structure lists in `Network` have to get right.
inline wdf::Circuit commonCathodeUnbypassed() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    c.addCap("in", "i", 100e-9);   // Ci
    c.addRes("i", "0", 1e6);       // Ri
    c.addRes("i", "g", 20e3);      // Rg
    c.addRes("k", "0", 1.5e3);     // Rk (no Ck)
    c.addRes("e", "p", 100e3);     // Rp
    c.addCap("p", "o", 10e-9);     // Co
    c.addRes("o", "0", 1e6);       // Ro
    c.addTri("g", "p", "k", "g", gridlessQuadric());
    return c;
}

/// Cathodyne's cathode follower: the plate sits on a load resistor tied to the
/// supply and the output is taken at the cathode. Unity-ish gain, output in
/// phase with the input -- a different root bias point than the common cathode,
/// which is the point of carrying it.
inline wdf::Circuit cathodeFollower() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    c.addRes("in", "g", 1e6);      // Rg (grid leak)
    c.addRes("e", "p", 47e3);      // plate load
    c.addRes("k", "0", 1.2e3);     // Rk (cathode)
    c.addCap("k", "out", 100e-9);  // coupling from the cathode
    c.addRes("out", "0", 1e6);     // load
    c.addTri("g", "p", "k", "g", gridlessQuadric());
    return c;
}

/// Two 12AX7 common-cathode stages in cascade with an interstage coupling cap
/// and grid leak -- the Dual Rectifier shape, reduced to its skeleton.
///
/// This is the important fixture: two nonlinear roots, but the coupling between
/// them is one-way, so the closed forms run in sequence instead of as one joint
/// Newton. It is what the group ordering has to get right, and what prices a
/// per-stage closed form against the multiport Newton on a real topology.
inline wdf::Circuit twoStageCascade() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    // stage 1
    c.addRes("in", "g1", 68e3);
    c.addRes("g1", "0", 1e6);
    c.addRes("e", "p1", 100e3);
    c.addRes("k1", "0", 1.5e3);
    c.addCap("k1", "0", 22e-6);
    // interstage
    c.addCap("p1", "g2", 22e-9);
    c.addRes("g2", "0", 1e6);
    // stage 2
    c.addRes("e", "p2", 100e3);
    c.addRes("k2", "0", 1.5e3);
    c.addCap("k2", "0", 10e-6);
    c.addCap("p2", "out", 10e-9);
    c.addRes("out", "0", 1e6);
    c.addTri("g1", "p1", "k1", "g1", gridlessQuadric());
    c.addTri("g2", "p2", "k2", "g2", gridlessQuadric());
    return c;
}

/// Four cascaded 12AX7 stages -- a full high-gain lead preamp chain
/// (clean gain -> overdrive -> cold clipper -> output buffer stage).
///
/// This tests whether the root-group decomposition scales to an entire multi-tube
/// preamp: 4 triodes, 12 root ports, 13 free nodes, 21 passive elements.
/// Because each interstage coupling enters the next tube's grid, the coupling
/// graph is strictly acyclic (1 -> 2 -> 3 -> 4) and all 4 roots are solved
/// in closed form without a single Newton iteration.
inline wdf::Circuit fourStagePreamp() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 300.0);

    // Stage 1: Clean gain stage
    c.addRes("in", "g1", 68e3);
    c.addRes("g1", "0", 1e6);
    c.addRes("e", "p1", 100e3);
    c.addRes("k1", "0", 1.5e3);
    c.addCap("k1", "0", 22e-6);

    // Interstage 1 -> Stage 2: Overdrive stage
    c.addCap("p1", "g2", 22e-9);
    c.addRes("g2", "0", 1e6);
    c.addRes("e", "p2", 100e3);
    c.addRes("k2", "0", 1.5e3);
    c.addCap("k2", "0", 1e-6);

    // Interstage 2 -> Stage 3: Cold clipper stage (39k unbypassed cathode)
    c.addCap("p2", "g3", 22e-9);
    c.addRes("g3", "0", 470e3);
    c.addRes("e", "p3", 100e3);
    c.addRes("k3", "0", 39e3);

    // Interstage 3 -> Stage 4: Output recovery stage
    c.addCap("p3", "g4", 22e-9);
    c.addRes("g4", "0", 1e6);
    c.addRes("e", "p4", 100e3);
    c.addRes("k4", "0", 1.5e3);
    c.addCap("k4", "0", 10e-6);
    c.addCap("p4", "out", 10e-9);
    c.addRes("out", "0", 1e6);

    c.addTri("g1", "p1", "k1", "g1", gridlessQuadric());
    c.addTri("g2", "p2", "k2", "g2", gridlessQuadric());
    c.addTri("g3", "p3", "k3", "g3", gridlessQuadric());
    c.addTri("g4", "p4", "k4", "g4", gridlessQuadric());
    return c;
}


/// Two triodes sharing one cathode resistor -- the long-tailed pair, i.e. the
/// phase inverter at the front of most push-pull amps, reduced to its skeleton.
///
/// This is the fixture that keeps the fast path honest. Every other multi-device
/// circuit here is a *cascade*, and a cascade's root can be split: the coupling
/// enters the next triode at its grid, a triode's grid carries no current of its
/// own, and so the coupling is one-way. Here both cathodes are the *same node*
/// and it carries both cathode currents, so a current into either triode moves the
/// other's port voltages. The coupling is two-way through a current-carrying
/// node, no ordering of the ports makes the root block-triangular, and the joint
/// multiport Newton is genuinely required.
inline wdf::Circuit sharedCathodePair() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    c.addRes("in", "g1", 68e3);    // drive into stage 1's grid
    c.addRes("g1", "0", 1e6);      // Rg1
    c.addRes("e", "p1", 100e3);    // plate load 1
    c.addCap("p1", "out", 10e-9);  // output taken from stage 1
    c.addRes("out", "0", 1e6);
    c.addRes("e", "p2", 100e3);    // plate load 2
    c.addRes("g2", "0", 1e6);      // Rg2 (the second input sits at ground)
    c.addRes("k", "0", 22e3);      // Rk, shared by both cathodes
    c.addTri("g1", "p1", "k", "g1", gridlessQuadric());
    c.addTri("g2", "p2", "k", "g2", gridlessQuadric());
    return c;
}

/// A single general JFET common-source stage. The 2N5952 preset carries the
/// LTspice card's linear parasitics, so this fixture exercises the two-port
/// group solve with real gate/drain/source networks rather than an isolated law.
/// Bias is Vgs = 0; the 2.2k drain load keeps it in saturation over the 0.2 V
/// input swing.
inline wdf::Circuit jfetCommonSource() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 15.0);
    c.addRes("in", "g", 1e3);
    c.addRes("g", "0", 1e6);
    c.addRes("e", "d", 2.2e3);
    c.addCap("d", "out", 10e-9);
    c.addRes("out", "0", 1e6);
    c.addJfet("d", "g", "0", wdf::k2N5952, "J1");
    return c;
}

/// The linear baseline: inductor high-pass. No device at all, so it stays on
/// the dense path and shows what the engine costs when the root is absent.
inline wdf::Circuit rlHighpass() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addInd("in", "out", 10e-3);
    c.addRes("out", "0", 100.0);
    return c;
}

/// Passive RC low-pass -- the original fixture, kept for continuity.
inline wdf::Circuit rcLowpass() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "out", 1e3);
    c.addCap("out", "0", 1e-6);
    return c;
}

/// A passive ladder: three RC sections stacked, all linear. Bigger N and P than
/// anything above with no nonlinearity, so it isolates "what does size alone
/// cost" from "what does a device root cost".
inline wdf::Circuit rcLadder() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "a", 1e3);
    c.addCap("a", "0", 47e-9);
    c.addRes("a", "b", 1e3);
    c.addCap("b", "0", 47e-9);
    c.addRes("b", "out", 1e3);
    c.addCap("out", "0", 10e-9);
    c.addRes("out", "0", 1e6);
    return c;
}

/// The reference `diode_clipper` from `wdf_compiler/tests/diode_clipper/`, built
/// to its own numbers: `Rin 1k`, `C1 1u`, and its `D1N914` model (`IS=1e-9 N=1`,
/// no `RS`). Two anti-parallel diodes, so both of its root ports are live on
/// every sample -- there is no half-cycle where the clipper's root is inactive,
/// which is what makes it a different measurement from the triode fixtures,
/// where one port (the internal grid) never carries current at all.
inline wdf::Circuit diodeClipper() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "out", 1e3);  // Rin
    c.addCap("out", "0", 1e-6);  // C1
    const wdf::DiodeModel d1n914(1e-9, 1.0, 0.0);
    c.addDiode("out", "0", d1n914, "DP");   // forward half
    c.addDiode("0", "out", d1n914, "DP2");  // reverse half
    return c;
}

/// A half clipper: the `diode_clipper` above with one of its two anti-parallel
/// junctions removed, so the root is a *single* diode port and the group solve can
/// take it.
///
/// One diode is the shape the group solve takes directly, and its sibling
/// `diodeClipper` is the control: two reversed ports across the same node pair
/// collapse to one scalar equation, so both are exact closed forms and the
/// difference is the pair solve against the single junction. The two together
/// are what prices "a diode root" against "a pair root". The joint Newton stays
/// available as the reference path. The reference single-diode deck
/// (`wdf_compiler/tests/diode_circuit/`) could not serve as this fixture: it
/// couples a 1 nF cap through a 100 ohm resistor, a 100 ns corner, two orders of
/// magnitude faster than a 48 kHz sample can resolve. At that step the cap is a
/// wire and an ngspice reference is not a reference -- which is why the *clipper*
/// fixture, whose reference deck does have an audio-rate `C1`, is the one that
/// carries the transient oracle. Reusing the clipper's own `Rin`/`C1` here keeps
/// this fixture in the regime the engine is built for and makes the two differ in
/// exactly one thing: the number of live junction ports.
inline wdf::Circuit diodeSingle() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "out", 1e3);  // Rin
    c.addCap("out", "0", 1e-6);  // C1
    c.addDiode("out", "0", wdf::DiodeModel(1e-9, 1.0, 0.0), "DP");
    return c;
}

/// The classic Fender "Blackface" (Twin Reverb / Deluxe Reverb) FMV tone stack.
/// Driven through 12AX7 plate impedance (38k), with Treble, Bass, and Mid controls.
/// Contains a non-series-parallel rigid bridge loop.
inline wdf::Circuit fenderToneStack() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addRes("in", "top", 38e3, "R_drive"); // 12AX7 plate drive impedance
    // Treble branch: C1 (250pF) -> treble_in -> Treble pot (250k) split around wiper 'out'
    c.addCap("top", "treble_in", 250e-12, "C1_treble");
    c.addRes("treble_in", "out", 125e3, "R_treble_top");
    c.addRes("out", "bass_mid", 125e3, "R_treble_bot");
    // Bass branch: C2 (100nF) -> bass_top -> Bass pot (250k)
    c.addCap("top", "bass_top", 100e-9, "C2_bass");
    c.addRes("bass_top", "bass_mid", 250e3, "R_bass");
    // Slope resistor: 100k from top to bass_mid
    c.addRes("top", "bass_mid", 100e3, "R_slope");
    // Mid pot & cap: 10k resistor and 47nF cap from bass_mid to ground
    c.addCap("bass_mid", "0", 47e-9, "C3_mid");
    c.addRes("bass_mid", "0", 10e3, "R_mid");
    // Load: 1M volume pot + 47pF cable
    c.addRes("out", "0", 1e6, "R_load");
    c.addCap("out", "0", 47e-12, "C_cable");
    return c;
}

/// Electro-Harmonix Big Muff Pi passive tilt tone stack.
/// Combines high-pass and low-pass RC filters blended through a 100k pot wiper.
inline wdf::Circuit bigMuffToneStack() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addRes("in", "top", 1e3, "R_drive");
    // High-pass leg: C1 (4nF) -> hp_node -> R2 (22k) to ground
    c.addCap("top", "hp_node", 4e-9, "C1");
    c.addRes("hp_node", "0", 22e3, "R2");
    // Low-pass leg: R1 (39k) -> lp_node -> C2 (10nF) to ground
    c.addRes("top", "lp_node", 39e3, "R1");
    c.addCap("lp_node", "0", 10e-9, "C2");
    // Tone pot split between hp_node and lp_node with wiper at out
    c.addRes("hp_node", "out", 50e3, "R_tone1");
    c.addRes("out", "lp_node", 50e3, "R_tone2");
    // Load
    c.addRes("out", "0", 100e3, "R_load");
    return c;
}

/// Passive Twin-T RC notch filter (1 kHz deep notch).
/// True rigid bridge structure with parallel load subtree.
inline wdf::Circuit twinTNotchFilter() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addRes("in", "drive", 100.0, "R_drive");
    // Highpass T: C1 (10nF), C2 (10nF), R3 (7.95k)
    c.addCap("drive", "mid1", 10e-9, "C1");
    c.addCap("mid1", "out", 10e-9, "C2");
    c.addRes("mid1", "0", 7.95e3, "R3");
    // Lowpass T: R1 (15.9k), R2 (15.9k), C3 (20nF)
    c.addRes("drive", "mid2", 15.9e3, "R1");
    c.addRes("mid2", "out", 15.9e3, "R2");
    c.addCap("mid2", "0", 20e-9, "C3");
    // Output load
    c.addRes("out", "0", 1e6, "R_load");
    c.addCap("out", "0", 200e-12, "C_cable");
    return c;
}

/// Ibanez TS9 Tube Screamer clipping stage:
/// Non-inverting op-amp VCVS (A=10^5) with 720 Hz HPF in inverting leg
/// and anti-parallel 1N4148 clipping diodes across feedback loop.
inline wdf::Circuit tubeScreamerClippingStage() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    // Op-amp: VCVS between out and 0, sensing in - inv
    c.addVCVS("out", "0", "in", "inv", 1e5, 10.0, "E_opamp");
    // Inverting leg to ground: R1 (4.7k) in series with C1 (47nF)
    c.addRes("inv", "n_rc", 4.7e3, "R1");
    c.addCap("n_rc", "0", 47e-9, "C1");
    // Feedback branch: Rfb (51k) || Cfb (51pF) || anti-parallel diodes
    c.addRes("out", "inv", 51e3, "R_fb");
    c.addCap("out", "inv", 51e-12, "C_fb");
    const wdf::DiodeModel d1n4148(4.0e-9, 1.0, 0.56);
    c.addDiode("out", "inv", d1n4148, "D1");
    c.addDiode("inv", "out", d1n4148, "D2");
    // Load
    c.addRes("out", "0", 10e3, "R_load");
    return c;
}

/// Single-Ended Class-A EL34 power stage with screen bias and cathode bypass.
inline wdf::Circuit singleEndedEl34PowerStage() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addSrc("bplus", "0", 350.0, 0.0, {}, "V_Bplus");
    c.addSrc("vscreen", "0", 300.0, 0.0, {}, "V_Screen");
    // Grid input coupling & leak
    c.addCap("in", "g", 47e-9, "C_in");
    c.addRes("g", "0", 220e3, "R_g");
    // Cathode bias: Rk || Ck
    c.addRes("k", "0", 250.0, "R_k");
    c.addCap("k", "0", 100e-6, "C_k");
    // Screen resistor & bypass
    c.addRes("vscreen", "s", 1e3, "R_s");
    c.addCap("s", "0", 22e-6, "C_s");
    // Plate load resistor (modeling transformer primary impedance)
    c.addRes("bplus", "p", 2.5e3, "R_plate");
    // EL34 power pentode
    c.addPentode("p", "s", "g", "k", wdf::kEL34, "XP1");
    return c;
}

/// Passive electric guitar pickup and controls circuit:
/// Pickup coil (Rdc, L, C_coil), volume pot, tone pot + cap, cable, and amp input.
inline wdf::Circuit guitarPickupAndCable() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    // Pickup coil: Rdc (6k) in series with L (2.5H), with winding C (110pF) in parallel
    c.addRes("in", "n_l", 6.0e3, "R_coil");
    c.addInd("n_l", "pu_out", 2.5, "L_coil");
    c.addCap("in", "pu_out", 110e-12, "C_coil");
    // Volume pot: 500k (wiper at center: 250k / 250k)
    c.addRes("pu_out", "vol_wiper", 250e3, "R_vol1");
    c.addRes("vol_wiper", "0", 250e3, "R_vol2");
    // Tone pot + cap: 250k + 22nF
    c.addRes("vol_wiper", "tone_node", 250e3, "R_tone");
    c.addCap("tone_node", "0", 22e-9, "C_tone");
    // Guitar cable capacitance (680 pF)
    c.addCap("vol_wiper", "0", 680e-12, "C_cable");
    // Amp input: 1M || 47pF
    c.addRes("vol_wiper", "0", 1e6, "R_amp");
    c.addCap("vol_wiper", "0", 47e-12, "C_amp");
    return c;
}

/// Active Baxandall Bass & Treble tone stage with op-amp VCVS.
inline wdf::Circuit baxandallActiveToneStage() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    // Inverting op-amp VCVS (gain 10^5, ro 10)
    c.addVCVS("out", "0", "0", "inv", 1e5, 10.0, "E_opamp");
    // Input resistor
    c.addRes("in", "mid_in", 10e3, "R_in");
    // Bass network: Cb1, Cb2, Rb1, Rb2
    c.addCap("mid_in", "inv", 100e-9, "Cb1");
    c.addRes("mid_in", "inv", 50e3, "Rb1");
    c.addCap("inv", "out", 100e-9, "Cb2");
    c.addRes("inv", "out", 50e3, "Rb2");
    // Treble network: Rt1, Ct1, Rt2, Ct2
    c.addCap("mid_in", "treb_node", 1e-9, "Ct1");
    c.addRes("treb_node", "inv", 4.7e3, "Rt1");
    c.addCap("treb_node", "out", 1e-9, "Ct2");
    // Output load
    c.addRes("out", "0", 10e3, "R_load");
    return c;
}

/// Passive 3-way speaker crossover (2nd order Butterworth, 8-ohm drivers).
inline wdf::Circuit threeWaySpeakerCrossover() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    // Woofer Low-pass: Lw (4mH) series, Cw (50uF) parallel, 8-ohm woofer
    c.addInd("in", "w_node", 4.0e-3, "L_woofer");
    c.addCap("w_node", "0", 50e-6, "C_woofer");
    c.addRes("w_node", "0", 8.0, "R_woofer");
    // Tweeter High-pass: Ct (6.8uF) series, Lt (0.5mH) parallel, 8-ohm tweeter
    c.addCap("in", "t_node", 6.8e-6, "C_tweeter");
    c.addInd("t_node", "0", 0.5e-3, "L_tweeter");
    c.addRes("t_node", "0", 8.0, "R_tweeter");
    // Midrange Band-pass: Cm1 (33uF) + Lm1 (1.5mH) series, Cm2 (10uF) || Lm2 (0.8mH) parallel
    c.addCap("in", "m_mid", 33e-6, "C_mid1");
    c.addInd("m_mid", "m_node", 1.5e-3, "L_mid1");
    c.addCap("m_node", "0", 10e-6, "C_mid2");
    c.addInd("m_node", "0", 0.8e-3, "L_mid2");
    c.addRes("m_node", "0", 8.0, "R_mid");
    return c;
}

/// Marshall / Fender 12AX7 Long-Tailed Pair (LTP) phase inverter:
/// Two triodes sharing a current-carrying cathode & tail network with AC ground on grid 2.
/// Tightly coupled in both directions, producing balanced differential outputs (out1, out2).
/// Strictly requires the multiport Newton solver (K = 6).
inline wdf::Circuit longTailedPair() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addSrc("bplus", "0", 300.0, 0.0, {}, "V_Bplus");

    // Grid 1 drive & leak
    c.addCap("in", "g1", 47e-9, "C_in");
    c.addRes("g1", "tail", 1e6, "R_g1");

    // Grid 2 AC grounded via 100nF, biased to tail
    c.addCap("g2", "0", 100e-9, "C_g2");
    c.addRes("g2", "tail", 1e6, "R_g2");

    // Shared cathode and tail network
    c.addRes("k", "tail", 470.0, "R_k");
    c.addRes("tail", "0", 22e3, "R_tail");

    // Plate loads (classic 82k / 100k asymmetry)
    c.addRes("bplus", "p1", 82e3, "R_p1");
    c.addRes("bplus", "p2", 100e3, "R_p2");

    // Balanced differential output coupling
    c.addCap("p1", "out1", 47e-9, "C_c1");
    c.addRes("out1", "0", 220e3, "R_load1");
    c.addCap("p2", "out2", 47e-9, "C_c2");
    c.addRes("out2", "0", 220e3, "R_load2");

    // Two 12AX7 triodes sharing cathode 'k'
    wdf::TriodeModel model = wdf::k12ax7Quadric();
    c.addTri("g1", "p1", "k", "g1", model);
    c.addTri("g2", "p2", "k", "g2", model);
    return c;
}

/// Classic asymmetric diode clipping stage (Klon Centaur / Boutique style):
/// Silicon diode forward (1N4148, Vf ~ 0.65 V) in parallel with Germanium diode reverse (1N34A, Vf ~ 0.25 V).
/// The two reversed ports collapse to one scalar port equation even though the
/// branch laws differ; the pair solve is exact for both.
inline wdf::Circuit asymmetricDiodeClipper() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addRes("in", "out", 10e3, "R_in");
    c.addCap("out", "0", 10e-9, "C_filter");
    c.addRes("out", "0", 100e3, "R_load");
    const wdf::DiodeModel d_silicon(4.0e-9, 1.0, 0.56);
    c.addDiode("out", "0", d_silicon, "D_Si");
    const wdf::DiodeModel d_germanium(2.0e-6, 1.3, 10.0);
    c.addDiode("0", "out", d_germanium, "D_Ge");
    return c;
}

/// Full-Wave 4-Diode Center-Tapped / Bridge Rectifier with smoothing filter:
/// Canonical cyclic bridge nonlinearity from DAFx literature.
/// Non-zero cyclic Z coupling across diodes, strictly requiring Newton (K = 4).
inline wdf::Circuit diodeBridgeRectifier() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    // AC input secondary: differential drive
    c.addVCVS("ac1", "0", "in", "0", 1.0, 10.0, "E1");
    c.addVCVS("0", "ac2", "in", "0", 1.0, 10.0, "E2");
    c.addRes("ac1", "0", 100e3, "R_ref1");
    c.addRes("ac2", "0", 100e3, "R_ref2");
    const wdf::DiodeModel d1n4007(1.4e-8, 1.9, 0.04);
    c.addDiode("ac1", "dc_pos", d1n4007, "D1");
    c.addDiode("ac2", "dc_pos", d1n4007, "D2");
    c.addDiode("0", "ac1", d1n4007, "D3");
    c.addDiode("0", "ac2", d1n4007, "D4");
    c.addCap("dc_pos", "0", 47e-6, "C_filter");
    c.addRes("dc_pos", "0", 1e3, "R_load");
    return c;
}

/// Push-Pull Class-AB Power Stage:
/// Two EL34 power pentodes sharing common cathode bias and center-tapped transformer load.
/// Strongly coupled across cathode and power supply nodes, strictly requiring Newton (K = 6).
inline wdf::Circuit pushPullPowerStage() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addSrc("bplus", "0", 350.0, 0.0, {}, "V_Bplus");
    c.addSrc("vscreen", "0", 300.0, 0.0, {}, "V_Screen");

    // Phase inversion for second pentode grid
    c.addVCVS("in_neg", "0", "0", "in", 1.0, 1e-4, "E_splitter");

    // Grid 1 drive & leak
    c.addCap("in", "g1", 47e-9, "C_in1");
    c.addRes("g1", "0", 220e3, "R_g1");

    // Grid 2 drive & leak
    c.addCap("in_neg", "g2", 47e-9, "C_in2");
    c.addRes("g2", "0", 220e3, "R_g2");

    // Shared cathode bias: Rk || Ck
    c.addRes("k", "0", 250.0, "R_k");
    c.addCap("k", "0", 100e-6, "C_k");

    // Screen grids
    c.addRes("vscreen", "s1", 1e3, "R_s1");
    c.addCap("s1", "0", 22e-6, "C_s1");
    c.addRes("vscreen", "s2", 1e3, "R_s2");
    c.addCap("s2", "0", 22e-6, "C_s2");

    // Center-tapped output transformer primary load
    c.addInd("p1", "bplus", 5.0, "L_p1");
    c.addInd("bplus", "p2", 5.0, "L_p2");
    c.addRes("p1", "bplus", 3.4e3, "R_load1");
    c.addRes("bplus", "p2", 3.4e3, "R_load2");

    // Two EL34 power pentodes
    c.addPentode("p1", "s1", "g1", "k", wdf::kEL34, "XP1");
    c.addPentode("p2", "s2", "g2", "k", wdf::kEL34, "XP2");
    return c;
}

/// One named fixture: how to build it, what to drive it with, and the source
/// count so callers know how many `sourceValues` slots `step()` needs.
struct CircuitCase {
    const char* name;
    wdf::Circuit (*build)();
    /// Sample of the driving source (slot 0). Supplies keep their DC value.
    double (*input)(int n);
};

inline double sine220(int n) {
    return 0.2 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * static_cast<double>(n) / 48000.0);
}

/// The diode fixtures are driven the way their reference decks drive them -- hard
/// enough to clip -- rather than at the shared 0.2 V level, where the junctions
/// barely conduct and the Newton would be a much easier problem than the element
/// is for. A fixture that under-drives its own device measures the wrong thing.
inline double sine1v(int n) {
    return 1.0 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * static_cast<double>(n) / 48000.0);
}

/// One full `sourceValues` vector for sample `n`: the driving source gets the
/// case's input, every other ideal source keeps its DC value.
///
/// `Engine::step()` reads `sourceValues` at the indices the offsets name, so a
/// short vector is an out-of-range read rather than a default -- every caller
/// has to hand over one slot per source.
inline std::vector<double> sampleSources(const wdf::Circuit& c, const CircuitCase& tc, int n) {
    std::vector<double> sv(c.src.size(), 0.0);
    for (size_t i = 0; i < c.src.size(); ++i)
        sv[i] = c.src[i].dc;
    if (!sv.empty())
        sv[0] = (*tc.input)(n);
    return sv;
}

inline const std::vector<CircuitCase>& circuitCases() {
    static const std::vector<CircuitCase> cases = {
        {"rc_lowpass", &rcLowpass, &sine220},
        {"rl_highpass", &rlHighpass, &sine220},
        {"common_cathode_bypassed", &commonCathodeBypassed, &sine220},
        {"common_cathode_unbypassed", &commonCathodeUnbypassed, &sine220},
        {"cathode_follower", &cathodeFollower, &sine220},
        {"two_stage_cascade", &twoStageCascade, &sine220},
        {"four_stage_preamp", &fourStagePreamp, &sine220},
        {"shared_cathode_pair", &sharedCathodePair, &sine220},
        {"jfet_common_source", &jfetCommonSource, &sine220},
        {"diode_clipper", &diodeClipper, &sine1v},
        {"diode_single", &diodeSingle, &sine1v},
        {"fender_tone_stack", &fenderToneStack, &sine220},
        {"big_muff_tone_stack", &bigMuffToneStack, &sine220},
        {"twin_t_notch_filter", &twinTNotchFilter, &sine220},
        {"tube_screamer_clipping", &tubeScreamerClippingStage, &sine1v},
        {"single_ended_el34", &singleEndedEl34PowerStage, &sine1v},
        {"guitar_pickup_cable", &guitarPickupAndCable, &sine220},
        {"baxandall_active_tone", &baxandallActiveToneStage, &sine220},
        {"three_way_crossover", &threeWaySpeakerCrossover, &sine220},
        {"long_tailed_pair", &longTailedPair, &sine1v},
        {"asymmetric_diode_clipper", &asymmetricDiodeClipper, &sine1v},
        {"diode_bridge_rectifier", &diodeBridgeRectifier, &sine1v},
        {"push_pull_power_stage", &pushPullPowerStage, &sine1v},
    };
    return cases;
}

}  // namespace lwdf_test
