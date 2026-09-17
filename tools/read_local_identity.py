import ctypes as c
from ctypes import wintypes as w
import json, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
input_path = Path('player-exclusion-live-before.json')
if not input_path.is_file():
    input_path = ROOT / 'analysis' / 'player-exclusion-live-before.json'

report=json.loads(input_path.read_text(encoding='utf-8'))
k=c.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD]; k.OpenProcess.restype=w.HANDLE
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.c_void_p]
k.CloseHandle.argtypes=[w.HANDLE]
h=k.OpenProcess(0x410,False,report['pid'])
def read(p,n):
    b=c.create_string_buffer(n)
    if not k.ReadProcessMemory(h,p,b,n,None): return None
    return b.raw
def q(p):
    b=read(p,8)
    return struct.unpack('<Q',b)[0] if b else 0
def describe(actor):
    model=q(actor+0x48); draw=q(model+8)
    result={'actor':hex(actor),'model':hex(model),'draw':hex(draw),'actor_vtable':hex(q(actor)), 'model_vtable':hex(q(model)), 'draw_vtable':hex(q(draw))}
    for offset in (0x1f90,0x1f80):
        mods=q(actor+offset); physics=q(mods+0x68); data=read(physics+0x80,12)
        result[hex(offset)]={'modules':hex(mods),'physics':hex(physics),'position':struct.unpack('<3f',data) if data else None}
    return result
base=int(report['native']['base'],16); world=q(base+0x477fdb8)
out={'world':hex(world),'local':describe(q(world+0x80)),'npc':describe(report['ds3scCompanionStatus']['actor'])}
print(json.dumps(out,indent=2))
out_path = ROOT / 'analysis' / 'local-identity-before.json'
out_path.write_text(json.dumps(out,indent=2), encoding='utf-8')
k.CloseHandle(h)
