"""Build exact-binary Greatwood reload and guest bonfire hooks. Requires pefile/binutils.

Does not patch the source DLL in place. Runtime behavior still needs game testing.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

import pefile

PROJECT = Path(__file__).resolve().parents[1]
SUPPORTED = {
    "a7869888c98f36c4874f7a7ff5e15e93c3cd9c453debeF7d48b5e9012394d24d".lower(),
    "8760fc0dcd0ec238dee61667a35fdf3f1744ca41949e58b911e10d5809708f88",
}
SITE = 0x4B5B9
RESUME = SITE + 7
EXPECTED = bytes.fromhex("48 8b 87 b0 00 00 00")
HOOK_RVA = 0x1C8000
BONFIRE_SITE = 0x4D125
BONFIRE_EXPECTED = bytes.fromhex("89 a8 cc 0a 00 00")
RELOAD_RVA = 0x4E070
GUEST_SITE = 0x8D391
GUEST_EXPECTED = bytes.fromhex("c7 83 5c 06 00 00 01 00 00 00")


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def assemble(source="greatwood_wipe.S", rva=HOOK_RVA, guest=False):
    fallback = Path(os.environ.get("DS3SC_BINUTILS", "C:/msys64/ucrt64/bin"))
    assembler = fallback / "as.exe"
    objcopy = fallback / "objcopy.exe"
    assembler = str(assembler) if assembler.is_file() else shutil.which("as")
    objcopy = str(objcopy) if objcopy.is_file() else shutil.which("objcopy")
    if not assembler or not objcopy:
        raise ValueError("GNU binutils required; set DS3SC_BINUTILS to their directory")
    with tempfile.TemporaryDirectory(prefix="ds3sc-greatwood-") as folder:
        obj = Path(folder) / "wipe.o"
        binary = Path(folder) / "wipe.bin"
        defines = ["--defsym", "GUEST_RELOAD=1"] if guest else []
        subprocess.run([assembler, "--64", *defines, "-o", str(obj),
                        str(PROJECT / "src/patches" / source)], check=True)
        coff = obj.read_bytes()
        section_count = struct.unpack_from("<H", coff, 2)[0]
        optional_size = struct.unpack_from("<H", coff, 16)[0]
        symbol_table, symbol_count = struct.unpack_from("<II", coff, 8)
        strings = symbol_table + symbol_count * 18
        relocations = []
        for number in range(section_count):
            header = 20 + optional_size + number * 40
            count = struct.unpack_from("<H", coff, header + 32)[0]
            pointer = struct.unpack_from("<I", coff, header + 24)[0]
            for index in range(count):
                offset, symbol, kind = struct.unpack_from("<IIH", coff, pointer + index * 10)
                name = coff[symbol_table+symbol*18:symbol_table+symbol*18+8]
                if name[:4] == b"\0" * 4:
                    start = strings + struct.unpack_from("<I", name, 4)[0]
                    name = coff[start:coff.index(b"\0", start)]
                else:
                    name = name.rstrip(b"\0")
                if kind != 4 or name != b"original_reload" or coff[header:header+5] != b".text":
                    raise ValueError("Unsupported COFF relocation; only reviewed relative calls allowed")
                relocations.append((offset, RELOAD_RVA))
        subprocess.run([objcopy, "-j", ".text", "-O", "binary", str(obj), str(binary)], check=True)
        code = bytearray(binary.read_bytes().rstrip(b"\x90"))
        for offset, target in relocations:
            addend = struct.unpack_from("<i", code, offset)[0]
            struct.pack_into("<i", code, offset, target - (rva + offset + 4) + addend)
    # UNWIND_INFO below describes exactly this seven-byte stack allocation.
    prologue, epilogue = ("48 81 ec d8 00 00 00", "48 81 c4 d8 00 00 00 c3") if source == "greatwood_wipe.S" else ("48 83 ec 18", "48 83 c4 18 c3")
    if guest:
        prologue, epilogue = "57 48 81 ec d0 00 00 00", "48 81 c4 d0 00 00 00 5f c3"
    if not code.startswith(bytes.fromhex(prologue)):
        raise ValueError("Hook prologue changed; review its unwind metadata")
    if not code.endswith(bytes.fromhex(epilogue)):
        raise ValueError("Hook epilogue changed; review its unwind metadata")
    return bytes(code)


def patch(data, code=None, enable_greatwood=True, enable_bonfire=True):
    if not enable_greatwood and not enable_bonfire:
        return bytes(data)
    if hashlib.sha256(data).hexdigest() not in SUPPORTED:
        raise ValueError("Unsupported DLL hash; only verified original/labeled v0.1.1 is accepted")
    pe = pefile.PE(data=data)
    site = pe.get_offset_from_rva(SITE)
    if data[site:site+7] != EXPECTED:
        raise ValueError("Hook instruction mismatch")
    bonfire_site = pe.get_offset_from_rva(BONFIRE_SITE)
    if data[bonfire_site:bonfire_site+6] != BONFIRE_EXPECTED:
        raise ValueError("Bonfire hook instruction mismatch")
    guest_site = pe.get_offset_from_rva(GUEST_SITE)
    if data[guest_site:guest_site+len(GUEST_EXPECTED)] != GUEST_EXPECTED:
        raise ValueError("Guest reload instruction mismatch")
    if pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].Size:
        raise ValueError("Signed DLLs are not supported")
    section_header = pe.sections[-1].get_file_offset() + 40
    if section_header + 40 > pe.OPTIONAL_HEADER.SizeOfHeaders:
        raise ValueError("No room for an additional PE section")
    if any(data[section_header:section_header+40]):
        raise ValueError("Section header slot is not empty")
    exception = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    old_table = pe.get_data(exception.VirtualAddress, exception.Size)
    if len(old_table) != exception.Size or len(old_table) % 12:
        raise ValueError("Invalid exception directory")
    rva = align(pe.OPTIONAL_HEADER.SizeOfImage, pe.OPTIONAL_HEADER.SectionAlignment)
    if rva != HOOK_RVA:
        raise ValueError("Unexpected hook RVA; relative calls require relinking")
    raw = align(len(data), pe.OPTIONAL_HEADER.FileAlignment)

    if code is None:
        code = assemble("greatwood_wipe.S", rva=HOOK_RVA, guest=False)

    bonfire_offset = align(len(code), 16)
    bonfire = assemble("guest_bonfire.S", rva + bonfire_offset)
    guest_offset = align(bonfire_offset + len(bonfire), 16)
    guest = assemble("greatwood_wipe.S", rva + guest_offset, guest=True)
    unwind_offset = align(guest_offset + len(guest), 4)
    # Version 1, no flags, prologue 7, 2 slots, no frame register.
    # UWOP_ALLOC_LARGE (OpInfo 0): 0xd8 / 8 = 27.
    unwind = bytes.fromhex("01 07 02 00 07 01 1b 00")
    bonfire_unwind = bytes.fromhex("01 04 01 00 04 22 00 00")
    # Guest additionally saves RDI: PUSH_NONVOL plus ALLOC_LARGE (0xd0).
    guest_unwind = bytes.fromhex("01 08 03 00 08 01 1a 00 01 70 00 00")
    table_offset = unwind_offset + len(unwind) + len(bonfire_unwind) + len(guest_unwind)
    entries = [struct.unpack_from("<III", old_table, n) for n in range(0, len(old_table), 12)]
    entries.append((rva, rva + len(code), rva + unwind_offset))
    entries.append((rva + bonfire_offset, rva + bonfire_offset + len(bonfire), rva + unwind_offset + len(unwind)))
    entries.append((rva + guest_offset, rva + guest_offset + len(guest), rva + unwind_offset + len(unwind) + len(bonfire_unwind)))
    entries.sort()
    table = b"".join(struct.pack("<III", *entry) for entry in entries)
    code_block = (code.ljust(bonfire_offset, b"\x90") + bonfire).ljust(guest_offset, b"\x90") + guest
    payload = code_block.ljust(unwind_offset, b"\0") + unwind + bonfire_unwind + guest_unwind + table
    raw_size = align(len(payload), pe.OPTIONAL_HEADER.FileAlignment)
    output = bytearray(data.ljust(raw, b"\0") + payload.ljust(raw_size, b"\0"))
    struct.pack_into("<8sIIIIIIHHI", output, section_header,
                     b".mfix\0\0\0", len(payload), rva, raw_size, raw, 0, 0, 0, 0, 0x60000020)
    struct.pack_into("<H", output, pe.FILE_HEADER.get_field_absolute_offset("NumberOfSections"),
                     pe.FILE_HEADER.NumberOfSections + 1)
    struct.pack_into("<I", output, pe.OPTIONAL_HEADER.get_field_absolute_offset("SizeOfImage"),
                     align(rva + len(payload), pe.OPTIONAL_HEADER.SectionAlignment))
    struct.pack_into("<I", output, pe.OPTIONAL_HEADER.get_field_absolute_offset("SizeOfCode"),
                     pe.OPTIONAL_HEADER.SizeOfCode + raw_size)
    struct.pack_into("<II", output, exception.get_file_offset(), rva + table_offset, len(table))

    if enable_greatwood:
        output[site:site+7] = b"\xe8" + struct.pack("<i", rva - (SITE + 5)) + b"\x90\x90"
        output[guest_site:guest_site+10] = b"\xe8" + struct.pack("<i", rva + guest_offset - (GUEST_SITE + 5)) + b"\x90" * 5
    if enable_bonfire:
        output[bonfire_site:bonfire_site+6] = b"\xe8" + struct.pack("<i", rva + bonfire_offset - (BONFIRE_SITE + 5)) + b"\x90"

    result = pefile.PE(data=bytes(output))
    struct.pack_into("<I", output, result.OPTIONAL_HEADER.get_field_absolute_offset("CheckSum"),
                     result.generate_checksum())
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--without-greatwood", action="store_true", help="Do not patch Greatwood reload hooks")
    parser.add_argument("--without-bonfire", action="store_true", help="Do not patch guest bonfire hook")
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("Input and output must differ; preserve the original")
    result = patch(
        args.input.read_bytes(),
        enable_greatwood=not args.without_greatwood,
        enable_bonfire=not args.without_bonfire
    )
    args.output.write_bytes(result)
    parts = []
    if not args.without_greatwood: parts.append("Greatwood reload")
    if not args.without_bonfire: parts.append("guest bonfire")
    patch_desc = " + ".join(parts) if parts else "no patches (original copy)"
    print(f"Patched candidate ({patch_desc}): {args.output}")
    print(f"SHA256: {hashlib.sha256(result).hexdigest()}")


if __name__ == "__main__":
    main()
