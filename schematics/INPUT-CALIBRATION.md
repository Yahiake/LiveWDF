# Factory input and output calibration

In the schematic editor, select **Input** and enter either **Input V peak at 0 dBFS** or **Input mV RMS at -18 dBFS**. Both fields set the same reference. At zero Trim, a digital sample of 1 drives the circuit with the chosen peak voltage. A −18 dBFS peak sine drives it with the chosen RMS voltage. **Output** holds the peak volts per full-scale sample used for matched playback. A fixed divisor applies the same gain to every sample, preserving the circuit’s dynamics.

With **Input from → Track** and **Output to → Track (level matched)** (the defaults), a track drives the circuit at its preset input reference and the return uses its matched output reference. The input voltage is `sample × reference_mV / 89.01946957 × 10^(Trim_dB/20)`. Files without an explicit input reference use 1 V peak at full scale. Historic Input `fullScaleVolts` annotations do not set playback gain.

To connect inserts at their simulated terminal voltages, choose **Output to → Next LiveWDF** on the sending instance and **Input from → Previous LiveWDF** on the receiving instance. Both use 13.8 V peak per full-scale sample. This places a +4 dBu studio alignment tone (1.228 V RMS) at −18 dBFS. Floating-point samples above 1.0 can carry higher terminal voltages between inserts. For example, 100 mV peak leaving a pedal arrives as 100 mV peak at the next preset, even if its native input alignment is different. Use unity DAW gain and zero output master/input trim between the two. Choose **Output to → Track (level matched)** at the end to return to the preset’s audition level. The controls also apply in standalone and offline rendering.

## Factory input references

Each of the 90 factory schematics stores its input reference. The [input table](INPUT-LEVELS.csv) lists both the RMS voltage of a −18 dBFS peak sine and the equivalent peak voltage at 0 dBFS. The basis and sources identify the connector, hardware revision, switch setting and any estimate. A maximum input rating describes headroom. An alignment tone describes the operating point for the specified test.

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

The sweep runs the plugin at 48 kHz, mono, 1×, in 128-frame callbacks with 100% wet mix and unity Trim and Output. It measures digital output level. Real-time deadlines and agreement with physical hardware require separate measurements.

On the third excerpt, 65 of 90 presets were within 1 dB of bypass and 84 were within 2 dB. The largest differences were +4.68 dB on the Maestro FZ-1 and +3.96 dB on the ZVEX Fuzz Factory. The highest output sample across both modes was −3.36 dBFS on the Roger Mayer Octavia. The largest difference between the Accurate and Fast Live median levels was 0.15 dB on the MXR M234.

To repeat the measurement, build the `LiveWDF_PluginBench` target and substitute its path below. The script requires Python 3, NumPy, SciPy and FFmpeg.

```sh
python3 tools/calibrate_factory_outputs.py /path/to/LiveWDF_PluginBench fit.csv
python3 tools/calibrate_factory_outputs.py /path/to/LiveWDF_PluginBench accurate.csv --verify
python3 tools/calibrate_factory_outputs.py /path/to/LiveWDF_PluginBench fast.csv --verify --fast-live
```
