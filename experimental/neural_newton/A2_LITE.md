# A2-Lite plate voltage seed

The model is a standard mono A2-Lite WaveNet with 23 dilated residual
convolution layers, three channels, Leaky ReLU with slope .01 and a 16-tap
head. It has 1,871 serialized values and a 6,347-sample receptive field.
The native benchmark uses NAM Core's optimized A2 path and includes the
inference time in its block timings.

The model learns a scalar from audio history. Its target is half the
difference between the two power-tube plate-to-cathode pair means. The
guesser applies opposite corrections to the two pairs to obtain the
predicted difference while keeping their common voltage unchanged. The
normal Newton solve then determines all port voltages and currents.

The experiment uses the fixed-control Rectifier revision from the earlier
warm-start benchmarks. The Sire and Strat DI are independent validation
and test signals. Each training epoch supervises all 9,120,000 samples of
the 190-second NAM V3.0.0 WAV. A uniform gain sets the input peak to .9
of the preset's input calibration. The trainer validates the source
checksum, waveform alignment and full coverage.

## Reproduce

Set `rig` to the rig-converter checkout containing the A2 Lite trainer
(`acdce52` was used). Set `core` to the NAM Core source directory and
`core_lib` to a Release static archive from the same revision with
`NAM_ENABLE_A2_FAST` enabled (`1f42f88` was used). Set `nam_v3` to the
official V3.0.0 WAV. Run from the LiveWDF checkout root.

```bash
scratch=$(mktemp -d)
cmake -S . -B "$scratch/build" -DCMAKE_BUILD_TYPE=Release \
  -DLIVEWDF_BUILD_PLUGIN=OFF -DLIVEWDF_BUILD_EDITOR=OFF \
  -DLIVEWDF_BUILD_TESTS=OFF -DLIVEWDF_BUILD_NEURAL_NEWTON=ON \
  -DLIVEWDF_NAM_CORE_SOURCE="$core" -DLIVEWDF_NAM_CORE_LIBRARY="$core_lib"
cmake --build "$scratch/build" --target livewdf_neural_newton -j4
bench="$scratch/build/livewdf_neural_newton"
fixture='schematics/amps/Mesa Boogie Dual Rectifier.livewdf'
controls='POT6=.65,POT7=.65,POT1=.756'

ffmpeg -v error -i "$nam_v3" -f f32le "$scratch/train-input.f32"
ffmpeg -v error -i tests/fixtures/guitar-techs-di/sire-t7-performance-11.flac \
  -f f32le "$scratch/sire-input.f32"
ffmpeg -v error -i tests/fixtures/guitarjam-di/strat-neck-middle-di.wav \
  -ar 48000 -ac 1 -f f32le "$scratch/strat-input.f32"

"$bench" "$fixture" "$scratch/train-input.f32" 0 190 .9 - "plate:$scratch/train" - 0 "$controls"
"$bench" "$fixture" "$scratch/sire-input.f32" 6 4 .9 - "plate:$scratch/sire" - 0 "$controls"
"$bench" "$fixture" "$scratch/strat-input.f32" 6 4 .5 - "plate:$scratch/strat" - 0 "$controls"

cc -O3 -march=native -ffp-contract=off -shared -fPIC \
  "$rig/amp-fit/web/training/a2lite.c" -lm -o "$scratch/liba2lite.so"
for recipe in mse nam; do
    python experimental/neural_newton/train_a2_plate.py \
      --rig-converter "$rig" --kernel "$scratch/liba2lite.so" \
      --captures "$scratch" --official-wave "$nam_v3" --output "$scratch/$recipe" \
      --epochs 20 --chunk 32768 --threads 6 --recipe "$recipe"
done

"$bench" "$fixture" "$scratch/sire-input.f32" 6 4 .9 - - "$scratch/stock.f64" 0 "$controls"
"$bench" "$fixture" "$scratch/sire-input.f32" 6 4 .9 "a2:$scratch/nam/a2-guess.nam" - "$scratch/learned.f64" 0 "$controls"
"$bench" "$fixture" "$scratch/sire-input.f32" 6 4 .9 "plate:$scratch/sire.y.f32" - "$scratch/perfect-scalar.f64" 0 "$controls"
```

Run the Strat comparison with `strat-input.f32`, `strat.y.f32` and peak
fraction .5. Gate zero tries the guess on every sample. Gate eight tries
when the previous sample needed at least eight iterations and line-search
trials combined.

The MSE recipe uses the native A2-Lite kernel with Adam and cosine learning
rate decay. The NAM recipe uses MSE plus .0005 multi-resolution STFT loss,
Adam at .004, weight decay 3.17e-7 and exponential epoch decay .994.
The Lite-only loader trains on the whole waveform and scores the separate
DI after every epoch. The upstream packed trainer fits Lite and Full
together, extracts train and validation regions from the V3 file, and selects
checkpoints using validation ESR. [Upstream A2 training configuration](https://github.com/sdatkinson/neural-amp-modeler/blob/main/nam/train/_resources/config_model_packed.json) · [Upstream V3 data split](https://github.com/sdatkinson/neural-amp-modeler/blob/main/nam/train/core.py)

## Training results

Each recipe completed 20 full epochs, 182,400,000 supervised sample visits,
and 940 optimizer updates. The trainer uses an explicit mask for the tail
of the final window and verifies valid target coverage after every epoch.
The train target RMS was 335.3 V. The Sire and Strat target RMS values were
449.1 V and 630.2 V.

| Recipe | Normalized MSE at epoch 1 | Epoch 10 | Epoch 20 | Sire RMSE at epoch 20 | Strat RMSE at epoch 20 |
|---|---:|---:|---:|---:|---:|
| MSE | .01468 | .01074 | .01037 | 410.6 V | 592.0 V |
| NAM loss and schedule | .01513 | .00964 | .00863 | 394.2 V | 557.5 V |

The MSE run improved by less than 1% over the final five epochs. The NAM
objective fell from .01801 to .00954 and still improved over the final
five epochs. Both learned models had substantial errors on the independent
DI. The MSE checkpoint's native Core predictions agreed with the training
kernel to within 2.45 mV peak on Sire.

The fixed-rail Rectifier revision has a 475 V output-plate supply and a
15.81:1 center-tapped transformer. On the Sire take the accepted simulated
plate-to-cathode excursions ranged from -1.88 to +2.86 kV in both powered
Preview and a tight-tolerance control run. Reconstructed node differences
agreed with their root voltages within 3 mV in Preview and about 1 nV
with the tight solve. These are the simulated primary-side voltages
used as the target. The Sire and Strat secondary output RMS voltages
were 28.0 V and 31.7 V.

## Native Newton results

The comparison used mono native Preview at 48 kHz with 128-frame blocks,
fixed adjusted controls and Balanced power mode. CPU p99 is the median
of two runs in reversed mode order. Each run has 1,500 blocks. The renders
for each mode repeated bit-for-bit.

| DI | Guess | CPU p99 | Late blocks | Newton iterations | Rejected samples |
|---|---|---:|---:|---:|---:|
| Sire | Stock | 4.76 ms | 548–1,349 | 1,015,736 | 3 |
| Sire | Exact plate difference | 4.06 ms | 561–1,031 | 1,058,617 | 6 |
| Sire | A2 Lite, MSE | 3.58 ms | 638–701 | 1,059,506 | 6 |
| Sire | A2 Lite, NAM loss | 3.46 ms | 567–634 | 1,058,596 | 4 |
| Strat | Stock | 3.58 ms | 169–331 | 1,155,838 | 19 |
| Strat | Exact plate difference | 3.67 ms | 221–461 | 1,191,952 | 18 |
| Strat | A2 Lite, MSE | 3.61 ms | 244–351 | 1,206,354 | 7 |
| Strat | A2 Lite, NAM loss | 3.65 ms | 329–418 | 1,210,417 | 6 |

The timing measurements varied with host load, especially the Sire stock
runs. A2 inference itself averaged .04–.05 ms per block. Even supplying the
exact plate difference added iterations on both tracks. The model trained
with the NAM loss produced raw output deltas of 42.4 V peak on Sire and
53.7 V peak on Strat relative to the stock solve. The narrow scalar
intervention remains an opt-in benchmark.

## Next-sample plate prediction

The history experiment feeds accepted simulated plate values back into the
guesser. The output is the same differential plate signal defined above,
one sample ahead. At sample `t`, A2-Lite receives `d[t-1]`. The small
network receives `d[t-1]` through `d[t-8]`, with an optional additional
feature for the current calibrated dry input. It has one hidden layer of 14
Leaky ReLU units and a scalar linear head. History starts at zero and
advances after each circuit solve, using the held value if the engine
rejects a sample. All models only supply the starting plate difference for
the same Newton solve.

Use the same captures and native Release build above. Train all three models
on all 9,120,000 targets from the V3 capture, for 20 epochs each. They use the
NAM loss, Adam and schedule described above.

```bash
python experimental/neural_newton/train_a2_plate.py \
  --rig-converter "$rig" --kernel "$scratch/liba2lite.so" \
  --captures "$scratch" --official-wave "$nam_v3" --output "$scratch/history-a2" \
  --input plate-history --epochs 20 --chunk 32768 --threads 6 --recipe nam
python experimental/neural_newton/train_small_plate_history.py \
  --rig-converter "$rig" --captures "$scratch" --official-wave "$nam_v3" \
  --output "$scratch/small-history" --epochs 20 --threads 2
python experimental/neural_newton/train_small_plate_history.py \
  --rig-converter "$rig" --captures "$scratch" --official-wave "$nam_v3" \
  --output "$scratch/small-conditioned" --epochs 20 --threads 2 --guitar
```

Use the same native DI command with `a2-history:$scratch/history-a2/a2-guess.nam`,
`small-plate:$scratch/small-history/guess.json` or
`small-plate:$scratch/small-conditioned/guess.json` as the model argument.
Keep the gate at zero to try every sample. For A2-Lite, the engine runs the
network one frame at a time inside block timing. The small models also run
without NAM Core.

Held-out training scores use previous samples from the stock DI trace.
During native testing each solver run uses its own accepted plate history.

### One-step prediction

Every model completed 182,400,000 supervised sample visits and 940 Adam
updates. The full NAM V3 excitation in the capture matched the original WAV
within 5.96e-8 after one uniform gain. Training uses waveform samples.
The spectral term compares STFT magnitudes at three window sizes.

| Inputs | Parameters | Sire RMSE | Strat RMSE |
|---|---:|---:|---:|
| Previous plate value directly | — | 741.0 V | 1126.3 V |
| Plate value two samples back | — | 268.8 V | 269.1 V |
| Least-squares, eight past plate samples | 9 | 253.5 V | 264.2 V |
| Least-squares, eight past plate samples and current guitar | 10 | 252.9 V | 264.0 V |
| Small net, eight past plate samples | 141 | 253.1 V | 240.0 V |
| Small net, eight past plate samples and current guitar | 155 | 249.5 V | 236.5 V |
| A2-Lite, plate history | 1,871 | 249.0 V | 230.3 V |

The least-squares fits use the same full V3 trace as the nets. The strong
two-sample correlation makes lag two a more informative baseline than
carrying the last value. Adding current guitar to the small net lowers
RMSE about 1.4% on each DI. A2-Lite trained on plate history improves
substantially over the original dry-input A2-Lite on the same two DIs.

| Model | NAM objective, epoch 1 | Epoch 15 | Epoch 20 |
|---|---:|---:|---:|
| Small net, plate history | .01053 | .00375 | .00366 |
| Small net, plate history and guitar | .01028 | .00366 | .00359 |
| A2-Lite, plate history | .00776 | .00390 | .00344 |

On both held-out stock plate traces, the interim epoch 9 A2-Lite checkpoint
returned numerically identical values in one-frame and 128-frame Core
inference. One-frame inference used about .16 ms CPU per 128 frames, up
from .056–.059 ms when batching known input. Both timings exclude prewarm.

### Native solve

The fixed-rail Rectifier ran at 48 kHz in full-rate mono Preview with
128-frame blocks, Balanced power, and one performance core. Both passes
used the same four seconds of each DI and ran the modes in opposite order.
Each render repeated bit for bit. CPU p99 is the median of the two runs.

| DI | Guess | CPU p99 | Late blocks / 1,500 | Newton iterations | Rejected samples | Output delta RMS relative to stock |
|---|---|---:|---:|---:|---:|---:|
| Sire | Stock | 4.35 ms | 11–150 | 1,015,736 | 3 | — |
| Sire | Exact plate difference from stock | 2.55 ms | 3–7 | 1,058,617 | 6 | −47.8 dB |
| Sire | Small net, plate history | 2.58 ms | 2–6 | 1,066,601 | 3 | −44.4 dB |
| Sire | Small net, plate history and guitar | 5.15 ms | 135–139 | 1,066,741 | 3 | −45.2 dB |
| Sire | A2-Lite, plate history | 4.13 ms | 20–153 | 1,067,189 | 2 | −44.6 dB |
| Strat | Stock | 2.91 ms | 39–53 | 1,155,838 | 19 | — |
| Strat | Exact plate difference from stock | 4.20 ms | 50–160 | 1,191,952 | 18 | −56.8 dB |
| Strat | Small net, plate history | 3.54 ms | 41–105 | 1,193,493 | 10 | −26.2 dB |
| Strat | Small net, plate history and guitar | 3.28 ms | 40–109 | 1,192,262 | 11 | −26.2 dB |
| Strat | A2-Lite, plate history | 4.08 ms | 122–124 | 1,191,347 | 9 | −26.1 dB |

Sire stock CPU p99 varied from 2.63 to 6.07 ms. All three learned seeds
increased Newton iterations and median CPU p50 on both takes. Their raw
output deltas reached about 43 V peak on Sire and 53–54 V on Strat.
Even the exact scalar from the stock trace added iterations, which shows
the limit of replacing the plate differential alone in the coupled seed.

A second check gated the A2-Lite and guitar-conditioned small net at eight
iterations plus line-search trials in the preceding sample. Both still
added iterations on each DI. The raw output differences were comparable
to their ungated runs. The models remain opt-in benchmark modes.
