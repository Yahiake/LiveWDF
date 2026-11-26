# Paired pentode Preview profile

The case is the local full EVH 5150III 100S channel 3 preset with both output-tube pairs collapsed. The benchmark requires a circuit with four pentodes that reduce to two and an installed packed Preview plan. Both renders use that same plan. The trial changes the nonlinear residual tolerance from the stock `1e-7` to `1e-3`. The stock render supplies the waveform reference.

The 1 mV setting is the stopping tolerance for the solver residual. Measure the voltage difference separately at the speaker output. The benchmark fails on a nonfinite sample or rejected solver state. The candidate runs only in this benchmark.

## Reproduce

From the repository root on Linux, set `amplifier` to a local full-amplifier `.livewdf` path. The DI files are already in the repository. The CLI normalizes each excerpt to −6 dBFS peak, then uses the circuit input calibration and adds silence before and after the excerpt.

```sh
amplifier=/path/to/full-amp.livewdf
scratch=$(mktemp -d)
cmake -S . -B "$scratch/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DLIVEWDF_BUILD_TESTS=OFF -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF
cmake --build "$scratch/build" --target livewdf
c++ -O2 -std=c++20 -Isrc tests/bench/bench_pentode_preview.cpp \
    "$scratch/build/liblivewdf.a" -pthread -ldl -o "$scratch/bench"

for rate in 48000 96000; do
    ffmpeg -v error -ss 3.6 -i tests/fixtures/guitar-techs-di/pf300-palm-mutes.flac \
        -t 1.6 -ar "$rate" -ac 1 -f f64le "$scratch/palm.f64"
    ffmpeg -v error -ss 3.0 -i tests/fixtures/guitar-techs-di/sire-t7-performance-11.flac \
        -t 1.6 -ar "$rate" -ac 1 -f f64le "$scratch/picked.f64"
    for clip in palm picked; do
        "$scratch/bench" "$amplifier" "$scratch/$clip.f64" "$rate" clean
        "$scratch/bench" "$amplifier" "$scratch/$clip.f64" "$rate" profile
    done
    rm -- "$scratch/palm.f64" "$scratch/picked.f64"
done
rm -r -- "$scratch"
```

`clean` reports thread CPU time from three alternating runs with phase counters disabled. `profile` turns on phase timing and sample counters for one run. Use the clean results when comparing runtime.

## Results

The Linux host was a Core Ultra 7 155H with the test pinned to CPU 2 and the default frequency governor. Each clean run used 648 mono blocks. The engine processed 128 samples per block at 48 kHz and 256 at 96 kHz, corresponding to a 2.667 ms host interval. Timing covers engine dispatch and the output calculation.

The median p99 across three clean runs improved by 5–29%. Both tolerances recorded zero rejected states. RMS difference is relative to the stock speaker waveform.

| Engine rate | DI | Stock p99 | 1 mV p99 | RMS difference | Peak difference |
|---|---|---:|---:|---:|---:|
| 48 kHz | Palm mutes | 3.05 ms | 2.56 ms | -90.4 dB | 10.9 mV |
| 48 kHz | Picked | 5.34 ms | 3.81 ms | -88.5 dB | 24.6 mV |
| 96 kHz | Palm mutes | 5.26 ms | 4.08 ms | -89.9 dB | 10.1 mV |
| 96 kHz | Picked | 6.20 ms | 5.87 ms | -89.4 dB | 15.9 mV |

The separate instrumented runs showed that the nonlinear solve accounted for 91–94% of stock phase time. Within the solve, LU used 34–42% and line search used 26–28%. The 1 mV runs used 18–24% fewer Newton iterations and 43–82% fewer line-search trials.
