import pefile
import capstone

exe_path = 'C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/DarkSoulsIII.exe'
pe = pefile.PE(exe_path, fast_load=True)
pe.parse_data_directories()
cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

base = pe.OPTIONAL_HEADER.ImageBase

def disas(rva, length=80):
    data = pe.get_data(rva, length)
    print(f"=== RVA 0x{rva:X} ===")
    for ins in cs.disasm(data, base + rva):
        print(f"0x{ins.address:X} (+0x{ins.address - base:X}): {ins.mnemonic} {ins.op_str}")

disas(0x5cec0, 80)
print()
disas(0x59220, 80)
print()
disas(0xd06e10, 80)
print()
disas(0xd07d50, 60)
