# Neural Newton warm starts

This benchmark fits a circuit-specific voltage correction for the joint Newton
solver. The model sees the stock guess, the previous port voltage change, the
exact residual at the stock guess and one control position. It trains on
accepted circuit states. The one- and two-hidden-layer models use Leaky ReLU with a negative slope of
0.01.

The solver evaluates the proposed voltages with the native device laws. It
uses the proposal if its maximum residual is at most half the stock residual.
The learned and stock seeds use the same Newton iteration budget, convergence
and physical state checks, and last-accepted-voltage fallback.

## Reproduce

Build the native benchmark and use Python with NumPy and PyTorch installed.
The examples assume a little-endian machine and ffmpeg. Run from the checkout
root.

```bash
scratch=$(mktemp -d)
cmake -S . -B "$scratch/build" -DCMAKE_BUILD_TYPE=Release \
  -DLIVEWDF_BUILD_PLUGIN=OFF -DLIVEWDF_BUILD_EDITOR=OFF \
  -DLIVEWDF_BUILD_TESTS=OFF -DLIVEWDF_BUILD_NEURAL_NEWTON=ON
cmake --build "$scratch/build" --target livewdf_neural_newton -j
bench="$scratch/build/livewdf_neural_newton"
di=tests/fixtures/guitar-techs-di
for track in pf300-palm-mutes pf300-major-chords pf300-high-bends sire-t7-performance-11; do
  ffmpeg -v error -i "$di/$track.flac" -f f32le "$scratch/$track.f32"
done
ffmpeg -v error -i tests/fixtures/guitarjam-di/strat-neck-middle-di.wav \
  -ac 1 -ar 48000 -f f32le "$scratch/strat.f32"

circuit='schematics/amps/Mesa Boogie Dual Rectifier.livewdf'
controls() { printf 'POT6=%s,POT7=%s,POT1=.756' "$1" "$1"; }
"$bench" "$circuit" "$scratch/pf300-palm-mutes.f32" 3 3 .9 - "$scratch/palm.bin" - 0 "$(controls .58)"
"$bench" "$circuit" "$scratch/pf300-major-chords.f32" 1 3 .9 - "$scratch/chord.bin" - 0 "$(controls .72)"
"$bench" "$circuit" "$scratch/pf300-high-bends.f32" 1 3 .9 - "$scratch/bends.bin" - 0 "$(controls .65)"
python experimental/neural_newton/train.py \
  --train "$scratch/palm.bin" "$scratch/chord.bin" --validation "$scratch/bends.bin" \
  --output "$scratch/model.weights" --widths 10 8 --epochs 35

"$bench" "$circuit" "$scratch/sire-t7-performance-11.f32" 6 4 .9 - - "$scratch/stock.f64" 8 "$(controls .65)"
"$bench" "$circuit" "$scratch/sire-t7-performance-11.f32" 6 4 .9 "$scratch/model.weights" - "$scratch/learned.f64" 8 "$(controls .65)"
```

Use `--widths 14` for the one-hidden-layer model or pass `--widths` without
values for an affine fit. The wider run used `--widths 20 12 --epochs 110`.

The final benchmark arguments are the previous sample's minimum Newton work
and the ordered pot list. Work is the sum of iterations and line-search
trials. Use a gate of zero to try the model on every unresolved seed. A
negative gate turns on the phase profiler. The retry counter counts the
normal last-accepted-voltage fallback after a selected learned seed. Use unprofiled runs with trace
collection off for timing. The first pot position is the model input, so keep
the pot list in the same order. The weights are tied to the schematic and
control order. The input peak fraction is relative to each preset's voltage
calibration. The output is raw node voltage in mono float64.

| Circuit | Pots in model order | Fit / validation and test position |
|---|---|---|
| Mesa Boogie Dual Rectifier | `POT6`, `POT7` (ganged Master), `POT1=.756` | .58 and .72 / .65 |
| EVH 5150 III 100S Stealth Channel 3 | `POT5` (Volume), `POT1=.7` | .58 and .72 / .65 |
| Soldano SLO-100 Overdrive | `VR Normal Gain`, `VR OD Master=.65` | .7 and .9 / .8 |
| Fuzz Face | `POT2` (Fuzz) | .72 and .9 / .8 |

## Scope

The benchmark runs the native mono engine at 48 kHz with 128-frame blocks,
the full signal path and settled live-control transfer. Powered Preview uses
the existing 3 mV Newton tolerance. Fuzz Face uses the stock Fast Live
Newton tolerance. The normal build excludes the experiment.

## Matched-budget follow-up

Both seeds now use the engine's maximum of 60 iterations, with the same
convergence and stagnation tests. On the same Rectifier Sire take, the
10/8 model selected 25,588 proposals and triggered four ordinary fallbacks.
The capped version interrupted 21,970 proposals before retrying the stock seed.
Full-budget Newton iterations still rose from 1,015,736 to 1,033,273 on that
take, and device evaluations from 15.9 million to 17.9 million.

On the same training examples and validation take, the 20/12 model trained
for 110 epochs reduced voltage prediction error across all four circuits.
Runtime comparisons use separate held-out Sire and Strat DI.

| Circuit | Normalized validation MSE, 10/8 → 20/12 | Sire Newton iterations, stock → 20/12 | Sire rejected samples, stock → 20/12 |
|---|---:|---:|---:|
| Dual Rectifier | 0.689 → 0.458 | 1,015,736 → 1,026,096 | 3 → 2 |
| EVH 5150 III | 0.228 → 0.124 | 784,907 → 776,556 | 141 → 133 |
| Soldano SLO-100 | 1.145 → 0.398 | 1,011,616 → 1,026,249 | 584 → 618 |
| Fuzz Face | 0.666 → 0.630 | 666,061 → 663,678 | 0 → 0 |

In the amp runs where the stock solver already rejected samples, even similar
rejection counts could correspond to different samples and visibly different
output. On the Sire take the wider model's raw output peak difference reached
21.6 V on the Rectifier and 34.9 V on the EVH. Its EVH p99 improved on the Sire
take but the run had more late blocks, and its p99 rose on the Strat take.

### NAM v3 augmentation

A separate Rectifier fit added four 1.5-second clips of the verified NAM
v3.0.0 input to the two stock guitar training traces. Use the source WAV with
SHA-256 `70f8ec7f25686a1bd77f25973de8e51a6721e957e81eec121822e5e53366bc41`.
Convert to float32 as above. Capture training traces starting at seconds 25,
53, 104 and 174 at .9 peak fraction and Master settings .58, .72, .58 and
.72 respectively. Add them to `--train` and use `--widths 20 12 --epochs 110`.
The Sire and Strat DI and the bend validation take stay held out.

The augmented seed made the Rectifier solve more expensive on both DI takes.
Sire iterations rose to 1,132,378 and ordinary fallbacks to 139. Both CPU
p99 and deadline misses worsened in the runs repeated on the same CPU.

## Original capped trial

This trial capped the learned pass at three iterations and restarted from the
stock seed on a miss. The current benchmark gives either seed the same
iteration budget.

Measured on a Core Ultra 7 155H using base commit `dcbc2f1`. The table uses
the two-hidden-layer model with the work gate set to eight. CPU times are
medians of two runs in stock/learned and learned/stock order on the held-out
Sire T7 performance at a 0.784 V peak. The budget is 2.667 ms per block.

| Circuit | CPU p99 stock → learned | CPU p99.9 stock → learned | Late blocks per 1,500 stock → learned |
|---|---:|---:|---:|
| Dual Rectifier | 3.96 → 4.59 ms | 4.45 → 5.10 ms | 1,009–1,016 → 1,212–1,217 |
| EVH 5150 III | 4.30 → 4.77 ms | 8.47 → 8.66 ms | 964–966 → 1,199–1,204 |
| Soldano SLO-100 | 11.78 → 12.06 ms | 17.18 → 17.57 ms | 851 → 868–872 |
| Fuzz Face | 0.419 → 0.421 ms | 0.472 → 0.454 ms | 0 → 0 |

On the independent Strat DI at 0.435 V peak, all four p99 values rose.
Both hidden-layer shapes improved offline voltage prediction on the validation
takes. Neither consistently improved engine timing. The affine fit also
increased Newton work on the Mesa and Fuzz Face tests.

The exact residual check selected 25,583 Mesa proposals on the Sire take.
Of those, 21,970 still retried the normal solve. A smaller initial residual
rarely translated into a cheap solve. The extra inference, device evaluation
and retries increased tail latency.

The Sire rejections were 3 → 3 for Mesa, 141 → 141 for EVH, 584 → 583 for
Soldano and 0 → 0 for Fuzz Face. The Soldano output differed by -48 dB RMS
relative to the stock AC output with a 14.4 V peak error. Its Strat comparison
was -125 dB RMS. Rejection-sensitive runs need time-aligned waveform checks
even when rejection counts are nearly identical.
