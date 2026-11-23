# 1N4003 rectifier verification

The 1N4003 card uses the manufacturer's static SPICE parameters. Its forward sweep matches the original Diodes Inc. source in ngspice 44.2 at 27 °C. The RMS difference is **15.0 pA** across 1,251 points, with an 87.1 pA maximum absolute difference. The RMS difference from 0.1 to 1 mA is 2.63 fA. The LiveWDF export and manufacturer decks give the same forward results in ngspice at matched temperature. [Manufacturer SPICE source](https://www.diodes.com/spice/download/337/1N4003.spice.txt)

| Forward current | LiveWDF 1N4003 at 27 °C | Diodes source SPICE at 25 °C | Legacy LiveWDF 1N4007 at 27 °C |
| ---: | ---: | ---: | ---: |
| 0.1 mA | 0.5280 V | 0.5324 V | 0.1193 V |
| 1.0 mA | 0.6144 V | 0.6182 V | 0.1787 V |
| 2.0 mA | 0.6404 V | 0.6441 V | 0.1967 V |
| 1.0 A | 0.9154 V | 0.9175 V | 0.3973 V |

The 25 °C manufacturer SPICE model differs by at most 11.6 mV from the digitized typical forward curve over 20 mA to 1 A. At 1 A, the graph reads approximately 0.927 V and the table specifies a 1.0 V maximum at 25 °C ambient. The graph uses 300 µs pulses at 2% duty cycle and 25 °C junction temperature. Its range starts near 10 mA, so it provides no direct measurement of the UREI bias region around 1 mA. [DS28002 Rev. 11-3, electrical table and Fig. 2](https://www.diodes.com/datasheet/download/1N4003.pdf)

At the 200 V reverse point, the native card gives about 5.00 µA, versus 4.41 µA in the 27 °C manufacturer deck and 4.52 µA in the 25 °C deck. The datasheet specifies 5.0 µA maximum at the rated reverse voltage and 25 °C ambient. The roughly 0.59 µA difference at 27 °C is concentrated at the modeled breakdown knee. Native simulation uses a fixed 27 °C thermal voltage and omits capacitance, transit time, and self-heating. [Manufacturer SPICE source](https://www.diodes.com/spice/download/337/1N4003.spice.txt) · [DS28002 electrical table](https://www.diodes.com/datasheet/download/1N4003.pdf)

## CPU and solver diagnostics

The timing fixtures are the existing 48 kHz semiconductor benchmark with a 440 Hz sine at ±1.8 V, a 1 kΩ source resistor, and a 100 kΩ load on the opposing-diode fixture. Each reported median and range cover seven interleaved passes of 48,000 frames after a 4,800-frame warm-up. Diagnostics use a separate untimed pass. The build used GCC 14.2 with -O3 and ran pinned to CPU 7 on an Intel Xeon Platinum 8573C.

| Fixture | Card | Median thread CPU ns/frame | Range | Iterations/frame | Evaluations/frame | Fallbacks | Rejected | Unconverged |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Single diode | 1N4003 | 146.0 | 137.1–166.0 | 0 | 1 | 0 | 0 | 0 |
| Single diode | Legacy 1N4007 | 145.6 | 136.5–163.4 | 0 | 1 | 0 | 0 | 0 |
| Opposing pair | 1N4003 | 469.0 | 456.5–501.5 | 0 | 1 | 0 | 0 | 0 |
| Opposing pair | Legacy 1N4007 | 674.4 | 643.5–738.7 | 0 | 1 | 0 | 0 | 0 |

The single-diode ranges overlap. The 1N4003 opposing pair used 30.5% less CPU time in this fixture. These isolated timings do not quantify complete UREI 545 runtime. All four forward/reverse sweeps also completed with zero rejected and unconverged steps.

The plot input includes the same voltage grid for native and SPICE and a vector digitization of the center of the factory curve outline. The plotted outline bounds describe the graphic line width, rather than production tolerances. The script reads the manufacturer PDF as `Diodes-DS28002.pdf` and the source model as `diodes-source.spice.txt` when regenerating the comparison.

[Plot](1n4003.png) · [Forward curves](1n4003-forward-curves.csv) · [Reverse curves](1n4003-reverse-curves.csv) · [Voltage checks](1n4003-voltage-checks.csv) · [Residuals](residuals.csv) · [Single-diode timing](timing.csv) · [Opposing-pair timing](clipping-timing.csv)
