# LiveWDF factory preset level audit

The audit measured 81 added presets from commit `ca38519` at 48 kHz and 1× oversampling in the native LiveWDF engine, using saved controls and two input levels per preset. All 162 renders completed with finite output and no load warnings. The measurements are in [results.csv](results.csv). A sample above 0 dBFS indicates output beyond the nominal digital range when using the preset's saved output scale and 0 dB Output Master. LiveWDF hands this level to the host without clipping it.

| Source | Input peak | Application |
|---|---|---|
| Ibanez PF300 chord DI | 120 and 360 mV | Boost, overdrive, distortion and fuzz |
| Strat neck and middle pickup DI | 120 and 360 mV | Clean and modulated guitar effects |
| Synthetic drum phrase and quiet/loud bursts | 0.50 and 2.50 V | Studio line processors and gates |
| Speech fixture | 0.50 and 2.50 V | De-essing and enhancement |
| Speech fixture | 20 and 60 mV | API 312 and MCI JH-400B microphone preamps |

The dry reference uses the same waveform and voltage within each row. RMS gain uses the active phrase with its DC offset removed. For gates, it uses the loud hits while the waveform includes quieter material. The reported digital peaks include the processed tail and use each preset's current voltage-to-digital-full-scale conversion. The 2.50 V line condition is a circuit-terminal voltage, attainable with input gain in the plug-in. These are fixed comparative input levels and leave hardware verification open.

## Pedal settings

The BD-2's revised Level 0.27 in commit `97f8fcc` reduced the 120 mV chord peak from +0.35 to −13.91 dBFS and the 360 mV peak from +1.03 to −13.18 dBFS. Its wet-to-dry RMS gain at those levels moved from +28.94 to +14.66 dB and from +21.39 to +7.12 dB. Both native renders had zero rejected samples.

These are tested candidates using existing physical output controls. I chose +4 dB of wet-versus-dry RMS as a working reference for drives and fuzzes and +2 dB for the compressors on these fixtures. The same settings can produce different gains on different guitars. Control positions use the normalized 0–1 range.

| Preset | Output setting before → candidate | 120 mV wet/dry RMS before → candidate | 120 / 360 mV peaks with candidate |
|---|---|---|---|
| ZVEX Fuzz Factory | Volume 0.500 → 0.033 | +27.70 → +4.19 dB | −17.41 / −17.08 dBFS |
| Maestro FZ-1 Fuzz-Tone 1962 | Volume 0.750 → 0.046 | +28.32 → +4.07 dB | −18.52 / −18.25 dBFS |
| Big Muff Ram’s Head Violet | Volume 0.550 → 0.082 | +20.49 → +3.96 dB | −23.72 / −22.55 dBFS |
| Marshall Guv’nor | Level 0.650 → 0.103 | +20.02 → +4.06 dB | −24.60 / −24.79 dBFS |
| Nobels ODR-1 | Level 0.700 → 0.235 | +18.95 → +4.88 dB | −21.39 / −19.56 dBFS |
| Pro Co RAT LM308 | Volume 0.650 → 0.219 | +18.47 → +4.01 dB | −27.73 / −27.67 dBFS |
| Boss MT-2 Metal Zone | Level 0.650 → 0.226 | +18.12 → +4.83 dB | −24.73 / −23.54 dBFS |
| MXR Dyna Comp | Output 0.780 → 0.480 | +9.91 → +1.99 dB | −12.83 / −8.13 dBFS |
| Diamond CPR-1 | Volume 0.670 → 0.360 | +11.09 → +1.99 dB | −17.15 / −9.73 dBFS |
| Boss CS-3 | Level 0.680 → 0.396 | +10.12 → +2.15 dB | −15.85 / −11.34 dBFS |

All candidates were rendered at 120 and 360 mV. Guv’nor had the same 47 rejected samples as stock at 360 mV, so its solver issue remains. The other candidate runs had zero rejects. Candidate renders and positions are in [candidates.csv](candidates.csv). A second pass used PF300 high bends for fuzz and PF300 palm mutes for other drives. Fuzz Factory and Maestro retained at least 17 dB peak headroom at both input levels. Their 120 mV chord-to-bend reference gains moved from +4.19 to +0.42 dB and from +4.07 to −4.02 dB. [Cross-fixture measurements](crosscheck.csv)

Colorsound Power Boost exceeded 0 dBFS at 360 mV with its existing 1 V output reference. It has no independent level control. Verify the expected input and output reference before changing its boost. Mu-Tron III reached +2.77 dBFS at 360 mV and needs its own filter and output-stage check. Electric Mistress and Small Stone were about 8–9.5 dB quieter on the Strat fixture. Compare them with the source circuits before choosing any compensation. The M234 Level affects the chorus mix, so moving it to meet a generic RMS target would change modulation. DM-2 and Carbon Copy stayed within +0.6 dB of the dry signal at both guitar input levels. HM-2 and PQ-4 had rejected samples even at 120 mV. At 360 mV, DS-1A and Guv’nor also had rejects.

## Studio settings

All 38 line-input presets stayed below 0 dBFS at 0.50 V peak. At 2.50 V peak, 23 crossed their current output mappings. The 40 studio presets use four output references, covering 1 V, 1.736 V, 8 V and 10 V per full scale. Many EQs are close to unity electrically with flat controls. Their 1.736 V output mapping still puts playback approximately 4.8 dB below the unity digital dry reference. A physically near-unity compressor mapped at 8 V is around 18 dB below the same digital dry reference.

The two mic preamps supplied +42.76 dB and +36.58 dB of electrical gain, respectively. Their 1 V digital output mapping is exceeded at 20 mV mic input. Their gain serves a mic-to-line conversion. Establish and verify an input/output voltage reference for line devices and another for mic preamps before choosing factory output trim settings. Leave compressor detector action and mic preamp input gain intact when making playback gain comparisons.

## Orange Squeezer correction

Commit `6978169` restored the GGG original-reference Orange Squeezer circuit, including the current-source bias, drain-to-gate AC feedback, post-coupling detector and 10 kΩ volume trim. At 440 Hz and 120 mV in, the old default generated 55.2% THD. The restored circuit generated 0.33%, with a handwritten ngspice model also giving 0.33%. The LiveWDF and ngspice fundamental gains were −4.38 and −4.43 dB. Both use a selected simulated 2N5457. The ngspice comparison uses its own one-pole op-amp model.

The restored preset had zero rejected samples on two guitar DIs at all five tested input levels from 60 mV to 1 V peak. Its Sire T7 output peak at 1 V input was −10.27 dBFS, compared with +1.12 dBFS before. The level-matched audio shows dry, previous and restored output in that order, with a common playback peak budget. [Editor capture](orange/orange-squeezer-editor.png) · [Tone comparison](orange/orange-squeezer-distortion.png) · [Guitar comparison](orange/orange-squeezer-dry-before-after.mp3)

Guitar-TECHS audio by Hegel Pedroza, Wallace Abreu, Ryan M. Corey and Iran R. Roman, licensed CC BY 4.0. Audition clips are excerpts with level matching or a shared playback gain as stated. [Guitar-TECHS archive](https://zenodo.org/records/14963133) · [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)

The Strat test uses GuitarJam clip 026, CC0. [GuitarJam archive](https://huggingface.co/datasets/Julian-br/GuitarJam)
