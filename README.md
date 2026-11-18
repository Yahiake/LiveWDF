# LiveWDF

LiveWDF is an audio plugin and app that simulates guitar-amp circuits using Wave Digital Filters.


## What is it?

LiveWDF models amplifier circuits so you can try them as a plugin in a music program (a DAW). It has two parts. The circuit editor is where you draw and save circuits. The plugin loads those circuits and lets you try them out on your audio. LiveWDF runs on Windows and Linux.

## Who is it for?

- **Anyone.** You do not need to know anything about circuits. Ready-made example circuits are planned, so you can pick one and play.
- **Researchers and tinkerers** who want to build and study circuits of their own.

## Getting started

Building the core and running its tests is confirmed to work. The editor and plugin builds are still being confirmed.

**You will need:**

On Windows:

- Visual Studio Build Tools, with the "Desktop development with C++" workload
- CMake 3.22 or newer
- Git

On Linux:

- A C++20 compiler (GCC or Clang)
- CMake 3.22 or newer
- Git

**Get the code.** Clone the repository from its GitHub page with Git. Then, inside the project folder, fetch the JUCE submodule. JUCE is only needed for the editor and the plugin, so you can skip this step if you only want the core.

```
git submodule update --init modules/JUCE
```

**Build the core and run its tests:**

Windows:

```
cmake -S . -B build/core -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF -DLIVEWDF_BUILD_RENDER=OFF
cmake --build build/core --config Release --target livewdf livewdf_tests
ctest --test-dir build/core -C Release --output-on-failure
```

Linux:

```
cmake -S . -B build/core -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF -DLIVEWDF_BUILD_RENDER=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --target livewdf livewdf_tests
ctest --test-dir build/core --output-on-failure
```


The first build takes several minutes. A few tests compare against a reference circuit simulator (ngspice). They skip themselves if it is not installed.

## Community

Questions, ideas or want to help? Join our Discord: https://discord.gg/ZJtGJQaPWh

## License

LiveWDF is released under the AGPL-3.0, see [LICENSE](LICENSE). Third-party software has its own terms, see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
