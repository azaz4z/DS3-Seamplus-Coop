import ctypes as c
from ctypes import wintypes as w
import struct

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

def read(addr, size):
    buf = c.create_string_buffer(size)
    if kernel.ReadProcessMemory(handle, c.c_void_p(addr), buf, size, None):
        return buf.raw
    return None

drawEntity = 140685072518912 # 0x7ff402ae5f00
print(f"drawEntity: 0x{drawEntity:X}")
data = read(drawEntity, 0x200)
if data:
    for offset in range(0, 0x120, 8):
        val = struct.unpack_from("<Q", data, offset)[0]
        print(f"  +0x{offset:02X}: 0x{val:X}")
else:
    print("Failed to read drawEntity")

kernel.CloseHandle(handle)
