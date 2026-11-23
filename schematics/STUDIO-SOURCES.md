# Classic analog studio equipment

These 40 circuits are the studio half of the factory expansion in [PR #38](https://github.com/Yahiake/LiveWDF/pull/38). Each entry targets a specific hardware signal path. Keep all entries when the exact schematic or part model still needs research.

The factory collection already packages nested directories. Place these circuits under `studio/` in effect-type subdirectories. Use line-level source material for listening tests and document the actual voltage, gain settings and source or load impedance.

## Preamps and console channels

| # | Device | Starting reference |
|---:|---|---|
| 1 | Neve 1073 | |
| 2 | Neve 1081 | |
| 3 | API 312 | [1972 API technical literature](https://www.steampoweredradio.com/pdf/api/manuals/api%201972%20schematics.pdf) |
| 4 | SSL 4000 E 242 Black EQ | |
| 5 | Trident A-Range EQ | |
| 6 | Trident 80B EQ | |
| 7 | Helios Type 69 EQ | |
| 8 | Harrison 32C channel EQ | |
| 9 | MCI JH-400B input module | |
| 10 | Soundcraft Series 600 mono input module | |

## Compressors, limiters and gates

| # | Device |
|---:|---|
| 11 | UREI 1176LN Rev D |
| 12 | Teletronix LA-2A |
| 13 | UREI LA-3A |
| 14 | Fairchild 660 |
| 15 | Gates Sta-Level |
| 16 | Altec 436C |
| 17 | EMI RS124 |
| 18 | dbx 160 VU |
| 19 | Manley Variable Mu |
| 20 | Pye 4060 |
| 21 | SSL 4000 G Bus Compressor |
| 22 | API 2500 |
| 23 | Neve 33609/J |
| 24 | ADR Compex F760X-RS |
| 25 | Drawmer DS201 |

## Equalizers

| # | Device | Starting reference |
|---:|---|---|
| 26 | Pultec EQP-1A | [Amplifier schematic](https://civade.com/AudioPro/PultecEQP.pdf) and [passive network](https://www.benmook.com/tech/documents/Schematics%202/Schematics%201/Pultec-EQP-1Aman%201.pdf) |
| 27 | Pultec MEQ-5 | |
| 28 | API 550A | |
| 29 | API 560 | |
| 30 | GML 8200 | |
| 31 | Sontec MES-432C | |
| 32 | Mäag EQ4 | |
| 33 | Manley Massive Passive | |
| 34 | UREI 545 | |

## De-essing, enhancement and saturation

| # | Device |
|---:|---|
| 35 | dbx 902 De-Esser |
| 36 | Aphex Aural Exciter Type B |
| 37 | BBE 482i Sonic Maximizer |
| 38 | SPL Transient Designer 9842 |
| 39 | Thermionic Culture Vulture |
| 40 | Empirical Labs FATSO EL7 |

## Parts references

- [THAT 2180-series datasheet](https://thatcorp.com/datashts/THAT_2180-Series_Datasheet.pdf) gives the internal equivalent circuit, nominal gain law and a complete test application. [AES paper 4055](https://www.thatcorp.com/datashts/AES4055_An_Improved_Monolithic_VCA.pdf) covers the compensated Blackmer cell and sources of distortion.
- [Analog Devices OP275 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/OP275.pdf) provides typical amplifier limits and a manufacturer SPICE macromodel.
