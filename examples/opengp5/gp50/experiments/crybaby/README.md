# Cry Baby on Cortex-M4

This offline experiment compiles the LiveWDF Dunlop Cry Baby GCB-95 at 44.1 kHz. It uses the document's default input and output impedances, the three Ebers–Moll transistors, the seven capacitors and the inductor. The actual logarithmic treadle taper drives 17 discrete positions. Moving the treadle keeps the capacitor and inductor histories. A nonlinear step commits only when its loaded junction residual converges.

The GP-50 model pack does not load this kernel. The commands below run a standalone Cortex-M4 probe in Unicorn. The counts measure Thumb instruction slots in visited blocks. Actual cycles, audio deadlines and recovery still require a physical GP-50 test.

The factory full-scale input sweep matched the LiveWDF reference within 38.7 µV peak and −103.4 dB relative RMS over 65,536 samples with zero rejected steps. The 2 V stress case exceeds the calibrated 0.871 V input range and produced 78 rejected steps. The standalone Cortex-M4 probe processed 1,024 moving-treadle blocks with zero rejected steps. It peaked at 204,626 instruction slots per 64 samples, with a median of 187,910. At an assumed 192 MHz, the entire firmware has about 279,000 cycles per block, including input and output service and any output effects. The probe uses 3,220 bytes of code, 16,116 bytes of coefficients and 88 bytes of state. The existing two-model pack leaves 792 bytes in fast RAM, less than the probe's 2,808 bytes of hot code.

## Reproduce on Linux x86-64

Run from the LiveWDF root with a C++20 compiler, the Arm GNU toolchain and Python with `unicorn` installed. Set `opengp5` to a local OpenGP5 checkout that has the GP-50 board port.

```sh
opengp5=/path/to/OpenGP5
probe=examples/opengp5/gp50/experiments/crybaby
schematic='schematics/pedals_fx/Dunlop Cry Baby GCB-95.livewdf'
out=build/crybaby-experiment

cmake -S . -B "$out" -DCMAKE_BUILD_TYPE=Release -DLIVEWDF_BUILD_TESTS=OFF \
  -DLIVEWDF_BUILD_EDITOR=OFF -DLIVEWDF_BUILD_PLUGIN=OFF
cmake --build "$out" --target livewdf
c++ -std=c++20 -O2 -Isrc "$probe/generate.cpp" "$out/liblivewdf.a" -o "$out/generate"
"$out/generate" "$schematic" > "$out/crybaby_model.h"
c++ -std=c++20 -O2 -Isrc -I"$out" "$probe/parity.cpp" "$out/liblivewdf.a" -o "$out/parity"
"$out/parity" "$schematic"

dsp="$opengp5/platform/dsp"
arm-none-eabi-gcc -std=c99 -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
  -O2 -ffreestanding -nostdlib -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wdouble-promotion -Werror -I"$out" -I"$dsp" \
  -Wl,-Ttext=0x4400,-Tbss=0x20001000,--entry=wah_block,--gc-sections \
  "$probe/arm_probe.c" "$dsp/gp5_math.c" -lgcc -o "$out/wah.elf"
arm-none-eabi-size -A "$out/wah.elf"
arm-none-eabi-nm -u "$out/wah.elf"
arm-none-eabi-objcopy -O binary --only-section=.text --only-section=.data_fast \
  --only-section=.rodata "$out/wah.elf" "$out/wah.bin"
python3 "$probe/count_thumb.py" "$out/wah.elf" "$out/wah.bin"
```
