# Joint port voltage warm starts

Two small networks predict corrections to the stock Newton guess for all 33
ports of the fixed-control Rectifier used in [the plate experiment](A2_LITE.md).
Both see the last accepted voltages, their change from the previous sample,
and the current dry input. One also sees the stock extrapolated voltages.
Both have one hidden layer of 14 Leaky ReLU units and apply their correction
to the same stock starting vector.

## Training and reproduction

Build the opt-in benchmark and decode the DI and official NAM V3 WAV as in
the plate experiment. The joint models do not require NAM Core.

```bash
fixture='schematics/amps/Mesa Boogie Dual Rectifier.livewdf'
controls='POT6=.65,POT7=.65,POT1=.756'
"$bench" "$fixture" "$scratch/train-input.f32" 0 190 .9 - "ports:$scratch/train" - 0 "$controls"
"$bench" "$fixture" "$scratch/sire-input.f32" 6 4 .9 - "ports:$scratch/sire" - 0 "$controls"
"$bench" "$fixture" "$scratch/strat-input.f32" 6 4 .5 - "ports:$scratch/strat" - 0 "$controls"

python experimental/neural_newton/train_port_history.py --rig-converter "$rig" \
  --captures "$scratch" --official-wave "$nam_v3" --output "$scratch/history"
python experimental/neural_newton/train_port_history.py --rig-converter "$rig" \
  --captures "$scratch" --official-wave "$nam_v3" --output "$scratch/stock" --stock
```

Each model used all 9,120,000 frames of the verified 190-second NAM V3
excitation for 20 epochs and 2,800 optimizer updates. Capture includes
the initial two accepted states, rejected frames and the stock pre-limiter
seed. The trainer reconstructs the held history after a rejection and the
DC state after eight consecutive rejections. Each update uses the MSE of
all 33 individually scaled waveforms. It adds the NAM .0005 MRSTFT term
on one port selected through shuffled cycles over all ports. Each port
received 84 or 85 spectral updates. Adam and its decay match the A2 recipe.

Use `ports:.../guess.txt` as the model argument for a live run. The text
weights include the circuit and fixed control fingerprint. Use
`ports-guarded:.../guess.txt` for the ordinary residual-checked seed path.
That path evaluates the device currents and Jacobian at the proposed guess,
checks that the whole-circuit residual is at most half the stock residual,
and reuses the candidate's evaluation if selected.

Use `paired` in place of the trace argument with `ports:.../guess.txt`
to score the two starting guesses from the same state. This mode runs
Newton from the stock guess and scores the candidate after the same initial
grid limiter. It excludes proposals outside the existing per-port movement
bounds. A reference run can use `ports-replay:$scratch/sire.y.f32` and
`paired` to check the trace's solved vector. Profile with gate `-1` and
run deadline timing separately with gate `0`.

## Held-out accuracy and paired residual

| Input and model | Sire normalized RMSE | Strat normalized RMSE |
|---|---:|---:|
| Stock Newton | .15314 | .21544 |
| Accepted voltage from two samples back | .07016 | .07142 |
| History and current guitar | .04746 | .04633 |
| History, current guitar and stock guess | .04855 | .04785 |

The training objective fell from .00421 to .00192 for history and guitar,
and from .00406 to .00191 with the stock feature. Both fits flattened late.
The stock feature slightly increased held-out error on both tracks.

| DI | Extra stock feature | Comparable guesses of 192,000 | Lower residual among comparable | Residual ratio p50 / p90 |
|---|---|---:|---:|---:|
| Sire | Off | 99,672 | 82.8% | .477 / 1.647 |
| Sire | On | 104,286 | 77.7% | .519 / 1.932 |
| Strat | Off | 36,857 | 85.7% | .435 / 1.379 |
| Strat | On | 40,049 | 80.1% | .442 / 1.505 |

A ratio below one favors the candidate. Median residual roughly halves
among comparable guesses and the upper tail grows. Paired renders were
byte-identical to stock with matching solver work. On allowed trace replay
guesses the exact full vector reduces median residual by about seven
orders of magnitude. It uses the already-solved sample as a reference.

## Live Newton solve

Release mono full-rate Preview at 48 kHz and 128 frames, Balanced power,
pinned to one P-core. Timing is the median of two reversed-order runs.
All corresponding renders were byte-identical across repeats.

| DI | Guess | CPU p99 ms | Late blocks / 1,500 | Newton steps | Rejected samples | Output delta RMS |
|---|---|---:|---:|---:|---:|---:|
| Sire | Stock | 2.685 | 4–69 | 1,015,736 | 3 | — |
| Sire | History and guitar | 3.429 | 140–716 | 1,208,743 | 2 | −45.58 dB |
| Sire | With stock input | 4.064 | 213–804 | 1,229,909 | 2 | −44.77 dB |
| Sire | History and guitar, guarded | 3.522 | 63–518 | 1,111,638 | 3 | −104.69 dB |
| Sire | With stock input, guarded | 3.367 | 44–160 | 1,092,185 | 4 | −50.05 dB |
| Strat | Stock | 2.984 | 41–46 | 1,155,838 | 19 | — |
| Strat | History and guitar | 3.402 | 138–165 | 1,249,814 | 11 | −26.14 dB |
| Strat | With stock input | 3.559 | 146–228 | 1,264,881 | 11 | −26.19 dB |
| Strat | History and guitar, guarded | 3.131 | 89–96 | 1,212,072 | 10 | −26.23 dB |
| Strat | With stock input, guarded | 3.800 | 108–133 | 1,208,876 | 11 | −26.25 dB |

Both networks improve held-out voltage error and the typical paired
residual. All live configurations add iterations and miss more deadlines
than stock. The guarded model largely preserves the Sire waveform. The
Strat render remains sensitive to different accepted and held states.

## Capped-step probe

`cap8-hold` and `cap8-accept` run one joint pass with eight Newton
iterations, no deep backtracking extension, no ordinary retry and no
domain re-passes. The hold mode keeps the convergence requirement. The
accept mode can commit an unconverged result that meets finite residual
and existing voltage, current and node bounds. These switches are only
built in the opt-in benchmark.

On these Rectifier takes, 184,669 of 192,000 Sire samples and 184,116
Strat samples remained unconverged in either capped mode. Accepting
within the bounds stopped the rejections and still missed 1,462 Sire and
1,474 Strat deadlines. Holding frequently triggered the existing
synchronous recovery path and missed 1,482 and 1,492 deadlines.
