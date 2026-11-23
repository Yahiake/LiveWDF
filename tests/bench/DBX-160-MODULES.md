# Original dbx 160/161 modules

The 160/161 factory bill of materials PL260182 identifies M1 as VCA 200 and M2 as RMS 209. The factory procedure also discusses an earlier 205 detector. The 207 is a documented service replacement with different pins. The two LiveWDF parts target the 200/209 combination with centered, calibrated controls at room temperature.

## VCA 200

The dbx internal drawing 110557, revision 01, dated March 27, 1975 shows the input JFET, error amplifier, VBE multiplier and two complementary matched transistor pairs. Both the input and output pair expose their base connections. The positive and negative current ratios follow exp((Pi−Po)/VT) and exp((No−Ni)/VT) respectively. The 160 links Pi to No and grounds Ni, with R25 and the symmetry trimmer returning Po to ground. This gives about **2.96 mV/dB at 25 °C**. Applying the later THAT2180's 6.1 mV/dB control constant to these pins halves the gain change.

The native part solves the current split directly. The input pair carries the signal current as its collector-current difference, while the product sets the standing current. The standing current at centered symmetry in the independent reference circuit is 5.17 µA. Displacing Ni relative to Pi changes that current exponentially. Independent base controls retain signal asymmetry and DC output from offset trim. Linked equal controls cancel the standing currents analytically. Output collector/base junctions conduct when the following current-to-voltage amplifier loses its virtual earth. A 10 Ω input sense resistance approximates the module's input servo. The fixed-supply reduction omits individual device mismatch, thermal drift, noise, the input servo's RF dynamics and supply-current draw.

The independent ngspice reference follows the internal wiring, including the 560 pF compensation capacitor, and uses the published Philips BC109C/BC179B and NSC 2N3687 cards. Siliconix cross-references KE3687 to 2N3687. R1 and R10 are factory-selected parallel resistors. Their fitted equivalent, 27.071 kΩ, nulls input offset with these cards at nominal ngspice temperature. The actual selected R2/R13 values and the optional R11 are unknown. The reference models centered symmetry with driven base pins and no optional offset injection. The native part models the calibrated device rather than a particular aged module.

[Original dbx internal drawing, page 1](https://proaudiodesignforum.com/images/pdf/dBx_200_202_202C_2001_2000T_202XT_Schematics_Merged.pdf) · [Philips transistor cards](https://github.com/kicad-spice-library/KiCad-Spice-Library/blob/master/Models/Transistor/BJT/BJT.lib) · [Siliconix 1977 FET Data Book](https://bitsavers.computerhistory.org/components/siliconix/_dataBooks/1977_Siliconix_FET_Data_Book.pdf) · [2N3687 SPICE card](https://ltwiki.org/index.php?title=Standard.jft)

## RMS 209

The factory drawing and calibration procedure establish the 23.2 kΩ external current-feed resistor R38, selected 22 µF timing capacitor C15, symmetry trim and system-level alignment. The bill of materials specifies a selected 909 kΩ nominal R35. For a 209, the factory selects R36 (typically 8–75 kΩ) for symmetry on the compensation pin at 60 dB below 1.228 Vrms. At 1.228 Vrms and 100 Hz, R27 and R34 minimize distortion, and R63 sets the R79 test point to −11 mV. The raw module output's absolute DC offset and a trace of the 209 internals remain unverified. The model specifies calibrated relative output, a negative log slope of 6 mV/dB inferred from the compatible 207 interface, a 10 Ω virtual-earth sense impedance and an adjustable reference current. With the 160's receiver and C11 high pass, the 100 Hz alignment corresponds to about 23.34 µArms at M2. The schematic applies the OA3 output offset separately.

Blackmer's patent describes rectification, doubling in the log domain and an antilog charging current opposed by a fixed current at the timing capacitor. In linear power coordinates this becomes first-order averaging. The 160's specified **120 dB/s** free release fixes the nominal time constant to 36.19 ms at 22 µF. At 6 mV/dB a 22 µF capacitor requires 15.84 µA for that release slope, consistent with the selected bias network. At other capacitances the time constant scales proportionally. The RMS 209 part exposes installed timing capacitance as a parameter. The package-specific trim, compensation and current-node impedances are outside this terminal model.

| Rise above an existing steady signal | 160 manual, 63% attack | Power-envelope model, 63% of dB step | Native 160 circuit, 2 kHz carrier |
|---|---:|---:|---:|
| 10 dB | 15 ms | 16.31 ms | 16.34 ms |
| 20 dB | 5 ms | 6.91 ms | 6.96 ms |
| 30 dB | 3 ms | 2.89 ms | 2.95 ms |

The analytic detector values use a step in RMS level. The circuit measurement uses a 2 kHz sine at 48 kHz, 24 mVrms before the step, minimum threshold, maximum ratio and centered output gain. One-cycle RMS measurements give the crossing at 63% of the final gain change in dB. Carrier frequency and phase affect the instantaneous crossing. The 207 replacement separately specifies 35 ms for a small change, 120 dB/s release and an 80 dB/ms large-signal rate. The 209 model does not assume that replacement module's slew limit.

[Factory bill of materials and calibration, sections 2A–5 and 9](https://www.lost-manuals.com/pdf/dbx--160--service--ID11749.pdf) · [Factory motherboard drawing, page 7](https://adn.harmanpro.com/product_documents/documents/592_1324058089/162%20Calibration%20Procedure_original.pdf) · [Factory 160/161 operator manual and 160 specifications](https://schematicsforfree.com/files/Audio/Products/Professional%20Audio/D/dbx/dbx%20160-161%20Compressor%20Limiter%20Manual.pdf) · [Blackmer RMS converter patent US3681618A](https://patents.google.com/patent/US3681618A/en) · [207 replacement pin and transfer information](https://www.studioelectronics.biz/sunshop/index.php?l=product_detail&p=2184)

## Verification

Run the Catch2 selectors `[dbx200],[dbx209]` with ngspice on PATH or set `NGSPICE` to its executable. The 200 checks positive, negative and zero input currents, both gain directions, linked and independent symmetry terminals against the transistor reference and exported macromodel. The 209 checks calibrated log polarity, free release, doubled timing capacitance and a continuous ngspice implementation of the patent's log-capacitor differential equation.

On the calibrated 160 circuit, a 100 Hz, 1.228 Vrms input measured −10.981 mV at OA3. With threshold at minimum, raising the input from 12.28 mVrms to 3.883 Vrms increased output by 12.49 dB at 4:1 and 0.416 dB at maximum ratio. Through 8 Vrms, the 44.1, 48 and 96 kHz benches had zero rejected or nonconverged samples. With 48 kHz speech at 1.755 V peak the same counts stayed zero over 236,640 samples.
