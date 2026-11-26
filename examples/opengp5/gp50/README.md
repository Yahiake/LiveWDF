# LiveWDF model pack for GP-50

This example generates GP-50 firmware for the DOD 250 and MXR Micro Amp.
Footswitch A recalls the previous patch and B recalls the next patch. Each
patch includes the model, all its settings, Input, Level, bypass, cabinet,
delay and reverb. The initial patch bank has one patch per model.

The left encoder adjusts Drive or Input, the middle encoder adjusts Level,
and the right encoder adjusts a second model control or Delay. Turn and press PARA to
browse and edit the other settings. GLOBAL opens the output controls. DRUM
is a shortcut to Delay, and EXIT leaves the current edit. SAVE updates the
active patch in RAM. The GP-50 loses these changes when it powers off.
Level extends to 200%, with up to 6 dB of extra output above 100%.

The DOD 250 Gray model runs the schematic's capacitor recurrence, the
LM741's dominant pole and rail limits, and the antiparallel 1N4148 pair.

The generator binds the six capacitor states at 44.1 kHz. Gain has 17 positions from 0 to 100%. Each position uses a transfer compiled by LiveWDF with the preset's reverse-log pot taper. A change keeps the capacitor and op-amp state. Level uses the schematic's log pot taper and the unloaded output wiper. Its default is 73%. The firmware maps input and output full scale to 1.2 V peak.

The solver samples LiveWDF's exact diode law every 1/512 V from 0 to 2 V. It finds and inverts a segment of the loaded curve in ten search steps, with a second solve when the amplifier clips. Above 2 V at the junction pair it extends the last segment. The generator checks for the DOD 250 wiring and device models.

The MXR Micro Amp model runs the factory circuit's five capacitor states
and the TL061's dominant pole, slew limit and rail saturation. Gain uses
the schematic's reverse-log taper across 17 positions. Level adjusts the
output after the circuit.

The output effects run in series. Cabinet is a built-in synthetic IR, Delay
mixes in a 250 ms feedback echo, and Reverb mixes in a stereo hall adapted
from Dreamcatcher. Each can be disabled independently. The overall bypass
copies the input unchanged to both outputs.

## Build and simulate

Build from LiveWDF with CMake. The export target needs only the core library and works in a checkout without the JUCE submodule.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLIVEWDF_BUILD_TESTS=OFF -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF
cmake --build build --target livewdf_opengp5_gp50
```

From an OpenGP5 checkout with the GP-50 emulator support, use the generated folder as the firmware. The example firmware and generator are provided under LiveWDF's AGPL-3.0 license.

```sh
example=/path/to/LiveWDF/examples/opengp5/gp50
firmware=/path/to/LiveWDF/build/examples/opengp5/gp50/livewdf-gp50
python3 tools/simulate.py "$firmware" --gp50 --check \
  --script "$example/controls.txt" --seconds 1.5
platform/build_firmware.sh --gp50-emu "$firmware"
```

The simulator renders the GP-50's 160 × 128 display and produces a WAV.
In a control script, `set Model 0` selects the DOD 250 and `set Model 1`
selects the Micro Amp.
The [live simulator](https://github.com/jopdorp/OpenGP5-Emulator) also accepts
the generated source folder and runs it with JACK audio. The board build
produces emulator inputs. Hardware playback and timing qualification are
separate steps.

## Desktop catalog

The desktop GP-50 emulator runs all 47 circuits in [catalog.cmake](catalog.cmake)
at 44.1 kHz with the factory knob settings, voltage references and port
impedances. For amps with a preamp tap, it uses that path.

Use an [OpenGP5-Emulator checkout with native DSP support](https://github.com/jopdorp/OpenGP5-Emulator/pull/3),
including its OpenGP5 submodule.

```sh
emulator=/path/to/OpenGP5-Emulator
cmake -S . -B build -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF \
  -DLIVEWDF_OPENGP5_EMULATOR="$emulator" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target livewdf_gp50_catalog
catalog="$PWD/build/examples/opengp5/gp50/livewdf-gp50-catalog"
pw-jack python3 "$emulator/tools/simulate.py" "$catalog" --gp50 --live \
  --host-dsp "$catalog/livewdf_gp50_catalog.so"
```

Use the same patch and encoder controls on the desktop. The **Control**
row selects a pot or rheostat for deeper edits. Circuit switches use
their factory settings.

Cab, Delay and Reverb are on the same screen. The emulator’s **Expression**
slider moves the original Cry Baby treadle pot. An offline script can use
`0.1 set Model 15` and `0.3 expression 255`.
