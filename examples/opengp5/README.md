# LiveWDF in the OpenGP5 simulator

This example runs a LiveWDF RC low-pass circuit at 44.1 kHz. The desktop exporter bakes its component values into a C99 sample step. OpenGP5 passes guitar samples to that step and provides the screen, wet/dry mix, and bypass. PARA selects and edits the mix. The footswitch toggles bypass.

From a LiveWDF checkout with an OpenGP5 checkout next to it:

```sh
cmake -S . -B build -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF
cmake --build build --target livewdf_codegen --config Release
cp -R examples/opengp5 build/livewdf-opengp5
build/livewdf_codegen examples/opengp5/rc-lowpass.livewdf 44100 --c99-float \
    > build/livewdf-opengp5/dsp/circuit.h
python3 ../OpenGP5/tools/simulate.py build/livewdf-opengp5 --check \
    --out build/livewdf-opengp5.wav --shots build/livewdf-opengp5-screen
```

Use a fixed-value linear circuit for `--c99-float`. This screen uses the OpenGP5 simulator's GP-5 hardware model. The GP-50 needs its own display and controls driver before this example can run on that pedal.
