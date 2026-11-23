# MiSTer-Discrete semiconductor port and circuit coverage

The audit compares the remote `circuit-compiler` tip `692e777` with `livewdf-parts` at `6a302de`. The usage inventory reads the actual `components` arrays at schematic revision `dcefe3f`. It includes reference variants and counts each placed device once.

| Inventory | Documents | Named op-amp sections | Ideal op-amp sections | BJTs | Diodes | JFETs |
|---|---:|---:|---:|---:|---:|---:|
| Pedals | 45 | 157 | 9 | 117 | 89 | 22 |
| Studio | 38 | 163 | 75 | 66 | 86 | 5 |

## Prior coverage and remaining differences

| Upstream work | LiveWDF status before this change |
|---|---|
| Real op-amps and class-AB stages (`10f2d37`, `6b32897`) | RC4558, NJM4558, TL082 and LM741 class-AB stage variants are available. The ordinary 42 pedal `JRC4558` and six `RC4558` placements retain their original cards. The LM324 card has measured gain, bandwidth, slew and swing. Its transistor-level output-stage reference remains upstream. |
| Slew, input bias, supply currents and coupled feedback (`893a3d8`, `580af18`, `b74e5bb`, `69bd77e`) | The native pole, slew and bounded coupled solves cover feedback and limiting. Some LiveWDF op-amps support rail sag through their supply model. Input bias, quiescent current and dynamic loading of the physical supply pins are separate effects and do not follow from finite gain or bandwidth alone. |
| Breakdown and diode leakage (`0176f12`) | Extended Shockley, series and parallel resistance, optional reverse breakdown and fitted leakage cards exist. The 98 schematic `1N4148` placements use the standard card. The Nexperia low-leakage variants remain explicit choices. |
| BJT leakage and Early effect, selected per circuit (`6393ca3`, `762c943`) | Opt-in 2N3904/3906 cards carry base-emitter recombination and forward Early effect. Separate IKF variants enable high-current gain roll-off. Nine pedal placements use `2N3904-MiSTer`. Standard 2N3904/3906 cards retain their prior behavior. The extracted germanium junction-capacitance options are also explicit. |
| Other semiconductor families | Soundcraft ED2162 issue 8 specifies a 2N4403 input pair. The Diamond Quartz reference specifies the OP275 and LM358. Other documented substitutions include HA1457W in the GE-7, 2SA1049-GR and 2SC2458-GR in the BD-2, 2SK30A-Y and 2SA970-GR in the HM-2, and NE5534 in the Soundcraft. These devices need individual source and operating-point checks. |

[Upstream parts and fixture notes](https://github.com/jopdorp/MiSTer-Discrete/tree/692e777112e4a3c05a17578d9cfda1ecbc9b98d6/circuits/parts) · [Upstream native device laws](https://github.com/jopdorp/MiSTer-Discrete/blob/692e777112e4a3c05a17578d9cfda1ecbc9b98d6/circuitsim/mna.py) · [Existing LiveWDF part validation](PARTS-VALIDATION.md) · [2N4403 model and test](2N4403.md)

## Legacy LM358

The LM358 card implements a 700 kHz dominant pole, 0.3 V/µs slew, a 40 mA source and a 20 mA sink. A 29 µA pulldown at 15 V accounts for low-side behavior around ground. The positive and negative supply terminals carry source and sink load currents respectively, with 321 µA per section of additional quiescent current. At other supply voltages the bias and integrator swing require corresponding calibration. With nominal ±15 V the pole stops 0.245 V above the negative supply and 0.47 V below the positive supply. The following stage determines the actual output swing at each load. Input common-mode cutoff, input offset, statistical spread, temperature drift and voltage-dependent quiescent current remain outside the card.

| Additional sink load at 15 V | Upstream ngspice output above the negative rail |
|---:|---:|
| 10 µA | 11.8 mV |
| 20 µA | 32.3 mV |
| 35 µA | 569.5 mV |
| 100 µA | 634.5 mV |
| 1 mA | 751.7 mV |
| 10 mA | 1.314 V |

The `[lm358]` tests use independent `.op` readings from the upstream lumped SPICE circuit. TI SLOS068AB Figure 5-47 shows the output-voltage change as sinking current crosses the pulldown range. The test uses the original LM358 at 15 V and keeps the newer LM358B revision separate.

[TI LM358 datasheet, electrical characteristics and Figure 5-47](https://www.ti.com/lit/ds/symlink/lm358.pdf) · [Upstream LM358 netlist](https://github.com/jopdorp/MiSTer-Discrete/blob/692e777112e4a3c05a17578d9cfda1ecbc9b98d6/circuits/parts/lm358.lib)

The two output branches and their steering diodes use the same existing bounded solver as the other devices. The floor diode and its bias combine algebraically into one Shockley diode. The 100 kΩ return paths and the 54 Ω output-stage resistance stay explicit. Two candidate reductions were compared on the Diamond before choosing this implementation. Direct interpolation of the upstream class-B law produced three rejected samples in the normal guitar test and seven in the high-level test. A light-load expansion converged, but its runtime at high drive varied and the first 100 ms differed from the full circuit. The LM358 part uses the complete steering stage. The physical Diamond configuration is an optional review fixture.

A 48 kHz guitar excerpt peaked at 0.120 V normally and 0.481 V in the stress test. Both levels completed at minimum, center and maximum compression, with zero rejected or nonconverged samples across the six one-second renders. At centered compression, three rotated runs on a shared Xeon Platinum 8573C gave median costs of 11.27 and 22.77 µs of process CPU per frame for the stock and physical versions. Their aligned output residual was −52.75 dB relative to stock, and the optical gain-reduction difference peaked at 0.0135 dB. At the higher input level the corresponding values were 11.03 and 26.80 µs, −52.46 dB and 0.0275 dB. Some settings exceed the 20.83 µs/sample budget at 48 kHz. Soundcraft speech at 0.585 and 2.340 V peak also completed with zero rejected or nonconverged samples using the 2N4403 pair.
