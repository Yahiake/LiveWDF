# LiveWDF — Third-Party Notices

LiveWDF is licensed under the **GNU Affero General Public License v3** (see `LICENSE`).

The following third-party components are used, ported, or referenced. Each entry notes
its license and how LiveWDF uses it.

| Component | License | Upstream | Use in LiveWDF |
|---|---|---|---|
| wdf_compiler | MIT | https://github.com/Chowdhury-DSP/wdf_compiler | Techniques (SPQR contraction, R-type Weinberg solve, `.wdf`/netlist semantics, codegen structure, eqn (10)/(39) of Werner et al. 2015 diode pair wave-domain start in `wdf_lib_diode_pair.h`) re-implemented in C++ from source study; prebuilt binaries used as test oracles. MIT notice reproduced below. |
| Wright-Omega approximations (Stefano D'Angelo) | MIT | historically upstream: `chowdsp_wdf/math/omega.h`, wdf_compiler `lib/wdf_lib_omega.h` | Closed-form nonlinear diode/tube solves (D'Angelo, Gabrielli, Turchet, DAFx-19). MIT notice reproduced below. |
| Extended Shockley diode model & WDPP reference (Bernardini et al. / ATTILA) | Academic derivation + GPL-3.0 (ATTILA code reference) | https://github.com/polimi-ispl/attila ; A. Bernardini, K. J. Werner, P. Maffezzoni, A. Sarti, "Wave Digital Modeling of the Diode-Based Ring Modulator," 144th AES Conv., 2018; A. Bernardini, P. Maffezzoni, A. Sarti, IEEE/ACM TASLP 2019; A. Bernardini et al., "Wave Digital Newton-Raphson for Circuits with Multiple Nonlinearities," IEEE/ACM TASLP 2021 | Extended Shockley junction law ($R_p$ parallel + $R_s$ series resistor, Eq. (43)/(53) of TASLP 2021 / `extendedSchockleyDiodeScat.m` and `antiExtSchockleyDiodeScat.m` in ATTILA, plus 1N4148/1N914 extended parameters in `attila/Examples/DOD.txt`) and the WDPP Potra-Pták reference (`attila/Functions/getIterFunc.m`). Reimplemented from equations and reference study. |
| Quadric triode stage reference (Stefano D'Angelo et al.) | MIT (Copyright (C) 2023 Stefano D'Angelo) | https://dangelo.audio/assets/code/triode_stage_quadric.m (implementation for Giampiccolo, D'Angelo, Bernardini, Sarti, "A Quadric Surface Model of Vacuum Tubes for Virtual Analog Applications," DAFX23, pp. 296-303) | **Line-for-line port** of the authors' GNU Octave script as `tests/handbuilt/QuadricStage.h`, plus the Sec. 8 / Table 4 topology itself. Test-only: it is the hand-built arm `livewdf_bench` races the generic Engine against, and is **not** part of the shipped signal path. The upstream notice is reproduced in that file (as MIT requires) and again below. Distinct from the 2019 Wright-Omega entry above — same author, different work. |
| Cathodyne (mmsaudio) | GNU GPLv3 | https://github.com/mmsaudio/Cathodyne | Quadric triode model; **verbatim data incorporated**: 13 of the 14 entries in `triodeTable` and `parallelTriodeTable` (name / kp / kp2 / kpg coefficients) from `src/dsp/TriodeQuadricWDF.h:19-54`, now `src/devices/QuadricTriode.h` (`kQuadricTriodeTable`, `kQuadricParallelTriodeTable`). The `12AX7` entry instead uses the DAFx-23 paper's Table 3 coefficients (see the Quadric triode stage reference row), so it is not Cathodyne data. GPLv3 code incorporated into this AGPLv3 work is combined under GPLv3 §13 terms; the combined work is distributed under AGPLv3. |
| Celine | GNU AGPLv3 | https://github.com/Celine-audio/Celine | Editor architecture patterns (component registry, union-find net extraction, diagnostics, threading). Same license as LiveWDF. |
| WaveDigitalFilters (jatinchowdhury18) | GNU GPLv3 | https://github.com/jatinchowdhury18/WaveDigitalFilters | WDF example techniques, runtime-tree (WDFPrototyper) design patterns. GPLv3 §13 combination as above. |
| JUCE | AGPLv3 (or commercial) | https://github.com/juce-framework/JUCE | Plugin/standalone shell. AGPLv3 option is used, consistent with LiveWDF's license. |
| PedalKernel op-amp element (`src/devices/OpAmp.h`) | AGPLv3-or-later + Section 7 hardware-revenue/commercial condition | https://github.com/ajmw-subs/pedalkernel | Op-amp VCVS model: the physical model was reimplemented from the public papers and SPICE documentation; only the general VCVS architecture and gain/pole/slew/rail structure common to PedalKernel's element was referenced. Source code was not copied; the comment in OpAmp.h acknowledging the PedalKernel origin is retained as provenance. Legal review of Section 7 implications is pending. |
| Catch2 (v2.13.7) | Boost Software License 1.0 | https://github.com/catchorg/Catch2 | Test framework (vendored single header in `third_party/catch2/`). |
| ngspice | BSD 3-Clause (ngspice) | https://ngspice.sourceforge.io | External accuracy oracle for tests (not linked; invoked as a separate process). |
| `tools/` sources (`Net.h`, `SpiceNet.h`, `Engine.h`, device headers, `verify.py`, `WavIO.h`) | Provided by the project owner for use in LiveWDF | local workspace (`C:\Code\test\try\tools`) | Network compilation (`rootTransfer`), multiport-Newton engine with keep-best, SPICE parsing patterns, verification methodology. |

## Model parameter data

The built-in JFET presets use parameter values from the LTspice `standard.jft`
library. The source file carries this notice:

```text
Copyright (c) 2000-2022 Analog Devices, Inc. All rights reserved.
```

Only the minimal supported parameter values needed by the presets are stored in
LiveWDF. The LTspice library file is not redistributed. The full cards contain
additional junction, temperature, noise, gate-current and manufacturer fields
that the simplified LiveWDF JFET model does not implement.

## Reproduced notices

### wdf_compiler (MIT)

```
MIT License

Copyright (c) 2026 Jatin Chowdhury

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Stefano D'Angelo — Quadric triode stage reference (MIT)

```
Copyright (C) 2023 Stefano D'Angelo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### Stefano D'Angelo — Wright Omega approximations (MIT)

```
Copyright (c) 2019 Stefano D'Angelo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
