# Joint Newton LU reuse experiment

This is a native renderer opt-in for powered Preview. It retains factored
matrices within one Newton pass. A new device Jacobian provides the right-hand
side. A cached step must meet the normal tolerance or reduce the actual maximum
residual by a factor of at least 2.5. A failed trial refreshes the matrix at the
current iterate and leaves the normal Newton budget and line search available.

## Reproduce

Run at the checkout root with ffmpeg and Python with NumPy.

```bash
scratch=$(mktemp -d)
cmake -S . -B "$scratch/build" -DCMAKE_BUILD_TYPE=Release \
  -DLIVEWDF_BUILD_PLUGIN=OFF -DLIVEWDF_BUILD_EDITOR=OFF
cmake --build "$scratch/build" --target livewdf_circuit_render -j4
ffmpeg -v error -ss 6 -t 4 -i tests/fixtures/guitar-techs-di/sire-t7-performance-11.flac \
  -ar 48000 -ac 1 -f f32le "$scratch/input.f32"
python3 - "$scratch" <<'PY'
import json
from pathlib import Path
import sys
import numpy as np
scratch = Path(sys.argv[1])
signal = np.fromfile(scratch / "input.f32", dtype="<f4").astype("<f8")
signal *= .9 / np.max(np.abs(signal))
signal.tofile(scratch / "input.f64")
fixture = {
    "circuit": "schematics/amps/Mesa Boogie Dual Rectifier.livewdf",
    "stimulus": str(scratch / "input.f64"),
    "realtime_preview": True,
    "warmup": 24000,
    "moves": [{"sample": 0, "positions": {"MASTER": .65, "GAIN": .756}}],
}
for mode in ("stock", "reuse"):
    fixture["joint_lu_reuse"] = mode == "reuse"
    (scratch / f"{mode}.fixture.json").write_text(json.dumps(fixture))
PY
for mode in stock reuse; do
  "$scratch/build/tests/livewdf_circuit_render" "$scratch/$mode.fixture.json" \
    preview "$scratch/$mode" render
done
```

Use the reports' `blocks.cpu_us` arrays to compare the p99 and counts over
2,666.7 µs. `rejected` counts rejected samples. Compare the paired
`.f64` renders directly. A separate run with `"profile_joint_lu": true`
reports `reuse_trials` and `reuse_accepted`.

For the EVH use `EVH 5150 III 100S Stealth - Channel 3.livewdf` with
`VOLUME` at .65 and `GAIN` at .7. For the Soldano use
`Soldano SLO-100 - Overdrive.livewdf` with `Normal Gain` at .8 and
`Overdrive Master` at .65. For the lower-drive take use
`tests/fixtures/guitarjam-di/strat-neck-middle-di.wav` at a .5 normalized peak.
The control movement test adds steps at 12000, 36000, 72000 and 120000 samples
with offsets of -.15, +.10, -.06 and 0 from the first control position.

## Results

Full signal paths, mono at 48 kHz with 128-frame blocks on a Core Ultra 7
155H. The 4-second Sire take uses a .784 V input peak and the Strat a .435 V
peak. These factory settings used the full reduced joint solve and the
regular powered Preview tolerance. A focused test checks the grouped path.

| Signal | Circuit | Control | CPU p99 stock → reuse | Late blocks out of 1,500 | Rejects | Waveform difference |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| Sire | Rectifier | Held | 5.35 → 4.54 ms | 1121 → 1178 | 3 → 3 | −111 dB RMS |
| Sire | EVH | Held | 5.10 → 4.90 ms | 1053 → 1095 | 141 → 144 | −42.5 dB RMS |
| Sire | Soldano | Held | 11.89 → 14.32 ms | 905 → 922 | 584 → 569 | −17.0 dB RMS |
| Sire | Rectifier | Stepped | 5.64 → 5.54 ms | 1261 → 1309 | 14 → 14 | −110 dB RMS |
| Sire | EVH | Stepped | 5.69 → 5.11 ms | 1222 → 1259 | 119 → 119 | −32.2 dB RMS |
| Strat | Rectifier | Held | 6.24 → 5.97 ms | 1481 → 1478 | 19 → 19 | −114 dB RMS |
| Strat | EVH | Held | 5.63 → 5.30 ms | 861 → 595 | 24 → 24 | −78.6 dB RMS |

In a second Sire run with reversed order, the Rectifier's p99 was
4.28 → 4.84 ms and the EVH's was 5.01 → 4.69 ms. Waveform deltas and
rejection counts repeated exactly. With profiling enabled, accepted cached
steps were 36,930 of 78,407 for the Rectifier, 66,331 of 72,010 for the EVH,
and 34,767 of 59,959 for the Soldano.

The workload-dependent CPU changes and EVH/Soldano waveform deltas keep this
an experiment. Reuse stays disabled in the engine by default. A fused direct
solve that retains the LU factors could separate first-solve arithmetic changes
from the effects of taking a cached step.
