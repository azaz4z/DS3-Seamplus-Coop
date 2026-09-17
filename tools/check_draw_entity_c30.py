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

drawEntity = 0x7FF3CBC5CF00
print(f"Reading drawEntity 0x{drawEntity:X} at +0xC30:")
data = read(drawEntity + 0xC30, 0x20)
if data:
    for i in range(len(data)):
        print(f"+0x{0xC30+i:X}: 0x{data[i]:02X}")
else:
    print("Cannot read at +0xC30")

kernel.CloseHandle(handle)
