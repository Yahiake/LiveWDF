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
