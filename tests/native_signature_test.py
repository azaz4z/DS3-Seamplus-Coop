"""Validate hook ABI guards against the user's executable without running it."""
import argparse
import re
import struct
from pathlib import Path
import pefile
import capstone

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("game_exe", type=Path)
args = parser.parse_args()
pe = pefile.PE(str(args.game_exe), fast_load=True)
source = (Path(__file__).resolve().parents[1] / "src/render/d3d11_hook.cpp").read_text(encoding="utf-8")
for name, rva in [("modelPrefix", 0xd06e10), ("asmModelPrefix", 0xd00200), ("dispatchPrefix", 0x59220), ("packetPrefix", 0x5cec0), ("submitPrefix", 0x59040), ("passRangePrefix", 0xd06f3f), ("passCullPrefix", 0xd06f50)]:
    match = re.search(r"std::array<unsigned char, (\d+)> " + name + r"\s*\{([^}]+)\}", source)
    assert match, f"Missing {name}"
    prefix = bytes(int(n.strip(), 0) for n in match[2].split(",") if n.strip())
    assert len(prefix) == int(match[1])
    assert pe.get_data(rva, len(prefix)) == prefix, f"{name} does not match executable"
    print(f"PASS: {name} at RVA {rva:x}, {len(prefix)} bytes (including instruction prefixes)")
assert struct.unpack("<Q", pe.get_data(0x2948f00, 8))[0] == pe.OPTIONAL_HEADER.ImageBase + 0xd06e10
assert struct.unpack("<Q", pe.get_data(0x2947dc0, 8))[0] == pe.OPTIONAL_HEADER.ImageBase + 0xd00200
assert struct.unpack("<Q", pe.get_data(0x2949088, 8))[0] == pe.OPTIONAL_HEADER.ImageBase + 0x59220
print("PASS: model-draw, dispatch, and packet entries match native ABI guards")

# Tie the copied descriptor extent to the real consumer, rather than just
# testing the fields the wrapper happens to copy. The first candidate copied
# 0x20 bytes but native code dereferences pointers at +0x20, +0x28 and +0xb0.
disasm = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
disasm.detail = True
required = 0
for instruction in disasm.disasm(pe.get_data(0x5c0e0, 0xdd7), 0x5c0e0):
    for operand in instruction.operands:
        if (operand.type == capstone.x86.X86_OP_MEM and
                operand.mem.base == capstone.x86.X86_REG_R13 and
                operand.mem.index == 0 and operand.mem.disp >= 0):
            required = max(required, operand.mem.disp + operand.size)
assert required == 0xb8, f"Native options layout changed: required extent {required:#x}"
header = (Path(__file__).resolve().parents[1] / "src/render/native_draw_packet.h").read_text(encoding="utf-8")
extent = re.search(r"NativeSubmissionOptions = std::array<std::byte, (0x[0-9a-f]+)>", header)
assert extent and int(extent[1], 16) >= required, "Submission descriptor copy truncates native pointer fields"
print(f"PASS: descriptor preserves native fields through {required:#x}, including bone/material pointers")
