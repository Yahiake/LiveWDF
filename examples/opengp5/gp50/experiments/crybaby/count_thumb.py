import argparse
import math
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_BLOCK
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2

parser = argparse.ArgumentParser(description="Count Cry Baby Cortex-M4 instructions per 64 samples")
parser.add_argument("elf", type=Path)
parser.add_argument("image", type=Path)
parser.add_argument("--blocks", type=int, default=1024)
args = parser.parse_args()
if args.blocks < 1:
    parser.error("--blocks must be positive")

image = args.image.read_bytes()
base = 0x4400
elf = args.elf.read_bytes()
if len(elf) < 28 or not elf.startswith(b"\x7fELF\x01\x01"):
    parser.error("expected a 32-bit little-endian Arm ELF")
entry, = struct.unpack_from("<I", elf, 24)
if not entry & 1 or not base <= entry - 1 < base + len(image):
    parser.error("ELF entry is outside the Thumb image")
cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
cpu.mem_map(0x4000, 0x16000)
cpu.mem_write(base, image)
cpu.mem_map(0x20000000, 0x8000)
cpu.mem_map(0xe000e000, 0x1000)
cpu.mem_write(0xe000ed88, struct.pack('<I', 0xf00000))
return_addr = 0x20006000
samples_in, samples_out = 0x20000000, 0x20000400
lengths = {}
count = 0

def block(_, address, length, _ignored):
    global count
    key = (address, length)
    if key not in lengths:
        n, byte = 0, address - base
        while byte < address - base + length:
            # Thumb-2 wide instructions start with 11101, 11110 or 11111.
            first_half = image[byte + 1]
            byte += 4 if first_half >= 0xe8 else 2
            n += 1
        lengths[key] = n
    count += lengths[key]

cpu.hook_add(UC_HOOK_BLOCK, block)
rejected = max_peak = 0
instructions = []
for block_num in range(args.blocks):
    count = 0
    motion = block_num // 4 % 32
    position = min(motion, 32-motion)
    audio = [int(2147483647 * (0.62 * math.sin(i*0.056945) + 0.27 * math.sin(i*0.151773) + 0.11 * math.sin(i*0.262841))) for i in range(block_num*64,(block_num+1)*64)]
    cpu.mem_write(samples_in, struct.pack('<64i', *audio))
    for reg,val in ((UC_ARM_REG_SP,0x20005000),(UC_ARM_REG_LR,return_addr|1),(UC_ARM_REG_R0,samples_in),(UC_ARM_REG_R1,samples_out),(UC_ARM_REG_R2,position)):
        cpu.reg_write(reg,val)
    cpu.emu_start(entry,return_addr,count=5_000_000)
    if cpu.reg_read(UC_ARM_REG_PC) != return_addr:
        raise RuntimeError(f"ARM block {block_num} did not return")
    rejected += cpu.reg_read(UC_ARM_REG_R0)
    max_peak = max(max_peak, max(map(abs,struct.unpack('<64i', cpu.mem_read(samples_out,256)))))
    instructions.append((count,block_num,position))
instructions.sort()
print(f'{args.blocks} ARM blocks, max {instructions[-1][0]}, p99 {instructions[int(.99*args.blocks)][0]}, '
      f'median {instructions[args.blocks//2][0]}, rejected {rejected}, output peak {max_peak/2**31:.6f}')
