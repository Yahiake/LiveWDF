# LiveWDF DOD 250 on GP-50

This example generates a GP-50 firmware folder from the factory DOD 250 Gray schematic. It runs the schematic's capacitor recurrence, the LM741's dominant pole and rail limits, and the antiparallel 1N4148 pair. The left encoder changes Gain, the middle encoder changes Level, and FS1 toggles bypass.

The generator binds the six capacitor states at 44.1 kHz. Gain has 17 positions from 0 to 100%. Each position uses a transfer compiled by LiveWDF with the preset's reverse-log pot taper. A change keeps the capacitor and op-amp state. Level uses the schematic's log pot taper and the unloaded output wiper. Its default is 73%. The firmware maps input and output full scale to 1.2 V peak.

The solver samples LiveWDF's exact diode law every 1/512 V from 0 to 2 V. It finds and inverts a segment of the loaded curve in ten search steps, with a second solve when the amplifier clips. Above 2 V at the junction pair it extends the last segment. The generator checks for the DOD 250 wiring and device models.

## Build and simulate

Build from LiveWDF with CMake. The export target needs only the core library and works in a checkout without the JUCE submodule.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLIVEWDF_BUILD_TESTS=OFF -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF
cmake --build build --target livewdf_opengp5_dod250
```

From an OpenGP5 checkout with the GP-50 emulator support, use the generated folder as the firmware. The example firmware and generator are provided under LiveWDF's AGPL-3.0 license.

```sh
example=/path/to/LiveWDF/examples/opengp5/dod250
firmware=/path/to/LiveWDF/build/examples/opengp5/dod250/livewdf-dod250
python3 tools/simulate.py "$firmware" --gp50 --check \
  --script "$example/controls.txt" --seconds 1.5
platform/build_firmware.sh --gp50-emu "$firmware"
```

The simulator renders the GP-50's 160 × 128 display and produces a WAV. The board build produces emulator inputs. Physical audio, cycle timing and flashing remain to be tested on the GP-50.
