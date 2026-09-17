import ctypes as c
from ctypes import wintypes as w
import struct

kernel = c.WinDLL('kernel32', use_last_error=True)
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
    if kernel.ReadProcessMemory(handle, c.c_void_p(addr), buf, size, None):
        return buf.raw
    return None

def read_qword(addr):
    raw = read(addr, 8)
    return struct.unpack('<Q', raw)[0] if raw else 0

def read_dword(addr):
    raw = read(addr, 4)
    return struct.unpack('<I', raw)[0] if raw else 0

world = read_qword(base + 0x477FDB8)

def get_rtti(addr):
    vt = read_qword(addr)
    if not vt: return None
    col = read_qword(vt - 8)
    if not col: return None
    cbuf = read(col, 24)
    if not cbuf: return None
    sig, off, cd_off, td_rva = struct.unpack('<IIII', cbuf[:16])
    td_addr = base + td_rva
    nbuf = read(td_addr + 0x10, 64)
    if not nbuf: return None
    return nbuf.split(b'\0')[0].decode('ascii', errors='ignore')

print("Testing pointers in WorldChrMan:")
for off in range(0, 0x1000, 8):
    val = read_qword(world + off)
    if val > 0x10000 and val < 0x7FFFFFFFFFFF:
        rtti = get_rtti(val)
        if rtti:
            print(f"  +0x{off:03X} -> 0x{val:X} ({rtti})")

kernel.CloseHandle(handle)
