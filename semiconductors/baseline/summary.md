# Semiconductor validation

The timed circuits drive one device per test at 48 kHz with a 440 Hz sine. Median and range cover seven passes of 48,000 samples. Warm-up and diagnostic collection take place outside the timed region. The DC comparison uses a separate low-resistance sense fixture.

| Device | Median ns/frame | Range ns/frame | Iterations/frame | Fallbacks | Rejects |
| --- | ---: | ---: | ---: | ---: | ---: |
| [2N3904-MiSTer](2n3904-mister.png) | 2265.4 | 2111.9–2801.2 | 1.992 | 0 | 0 |
| [2N3906-MiSTer](2n3906-mister.png) | 2263.2 | 2121.4–2605.5 | 1.997 | 0 | 0 |
| [BC109C](bc109c.png) | 2696.1 | 1811.3–2802.2 | 1.995 | 0 | 0 |
| [2SC2240](2sc2240.png) | 2554.9 | 1911.0–2597.6 | 1.992 | 0 | 0 |
| [2N270](2n270.png) | 2756.1 | 2601.9–3059.8 | 2.484 | 0 | 0 |
| [2SK184-GR](2sk184-gr.png) | 1574.8 | 1171.6–1783.8 | 0.000 | 0 | 0 |
| [2N5458](2n5458.png) | 1455.7 | 1421.9–1796.8 | 0.000 | 0 | 0 |
| [1N914-Nexperia](1n914-nexperia.png) | 426.1 | 94.0–676.3 | 0.000 | 0 | 0 |
| [1S1588-Proxy](1s1588-proxy.png) | 439.8 | 424.2–853.7 | 0.000 | 0 | 0 |
| [MA150-Proxy](ma150-proxy.png) | 622.6 | 425.7–1053.8 | 0.000 | 0 | 0 |
| [1S2075](1s2075.png) | 685.0 | 348.5–961.0 | 0.000 | 0 | 0 |
| [1N34A-MiSTer](1n34a-mister.png) | 510.5 | 362.1–893.9 | 0.000 | 0 | 0 |
| [1N4002](1n4002.png) | 678.2 | 348.4–1016.9 | 0.000 | 0 | 0 |
| [1SS133](1ss133.png) | 424.1 | 337.0–621.1 | 0.000 | 0 | 0 |

## Independent datasheet checks

The datasheet values apply at the published room-temperature test conditions. The shipped cards use fixed room-temperature thermal voltages. Sense resistors drop at most 1 mV at the BJT gain comparison points.

| Device | Native result and manufacturer limit | Result |
| --- | --- | --- |
| 2N3904-MiSTer | hFE 181 at 10 mA, \|VCE\| 0.999 V (range 100–300). [onsemi 2N3904, p. 2](https://www.onsemi.com/pdf/datasheet/2n3904-d.pdf) | Within limit |
| 2N3906-MiSTer | hFE 183 at 10 mA, \|VCE\| 0.999 V (range 100–300). [onsemi 2N3906, p. 2](https://www.onsemi.com/pdf/datasheet/2n3906-d.pdf) | Within limit |
| BC109C | hFE 677 at 2 mA, \|VCE\| 5 V (range 420–800). [Philips BC107 to BC109C, electrical table](https://www.josvandijken.nl/downloads/BC109.PDF) | Within limit |
| 2SC2240 | hFE 310 at 2 mA, \|VCE\| 6 V (range 200–400). [Toshiba 2SC2240, p. 2](https://www.taydaelectronics.com/datasheets/A-904.pdf) | Within limit |
| 2SK184-GR | IDSS 4.2 mA at VGS 0 V (range 2.6–6.5 mA). [Toshiba 2SK184, electrical table](https://aitendo3.sakura.ne.jp/aitendo_data/product_img/transisitor/2SK184/2SK184_toshiba.pdf) | Within limit |
| 2SK184-GR | \|VGS(off)\| 1.26 V at 100 nA (range 0.2–1.5 V). [Toshiba 2SK184, electrical table](https://aitendo3.sakura.ne.jp/aitendo_data/product_img/transisitor/2SK184/2SK184_toshiba.pdf) | Within limit |
| 2N5458 | IDSS 4.24 mA at VGS 0 V (range 2–9 mA). [onsemi 2N5457/2N5458, electrical table](https://www.pilianidis.gr/images/sync_products/01-2N5458/2N5458.pdf) | Within limit |
| 2N5458 | \|VGS(off)\| 4.34 V at 10 nA (range 1–7 V). [onsemi 2N5457/2N5458, electrical table](https://www.pilianidis.gr/images/sync_products/01-2N5458/2N5458.pdf) | Within limit |
| 1N914-Nexperia | VF 0.729 V at 10 mA (max 1 V). [onsemi 1N914/1N4148, electrical table](https://www.onsemi.com/pdf/datasheet/1n914-d.pdf) | Within limit |
| 1N914-Nexperia | IR 20 µA at 20 V reverse (max 0.025 µA, 800.2×). [onsemi 1N914/1N4148, electrical table](https://www.onsemi.com/pdf/datasheet/1n914-d.pdf) | Outside limit |
| 1S2075 | VF 0.71 V at 10 mA (max 0.8 V). [Renesas 1S2075(K), electrical table](https://www.renesas.com/en/document/dst/1s2075k-datasheet) | Within limit |
| 1S2075 | IR 30 µA at 30 V reverse (max 0.1 µA, 300.0×). [Renesas 1S2075(K), electrical table](https://www.renesas.com/en/document/dst/1s2075k-datasheet) | Outside limit |
| 1N34A-MiSTer | VF 0.333 V at 5 mA (max 1 V). [Central Semiconductor 1N34A, electrical table](https://my.centralsemi.com/datasheets/1N34A.PDF) | Within limit |
| 1N34A-MiSTer | IR 12.6 µA at 10 V reverse (max 30 µA, 0.4×). [Central Semiconductor 1N34A, electrical table](https://my.centralsemi.com/datasheets/1N34A.PDF) | Within limit |
| 1N4002 | VF 0.961 V at 1000 mA (max 1.1 V). [Vishay 1N4001–1N4007, p. 2](https://www.vishay.com/docs/88503/1n4001.pdf) | Within limit |
| 1N4002 | IR 100 µA at 100 V reverse (max 5 µA, 20.0×). [Vishay 1N4001–1N4007, p. 2](https://www.vishay.com/docs/88503/1n4001.pdf) | Outside limit |

[DC residuals](residuals.csv) · [timing and diagnostic counts](timing.csv)
