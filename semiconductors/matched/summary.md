# Semiconductor validation

The timed circuits drive one device per test at 48 kHz with a 440 Hz sine. Median and range cover seven passes of 48,000 samples. Warm-up and diagnostic collection take place outside the timed region. The DC comparison uses a separate low-resistance sense fixture and excludes JFET displacement currents. The timing fixture uses the complete JFET card.

| Device | Median ns/frame | Range ns/frame | Iterations/frame | Fallbacks | Rejects |
| --- | ---: | ---: | ---: | ---: | ---: |
| [2N3904-MiSTer](2n3904-mister.png) | 1459.4 | 1297.6–2183.3 | 1.992 | 0 | 0 |
| [2N3906-MiSTer](2n3906-mister.png) | 1305.8 | 1071.9–1652.0 | 1.997 | 0 | 0 |
| [2N3904-MiSTer-IKF](2n3904-mister-ikf.png) | 1517.1 | 1207.4–1719.9 | 1.992 | 0 | 0 |
| [2N3906-MiSTer-IKF](2n3906-mister-ikf.png) | 1503.0 | 1445.8–1691.7 | 1.997 | 0 | 0 |
| [BC109C](bc109c.png) | 1391.4 | 1259.8–1447.6 | 1.995 | 0 | 0 |
| [2SC2240](2sc2240.png) | 1408.3 | 1151.0–1496.4 | 1.992 | 0 | 0 |
| [2N270](2n270.png) | 1686.1 | 1515.8–2143.4 | 2.484 | 0 | 0 |
| [2SK184-GR](2sk184-gr.png) | 855.5 | 669.3–998.6 | 0.000 | 0 | 0 |
| [2N5458](2n5458.png) | 921.3 | 695.6–1009.1 | 0.000 | 0 | 0 |
| [1N914-Nexperia](1n914-nexperia.png) | 432.4 | 104.9–513.3 | 0.000 | 0 | 0 |
| [1N914-Nexperia-LowLeakage](1n914-nexperia-lowleakage.png) | 264.4 | 180.6–529.6 | 0.000 | 0 | 0 |
| [1S1588-Proxy](1s1588-proxy.png) | 428.7 | 261.3–677.4 | 0.000 | 0 | 0 |
| [MA150-Proxy](ma150-proxy.png) | 271.1 | 183.1–529.8 | 0.000 | 0 | 0 |
| [1S2075](1s2075.png) | 262.0 | 260.4–668.1 | 0.000 | 0 | 0 |
| [1S2075-LowLeakage](1s2075-lowleakage.png) | 261.2 | 179.0–605.4 | 0.000 | 0 | 0 |
| [1N34A-MiSTer](1n34a-mister.png) | 272.0 | 263.3–511.3 | 0.000 | 0 | 0 |
| [1N4002](1n4002.png) | 439.1 | 176.7–513.0 | 0.000 | 0 | 0 |
| [1N4002-LowLeakage](1n4002-lowleakage.png) | 266.4 | 219.9–428.0 | 0.000 | 0 | 0 |
| [1SS133](1ss133.png) | 259.8 | 259.1–883.6 | 0.000 | 0 | 0 |

## Independent datasheet checks

The datasheet values apply at the published room-temperature test conditions. The shipped cards use fixed room-temperature thermal voltages. Sense resistors drop at most 1 mV at the BJT gain comparison points.

| Device | Native result and manufacturer limit | Result |
| --- | --- | --- |
| 2N3904-MiSTer | hFE 181 at 10 mA, \|VCE\| 0.999 V (range 100–300). [onsemi 2N3904, p. 2](https://www.onsemi.com/pdf/datasheet/2n3904-d.pdf) | Within limit |
| 2N3906-MiSTer | hFE 183 at 10 mA, \|VCE\| 0.999 V (range 100–300). [onsemi 2N3906, p. 2](https://www.onsemi.com/pdf/datasheet/2n3906-d.pdf) | Within limit |
| 2N3904-MiSTer-IKF | hFE 160 at 10 mA, \|VCE\| 0.999 V (range 100–300). [onsemi 2N3904, p. 2](https://www.onsemi.com/pdf/datasheet/2n3904-d.pdf) | Within limit |
| 2N3906-MiSTer-IKF | hFE 163 at 10 mA, \|VCE\| 0.999 V (range 100–300). [onsemi 2N3906, p. 2](https://www.onsemi.com/pdf/datasheet/2n3906-d.pdf) | Within limit |
| BC109C | hFE 677 at 2 mA, \|VCE\| 5 V (range 420–800). [Philips BC107 to BC109C, electrical table](https://www.josvandijken.nl/downloads/BC109.PDF) | Within limit |
| 2SC2240 | hFE 310 at 2 mA, \|VCE\| 6 V (range 200–400). [Toshiba 2SC2240, p. 2](https://www.taydaelectronics.com/datasheets/A-904.pdf) | Within limit |
| 2SK184-GR | IDSS 4.2 mA at VGS 0 V (range 2.6–6.5 mA). [Toshiba 2SK184, electrical table](https://aitendo3.sakura.ne.jp/aitendo_data/product_img/transisitor/2SK184/2SK184_toshiba.pdf) | Within limit |
| 2SK184-GR | \|VGS(off)\| 0.549 V at 100 nA (range 0.2–1.5 V). [Toshiba 2SK184, electrical table](https://aitendo3.sakura.ne.jp/aitendo_data/product_img/transisitor/2SK184/2SK184_toshiba.pdf) | Within limit |
| 2N5458 | IDSS 4.24 mA at VGS 0 V (range 2–9 mA). [onsemi 2N5457/2N5458, electrical table](https://www.pilianidis.gr/images/sync_products/01-2N5458/2N5458.pdf) | Within limit |
| 2N5458 | \|VGS(off)\| 2.89 V at 10 nA (range 1–7 V). [onsemi 2N5457/2N5458, electrical table](https://www.pilianidis.gr/images/sync_products/01-2N5458/2N5458.pdf) | Within limit |
| 1N914-Nexperia | VF 0.729 V at 10 mA (max 1 V). [onsemi 1N914/1N4148, electrical table](https://www.onsemi.com/pdf/datasheet/1n914-d.pdf) | Within limit |
| 1N914-Nexperia | IR 20 µA at 20 V reverse (max 0.025 µA, 800×). [onsemi 1N914/1N4148, electrical table](https://www.onsemi.com/pdf/datasheet/1n914-d.pdf) | Outside limit |
| 1N914-Nexperia-LowLeakage | VF 0.729 V at 10 mA (max 1 V). [onsemi 1N914/1N4148, electrical table](https://www.onsemi.com/pdf/datasheet/1n914-d.pdf) | Within limit |
| 1N914-Nexperia-LowLeakage | IR 0.00778 µA at 20 V reverse (max 0.025 µA, 0.311×). [onsemi 1N914/1N4148, electrical table](https://www.onsemi.com/pdf/datasheet/1n914-d.pdf) | Within limit |
| 1S2075 | VF 0.71 V at 10 mA (max 0.8 V). [Renesas 1S2075(K), electrical table](https://www.renesas.com/en/document/dst/1s2075k-datasheet) | Within limit |
| 1S2075 | IR 30 µA at 30 V reverse (max 0.1 µA, 300×). [Renesas 1S2075(K), electrical table](https://www.renesas.com/en/document/dst/1s2075k-datasheet) | Outside limit |
| 1S2075-LowLeakage | VF 0.71 V at 10 mA (max 0.8 V). [Renesas 1S2075(K), electrical table](https://www.renesas.com/en/document/dst/1s2075k-datasheet) | Within limit |
| 1S2075-LowLeakage | IR 0.00139 µA at 30 V reverse (max 0.1 µA, 0.0139×). [Renesas 1S2075(K), electrical table](https://www.renesas.com/en/document/dst/1s2075k-datasheet) | Within limit |
| 1N34A-MiSTer | VF 0.333 V at 5 mA (max 1 V). [Central Semiconductor 1N34A, electrical table](https://my.centralsemi.com/datasheets/1N34A.PDF) | Within limit |
| 1N34A-MiSTer | IR 12.6 µA at 10 V reverse (max 30 µA, 0.42×). [Central Semiconductor 1N34A, electrical table](https://my.centralsemi.com/datasheets/1N34A.PDF) | Within limit |
| 1N4002 | VF 0.961 V at 1000 mA (max 1.1 V). [Vishay 1N4001–1N4007, p. 2](https://www.vishay.com/docs/88503/1n4001.pdf) | Within limit |
| 1N4002 | IR 100 µA at 100 V reverse (max 5 µA, 20×). [Vishay 1N4001–1N4007, p. 2](https://www.vishay.com/docs/88503/1n4001.pdf) | Outside limit |
| 1N4002-LowLeakage | VF 0.961 V at 1000 mA (max 1.1 V). [Vishay 1N4001–1N4007, p. 2](https://www.vishay.com/docs/88503/1n4001.pdf) | Within limit |
| 1N4002-LowLeakage | IR 0.0141 µA at 100 V reverse (max 5 µA, 0.00282×). [Vishay 1N4001–1N4007, p. 2](https://www.vishay.com/docs/88503/1n4001.pdf) | Within limit |
| 1SS133 | VF 0.844 V at 100 mA (max 1.2 V). [ROHM 1SS133](https://www.rohm.com/products/diodes/switching-diodes/standard/1ss133-product) | Within limit |
| 1SS133 | IR 80 µA at 80 V reverse (max 0.5 µA, 160×). [ROHM 1SS133](https://www.rohm.com/products/diodes/switching-diodes/standard/1ss133-product) | Outside limit |

[DC residuals](residuals.csv) · [timing and diagnostic counts](timing.csv)

The clipping fixture uses two opposing diodes, a 1 kΩ source resistance and a 100 kΩ load. It uses the same timing protocol and a ±1.8 V sine. [Clipping timing](clipping-timing.csv)

[Forward and reverse shunt comparison](leakage-before-after.png)
