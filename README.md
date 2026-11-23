# LiveWDF pedal listening examples

Each of the 29 tracks plays a dry guitar excerpt, pauses for about one third of a second, then plays the same input through the named LiveWDF circuit. The reel follows the same order and has embedded chapter markers. The recordings are mono at 48 kHz.

## Reel index

| # | Pedal | Dry starts | Wet starts | Cue |
|---:|---|---:|---:|---|
| 01 | Boss SD-1 Super OverDrive | 00:00.000 | 00:05.010 | Check the mids and pick attack. |
| 02 | Boss DS-1A Distortion | 00:10.270 | 00:14.800 | Listen for the harder edge on short hits. |
| 03 | Pro Co RAT LM308 | 00:19.580 | 00:24.110 | Listen for the heavier clipping and high-end rolloff. |
| 04 | Boss MT-2 Metal Zone | 00:28.890 | 00:33.420 | Listen for the strong filtering and sustain between attacks. |
| 05 | Marshall Guv'nor | 00:38.200 | 00:42.830 | Listen for the difference in gain between hard and soft picking. |
| 06 | EHX Big Muff Pi Ram's Head Violet | 00:47.710 | 00:52.200 | Listen for sustain through the bends. |
| 07 | Sola Sound Tone Bender Professional Mk II | 00:56.940 | 01:01.430 | Listen for the harder decay and texture as notes trail off. |
| 08 | MXR Micro Amp | 01:06.170 | 01:10.800 | The same playback gain lets the level boost come through. |
| 09 | Boss GE-7 Equalizer | 01:15.680 | 01:20.690 | A broad mid scoop, deepest at 800 Hz. |
| 10 | Boss PQ-4 Parametric EQ | 01:25.950 | 01:30.960 | The boosted band sweeps from roughly 125 Hz to 1 kHz and back. |
| 11 | Dan Armstrong Orange Squeezer | 01:36.220 | 01:40.850 | Listen for restrained peaks and the lift on quieter notes. |
| 12 | MXR Dyna Comp CA3080 | 01:45.730 | 01:50.360 | Listen for the change in attack and the held level. |
| 13 | Boss NS-2 Noise Suppressor | 01:55.240 | 02:00.170 | Listen between the muted hits for the test hum being closed down. |
| 14 | Musitronics Mu-Tron III | 02:05.350 | 02:09.980 | Listen for the filter sweep responding to pick strength. |
| 15 | MXR Phase 90 Script | 02:14.860 | 02:19.870 | The held chord makes the moving notches easier to hear. |
| 16 | Shin-ei Univox Uni-Vibe 915 | 02:25.130 | 02:30.140 | Listen for the pulsing phase shift under the chords. |
| 17 | Boss TR-2 Tremolo | 02:35.400 | 02:40.410 | The held notes expose the steady amplitude modulation. |
| 18 | Boss CE-2 Chorus | 02:45.670 | 02:50.680 | Listen to the pitch movement in the sustained portion of the chords. |
| 19 | EHX Small Clone MN3007 | 02:55.940 | 03:00.950 | Listen for the more pronounced pitch motion. |
| 20 | Boss DM-2 Analog Delay | 03:06.210 | 03:11.300 | The revised compander lets the repeats decay with less distortion. |
| 21 | DOD 250 Overdrive Preamp Gray | 03:16.640 | 03:21.570 | Listen for the LM741 and silicon clipping on hard attacks. |
| 22 | Marshall Bluesbreaker Mk I | 03:26.750 | 03:31.380 | Listen for the change in breakup between light and hard picking. |
| 23 | Boss BF-2 Flanger | 03:36.260 | 03:41.270 | Held chords expose the comb-filter sweep and feedback resonance. |
| 24 | Boss BD-2 Blues Driver | 03:46.530 | 03:51.160 | Listen for the transition from clean attack to clipping. |
| 25 | Colorsound Power Boost 18V | 03:56.040 | 04:00.670 | The combined bass and treble network reshapes the driven transistor stage. |
| 26 | Nobels ODR-1 Natural Overdrive | 04:05.550 | 04:10.560 | Full chords bring out the Spectrum circuit and the soft clipping. |
| 27 | EHX Electric Mistress 9V | 04:15.820 | 04:20.830 | Listen for the slow flanging motion across sustained chords. |
| 28 | Boss HM-2 Heavy Metal | 04:26.090 | 04:30.620 | Palm-muted hits show the clipping and the Color Mix resonance. |
| 29 | Roger Mayer Octavia Transistor | 04:35.400 | 04:39.890 | High-string notes expose the octave-up and fuzz. |

## Source and signal levels

Each circuit is rendered from the PR branch. Dry and wet use shared playback gain, with a separate RMS trim on the two EQ wet halves to highlight tonal differences. A shared zero-phase 22 Hz high-pass and short edit fades are applied to both halves. The renders capture the circuit voltages before plugin output normalization. There is no amp or cab processing.

Knob values below are normalized positions from 0 to 1. Settings say **Factory** when the saved circuit was used as-is.

| # | Direct input and time in the 48 kHz fixture | Input calibration | Knob changes | Shared gain and EQ trim | Rejects in warm-up / playback |
|---:|---|---:|---|---:|---:|
| 01 | PF300 major chords (7.72–12.40 s) | 1 V/FS | Factory | +4.5 dB | 0 / 0 |
| 02 | PF300 palm mutes, six shortened takes (0.00–0.70 s, 3.77–4.47 s, 7.77–8.47 s, 11.77–12.47 s, 15.77–16.47 s, 19.77–20.47 s) | 2 V/FS | Factory | +5.9 dB | 0 / 0 |
| 03 | PF300 palm mutes, six shortened takes (0.00–0.70 s, 3.77–4.47 s, 7.77–8.47 s, 11.77–12.47 s, 15.77–16.47 s, 19.77–20.47 s) | 2 V/FS | Volume 0.52 | +1.7 dB | 0 / 0 |
| 04 | PF300 palm mutes, six shortened takes (0.00–0.70 s, 3.77–4.47 s, 7.77–8.47 s, 11.77–12.47 s, 15.77–16.47 s, 19.77–20.47 s) | 2 V/FS | Level 0.48 | +1.1 dB | 0 / 0 |
| 05 | Sire T7 performance 11 (1.53–5.83 s) | 1 V/FS | Level 0.42 | +1.0 dB | 0 / 0 |
| 06 | PF300 high-string bends (1.06–5.22 s) | 1 V/FS | Volume 0.4 | -2.5 dB | 0 / 0 |
| 07 | PF300 high-string bends (1.06–5.22 s) | 1 V/FS | Factory | +0.1 dB | 0 / 0 |
| 08 | GuitarJam 026 (0.00–4.30 s) | 1 V/FS | Factory | +3.6 dB | 0 / 0 |
| 09 | PF300 major chords (7.72–12.40 s) | 0.5 V/FS | 400 Hz 0.22, 800 Hz 0.08, 1.6 kHz 0.25 | +15.7 dB, +6.3 dB wet trim | 0 / 0 |
| 10 | PF300 major chords (5.00–9.68 s) | 0.244 V/FS | Middle Frequency 0.10 → 0.90 → 0.10, Middle Gain 0.87 | +22.3 dB, -4.4 dB wet trim | 112 / 0 |
| 11 | Sire T7 performance 11 (1.53–5.83 s) | 1 V/FS | Factory | +2.2 dB | 0 / 0 |
| 12 | Sire T7 performance 11 (1.53–5.83 s) | 1 V/FS | Factory | +1.8 dB | 0 / 0 |
| 13 | PF300 palm mutes with test hum (15.67–20.27 s) | 2 V/FS | Factory | +9.0 dB | 0 / 0 |
| 14 | Sire T7 performance 11 (1.53–5.83 s) | 1.5 V/FS | Factory | +3.0 dB | 0 / 0 |
| 15 | PF300 major chords (7.72–12.40 s) | 1 V/FS | Factory | +8.3 dB | 0 / 0 |
| 16 | PF300 major chords (7.72–12.40 s) | 1 V/FS | Factory | +10.1 dB | 0 / 0 |
| 17 | PF300 major chords (7.72–12.40 s) | 1 V/FS | Factory | +10.1 dB | 0 / 0 |
| 18 | PF300 major chords (7.72–12.40 s) | 1 V/FS | Factory | +9.3 dB | 0 / 0 |
| 19 | PF300 major chords (7.72–12.40 s) | 0.6 V/FS | Factory | +14.5 dB | 0 / 0 |
| 20 | Sire T7 performance 11 with echo tail (1.53–5.04 s, then 1.25 s with the input silent) | 0.75 V/FS | Factory | +19.0 dB | 0 / 0 |
| 21 | PF300 palm mutes (15.67–20.27 s) | 1 V/FS | Factory | +10.0 dB | 0 / 0 |
| 22 | Sire T7 performance 11 (1.53–5.83 s) | 1 V/FS | Factory | +10.3 dB | 0 / 0 |
| 23 | PF300 major chords (7.72–12.40 s) | 0.65 V/FS | Factory | +12.2 dB | 0 / 0 |
| 24 | Sire T7 performance 11 (1.53–5.83 s) | 0.9 V/FS | Factory | -11.9 dB | 0 / 0 |
| 25 | Sire T7 performance 11 (1.53–5.83 s) | 1 V/FS | Factory | -1.6 dB | 0 / 0 |
| 26 | PF300 major chords (7.72–12.40 s) | 0.85 V/FS | Factory | +0.5 dB | 0 / 0 |
| 27 | PF300 major chords (7.72–12.40 s) | 0.65 V/FS | Factory | +13.8 dB | 0 / 0 |
| 28 | PF300 palm mutes, six shortened takes (0.00–0.70 s, 3.77–4.47 s, 7.77–8.47 s, 11.77–12.47 s, 15.77–16.47 s, 19.77–20.47 s) | 0.3 V/FS | Factory | +6.5 dB | 0 / 0 |
| 29 | PF300 high-string bends (1.06–5.22 s) | 0.85 V/FS | Factory | +3.3 dB | 0 / 0 |

Each circuit received 1 second of silent warm-up before the portion used for playback.
 All 29 playback excerpts rendered without a rejected sample.

These rejects occurred during discarded silent warm-up.

- Boss PQ-4 Parametric EQ had 112 rejects during warm-up.

## Recording credits

- **Guitar-TECHS**, Hegel Pedroza, Wallace Abreu, Ryan M. Corey, and Iran R. Roman. The PF300 uses player 1 in P1_techniques.zip and P1_chords.zip, and the Sire T7 uses player 3 in P3_music.zip. The PF300 chords fixture starts 8 s into directinput_Set1_maj.wav, the bends fixture starts 160 s into directinput_Bendings.wav, and the palm-mute fixture starts at 0 s in directinput_PalmMute.wav. Sire T7 is directinput_11.wav. The palm-muted comparison for several drives cuts the rests between six actual hits. The NS-2 source adds synthetic 50/100 Hz hum equally to both halves to show the gate closing. The dataset is licensed [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). [Source archive](https://zenodo.org/records/14963133)
- **GuitarJam**, test split clip 026. The Fender Stratocaster direct input was recorded through a Focusrite Scarlett Solo and is resampled from 44.1 to 48 kHz for this example. The dataset is licensed CC0 1.0. [Dataset](https://huggingface.co/datasets/Julian-br/GuitarJam) · [Original clip](https://huggingface.co/datasets/Julian-br/GuitarJam/resolve/main/guitar_jam/test/026.wav)
