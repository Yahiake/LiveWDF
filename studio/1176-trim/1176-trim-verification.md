# UREI 1176LN Rev D distortion trim

The Rev D service schematic for serial numbers 1239–2331 shows R85 and R86 in series between the Q2 emitter and the C9 feedback junction. Figure 3 of Universal Audio's 2-1176 manual shows the same connection more clearly. The previous preset connected both resistors to ground. The revised preset joins their ground ends into the factory feedback path and keeps the 150 Ω R85 and fixed 30 Ω R86 values.

- [Original Rev D service drawing, PDF page 52](https://www.sowter.co.uk/schematics/1176manual.pdf)
- [Universal Audio gain-reduction diagram, printed page 26](https://media.uaudio.com/assetlibrary/2/-/2-1176_manual.pdf)

## Verification

The engine benchmark uses a 120 ms zero-input warmup followed by 180 ms capture at each of 48, 96 and 192 kHz. It drives a 220 Hz, 997 Hz, 2.1 kHz and 5.9 kHz multitone at several amplitudes. Tests use one engine on the same CPU core, the 60-iteration default and the 1e-9 V residual target. The timings are case-level elapsed wall time per frame. They do not measure the host or stereo callback.

| Sample rate | Signal peak | Previous µs/frame | Corrected µs/frame | Previous rejected frames | Corrected rejected frames |
|---|---:|---:|---:|---:|---:|
| 48 kHz | 0 V | 81.78 | 14.13 | 0 | 0 |
| 48 kHz | 0.12 V | 54.31 | 15.65 | 0 | 0 |
| 96 kHz | 0 V | 73.19 | 5.68 | 0 | 0 |
| 96 kHz | 0.12 V | 60.63 | 17.68 | 0 | 0 |
| 192 kHz | 0 V | 263.29 | 5.52 | 2,424 | 0 |
| 192 kHz | 0.12 V | 23.36 | 11.09 | 24 | 0 |

After 120 ms warmup at 48 kHz zero input, output RMS fell from about 1 mV to about 1.3 µV. Average Newton work over the same window dropped from 13.80 to 1.00 iteration per frame. The earlier result contained a strong near-Nyquist oscillation even with no input. The one-step capacitor and inductor state Jacobian had two unstable poles before the wiring change. Both disappeared with the corrected resistor connection.

The same two-resistor change was checked in a matched ngspice transient deck. The export's four generic K=1 entries were replaced with each winding matrix's L12/sqrt(L11*L22) to retain the LiveWDF model's existing leakage. At zero input, adaptive Gear showed 6.94 V peak-to-peak at output and 7.35 V at Q7 collector in the previous model's first 100 µs. Over the first 10 ms in the corrected model, the same nodes measured under 1 nV and 80 nV peak-to-peak. The continuous reference uses the model's circuit and approximate device laws.

A +4 dBu, 250 ms, 1 kHz burst produced 12.59 dB of steady gain reduction at the corrected default and approximately 0 dB with detector drive near minimum. After 1.5 ms, the fast and slow attack settings produced 11.39 and 0.92 dB of reduction. Fifty milliseconds after the burst, the fast and slow release settings retained 0.63 and 8.75 dB of reduction. These describe preset controls. The continuous detector drive and nominal trim still approximate the hardware controls and component tuning.

## Playback

The two-level demo repeats the same synthetic drum phrase at two amplitudes, with no dynamics processing on the dry excerpt. Maximum modeled input is 2.5 V. The quiet and loud phrase levels were 18.14 dB apart dry and 11.89 dB apart corrected. The previous model gave 14.42 dB separation and substantially higher gain. Each preview matches overall RMS between excerpts with a constant gain per excerpt, so the dynamic range within each excerpt is preserved.

A second validation render used a dry Sire T7 excerpt from Guitar-TECHS (CC BY 4.0, test fixture source documentation). Both musical fixtures completed with zero rejected frames at 48 kHz. The full preset remains an approximation of the hardware's transformers, switches and part selection.
