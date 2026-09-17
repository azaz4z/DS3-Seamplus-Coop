"""Read-only diagnostics for the loaded DS3 outline extension (no injection)."""
import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime, timezone
import json
from pathlib import Path
import struct

import pefile


def inspect(pid):
    kernel = c.WinDLL("kernel32", use_last_error=True)
    psapi = c.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
    psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), w.DWORD]
    psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, c.c_void_p, w.LPWSTR, w.DWORD]
    handle = kernel.OpenProcess(0x410, False, pid)
    if not handle:
        raise c.WinError(c.get_last_error())

    def read(address, count):
        data = c.create_string_buffer(count)
        if not kernel.ReadProcessMemory(handle, address, data, count, None):
            raise c.WinError(c.get_last_error())
        return data.raw

    try:
        modules = (c.c_void_p * 1024)()
        needed = w.DWORD()
        if not psapi.EnumProcessModulesEx(handle, modules, c.sizeof(modules), c.byref(needed), 3):
            raise c.WinError(c.get_last_error())
        loaded = []
        for base in modules[:min(needed.value // 8, len(modules))]:
            path = c.create_unicode_buffer(1024)
            psapi.GetModuleFileNameExW(handle, base, path, len(path))
            loaded.append((base, Path(path.value)))
        result = {"time_utc": datetime.now(timezone.utc).isoformat(), "pid": pid}
        for base, path in loaded:
            if path.name.lower() == "darksoulsiii.exe":
                result["native"] = {
                    "base": hex(base),
                    "modelPrefix": read(base + 0xd06e10, 16).hex(),
                    "dispatchPrefix": read(base + 0x59220, 22).hex(),
                    "packetPrefix": read(base + 0x5cec0, 28).hex(),
                    "modelVtableSlot": hex(struct.unpack("<Q", read(base + 0x2948f00, 8))[0] - base),
                    "dispatchVtableSlot": hex(struct.unpack("<Q", read(base + 0x2949088, 8))[0] - base),
                }
            if path.name.lower() != "ds3sc_companion.dll":
                continue
            result["dll"] = str(path)
            # Read export RVAs from the loaded image, not a potentially replaced
            # on-disk DLL. This also works while testing a staged build.
            image = pefile.PE(data=read(base, 4096), fast_load=True)
            directory = image.OPTIONAL_HEADER.DATA_DIRECTORY[0]
            exports = struct.unpack("<IIHHIIIIIII", read(base + directory.VirtualAddress, 40))
            count, functions, names, ordinals = exports[7:11]
            for i in range(count):
                name_rva = struct.unpack("<I", read(base + names + i * 4, 4))[0]
                name = read(base + name_rva, 96).split(b"\0", 1)[0].decode("ascii")
                if not name.startswith(("ds3scOutline", "ds3scAlly", "ds3scLocal", "ds3scD3D", "ds3scRenderTrace", "ds3scCompanionStatus")):
                    continue
                ordinal = struct.unpack("<H", read(base + ordinals + i * 2, 2))[0]
                rva = struct.unpack("<I", read(base + functions + ordinal * 4, 4))[0]
                address = base + rva
                if name == "ds3scCompanionStatus":
                    values = struct.unpack("<4I5Q", read(address, 56))
                    result[name] = dict(zip(("abi", "state", "error", "giftCount", "actor", "model", "updates", "uses", "drawEntity"), values))
                elif name == "ds3scRenderTrace":
                    values = struct.unpack("<4I27Q", read(address, 232))
                    trace = dict(zip(("abi", "modelThread", "drawThread", "totalDraws", "modelEntity", "modelContext", "immediateContext"), values[:7]))
                    trace["hookedTargets"] = list(values[7:11])
                    trace["actualTargets"] = list(values[11:15])
                    trace["sameD3DTargets"] = values[7:11] == values[11:15]
                    trace["drawStack"] = [hex(v) for v in values[15:] if v]
                    result[name] = trace
                elif name in ("ds3scAllyLastPacket", "ds3scAllyLastPacketEntity"):
                    result[name] = struct.unpack("<Q", read(address, 8))[0]
                else:
                    result[name] = struct.unpack("<I", read(address, 4))[0]
            result["outlinePresentInLoadedDll"] = "ds3scD3D11Hooked" in result
        status = result.get("ds3scCompanionStatus", {})
        entity = status.get("drawEntity", 0)
        if entity:
            try:
                data = read(entity, 0xc60)
                u32 = lambda offset: struct.unpack_from("<I", data, offset)[0]
                ptr = lambda offset: hex(struct.unpack_from("<Q", data, offset)[0])
                result["modelGates"] = {
                    "vtable": ptr(0), "interfaceVtable": ptr(0xd0),
                    "resource": ptr(0xe8), "mask": u32(0x70),
                    "groups": data[0x88:0xa8].hex(), "visibilityGroups": data[0xa8:0xc8].hex(),
                    "flags": u32(0xc3c), "passCull": data[0xa5d],
                    "lod": u32(0x178), "nextLod": u32(0x17c),
                    "fade": struct.unpack_from("<f", data, 0x170)[0],
                    "baseFade": struct.unpack_from("<f", data, 0x16c)[0],
                }
                resource = struct.unpack_from("<Q", data, 0xe8)[0]
                result["modelGates"]["resourceData"] = hex(struct.unpack("<Q", read(resource + 8, 8))[0]) if resource else "0x0"
                game = int(result["native"]["base"], 16)
                manager = struct.unpack("<Q", read(game + 0x4796298, 8))[0]
                if manager:
                    manager_data = struct.unpack("<Q", read(manager + 0x10, 8))[0]
                    result["modelGates"]["renderMask"] = struct.unpack("<I", read(manager_data + 0x544, 4))[0] if manager_data else 0xffffffff
                result["modelGates"]["lodBlend"] = struct.unpack_from("<f", data, 0x168)[0]
                result["modelGates"]["lodStates"] = [u32(0x160), u32(0x164)]
            except OSError:
                result["modelGates"] = {"unavailable": True}
        return result
    finally:
        kernel.CloseHandle(handle)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.pid is None:
        kernel = c.WinDLL("kernel32", use_last_error=True)
        psapi = c.WinDLL("psapi", use_last_error=True)
        kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        kernel.OpenProcess.restype = w.HANDLE
        kernel.CloseHandle.argtypes = [w.HANDLE]
        kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
        process_ids = (w.DWORD * 4096)()
        needed = w.DWORD()
        if not psapi.EnumProcesses(process_ids, c.sizeof(process_ids), c.byref(needed)):
            raise c.WinError(c.get_last_error())
        pids = []
        for candidate in process_ids[:needed.value // 4]:
            handle = kernel.OpenProcess(0x1000, False, candidate)
            if not handle:
                continue
            try:
                path = c.create_unicode_buffer(1024)
                count = w.DWORD(len(path))
                if kernel.QueryFullProcessImageNameW(handle, 0, path, c.byref(count)) and Path(path.value).name.lower() == "darksoulsiii.exe":
                    pids.append(candidate)
            finally:
                kernel.CloseHandle(handle)
        if len(pids) != 1:
            raise SystemExit("Expected one running DarkSoulsIII.exe")
        args.pid = pids[0]
    report = json.dumps(inspect(args.pid), indent=2)
    print(report)
    if args.output:
        args.output.write_text(report + "\n", encoding="utf-8")
