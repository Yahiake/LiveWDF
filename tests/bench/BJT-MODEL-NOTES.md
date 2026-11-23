# BJT accuracy and real-time cost

LiveWDF uses a large-signal Ebers–Moll transport law. The MiSTer cards add Early
effect and selected leakage terms. The term coverage depends on the preset.
The DC Jacobian accounts for the two independent junction voltages. The
device law obtains the third terminal current by current conservation. The
solver uses analytic derivatives and the DAFx-23 junction step damping.

Keep the existing static law as the baseline. Compare its transfer to the
same-law ngspice card before comparing it to full Gummel–Poon or measurements.
Match temperature, model revision, terminal convention and source impedance.
A correct solve of reduced equations can disagree with full-device behavior.

## onsemi gain options

The optional `2N3904-onsemi` and `2N3906-onsemi` cards use the existing
transport law with forward gain roll-off. The 2N3906 also uses base-emitter
recombination. The 2N3904 fit uses the typical absolute gain in onsemi
figure 1 at 5 V. The 2N3906 fit uses the typical gain shape in figure 13 at
−1 V, normalized to 10 mA. Its nominal gain at 10 mA is about 200, within
onsemi's 100–300 limit.

Mean absolute relative gain error over 121 log-spaced points drops from
37.47% to 0.37% over 0.1–100 mA for the 2N3904 and from 12.61% to 4.46%
over 0.102–44.85 mA for the 2N3906. The plots are specified at 25 °C. The
comparison uses the native fixed thermal voltage, equivalent to 26.83 °C.
These fits cover forward DC gain. Saturation, junction charge and temperature
behavior require separate measurements. Existing presets retain their cards.

[onsemi 2N3904](https://www.onsemi.com/pdf/datasheet/2n3904-d.pdf) ·
[onsemi 2N3906](https://www.onsemi.com/pdf/datasheet/2n3906-d.pdf)

## Physical changes to measure

1. Measure bias, forward and reverse Gummel curves, collector families and
   saturation across the operating region each circuit actually visits.
2. Test card-specified base, collector and emitter resistances and base-emitter
   and base-collector capacitance at an operating point. Put constant terms in
   the compiled linear circuit. Record the improvement before adding
   voltage-dependent charge, where both charge and its derivative must be
   consistent. Measure gain and phase, clipping recovery and waveform error.
3. Test high-injection beta roll-off where the collector-current sweep reaches
   it. In germanium stages also check leakage, bias and temperature across
   measured samples. Prefer measured device ranges to one fitted specimen.

Holmes, Holters and van Walstijn found that adding constant small-signal
parasitics changed the Rangemaster and Fuzz Face responses at modest cost.
The denser static Gummel–Poon law cost more. Treat their result as a testable
priority for germanium pedals. In high-current silicon stages, measured
gain roll-off can be the more important correction.

## Solve choices

An isolated BJT coupled to a linear network can be expressed in two junction
voltages with a compiled 2 × 2 port matrix. Reuse the analytic device Jacobian
and the last accepted solution. Prefer a direct solve when compilation proves
the junction equations decouple or reduce to a supported scalar law. The
Lambert W wave mappings in the literature apply to particular topologies.

A coupled BJT stage generally needs a local nonlinear solve. Measure the
total residual evaluations through retries, line searches and fallback, not
only its nominal Newton iteration limit. For common types of circuit, report
median and tail callback time at 48 and 96 kHz, including pot changes,
startup, saturation, and recovery. Keep a residual check on direct paths and
require explicit diagnostics on rejected frames.

Published neural scattering maps have fixed inference cost and evidence on
single-transistor examples. Their approximation and operating range would
need validation for each supported card, topology and control range. The
analytic model gives a reference against which such a map could be checked.

[Holmes et al., DAFx-17](https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holmes-Holters-van-Walstijn-2017-Comparison-of-Germanium-Bipolar-Junction-Transistor-Models-for-Real-time-Circuit-Simulation2.pdf) ·
[Bernardini et al., Lambert W](https://re.public.polimi.it/handle/11311/1003022) ·
[Massi et al., DAFx-23](https://www.dafx.de/paper-archive/2023/DAFx23_paper_60.pdf) ·
[Massi et al., ISCAS 2025](https://re.public.polimi.it/handle/11311/1293368) ·
[ngspice BJT model](https://ngspice.sourceforge.io/docs/ngspice-manual.pdf)
