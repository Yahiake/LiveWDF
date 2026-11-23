# LiveWDF factory expansion regression screen

The Release engine at revision 29ca967 was screened on all 81 added factory circuits at their saved controls. The mono tests ran at 48 and 96 kHz. Each instance received 120 ms of silent warm-up and 200 ms per level of a smooth-gated 220 Hz, 997 Hz, 2.1 kHz and 5.9 kHz multitone. Input level rose through 0.1, 0.4, 1 and 2 times the category reference. The references are 0.12 V peak for pedals, 0.02 V peak for the selected mic preamps and 1.25 V peak for other studio devices. The CSV reports measured peaks, voltage RMS after removing the initial silent-output offset, rejects and thread CPU per frame. Timing is an initial screen on the shared host. Timing rows above budget need repeated tests with representative input and controls. Engine-only results exclude plug-in overhead, oversampling filters and stereo costs.

| Saved settings, nominal input | 48 kHz | 96 kHz |
| --- | ---: | ---: |
| Circuits | 81 | 81 |
| Circuits with a nonfinite sample | 0 | 0 |
| Circuits with audible-region rejects | 3 | 1 |
| Circuits above the mono budget in this screen | 19 | 35 |

Across all four levels, audio-region rejects appear in the Boss HM-2, Boss DS-1A, Boss PQ-4 and Marshall Guv'nor. HM-2 rejects 198 of 9,600 frames at nominal 48 kHz, DS-1A 12 and Guv'nor 21. PQ-4 also rejects during preroll at 48 kHz. These results prioritize investigation. They establish neither audible artifact severity nor hardware error on their own.

The latest full release test run passed 364 of 365 cases. The factory-wide SPICE export test stopped after 24 nonconverged samples in the PQ-4 comparison. This short test uses a different source from the catalog screen. Source-specific tests, preset state, repeated timings and listening are required before sign-off.
