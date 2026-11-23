# GuitarJam dry electric guitar fixture

This is the original mono PCM WAV from GuitarJam's test split, clip
`guitar_jam/test/026.wav`. The dataset was recorded from a Fender
Stratocaster with neck and middle pickup settings through a Focusrite
Scarlett Solo. The clip is 16.67 seconds, 44.1 kHz, 16-bit, and unprocessed.

- [GuitarJam dataset and CC0 1.0 license](https://huggingface.co/datasets/Julian-br/GuitarJam)
- [Original clip](https://huggingface.co/datasets/Julian-br/GuitarJam/resolve/main/guitar_jam/test/026.wav)
- SHA-256 `700dbfa9399c944ba60bb03d2e598a704d9d28892393665c9433558cb3fe26ab`

Use this recording for audible checks and to exercise solver stability over
real electric-guitar transients. When running at 48 kHz, resample the file before
sending it to the circuit. Record the input voltage scaling alongside the
result. Use sine, pulse, and sweep inputs to measure gain, compression,
frequency response, and modulation quantitatively.
