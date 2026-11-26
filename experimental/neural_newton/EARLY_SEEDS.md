# Voltage and current seeds before device evaluation

The voltage model predicts a correction to the usual extrapolated voltage
guess. The current model predicts a correction to the previous accepted port
currents. Lift the predicted currents through the exact linear part of the
network with `v = vOc - Zred * i`. The carried-current baseline uses the
previous accepted currents directly.

Both networks read the previous port voltages, their recent change and the
current change in open-circuit drive. They run before the first device or
Jacobian evaluation. Finite proposals within the usual voltage movement
bounds start the normal Newton pass. The device laws, iteration budget and
convergence checks still apply.

## Reproduce

Build the opt-in benchmark and convert the DI files to 48 kHz float32 as in
[the original warm-start experiment](README.md). Use fixed live controls for
both trace capture and playback.

```bash
fixture='schematics/amps/Mesa Boogie Dual Rectifier.livewdf'
controls='POT6=.65,POT7=.65,POT1=.756'
for mode in voltage current; do
    "$bench" "$fixture" "$scratch/pf300-palm-mutes.f32" 3 3 .9 - "$mode:$scratch/$mode-palm.bin" - 0 "$controls"
    "$bench" "$fixture" "$scratch/pf300-major-chords.f32" 1 3 .9 - "$mode:$scratch/$mode-chord.bin" - 0 "$controls"
    "$bench" "$fixture" "$scratch/pf300-high-bends.f32" 1 3 .9 - "$mode:$scratch/$mode-bends.bin" - 0 "$controls"
    python experimental/neural_newton/train.py \
      --train "$scratch/$mode-palm.bin" "$scratch/$mode-chord.bin" \
      --validation "$scratch/$mode-bends.bin" --output "$scratch/$mode.weights" \
      --widths 20 12 --epochs 110
done
for seed in - carry-current "voltage:$scratch/voltage.weights" "current:$scratch/current.weights"; do
    "$bench" "$fixture" "$scratch/sire-t7-performance-11.f32" 6 4 .9 "$seed" - - 8 "$controls"
done
```

Repeat with the Strat DI and a .5 peak fraction. The weights match the
chosen seed mode and fixed control settings. With gate eight, the learned
variants try a correction when the previous sample needed at least eight
iterations plus line-search trials. The carried-current baseline tries
on every sample.

## Results

The matched runs used the earlier Rectifier circuit and engine from the
original experiment, 48 kHz at 128 frames in mono native Preview. The Sire
input peaks at 0.784 V and the Strat at 0.435 V. CPU p99 is the median of two
opposite-order runs on Balanced. Late blocks exceed 2.667 ms.

| DI | Seed | CPU p99 | Late blocks per 1,500 | Iterations | Rejected samples | Peak output difference |
|---|---|---:|---:|---:|---:|---:|
| Sire | Stock | 2.95 ms | 60–165 | 1,015,736 | 3 | — |
| Sire | Carried currents | 3.23 ms | 226–488 | 1,098,936 | 34 | 79.65 V |
| Sire | Learned voltage | 3.37 ms | 421–562 | 1,094,538 | 3 | 5.98 mV |
| Sire | Learned current | 3.56 ms | 656–775 | 1,120,891 | 3 | 6.74 mV |
| Strat | Stock | 3.35 ms | 130–162 | 1,155,838 | 19 | — |
| Strat | Carried currents | 3.37 ms | 201–259 | 1,208,764 | 33 | 56.83 V |
| Strat | Learned voltage | 3.69 ms | 260–324 | 1,197,153 | 15 | 52.58 V |
| Strat | Learned current | 3.70 ms | 261–310 | 1,207,927 | 18 | 44.89 V |

The early voltage fit lowered normalized validation MSE from 1.786 to .380.
The current fit lowered its own normalized MSE from 1.084 to .253. The
metrics use different units and scales. On the held-out DI both learned
seeds still increased Newton work, even with the initial residual comparison
removed. The large Strat waveform differences occurred in runs with
existing rejection-sensitive behavior.
