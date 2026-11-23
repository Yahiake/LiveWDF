# Neve 33609/J live crackle

The current PR #38 and PR #42 preset files both have SHA-256 `7bb19ebdbbaa0ff077a14fa7ee8ab3010a1566d63dae17539cd3b1b706e9e052`. Their 24 nonlinear devices solve together in one coupled root. The plugin misses the real-time callback deadline during ordinary audio, while the native solver converges and simulated audio remains continuous.

## Plugin callback reproduction

`plugin_probe` instantiates one LiveWDF plugin processor per condition with 1×, mono, 0 dB trim, saved circuit controls, and 96 frames at 48 kHz. Both channels receive the same input. The budget is 2 ms per callback. The times are thread CPU time on the shared Xeon Platinum 8573C. The table excludes the first 250 ms of each fixture. P99 values vary with CPU contention and frequency.

| Input and mode | Median | P99 | Blocks over 2 ms |
| --- | ---: | ---: | ---: |
| 250 mV peak 1 kHz tone, Accurate | 3.52 ms | 7.49 ms | 875/875 |
| 750 mV peak active drums, Accurate | 2.78 ms | 6.26 ms | 703/875 |
| Same drums, Fast Live | 2.27 ms | 5.80 ms | 530/875 |

The sustained tone reached 0.031 full scale at the output. The drum fixture stayed below 0.10 full scale. A second run using the existing `plugin-bench-cli --wav ... --block 96` independently exceeded the budget on the 250 mV tone. The offline waveform does not contain the irregular gaps a realtime host can produce when the callback runs late.

## Native signal checks

Native output at silence settled below \(10^{-18}\) V peak. All tested 250 mV tone runs at 44.1, 48 and 96 kHz, 4 and 8 V tone runs at 48 kHz, and active 48 kHz drums had zero convergence misses, state rejects and block fallbacks. The 250 mV tone had 0.005% THD measured from the second half-second. The 4 V and 8 V tones measured 2.14% and 3.66% THD, respectively, with peak outputs of 2.02 and 2.28 V. Tone distortion at these higher levels appears as harmonics, with the residual after fitting 12 harmonics below −104 dB. Active drums averaged 3.88 solver iterations and 166 device evaluations per sample, and the 250 mV tone averaged 5.29 and 212. No joint line search or voltage limiting was recorded on either.

The manufacturer's EX11475 signal path uses a transformer, balanced pad and diode bridge, high-impedance buffering, makeup stage, and independent compressor and limiter sidechains that select the stronger reduction. The preset reproduces this structure with equivalent rectifier and control amplifiers. The service schematic uses transistor stages there. [Neve 33609/J technical manual, section 3](https://media.soniccircus.com/productpdfs/Manuals/Neve/Neve_33609J_manual.pdf)

## Optimization trials

The same-equation solver changes from the PR #45 integration (`coupled-root-performance`, `66a24e0`) improve native active drums from 2.86 to 1.65 ms median and 7.25 to 3.59 ms p99. For the tone the native median drops from 3.85 to 1.95 ms. The new runs still exceed 2 ms in 123/375 settled drum blocks and 146/375 tone blocks. Both model runs stay converged with waveform deltas below 11 nV peak for drums and 55 pV for the tone. The experimental runtime and fine-block partitions both leave it one coupled root with identical measured iteration and evaluation counts. Forcing joint-only solving increases callback time.

Substituting four existing sidechain amp equivalents with ideal closed-loop stamps shrinks the numerical root from 24 to 19 active ports. It improves ordinary callback times while still missing the budget in 486/875 Accurate drum blocks. It also changes the sidechain frequency and overload behavior. No preset edit was made.

A next engine optimization can target factorization and trial evaluation within that coupled root. The same-equation profile assigns about 0.28 s to LU and 0.37 s to trial duties in the one-second tone run. Those diagnostic phase times use wall-clock instrumentation. Native timing excludes the host and plugin. Larger buffers can absorb spikes when average throughput is below the deadline. The reproduced sustained overload needs a lower per-frame cost.

## Reproduce

`plugin_probe.cpp` uses the plugin build flags in `build-plugin.py`. Run the two-second active drum fixture through the processor to reproduce the timing:

```sh
taskset -c 2 ./plugin_probe /absolute/path/to/Neve-33609J.livewdf active-drums-48000.f32 drums-out.f32 drums-timing.csv 48000 0
```

A final argument of 1 selects Fast Live. The active fixture uses the first two seconds of the workspace studio drum waveform at a 0.75 normalized peak. `native.cpp` produces the detailed convergence counts. Its optional final mode can be stock, runtime, fine, joint or profile. The experimental preset variations in this directory are for timing and have unused wire stubs at the former op-amp power pins.
