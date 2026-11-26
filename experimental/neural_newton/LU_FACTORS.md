# Continuous LU factor prediction

The predictor emits new numerical entries for the two triangular factors at
the first joint Newton step. Its inputs are the current port voltages and
reciprocals of the reduced matrix's diagonal magnitudes, bounded above by one.
The diagonal is formed from the current device Jacobian and the fixed circuit
transfer. The model predicts eight coefficients of a basis fitted to
diagonally scaled, fixed-order LU factors. The decoded factors operate on the
equilibrated native right-hand side with the fixed pivot order.

The experiment tries the proposed correction once. It takes the correction
when the real nonlinear residual is finite and strictly decreases. When a
direction fails that check, the unchanged iterate goes through the existing
exact factorization. The normal Newton budget and convergence checks apply.

## Reproduce

Build the opt-in `livewdf_neural_newton` target and capture the stock LU traces
as shown in [the LU-bank experiment](LU_BANK.md). Keep all traces at the same
control settings. The default training run uses 70 factor-fit epochs and 12
epochs of fine-tuning against the true linearized voltage defect.

```bash
python experimental/neural_newton/train_lu_factors.py \
    --train "$scratch/palm.bin" "$scratch/chord.bin" \
    --holdout "$scratch/sire.bin" "$scratch/strat.bin" \
    --output "$scratch/factors.weights"
"$bench" "$preset" "$scratch/sire-t7-performance-11.f32" 6 4 .9 \
    lu-direct:"$scratch/factors.weights.correction" - "$scratch/direct.f64" 8 "$controls"
"$bench" "$preset" "$scratch/sire-t7-performance-11.f32" 6 4 .9 \
    - - "$scratch/stock.f64" 8 "$controls"
```

The learner also exposes `--scaling row` as an offline comparison using the
full matrix's row maxima. Native inference uses diagonal scaling, which
needs only the existing device Jacobian and the diagonal products.

## Held-out linear test

Same circuit and tracks as the LU-bank experiment. The Rectifier matrix has
26 rows, the input has 33 voltages, and each held-out trace has 12,000
first-Newton systems. The threshold is normalized linearized voltage error
below 0.4 on the exact current system.

| Method | Sire | Strat |
| --- | ---: | ---: |
| Neural cached-factor selection | 58.7% | 75.0% |
| Continuous factor fit | 55.0% | 60.3% |
| Continuous factor fit + correction loss | 65.2% | 75.0% |
| Row maximum scaling + correction loss | 47.9% | 60.6% |

In a subset of 300 training systems, partial pivoting found 108 unique row
orders after diagonal scaling. The fixed order chosen on the training set was
the original row order. Of 24,000 training systems, 23,951 had finite
fixed-order factors within the training limits. The correction-loss fit
produced finite linearized defects on all held-out traces.

## Native result

Mono powered Preview on the same adjusted Rectifier at 48 kHz and 128-frame
blocks. CPU p99 is the median of two runs in opposite mode order on one
pinned Core Ultra 7 155H performance core under Balanced. Each run contains
1,500 blocks. Output deltas are relative to the stock AC waveform RMS.

| Track | CPU p99 stock → direct | Late blocks stock → direct | Accepted proposed steps | Rejected samples stock → direct | Raw output difference |
| --- | ---: | ---: | ---: | ---: | ---: |
| Sire, 0.784 V peak | 2.72 → 3.49 ms | 25 → 685–703 | 158,504 / 191,159 | 3 → 4 | −45.2 dB RMS, 42.43 V peak |
| Strat, 0.435 V peak | 3.09 → 3.65 ms | 76–88 → 584–593 | 171,220 / 191,716 | 19 → 14 | −26.3 dB RMS, 51.90 V peak |

The decoder updates 337 of 676 packed factor entries. The 59 → 20 → 8
inference and rank-eight reconstruction take 4,036 multiply-accumulates,
plus equilibration, triangular solves, back-substitution and the true
residual evaluation. This is a count, and the timings above measure the
complete engine. Sire counted 1,015,736 stock iterations and 1,313,794 with
predicted factors. Subtracting all 191,159 proposed directions still leaves
106,899 more counted iterations. Strat similarly leaves 89,080 more after
subtracting its 191,716 proposals. Both render modes repeated bit-for-bit.

Direct generation skipped many first factorizations. Its total timing and
output precision keep this configuration experimental.
