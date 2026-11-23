# Legacy BJT runtime regression

The branch uses the same Ebers–Moll and optional corrected laws as Parts. For an engine's fixed BJT models, validation now happens during netlist compilation and the audio path checks the validation result. Public device evaluation keeps its validation. Every trial still checks its input voltages, usable derived junction voltages, and calculated currents. An invalid raw model still contributes zero current and derivative through the solver. Invalid explicit document cards and invalid cards passed to Circuit::addBjt still fail through their original entry points.

The engine owns copies of circuit data and its compiled models. The live resistance, switch, and pot updates do not modify or replace those models. A new Engine compiles and checks a changed model. The focused test covers a mutable caller netlist that becomes invalid while an existing engine continues through a linear-network rebuild.

## Timing

Eight interleaved passes pinned to CPU 7, using GCC C++20 -O3 -DNDEBUG and the root's existing four-level multitone fixture. Each cell is the median of per-thread CPU microseconds per frame at nominal level, with a 120 ms warmup and 200 ms per stimulus level. Pair percentages use the median of each individual Parts-after/Parts-before ratio. Host clock and load can change absolute times.

| Circuit | Rate | Main | Core | Parts before | Parts after | Paired change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Legacy Fuzz Face | 48 kHz | 3.589 | 3.538 | 4.199 | 3.686 | −11.25% |
| Legacy Fuzz Face | 96 kHz | 2.440 | 2.531 | 2.878 | 2.492 | −14.88% |
| Legacy GCB-95 | 48 kHz | 2.287 | 2.125 | 2.436 | 2.208 | −8.17% |
| Corrected GCB-95 | 48 kHz | 2.150 | 2.135 | 2.450 | 2.240 | −9.16% |
| TS-808 control | 48 kHz | 1.888 | 1.781 | 1.876 | 1.819 | +0.12% |

Root's original Parts and Pedals comparison binaries have the same SHA-256 d0c41526f78c2b2d0df6a51b5000a4849b4bfd33981d24b26fb02be5cddf9145. The separate 2.51→2.71 µs result after Parts was timing variance. Compiler disassembly also shows that GCC merged identical source checks already, so the remaining runtime recovery comes from moving the expanded model-validation block to compilation.

## Correctness

Six fixtures (legacy Fuzz Face, old/corrected GCB-95, Fuzz Face terminal swaps to the onsemi PNP, MiSTer high-injection PNP, and germanium-cap options) at two rates and four levels match in exact-bit sample hashes, device evaluations, Newton iterations and rejection counts. The invalid explicit document produces the same exception text before and after. The focused Catch2 BJT tests include ngspice comparisons, a raw mutated-card case, and the existing engine after a resistance rebuild. Raw data are in timing.csv and wavehash.csv.
