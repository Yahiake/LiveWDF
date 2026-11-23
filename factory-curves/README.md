# Manufacturer curves versus LiveWDF

[2N3904 overlay](2n3904-onsemi-overlay.png) · [2N3906 overlay](2n3906-onsemi-overlay.png) · [1N4002 overlay](1n4002-vishay-overlay.png) · [BJT errors](errors.csv) · [1N4002 errors](1n4002-errors.csv)

| Operating point | Manufacturer typical | LiveWDF original | LiveWDF opt-in | Full MiSTer SPICE donor |
| --- | ---: | ---: | ---: | ---: |
| 2N3904 hFE at 1 mA | 229.5 | 141.3 | 139.6 | 138.2 |
| 2N3904 hFE at 10 mA | 223.1 | 189.9 | 168.9 | 167.2 |
| 2N3904 hFE at 50 mA | 193.9 | 226.0 | 139.4 | 137.8 |
| 2N3906 hFE normalized at 10 mA, evaluated at 1 mA | 0.892 | 1.003 | 1.114 | 1.115 |
| 2N3906 hFE normalized at 10 mA, evaluated at 25 mA | 0.903 | 0.999 | 0.857 | 0.855 |
| 1N4002 forward voltage at 1 A | 0.903 V | 0.962 V | 0.962 V | 0.963 V |
| 1N4002 reverse current at 100 V | 0.219 µA | 100.014 µA | 0.0141 µA | 0.0121 µA |

The opt-in IKF cards reproduce the donor's forward high-current gain rolloff. For these digitized typical transistors, the mean absolute relative error across all overlapping log-spaced current samples increases from 30.26% to 35.23% on the 2N3904 and from 12.88% to 18.82% on the 2N3906. The full donor errors are 35.90% and 18.94%, respectively. The native IKF and ngspice IKF-export curves differ by less than 0.000005% relative gain on the sampled comparison grids.

The 1N4002 LowLeakage card avoids the legacy 1 MΩ shunt. It meets Vishay's 5 µA reverse-current maximum and is about 15.5 times below the digitized typical reverse current at 100 V. The SPICE donor underpredicts the typical reverse curve by a similar amount. The diode's forward error is also present in the full donor. The legacy and opt-in forward voltages differ by about 0.048 µV at 1 A.

## Provenance and conditions

- **2N3904.** The onsemi figure shows typical pulsed gain at 25°C and a 5 V collector–emitter voltage. LiveWDF used a +5 V supply, a 10 mΩ collector sense resistor and temperature-adjusted thermal voltage at 25°C. Supply droop was at most about 1 mV over the 0.10–100 mA comparison. The fixed-temperature DC sweeps approximate the low-heating test condition. Both LiveWDF sweeps completed without rejects. [onsemi 2N3904, Rev. 3, figure 1](https://www.onsemi.com/pdf/datasheet/2n3904-d.pdf)
- **2N3906.** The onsemi plot gives normalized gain at 25°C and a 1 V collector–emitter voltage magnitude. The existing fixtures use a −1 V supply, a 0.1 Ω collector sense resistor and the card's equivalent thermal voltage at 26.83°C. The figure does not state its numerical baseline. We divided each curve by its own hFE at 10 mA, including the published trace whose raw 10 mA ordinate was 0.989. The magnitudes remove the PNP sign convention. The sample overlap is 0.101–44.85 mA. The later part of the published curve appears for context. [onsemi 2N3906, Rev. 4, figure 13](https://www.onsemi.com/pdf/datasheet/2n3906-d.pdf)
- **1N4002.** Vishay shows a 25°C typical forward curve from 300 µs pulses at 1% duty cycle. Its 25°C reverse plot expresses voltage as a percentage of the family members' peak rating. We used 100 V for the 1N4002. The LiveWDF and compact export cards are fixed near 27°C. The original SPICE donor is evaluated at 25°C. The forward point at 1 A does not use the reverse fixture. [Vishay 1N4001–1N4007, document 88503, figures 4 and 5](https://www.vishay.com/docs/88503/1n4001.pdf)

Vector trace points were extracted from the original PDF path strokes. The digitized CSVs retain PDF point coordinates. The calibration tables name the plot revision, page, axis scaling and source SHA-256. The transistor axis fits have maximum tick residuals of 0.07% in current and 0.10 in gain for the 2N3904, and 1.64% in current and 1.86% in normalized gain for the 2N3906. The stroke width, interpolation and factory device spread add uncertainty. These are typical population examples. Production guarantees are given separately in the datasheet electrical-characteristics tables.

The full MiSTer models come from revision 692e777. Their model statements remain intact. They run in ngspice at the stated fixture temperature with nominal temperature 27°C, the SPICE default for these cards. The plotted relative BJT errors use 121 log-spaced samples across all models' common current range per device. Diode residuals use 121 voltage-spaced samples and report log10 of the current ratio. The trace archive contains the full source simulation decks and logs.
