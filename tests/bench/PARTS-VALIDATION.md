# Analog part verification

The ngspice overlays for exported equations check LiveWDF and its exporter
against a second solver. The manufacturer references check the modeled behavior
against the cited operating point or curve. All values below are from the
specified fixtures.

| Part | Fixture | Result |
| --- | --- | --- |
| THAT2180 | 11 DC control settings, −100 to +40 dB at 25 °C | Maximum gain difference 9.54 × 10⁻¹⁰ dB from the exported ngspice equations and 0.00695 dB from the nominal manufacturer law. |
| CA3080 | ±15 V, 100 µV differential input, 5 µA–1 mA bias | Maximum difference 0.0304% from exported ngspice. At 500 µA, 9.413 mS against 9.6 mS typical (−1.95%). |
| Log RMS detector | 1 V to 2 V step, 16 ms power averaging | 4.64 nV RMS and 23.3 nV peak difference from exported ngspice. The nominal 16 ms model uses an external timing setting. |
| MN3005 | Held-charge step, nine clock settings from 10–100 kHz, sampled at 48 kHz | Maximum delay difference 116.7 µs from 2048 / f_clock, including frame and clock-edge timing. |
| OP275 | Noninverting gain of 100, 10 mV peak, ±15 V, 2 kΩ load, 192 kHz | At 1 kHz, −0.0027 dB and −0.010° against the corrected ngspice deck. At 20 kHz, −0.349 dB and +0.606°. The time-aligned 20 kHz residual is 49.1 mV RMS including the vendor deck's DC offset. |

The THAT2180 factory trace uses equation 2. The CA3080 nominal line is
proportional to bias and anchored at the table's 500 µA typical value. The RMS
transient uses the manufacturer's 6.1 mV/dB scale and the selected time
constant. The MN3005 export omits clocked charge transfer, so its comparison
uses the Panasonic timing relation. BBD distortion and bandwidth, absolute
noise, and full coverage of the transistor, diode, and op-amp cards are pending.

The OP275 reference is ADI Rev. C, page 11, transcribed with SPICE continuation
lines. Its R8 / R9 values as printed give a DC gain of 10⁻⁶ through that stage
and conflict with the annotated 1.5 MHz pole and 2.7 MHz zero. The comparison
uses R8 = 1 kΩ and R9 = 1.25 kΩ, which recover both labels with the given
47.2 pF capacitor. These are inferred corrections. The factory trace is read
as approximately 40 dB through the plotted audio band in TPC 5. The ngspice
deck retains its input offset and higher poles. LiveWDF uses a compact
backward-Euler dominant-pole model. TPC 4 marks 60.115 dB at 15.309 kHz,
which sets the audio-band gain product to 15.51 MHz. The 9 MHz data-sheet
value describes the unity-gain crossover. Backward Euler still attenuates
the 20 kHz tone in the gain-of-100 fixture.

In the THAT2180 application fixture at its saved −10 dB control setting,
the corrected ngspice deck gives −10.024 dB and −3.235° at 20 kHz. The
native engine gives −10.029 dB and −3.349° at 192 kHz and −10.126 dB and
−8.972° at 48 kHz. The 20 kΩ / 22 pF feedback network accounts for most
of the additional phase shift at 48 kHz.

[THAT2180, Eq. 2](https://thatcorp.com/datashts/THAT_2180-Series_Datasheet.pdf) ·
[Intersil CA3080, FN475](https://media.digikey.com/pdf/Data%20Sheets/Intersil%20PDFs/CA3080%28A%29.pdf) ·
[THAT 2252](https://www.thatcorp.com/datashts/THAT_2252_Datasheet.pdf) ·
[Panasonic MN3005](https://s3.amazonaws.com/tubedepot-com-production/spree/attached_files/87983_PANASONIC_MN3005.pdf) ·
[Analog Devices OP275 Rev. C](https://www.analog.com/media/en/technical-documentation/data-sheets/OP275.pdf)

## Runtime

Release, GCC 14.2, shared Xeon Platinum 8573C host, 48 kHz mono. Each row is
48,000 timed frames after 4,800 warm-up frames, repeated seven times with cases
interleaved. The passive reference took 0.039 µs per frame at the median.
Selected medians and observed pass ranges follow.

| Fixture | Median µs/frame | Min–max µs/frame | Mean Newton iterations |
| --- | ---: | ---: | ---: |
| MN3005 at 100 kHz | 0.154 | 0.068–0.571 | 0 |
| Photoresistor | 0.277 | 0.204–0.725 | 0 |
| THAT2180 | 0.271 | 0.183–0.521 | 0 |
| Log RMS detector | 0.319 | 0.153–1.584 | 0 |
| CA3080 | 2.429 | 2.068–3.969 | 1.642 |
| CA3080 + eight clippers, default | 14.459 | 10.323–19.978 | 2.138 |
| CA3080 + eight clippers, forced joint | 14.624 | 10.331–20.730 | 2.138 |
| RC4558 | 1.049 | 0.582–1.636 | 0 |
| RC4558 Class AB | 3.346 | 2.266–4.790 | 3.513 |
| OP275 | 0.945 | 0.756–1.321 | 0 |
| THAT2180 + OP275 application fixture | 1.458 | 1.112–2.527 | 0 |
| MXR Dyna Comp circuit | 16.347 | 15.470–31.991 | 2.109 |

The diagnostic pass found zero solver fallbacks and zero rejected frames in
these fixtures. It gives the same iteration and evaluation counts for the two
eight-clipper cases. Wall-clock ranges reflect the shared host and describe
average cost per pass.

## Reproduce

Build the `livewdf_tests` and `livewdf_bench_parts` targets in Release. Run
`livewdf_tests '[parts-validation]' --reporter compact` with ngspice on PATH
or `NGSPICE` set to its executable. The test log prints the CSV paths. To
include the separate OP275 check, set `LIVEWDF_OP275_SPICE_MODEL` to a local
compatible `.SUBCKT OP275` file and run `livewdf_tests '[op275-reference]'`.
Run `python tests/bench/plot_parts.py CSV_DIR OUTPUT_DIR` to redraw the
overlays and residuals.

Run `livewdf_bench_parts` for isolated fixtures. Pass the paths of the
THAT2180 application fixture and Dyna Comp schematic as positional arguments
to include the complete circuits.
