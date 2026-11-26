# Learned LU-bank experiment

This benchmark uses the joint Newton loop with its normal iteration budget.
The network selects a pivoted LU factorization from eight representative
Newton matrices. The program factors the representatives with the native
dense solver during setup. The live right-hand side comes from the circuit
Jacobian at the current iterate.

A proposal gets one trial with the usual voltage and junction limits. If the
true circuit residual drops below 40% of its previous value, Newton continues
from the new iterate. Otherwise the solver builds and factors the current
matrix at the same iterate. The regular convergence tolerance applies.

The bank is specific to a circuit and settled control settings. It targets the
first joint Newton correction and uses the existing benchmark-only build
option. Op-amp systems use the normal solve.

## Reproduce

Build `livewdf_neural_newton` and convert the DI fixtures to mono 48 kHz
float32 as in the [voltage-guess benchmark](README.md). The fitting script
also needs SciPy.

```bash
preset='schematics/amps/Mesa Boogie Dual Rectifier.livewdf'
controls='POT6=.65,POT7=.65,POT1=.756'
for track in palm chord sire strat; do
    start=6
    drive=.9
    case $track in
        palm) input=pf300-palm-mutes; start=3;;
        chord) input=pf300-major-chords; start=1;;
        sire) input=sire-t7-performance-11;;
        strat) input=strat; drive=.5;;
    esac
    "$bench" "$preset" "$scratch/$input.f32" "$start" 3 "$drive" - "lu:$scratch/$track.bin" - 8 "$controls"
done
python experimental/neural_newton/train_lu_bank.py \
    --train "$scratch/palm.bin" "$scratch/chord.bin" \
    --holdout "$scratch/sire.bin" "$scratch/strat.bin" \
    --output "$scratch/bank.txt"
for selector in lu lu-nearest lu-constant; do
    "$bench" "$preset" "$scratch/sire-t7-performance-11.f32" 6 4 .9 "$selector:$scratch/bank.txt" - - 8 "$controls"
done
"$bench" "$preset" "$scratch/sire-t7-performance-11.f32" 6 4 .9 - - - 8 "$controls"
```

The holdout script compares the network with the nearest representative,
the best constant factorization on the training set, and an offline oracle.
Their relative linearized voltage defect is
`max(abs(Z * (A * deltaCurrent - rhs))) / max(abs(F))`. The native DI run
measures the separately checked nonlinear residual and end-to-end timing.

## Results

The Rectifier had 33 voltage ports and a 26 × 26 reduced Newton matrix.
Training used 24,000 systems from palm mutes and chords. Sire and Strat
supplied 12,000 held-out systems each.

| Selector | Sire defect below 0.4 | Strat defect below 0.4 |
|---|---:|---:|
| Best constant | 35.6% | 53.5% |
| Nearest representative | 52.2% | 68.7% |
| Neural selector | 58.7% | 75.0% |
| Offline oracle | 67.2% | 78.4% |

Native mono Preview tests ran at 48 kHz with 128-frame blocks on one pinned
Core Ultra 7 155H performance core. The following CPU p99 values are medians
of two runs in opposite order. Late blocks exceeded 2.667 ms. The runs used
fixed adjusted control settings and include solver overhead but exclude the
plugin callback.

| DI and selector | CPU p99 | Late blocks per 1,500 | Accepted bank trials | Rejected samples |
|---|---:|---:|---:|---:|
| Sire stock | 5.20 ms | 1,200–1,269 | — | 3 |
| Sire constant | 4.86 ms | 1,346 | 29,483 | 3 |
| Sire nearest | 6.15 ms | 1,405 | 43,814 | 4 |
| Sire neural | 6.35 ms | 1,393–1,399 | 46,443 | 3 |
| Strat stock | 5.66 ms | 1,366–1,372 | — | 19 |
| Strat constant | 5.18 ms | 1,480–1,485 | 17,607 | 19 |
| Strat nearest | 5.35 ms | 1,482–1,490 | 27,559 | 20 |
| Strat neural | 5.49 ms | 1,482–1,485 | 28,445 | 19 |

The learned selector tried 192,000 bank directions on each track. Each
accepted trial skipped the first fresh factorization. The native circuit
check passed 24.2% on the Sire DI at 0.784 V peak and 14.8% on the Strat at
0.435 V peak. Reported iterations increased from 1,015,736 to 1,242,741
on Sire and from 1,155,838 to 1,369,113 on Strat, including the bank trials.
Both tracks still had more counted Newton iterations after removing those
attempts.

Relative to the stock AC waveform RMS, the neural selector differed by
−105.8 dB on Sire and −109.7 dB on Strat. Peak raw output deltas were
11.9 mV and 8.6 mV. The nearest selector changed the rejection count and
produced peak raw deltas of 12.0 V and 40.1 V. The constant selector also
produced an 18.6 V peak deviation on Sire despite matching the rejection
count.

The learned selector improved the offline score, while the online trial
increased deadline misses on both clips. The benchmark remains opt-in.
