# Drawmer DS201 live crackle

The Drawmer DS201 preset is identical on PR #38 and PR #42 at the checked revisions. Its SHA-256 is `29bb3bf6163dfa89630969e3be8192ffa069f8559e97bef8a6bb283418e46401`.

The plugin callback misses its 2 ms deadline at 48 kHz and 96 frames. The offline circuit outputs remained continuous through a threshold sweep, a gated drum program and a forced-open audio test. Opening the Range fully and engaging circuit Bypass each preserved the plugin overload. Those controls alter the audio transfer, while the engine still evaluates the entire circuit.

## Actual callback

The harness copies the same mono fixture into both plugin channels and measures processBlock thread CPU. No audio allocation was recorded. The second harness applies the requested Accurate/Fast Live parameter before prepareToPlay and dumps actual output samples outside the callback timer. Times in the table come from that run.

| Fixture | Accurate median | Accurate p99 | Fast Live median | Fast Live p99 |
|---|---:|---:|---:|---:|
| Silence | 0.91 ms | 2.35 ms | 1.02 ms | 2.77 ms |
| 1 kHz at changing level near threshold | 2.24 ms | 3.48 ms | 2.08 ms | 3.12 ms |
| Loud synthetic drums | 1.85 ms | 3.88 ms | 1.64 ms | 4.05 ms |

A first loud-drum pass on the same runner gave approximately 3.15 ms median in normal mode, 3.17 ms with Range fully open, and 3.08 ms in circuit Bypass. The two runs give different absolute timing under changing load and all three modes miss the callback deadline in that matched first pass. Fast Live's raw output differed from Accurate by under 7.5e-9 normalized peak on the two driven fixtures, while both exceeded deadline at the 99th percentile.

## Offline circuit

The native engine benchmark used one mono engine at 48 kHz. It counted zero rejected or unconverged frames on the 6.29 second, 301,920-frame two-level drum fixture, the same fixture with Range open, the near-threshold tone, and silence. On the silence capture, native output was about 0.033 nV RMS including startup. With 1 kHz near threshold, windowed native energy above 8 kHz was down about 144 dB relative to the output. The dry/wet preview preserves a common playback gain and the resulting gate attenuation.

The model contains one jointly coupled block with 52 physical root ports and 26 independent nonlinear current unknowns, 11 of which are op-amp outputs. At 0.12 V peak, the native block solver averaged about five Newton iterations per frame. Forcing joint Newton or enabling the fine split increased cost. Local prototypes bypassing unused affine setup, reusing the existing matrix buffer more directly, using a size-26 specialized LU, and extrapolating previous root voltages did not produce a reliable deadline fix. The preset and repository files were left as checked.
