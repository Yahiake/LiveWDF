# Factory input and output calibration

In the schematic editor, select **Input** and enter either **Input V peak at 0 dBFS** or **Input mV RMS at -18 dBFS**. Both fields set the same reference. At zero Trim, a digital sample of 1 drives the circuit with the chosen peak voltage. A −18 dBFS peak sine drives it with the chosen RMS voltage. **Output** holds the peak volts per full-scale sample used for matched playback. A fixed divisor applies the same gain to every sample, preserving the circuit’s dynamics.

To set the listening level of a circuit you’re editing, select its main **Output** and click **Calibrate output**. The editor drives a separate copy of the current circuit with a −9 dBFS peak multitone at 48 kHz, matches its AC RMS level to the dry signal, and reserves 1 dB of peak headroom on that test. The result appears in **Matched output V peak at 0 dBFS**. You can type your own value there, undo the change, and save the circuit. Calibration runs only when requested. The factory catalog uses the multi-passage sweep described below.

With **Input from → Track** and **Output to → Track (level matched)** (the defaults), a track drives the circuit at its preset input reference and the return uses its matched output reference. The input voltage is `sample × reference_mV / 89.01946957 × 10^(Trim_dB/20)`. Files without an explicit input reference use 1 V peak at full scale. Historic Input `fullScaleVolts` annotations do not set playback gain.

To connect inserts at their simulated terminal voltages, choose **Output to → Next LiveWDF** on the sending instance and **Input from → Previous LiveWDF** on the receiving instance. Both use 13.8 V peak per full-scale sample. This places a +4 dBu studio alignment tone (1.228 V RMS) at −18 dBFS. Floating-point samples above 1.0 can carry higher terminal voltages between inserts. For example, 100 mV peak leaving a pedal arrives as 100 mV peak at the next preset, even if its native input alignment is different. Use unity DAW gain and zero output master/input trim between the two. Choose **Output to → Track (level matched)** at the end to return to the preset’s audition level. The controls also apply in standalone and offline rendering.

## Source impedance and output load

The player shows these controls on presets that define a range. **Source R** sets the series resistance of the device feeding the input. **Output load** sets the shunt resistance of the device receiving the output. The readout shows the effective external resistance. An ideal voltage source reads **0 Ω**. **Open** means no extra load across the output. The circuit's own input and output components still affect its impedance. Double-click to recall the preset's default.

| Factory preset | Source default (adjustment range) | Output load default (adjustment range) |
| --- | --- | --- |
| Cry Baby GCB-95, Mu-Tron III | 5.9 kΩ (100 Ω–150 kΩ) | 1 MΩ (10 kΩ–1 MΩ) |
| Fuzz Face, Tone Bender Mk II, FZ-1, Fuzzrite, Big Muff, Colorsound Power Boost | 5.9 kΩ (100 Ω–150 kΩ) | 1 MΩ (5 kΩ–1 MΩ) |
| ZVEX Fuzz Factory, Roger Mayer Octavia | 5.9 kΩ (100 Ω–150 kΩ) | — |
| API 312 Preamp VP312 Rev A | 150 Ω (20–600 Ω) | 600 Ω (600 Ω–100 kΩ) |
| Neve 1073 line preamp | 150 Ω (10–600 Ω) | 600 Ω (600 Ω–100 kΩ) |
| Soundcraft Series 600 | 600 Ω (10–600 Ω) | 22 kΩ (10–100 kΩ) |
| UREI 1176LN Rev D, UREI LA-3A, Teletronix LA-2A, Fairchild 660, Altec 436C, Gates Sta-Level | — | 600 Ω (600 Ω–100 kΩ) |
| API 550A, API 560, Pultec EQP-1A, Pultec MEQ-5, UREI 545 | — | 600 Ω (600 Ω–100 kΩ) |
| dbx 160 VU, dbx 902, SPL Transient Designer 9842 | — | 10 kΩ (10–100 kΩ) |
| Manley Massive Passive, GML 8200, Mäag EQ4 | — | 100 kΩ (10–100 kΩ) |

The instrument defaults assume a passive guitar feeding a pedal and, where the output is adjustable, a high-impedance amplifier input. The 5.9 kΩ series resistance is a resistive approximation using the winding resistance of a [Fender Pure Vintage ’65 Strat pickup](https://www.fender.com/products/pure-vintage-65-strat-pickups). Its 2.6 H inductance, the guitar controls and the cable produce a frequency-dependent source impedance. To model that interaction, use those components in the schematic. A buffered pedal driving the same input is a different source. For example, the [BOSS DS-2](https://www.boss.info/global/products/ds-2/specifications/) specifies a 1 kΩ output and a 1 MΩ input. Studio presets with a drawn source or termination use the resistor value on that particular schematic.

LiveWDF inserts exchange voltage. To model the loading between them, choose the receiving device's input resistance as the sender’s output load and avoid adding the same voltage divider a second time on the receiver. Numeric resistance changes are smoothed. Adding or removing the external resistor changes the topology and resets stored energy, including effect tails. Output level matching uses the preset's saved gain while you adjust the termination.

For a custom preset, set `values.externalMinOhms` and `values.externalMaxOhms` on its main input or output. To adjust a resistor already drawn in the schematic, set `properties.externalResistor` to its component ID. Its saved value is the default. For a separate external resistor, `values.externalDefaultOhms` sets the default within the range. Omitting it starts with an ideal source or an open external load.

## Factory input references

Each factory schematic stores its input reference. The [input table](INPUT-LEVELS.csv) lists both the RMS voltage of a −18 dBFS peak sine and the equivalent peak voltage at 0 dBFS. The basis and sources identify the connector, hardware revision, switch setting and any estimate. A maximum input rating describes headroom. An alignment tone describes the operating point for the specified test.

| Preset | Input at −18 dBFS, sine RMS | Input at 0 dBFS, peak | Basis |
| --- | ---: | ---: | --- |
| BOSS BD-2 | 77.5 mV | 0.871 V | Published −20 dBu guitar input. This same value serves as an estimate for guitar inputs without a published reference. |
| MCI JH-400B mic preamp | 7.74 mV | 0.08695 V | MCI’s nominal distortion test at 34 dB gain yields about 388 mV RMS out. |
| Neve 1081 Gold Label EQ | 137.82 mV | 1.548 V | Drawing EB20031 marks −15 dBm at the internal filter entrance. The preset starts at this point. |
| BBE 482i | 245.08 mV | 2.753 V | The specified −10 dBu operating level on channel B. |
| dbx 160 VU | 1.22829 V | 13.798 V | Factory 0 VU alignment. The threshold and ratio still control gain reduction. |

The Helios Musicland EQ, SSL 82E242 EQ and Pye 4060 use 0.775 V RMS as a working reference. The table identifies the original drawings and desk alignment evidence for each. A physical connection to a console section feeds its modeled node directly. For example, a +4 dBu signal needs 19 dB of attenuation to reach the Neve 1081 filter’s −15 dBu operating point. The preset input reference handles that when feeding it directly from a track.

## Output sweep

At the preset’s saved controls, **Track (level matched)** uses one fixed output divisor. The calibration compares K-weighted RMS level on active 50 ms frames with the digital bypass. Guitar circuits use recorded DI passages. Studio circuits use a repeatable broadband bus or voiced/sibilant signal. Gates are matched over the strongest input frames so their closed intervals do not drive up the makeup gain.

Two excerpts per preset are played at −12, −9 and −6 dBFS peak, with 400 ms of silence between excerpts. The fit averages the two level differences in dB at each drive and takes the median of the three results. A peak constraint reserves 1 dB of digital headroom on the fitting passages. A third excerpt at −9 dBFS tests how the setting translates to other material. A positive error in the [output measurements](../tests/bench/FACTORY-OUTPUT-LEVELS.csv) means the effect returned more level than bypass. Drive, release, EQ and playing style can move that value away from zero.

Presets marked `amp` in the output table fit on palm mutes and strummed chords from an Ibanez PF300 DI, then verify on a separate Sire T7 performance. The dry preamps end at the loaded output indicated in each schematic. The Tweed Champ includes its 6V6 output stage, transformer and resistive speaker load. Turn **Power Amp** off beside **Circuit** to use its AC-coupled preamp output with the original 220 kΩ grid load. The switch is present on presets with a preamp tap. Both settings use the same input reference and have their own output match. Use **Cabinet** for speaker coloration in either setting.

To add this option to a schematic, set `properties.powerAmp` to `"1"` on the components to remove, including any feedback from the power stage. On a retained component at the preamp output, set `properties.preampTap` to its signal pin name and `values.preampFullScaleVolts` to its calibrated output divisor. A shared rail can use `values.preampDc` to maintain its voltage under the reduced load.

The sweep runs the plugin at 48 kHz, mono, 1×, in 128-frame callbacks with 100% wet mix and unity Trim and Output. It measures digital output level. Real-time deadlines and agreement with physical hardware require separate measurements.

On the third excerpt, 78 of 111 presets were within 1 dB of bypass and 104 were within 2 dB, including 18 of the 20 amps from the mixed DI sweep. The largest differences were +4.67 dB on the Maestro FZ-1 and +4.01 dB on the ZVEX Fuzz Factory. The highest output sample across both modes was −3.25 dBFS on the Roger Mayer Octavia. The largest difference between the Accurate and Fast Live median levels was 0.15 dB on the MXR M234.

The Silver Jubilee uses 0.95 dB more output headroom than the mixed DI fit to accommodate harder-picked passages.

To repeat the measurement, build the `LiveWDF_PluginBench` target and substitute its path below. The script requires Python 3, NumPy, SciPy and FFmpeg. For the mixed amp DI sweep, pass `--amp-di` and select the desired presets with `--filter` or `--catalog`.

```sh
python3 tools/calibrate_factory_outputs.py /path/to/LiveWDF_PluginBench fit.csv
python3 tools/calibrate_factory_outputs.py /path/to/LiveWDF_PluginBench accurate.csv --verify
python3 tools/calibrate_factory_outputs.py /path/to/LiveWDF_PluginBench fast.csv --verify --fast-live
python3 tools/calibrate_factory_outputs.py /path/to/LiveWDF_PluginBench champ-preamp.csv --amp-di --preamp-only --filter "Tweed Champ" --verify
```
