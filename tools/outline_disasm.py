"""Print verified instructions/RVAs from the installed DS3 executable, read-only."""
import sys
import struct
import pefile
import capstone

pe = pefile.PE('C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/DarkSoulsIII.exe', fast_load=True)
cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for value in sys.argv[1:]:
    parts = value.split(':')
    rva, size = (int(n, 16) for n in parts[:2])
    if len(parts) == 3:
        for offset in range(rva, rva + size, 8):
            pointer = struct.unpack('<Q', pe.get_data(offset, 8))[0]
            print(f'{offset:x}: {pointer - pe.OPTIONAL_HEADER.ImageBase:x}')
    else:
        for ins in cs.disasm(pe.get_data(rva, size), rva):
            print(f'{ins.address:x} {ins.mnemonic} {ins.op_str}')
