# Compile an EQ Preview

The offline builder measures a fixed circuit and fits a stable low-order impulse response. It writes a sidecar only when one linear model reproduces both small and large impulses and three independent multitone runs of the physical engine. Circuits with deliberate distortion or signal-dependent gain will usually fail this test.

From a LiveWDF checkout, with CMake, a C++20 compiler, Python, NumPy and SciPy installed:

```sh
python3 tools/preview/build.py 'schematics/studio/eq/Neve 1081 Gold Label Equalizer.livewdf' 48000
```

The first invocation builds the native measurement tool in `build-preview`. Subsequent calls reuse it. Python dependencies are needed only to author a sidecar. Use `--build-dir` for a different build tree or `--output` for a different destination. Building at a second sample rate creates a separate file alongside the circuit.

The default filename is `<document>.livewdf.<Hz>.preview.json`. Coefficients map AC input volts to output volts, including a measured output bias. Each row stores a stable pole's real and imaginary parts and its cosine and sine impulse weights. The model applies to the saved control and switch positions. Changes to the document hash, active controls, sample rate or reference engine invalidate it.

The builder requires normalized high- and low-level impulse responses to agree within −45 dB relative RMS. The impulse fit must achieve −60 dB. A one-second test at each of 35 mV, 360 mV and 1.2 V peak must achieve −55 dB relative RMS and a peak difference below −35 dB relative to the reference peak. The sidecar records the measurements. These probes compare the approximation with the engine for the measured settings. Hardware calibration uses independent data.
