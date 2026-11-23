# LiveWDF

[![Tests](https://github.com/Yahiake/LiveWDF/actions/workflows/tests.yml/badge.svg?branch=main)](https://github.com/Yahiake/LiveWDF/actions/workflows/tests.yml) [![Nightly](https://github.com/Yahiake/LiveWDF/actions/workflows/nightly.yml/badge.svg?branch=main)](https://github.com/Yahiake/LiveWDF/actions/workflows/nightly.yml)

LiveWDF simulates analog audio circuits in real time with Wave Digital Filters (WDFs). Play through a factory amp, pedal or piece of studio gear, or build your own circuit.

## Download

The nightly workflow builds the editor, VST3 plug-in and standalone player for Windows, macOS and Linux. Nightlies appear as prereleases on [GitHub Releases](https://github.com/Yahiake/LiveWDF/releases).

![The Fuzz Face schematic in the editor, then the factory preset browser and Fuzz Factory in the VST3 plug-in](media/livewdf.gif)

## Play or build a circuit

- **Editor** — Wire up a circuit, change component values and save it as a `.livewdf` file.
- **VST3** — Load the same file in your DAW, play through it and adjust its controls. Open the standalone player to do the same with an audio interface.

Search the factory library by name and filter by collection or effect type. Use the arrows beside the loaded preset to step through the results. See the [pedal](schematics/PEDAL-SOURCES.md) and [studio](schematics/STUDIO-SOURCES.md) circuit notes for references.

New plug-in instances use **Fast Live** for playback. Offline rendering defaults to **Accurate** and 4× oversampling.

## Build from source

Use CMake 3.22 or newer and a C++20 compiler. On Windows, install the Visual Studio Build Tools with the *Desktop development with C++* workload. Linux audio and GUI dependencies are listed in the [build workflow](.github/workflows/build.yml).

```bash
git clone --recurse-submodules https://github.com/Yahiake/LiveWDF.git
cd LiveWDF
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLIVEWDF_BUILD_TESTS=OFF
cmake --build build --config Release --target LiveWDF_Editor LiveWDF_Plugin_VST3 LiveWDF_Plugin_Standalone
```

### Run the core tests

The simulation core builds without JUCE. Some tests use ngspice as an external reference when it is installed.

```bash
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF
cmake --build build/core --config Release --target livewdf_tests
ctest --test-dir build/core -C Release --output-on-failure
```

## Contributing

Open an issue for bugs or ideas, or join the [LiveWDF Discord](https://discord.gg/ZJtGJQaPWh).

## License

LiveWDF is licensed under the [GNU Affero General Public License v3.0](LICENSE) (AGPL-3.0).

Third-party dependencies and their respective licenses are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
