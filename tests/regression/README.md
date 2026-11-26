# Circuit regression checks

CTest renders the factory circuits in Accurate, Fast Live, and Preview at
48 kHz, mono, 1×, and 128 samples per block. The renderer uses the plugin's input
alignment, Preview peak, and pad eligibility. Python 3.8 or newer runs the
comparisons against committed references.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure -L regression
```

Reports are written to `build/tests/circuit-*.json`.

## Coverage

- **Factory smoke** checks every factory preset in each mode. It compares waveform
  checkpoints, full-rate peak, DC and step bounds, and spectral levels. Rejected
  states cannot increase above the saved count.
- **Controls** check eleven groups at 20%, 50%, and 80%, plus ramps in both
  directions back to the starting value. The checks apply ganged changes
  together, select the Neve's 330 Hz bass band, and allow time for delay output.
  They also verify that the controls have a measurable effect.
- **Saved ngspice** checks Micro Amp, SD-1, TS808, and Neve 1081 at three
  positions and 375, 1500, and 6000 Hz. Micro Amp and SD-1 also have transient
  comparisons driven into clipping. The decks, hashes, and simulator version
  are retained alongside the 38 references.
- **Solver work** records Newton iterations, device evaluations, line-search
  trials, and prepared-route hits. Work budgets apply when the routes are
  comparable. Route changes and timing remain visible in the report.

The original waveform baseline is core revision
`74c7d8bf20516a20d8a173c9804df1ab3362b8c6`. The factory amp collection was recorded
with the renderer at `c2ce044`. Saved waveform RMS limits are
5% for Accurate and 10% for Fast Live and Preview, with an absolute numerical
floor. The ngspice limits are 0.75 dB below 6 kHz, 1.25 dB at 6 kHz, 10° of phase,
and 8% transient RMS error. The 6 kHz limit accounts for the measured baseline
difference between the 48 kHz engine and the analog reference.

The compact snapshot stores every seventh sample. Seven is coprime to the
stimulus periods, and the checker also tests full-rate bounds. The fixture
budget is 2 MiB. Timing is advisory. Plugin parameter smoothing, oversampling,
stereo, and concurrent DAW hosting require integration tests.

## Updating references

Review the independent comparisons and rebuild the renderer before recording.
Only one writer should record a suite at a time. Use `--filter` to replace
matching cases or `--reference-revision` to declare the renderer's source
revision when it differs from HEAD.

```sh
for suite in smoke controls; do
  python3 tools/check_circuit_regressions.py \
    --renderer build/tests/livewdf_circuit_render --suite "$suite" \
    --baseline tests/fixtures/circuit_regressions \
    --report "build/tests/record-$suite.json" --record
done
python3 tools/check_ngspice_references.py \
  --renderer build/tests/livewdf_circuit_render \
  --baseline tests/fixtures/circuit_regressions \
  --report build/tests/record-ngspice.json --record --ngspice /path/to/ngspice
```

The checkers verify lengths and hashes before comparison. Input and output use
little-endian float64. Saved waveform checkpoints use little-endian float32.
Recording installs the manifest atomically after the reference files are ready.
