import ctypes as c
from ctypes import wintypes as w
import capstone

kernel = c.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]

import subprocess
out = subprocess.check_output('tasklist /FI "IMAGENAME eq DarkSoulsIII.exe" /FO CSV /NH', shell=True).decode()
pid = None
for line in out.strip().splitlines():
    parts = [p.strip('"') for p in line.split('","')]
    if len(parts) >= 2 and parts[0].lower() == 'darksoulsiii.exe':
        pid = int(parts[1])
        break

handle = kernel.OpenProcess(0x410, False, pid)
base = 0x7ff654b10000

def read(addr, size):
    buf = c.create_string_buffer(size)
    kernel.ReadProcessMemory(handle, c.c_void_p(addr), buf, size, None)
    return buf.raw

cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

data = read(base + 0xD06F80, 150)
for ins in cs.disasm(data, base + 0xD06F80):
    print(f"0x{ins.address:X} (+0x{ins.address - base:X}): {ins.mnemonic} {ins.op_str}")

kernel.CloseHandle(handle)
