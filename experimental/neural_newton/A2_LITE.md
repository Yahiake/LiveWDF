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
