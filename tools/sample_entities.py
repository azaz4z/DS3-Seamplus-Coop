import ctypes as c
from ctypes import wintypes as w
import struct, time

kernel = c.WinDLL('kernel32', use_last_error=True)
psapi = c.WinDLL('psapi', use_last_error=True)
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), w.DWORD]
psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, c.c_void_p, w.LPWSTR, w.DWORD]

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

modules = (c.c_void_p * 1024)()
needed = w.DWORD()
psapi.EnumProcessModulesEx(handle, modules, c.sizeof(modules), c.byref(needed), 3)
comp_base = None
for b in modules[:needed.value//8]:
    buf = c.create_unicode_buffer(1024)
    psapi.GetModuleFileNameExW(handle, c.c_void_p(b), buf, 1024)
    if 'ds3sc_companion.dll' in buf.value.lower():
        comp_base = b
        break

print(f'comp_base: 0x{comp_base:X}')

# Read exports from comp_base
hdr = read(comp_base, 0x1000)
e_lfanew = struct.unpack_from('<I', hdr, 0x3C)[0]
opt_hdr = comp_base + e_lfanew + 0x18
export_rva, export_size = struct.unpack('<II', read(opt_hdr + 0x70, 8))

exp_data = read(comp_base + export_rva, export_size)
header = struct.unpack('<IIHHIIIIIII', exp_data[:40])
num_funcs, num_names, funcs_rva, names_rva, ords_rva = header[6:11]

names_raw = read(comp_base + names_rva, num_names * 4)
funcs_raw = read(comp_base + funcs_rva, num_funcs * 4)
ords_raw = read(comp_base + ords_rva, num_names * 2)

exports = {}
for i in range(num_names):
    nrva = struct.unpack_from('<I', names_raw, i*4)[0]
    name = read(comp_base + nrva, 64).split(b'\0')[0].decode('ascii')
    ord_val = struct.unpack_from('<H', ords_raw, i*2)[0]
    frva = struct.unpack_from('<I', funcs_raw, ord_val*4)[0]
    exports[name] = comp_base + frva

last_packet_addr = exports['ds3scAllyLastPacket']
last_entity_addr = exports['ds3scAllyLastPacketEntity']
packet_calls_addr = exports['ds3scAllyPacketCalls']
companion_status_addr = exports['ds3scCompanionStatus']
execute_calls_addr = exports['ds3scAllyExecuteCalls']

comp_data = read(companion_status_addr, 56)
c_vals = struct.unpack('<4I5Q', comp_data)
comp_draw = c_vals[8]
print(f'Companion drawEntity: 0x{comp_draw:X}')

entities = set()
print('Sampling entities over 2 seconds...')
t0 = time.time()
found_comp = False
while time.time() - t0 < 2.0:
    ent = struct.unpack('<Q', read(last_entity_addr, 8))[0]
    if ent:
        entities.add(ent)
        if ent == comp_draw:
            found_comp = True
    time.sleep(0.002)

calls = struct.unpack('<I', read(packet_calls_addr, 4))[0]
exec_calls = struct.unpack('<I', read(execute_calls_addr, 4))[0]
print(f'Total packet calls: {calls}')
print(f'Ally execute calls: {exec_calls}')
print(f'Sampled {len(entities)} unique entities. Companion drawEntity observed: {found_comp}')
for e in sorted(entities):
    vt_data = read(e, 8)
    vt = struct.unpack('<Q', vt_data)[0] if vt_data else 0
    print(f'  Entity 0x{e:X} -> vtable 0x{vt:X} (+0x{vt - base:X})')

kernel.CloseHandle(handle)
