# tests/oracle

Generated-code oracles produced by the **prebuilt wdf_compiler** binary from the
workspace root (`C:\Code\test\try`). They are checked in so tests run without
re-invoking the toolchain.

Regenerate with (from `C:\Code\test\try`):

```bat
wdf_compiler-windows-x86_64.exe wdf_compiler\tests\rc_lowpass\rc_lowpass.net ^
    LiveWDF\tests\oracle\rc_lowpass.h -netlist
```

Each generated header records its exact command line in the banner.

Used by `tests/test_oracle_wdf_compiler.cpp` to validate LiveWDF's parser +
Engine sample-for-sample against wdf_compiler's output (MIT, Chowdhury-DSP).
