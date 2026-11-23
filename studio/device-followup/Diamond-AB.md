# Diamond CPR-1 physical LM358 review

The review fixture uses OP275 sections at IC1A/B and legacy LM358 sections at IC2A/B of the Aion Quartz reference. Both LM358 pole limits are set for the schematic’s 8.65 V internal supply. The physical stage resolves source, sink and pulldown currents. Its worst digital timing on this comparison exceeds a 48 kHz mono frame, while its gain-reduction difference is small. Keep the factory preset at its current settings.

## Guitar comparison

The native engine rendered the same centered controls and guitar take at 48 kHz, with 250 ms of silence followed by 1.25 seconds of recorded DI. The CPU figures are process time per frame over the entire 72,000-frame render, median of three passes in rotated order on a shared Intel Xeon Platinum 8573C. Audio measurements exclude the leading silence. Each of the 18 renders finished with zero rejected or nonconverged frames.

| Input peak | Configuration | CPU µs/frame | Output RMS | Peak optical reduction |
|---:|---|---:|---:|---:|
| 0.120 V | Stock TL072 | 11.27 | 97.713 mV | 4.189 dB |
| 0.120 V | OP275 at IC1, TL072 at IC2 | 10.58 | 97.703 mV | 4.192 dB |
| 0.120 V | OP275 at IC1, physical LM358 at IC2 | 22.77 | 97.699 mV | 4.201 dB |
| 0.481 V | Stock TL072 | 11.03 | 178.531 mV | 14.217 dB |
| 0.481 V | OP275 at IC1, TL072 at IC2 | 11.57 | 178.504 mV | 14.214 dB |
| 0.481 V | OP275 at IC1, physical LM358 at IC2 | 26.80 | 178.459 mV | 14.217 dB |

| Input peak | Stock-to-physical output level change | Aligned residual relative to stock | Peak instantaneous optical-reduction difference |
|---:|---:|---:|---:|
| 0.120 V | −0.00130 dB | −52.75 dB | 0.0135 dB |
| 0.481 V | −0.00351 dB | −52.46 dB | 0.0275 dB |

Comparing OP275-only with the full physical configuration isolates the LM358 contribution. The aligned residuals were −75.39 dB and −68.45 dB at the two input levels. Optical reduction is the calculated midband attenuation relative to the dark VTL5C3, with 100 kΩ series resistance and the 22 MΩ and 476 kΩ parallel load paths. It is measured from the native engine’s instantaneous photocell resistance. The listening comparison uses common gain across the two renders.
