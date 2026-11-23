# BBE 482i high-band correction

Tom Farrand’s Channel B analysis places the peak detector after the broadband input buffer and applies its gain to the high-band branch. It shows pin 7 with R21 3.32 kΩ in series with C15 0.22 µF and pin 4 with separate C13 2.2 µF. The earlier preset combined the timing network into a 22 µF parallel RC, used a stiff peak diode equivalent and reached most of its gain adjustment in approximately one sample. The revised sheet keeps the two timing pins, uses a bounded diode-limited gain curve and calibrates the acquisition and release impedances. The audio filter, summing amplifier and both original control positions are preserved. The peak diode and the internal gain curve are functional equivalents. Hardware measurements of this unit remain unavailable.

- New Japan Radio NJM2153 datasheet, electrical-characteristics table, application circuit and gain graph https://www.electronicsdatasheets.com/download/51b18948e34e24ce29000725.pdf?format=pdf
- Tom Farrand 8 September 2004 Rev. 0 Channel B analysis https://guitar-gear.ru/forum/index.php?app=core&attach_id=29567&module=attach&section=attach

## Gain and timing

The reference comparison sets R19 and R31 to 13.3 kΩ with both controls fully up and keeps the remaining 482i network, including the board’s 28.2 kΩ summing feedback resistor. The complete NJM2153 application fixture has its own summing network. The 482i preset itself retains the traced 10 kΩ fixed resistances.

| 10 kHz input | NJM2153 typical | Original reference-resistor run | Revised reference-resistor run |
| --- | ---: | ---: | ---: |
| 0 dBV | +12.0 dB | +11.85 dB | +12.20 dB |
| −35 dBV | +9.2 dB | +8.04 dB | +9.02 dB |
| −50 dBV | 0.0 dB | −0.52 dB | +0.21 dB |

The revised 20 kHz silence-to-0 dBV attack reached 90% of the gain-command span in 136, 104, 115 and 109 µs at 44.1, 48, 96 and 192 kHz. At 48 kHz the original model reached 90% in 21 µs. A 10 kHz 0-to-−50 dBV step reached 1 dB of its settled gain command in 498 ms after revision, compared to about 403 ms originally. The datasheet gives 100 µs and 500 ms typical and gives the drive changes used in those tests. The measured model endpoint definitions are stated here for repeatability.

## Exact two-tone and level-step comparisons

Both panel controls were at 0.5 at 48 kHz for the two-tone file. A continuous 6 kHz −45 dBV signal receives 220 Hz bass at the given amplitudes for successive one-second segments. The readings use coherent 6 kHz gain over the final 180 ms of each segment.

| 220 Hz input peak | Original 6 kHz gain | Revised 6 kHz gain |
| ---: | ---: | ---: |
| 0.00 V | −4.55 dB | −4.20 dB |
| 0.10 V | +3.37 dB | +3.04 dB |
| 0.00 V | −4.55 dB | −4.20 dB |
| 0.25 V | +3.36 dB | +3.43 dB |
| 0.00 V | −4.55 dB | −4.20 dB |

The quiet 6 kHz tone still changes by about 7.25 and 7.63 dB with bass in the revised model. A broadband detector deliberately continues to respond to bass. The source drawing establishes that signal tap. The exact amount of cross-modulation in a physical unit is uncertain.

| Time after the final bass burst | Original 6 kHz gain | Revised 6 kHz gain |
| ---: | ---: | ---: |
| 100 ms | +3.37 dB | +2.83 dB |
| 200 ms | −2.13 dB | +1.14 dB |
| 300 ms | −4.54 dB | −3.34 dB |
| 400 ms | −4.54 dB | −4.14 dB |

The native board values at Process/Lo 0.5 give revised 10 kHz gain +3.54, +0.87 and −6.78 dB at −20, −35 and −50 dBV. The original measured +3.61, −0.05 and −7.31 dB. After the final 0-to-−50 dBV step in the exact 14-second fixture, the revised output at 200, 300, 400, 500 and 650 ms was +3.48, +2.66, −2.17, −6.16 and −6.76 dB. The original gave +3.61, −0.53, −6.69, −7.31 and −7.31 dB. Process maximum with Lo at 0.5 also passed the level-step test.

## Solver and audition

The committed build uses 20 root ports, the same number as the original. There are no new engine solver paths. The final diode-limited current stages use the existing nonlinear diode evaluator, linear gain blocks and passive networks. The native loader recovered 43/43 intended nets. All 5,380,125 revised static, transient, two-tone, level-step and program samples completed without rejection. In a discarded three-op-amp curve candidate, the 44.1 kHz static sweep rejected two frames at startup, at about 23 and 68 µs. The committed circuit passed that sweep. On the 14-second 48 kHz level-step fixture, three alternating runs gave a median 3.26 s CPU original and 2.15 s revised on the same local runner. This includes the same test capture for both versions.

`BBE-482i-HF-comparison.wav` uses the same four-second sources, both knobs at 0.5 and a common 0.61929 V per full-scale playback conversion, with short endpoint fades and 250 ms gaps.

| Start | Pass |
| ---: | --- |
| 0.00 s | Guitar dry, 0.14 Vpeak input |
| 4.25 s | Guitar through original model |
| 8.50 s | Guitar through revised model |
| 12.75 s | Drums dry, 0.38 Vpeak input |
| 17.00 s | Drums through original model |
| 21.25 s | Drums through revised model |
