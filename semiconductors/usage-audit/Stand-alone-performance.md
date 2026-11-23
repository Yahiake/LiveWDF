# Isolated 48 kHz part performance

The benchmark used the Release compiler and part card from `45eb167`, compiled with `-O3 -DNDEBUG -mavx2 -mfma`. Each card processed the same 440 Hz input as its comparison part, with 4,096 warm-up frames followed by 48,000 timed frames. Nine passes rotated card order on one pinned CPU of a shared Intel Xeon Platinum 8573C. Timing is thread CPU time per frame. Iteration and fallback counts come from a separate 48,000-frame pass using the same input and reset. Sub-microsecond timings are sensitive to machine load.

| Fixture and card | Median µs/frame (range) | Newton iterations/frame, mean / max | Block fallbacks | Rejected / nonconverged |
|---|---:|---:|---:|---:|
| Common-emitter 2N2907 | 0.773 (0.742–0.856) | 1.985 / 2 | 0 | 0 / 0 |
| Common-emitter 2N4403 | 0.837 (0.745–3.060) | 1.986 / 2 | 0 | 0 / 0 |
| Unity follower TL072 | 0.305 (0.296–0.316) | 0 / 0 | 0 | 0 / 0 |
| Unity follower LM358 | 6.881 (6.632–15.707) | 5.252 / 33 | 80 | 0 / 0 |
| Low-rail load TL072 | 0.311 (0.294–0.552) | 0 / 0 | 0 | 0 / 0 |
| Low-rail load LM358 | 5.128 (5.020–17.425) | 4.471 / 7 | 0 | 0 / 0 |

The PNP emitter returns to ground through 100 Ω, and the collector connects to −9 V through 4.7 kΩ. The base receives −0.80 ± 0.04 V through 10 kΩ. The 2N4403 collector current ranges from 0.402 to 0.816 mA. The op-amp follower uses a 15 V supply, 7.5 ± 1.0 V input and 2.2 kΩ load to midrail. The load crosses between sourcing and sinking 0.455 mA. For the low-rail bench, the follower input is grounded and a 1.4 ± 1.3 V source feeds the output through 15 kΩ. The LM358 sinks 6.2–136.9 µA and its output traverses 7–647 mV. The TL072 uses its 1.5 V output-swing bound. A block fallback means the solver retried with the joint Newton iteration. All 80 resulting samples converged.

[Benchmark source](standalone-performance.cpp) · [Measurements and counters](standalone-performance.csv)

## 2N4403 factory trace

The original overlay compares continuous native and ngspice curves with four onsemi datasheet gain-limit test points. The supplementary figure extracts the 25 °C typical pulsed hFE trace from Fairchild’s original 2N4403/MMBT4403 Rev. 1.1.1, Figure 3, printed page 4 (PDF page 5). Its test condition is |VCE| = 5 V. The extraction follows the vector line and cubic Bézier segments of the middle temperature curve, calibrates the logarithmic current axis at 0.1 and 100 mA, and calibrates the linear gain axis from 0 to 500. The sampled points are interpolated along this trace. Accuracy is limited by the thickness of the published line. The native card runs at its shipped fixed thermal voltage of 25.85 mV. The independent ngspice deck runs at 25 °C with model reference temperature 27 °C.

| Collector current | Fairchild Figure 3 hFE | Native hFE | Full ngspice hFE | Native deviation from figure |
|---:|---:|---:|---:|---:|
| 0.1 mA | 252.3 | 191.9 | 190.0 | −23.9% |
| 1 mA | 243.9 | 211.4 | 209.3 | −13.3% |
| 10 mA | 235.6 | 217.6 | 215.4 | −7.6% |
| 150 mA | 184.5 | 196.3 | 194.1 | +6.4% |
| 490 mA | 102.5 | 155.2 | 153.0 | +51.3% |

[Fairchild Figure 3 source](https://www.gocarelectronic.es/pdf/2N4403FE.pdf) · [Figure 3 overlay](2n4403-fairchild-figure3.png) · [Digitized trace and model readings](2n4403-fairchild-figure3.csv) · [Original four-limit-point overlay](2n4403-overlay.png)
