"""Execute the actual injected x64 code against synthetic memory, not the C++ model.

These tests validate memory scope/ABI/PE structure; they cannot validate the
inferred game objects, encounter reload or replication without a live session.
"""
import importlib.util
import ctypes
import os
from pathlib import Path
import struct
import sys
import unittest

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools/vendor/python"))
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn import x86_const as x86

spec = importlib.util.spec_from_file_location("greatwood", PROJECT / "tools/patch-greatwood.py")
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)


class GreatwoodPatchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        candidates = [
            PROJECT / "SeamlessCoop/ds3sc_original.dll",
            PROJECT / "SeamlessCoop/ds3sc.dll",
            PROJECT / "build/backups/ds3sc-before-greatwood-8760fc0d.dll",
            PROJECT.parent / "SeamlessCoop/ds3sc_original.dll",
            PROJECT.parent / "SeamlessCoop/ds3sc.dll",
        ]
        orig_path = next((c for c in candidates if c.is_file()), None)
        if not orig_path:
            raise FileNotFoundError(f"DLL original no encontrada. Rutas buscadas: {candidates}")
        cls.original = orig_path.read_bytes()
        cls.code = patcher.assemble()
        cls.patched = patcher.patch(cls.original, cls.code)

    def test_unknown_binary_refused(self):
        with self.assertRaisesRegex(ValueError, "Unsupported DLL"):
            patcher.patch(self.original[:-1] + bytes([self.original[-1] ^ 1]), self.code)
        with self.assertRaisesRegex(ValueError, "Unsupported DLL"):
            patcher.patch(self.patched, self.code)

    def test_pe_and_original_code_preserved(self):
        before = pefile.PE(data=self.original)
        after = pefile.PE(data=self.patched)
        self.assertTrue(after.verify_checksum())
        for old, new in zip(before.sections, after.sections):
            self.assertEqual(old.__pack__(), new.__pack__())
            expected = bytearray(old.get_data())
            if old.Name.startswith(b".text"):
                for site, length in [(patcher.SITE, 7), (patcher.BONFIRE_SITE, 6), (patcher.GUEST_SITE, 10)]:
                    offset = site - old.VirtualAddress
                    expected[offset:offset+length] = new.get_data()[offset:offset+length]
            self.assertEqual(expected, new.get_data())
        self.assertEqual(len(after.DIRECTORY_ENTRY_EXCEPTION), len(before.DIRECTORY_ENTRY_EXCEPTION) + 3)
        for old, new in zip(before.DIRECTORY_ENTRY_EXCEPTION, after.DIRECTORY_ENTRY_EXCEPTION):
            self.assertEqual(old.struct.__pack__(), new.struct.__pack__())
        entry = after.DIRECTORY_ENTRY_EXCEPTION[-3].struct
        self.assertEqual(after.get_data(entry.BeginAddress, len(self.code)), self.code)
        self.assertEqual(after.get_data(entry.UnwindData, 8), bytes.fromhex("01 07 02 00 07 01 1b 00"))
        # Actual Windows x64 unwind below is tested separately using RtlVirtualUnwind.
        self.assertEqual(after.sections[-1].Characteristics, 0x60000020)

    @unittest.skipUnless(os.name == "nt", "Windows unwind API")
    def test_windows_unwind(self):
        pe = pefile.PE(data=self.patched)
        mapped = ctypes.create_string_buffer(pe.get_memory_mapped_image())
        base = ctypes.addressof(mapped)
        stack = ctypes.create_string_buffer(0x2000)
        entry_sp = ((ctypes.addressof(stack) + 0x1000) & -16) + 8
        return_pc = 0x1234567890
        ctypes.c_uint64.from_address(entry_sp).value = return_pc
        unwind = ctypes.WinDLL("ntdll").RtlVirtualUnwind
        unwind.argtypes = [ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64,
                           ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                           ctypes.c_void_p, ctypes.c_void_p]
        unwind.restype = ctypes.c_void_p
        for ordinal in [-3, -2, -1]:
            entry = pe.DIRECTORY_ENTRY_EXCEPTION[ordinal].struct
            entry_address = base + pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress
            entry_address += (len(pe.DIRECTORY_ENTRY_EXCEPTION) + ordinal) * 12
            size = entry.EndAddress-entry.BeginAddress
            prologue, frame_size, epilogue = (4, 0x18, 5) if ordinal == -2 else (7, 0xd8, 8)
            if ordinal == -1:
                prologue, frame_size, epilogue = 8, 0xd8, 9
                ctypes.c_uint64.from_address(entry_sp-8).value = 0x12341234
            for offset, allocation in [(0, 0), (prologue, frame_size), (size-epilogue, frame_size), (size-1, 0)]:
                with self.subTest(ordinal=ordinal, offset=offset):
                    storage = ctypes.create_string_buffer(0x500)
                    context = (ctypes.addressof(storage) + 15) & -16
                    ctypes.c_uint32.from_address(context + 48).value = 0x10000b
                    ctypes.c_uint64.from_address(context + 152).value = entry_sp - allocation
                    ctypes.c_uint64.from_address(context + 176).value = 0x12341234 if allocation == 0 else 0xdeadbeef
                    pc = base + entry.BeginAddress + offset
                    ctypes.c_uint64.from_address(context + 248).value = pc
                    handler, frame = ctypes.c_void_p(), ctypes.c_uint64()
                    unwind(0, base, pc, entry_address, context,
                           ctypes.byref(handler), ctypes.byref(frame), None)
                    self.assertEqual(ctypes.c_uint64.from_address(context + 248).value, return_pc)
                    self.assertEqual(ctypes.c_uint64.from_address(context + 152).value, entry_sp + 8)
                    if ordinal == -1:
                        self.assertEqual(ctypes.c_uint64.from_address(context + 176).value, 0x12341234)

    def run_hook(self, *, base=0x180000000, marker=905180, slot=0, half=1,
                 state=3, defeated=False, alt_defeated=False, missing=None,
                 index=0, alias=False, alt=True, ready=True, repeat=False, reloading=False, guest=False):
        uc = Uc(UC_ARCH_X86, UC_MODE_64)
        pe = pefile.PE(data=self.patched)
        uc.mem_map(base, pe.OPTIONAL_HEADER.SizeOfImage)
        uc.mem_write(base, pe.get_memory_mapped_image())
        arena, arena_size = 0x20000000, 0x30000
        uc.mem_map(arena, arena_size)
        root = arena
        session = arena + 0x1000
        menu_wrapper, menu_global, menu = arena + 0x2000, arena + 0x3000, arena + 0x4000
        flag_wrapper, flag_global, manager = arena + 0x7000, arena + 0x8000, arena + 0x9000
        lookup = arena + 0xB000
        primary, alternate, alt_table = arena + 0xC000, arena + 0xD000, arena + 0xE000
        permanent, temporary = arena + 0x10000, arena + 0x11000
        alt_permanent, alt_temporary = arena + 0x12000, arena + 0x13000
        world_wrapper, world_global, world = arena+0x14000, arena+0x15000, arena+0x16000
        network, peer_vector, peer = arena+0x17000, arena+0x18000, arena+0x19000
        if alias:
            alternate, alt_permanent, alt_temporary = primary, permanent, temporary
        def q(address, value):
            uc.mem_write(address, struct.pack("<Q", value))
        def d(address, value):
            uc.mem_write(address, struct.pack("<I", value))
        pointers = {
            "session": root + 0xb0,
            "menu_wrapper": root + 0x60, "menu_global": menu_wrapper, "menu": menu_global,
            "flag_wrapper": root + 0x48, "flag_global": flag_wrapper + 0x20,
            "manager": flag_global, "lookup": flag_wrapper + 0x40,
            "primary": manager + 0x40, "alt_table": manager + 0x218,
            "alternate": alt_table + 0x18,
            "world_wrapper": root+0x10, "world_global": world_wrapper+0x28,
            "world": world_global, "network": root+8, "peer_vector": network+0x40,
        }
        for name, value in locals().copy().items():
            if name in pointers:
                q(pointers[name], value)
        q(session, state)
        q(peer_vector, peer_vector+0x40)
        q(peer_vector+8, peer_vector+0x50)
        for n, flags in [(0, 5 if guest else 3), (1, 9)]:
            q(peer_vector+0x40+n*8, peer+n*0x200)
            q(peer+n*0x200, 0x12345600+n)
            d(peer+n*0x200+0x120, flags)
        d(world+0x65c, 1 if reloading else 0)
        uc.mem_write(manager + 0x228, bytes([int(ready)]))
        if not alt:
            q(manager + 0x218, 0)
        bank_index = max(0, index)
        for bank, perm, temp in [(primary, permanent, temporary), (alternate, alt_permanent, alt_temporary)]:
            q(bank + bank_index * 0xA8, perm)
            q(bank + bank_index * 0xA8 + 0x50, temp)
            uc.mem_write(perm, b"\x55" * 128)  # 13100800 clear; nearby progress remains set.
            uc.mem_write(temp, b"\xff" * 128)
        if defeated:
            d(permanent + 100, 0xffffffff)
        if alt_defeated:
            d(alt_permanent + 100, 0xffffffff)
        pointers.update({"permanent": primary + bank_index * 0xA8,
                         "temporary": primary + bank_index * 0xA8 + 0x50,
                         "alt_permanent": alternate + bank_index * 0xA8,
                         "alt_temporary": alternate + bank_index * 0xA8 + 0x50})
        slots = [0x1af8, 0x1b90, 0x1c28]
        for offset in slots:
            q(menu + offset, 0x1122334455667788)
        d(menu + slots[slot] + half * 4, marker)
        if missing:
            q(pointers[missing], 0)
        # A real callback is allowed to clobber every volatile register, upper
        # return bits, all shadow space and XMM0..5. Check preservation/alignment.
        uc.mem_write(lookup, b"\xc3")
        uc.mem_write(base+0x68c40, b"\xc3")
        uc.mem_write(base+0x68f40, b"\xc3")
        calls = []
        resets = []
        receives = []
        def on_code(machine, address, size, _):
            if address == base+0x68f40:
                self.assertTrue(guest)
                self.assertEqual(machine.reg_read(x86.UC_X86_REG_R8), 0x14)
                self.assertEqual(machine.reg_read(x86.UC_X86_REG_RDX), 0x12345600)
                self.assertEqual(machine.reg_read(x86.UC_X86_REG_RSP) % 16, 8)
                machine.mem_write(machine.reg_read(x86.UC_X86_REG_R9), b"\0")
                machine.reg_write(x86.UC_X86_REG_RAX, 1)
                receives.append(0x14)
                return
            if address == base+0x68c40:
                sp = machine.reg_read(x86.UC_X86_REG_RSP)
                self.assertEqual(sp % 16, 8)
                self.assertEqual(machine.reg_read(x86.UC_X86_REG_RCX), network)
                self.assertEqual(machine.reg_read(x86.UC_X86_REG_R9), 1)
                payload = machine.reg_read(x86.UC_X86_REG_R8)
                self.assertEqual(machine.mem_read(payload, 1), b"\0")
                self.assertEqual(struct.unpack("<I", machine.mem_read(sp+0x28, 4))[0], 0x14)
                self.assertEqual(machine.mem_read(sp+0x30, 1), b"\x03")
                resets.append(machine.reg_read(x86.UC_X86_REG_RDX))
                return
            if address != lookup:
                return
            sp = machine.reg_read(x86.UC_X86_REG_RSP)
            self.assertEqual(sp % 16, 8)
            self.assertEqual(machine.reg_read(x86.UC_X86_REG_RCX), manager)
            self.assertEqual(machine.reg_read(x86.UC_X86_REG_RDX), 31)
            self.assertEqual(machine.reg_read(x86.UC_X86_REG_R8), 0)
            self.assertEqual(machine.reg_read(x86.UC_X86_REG_R9), 0)
            calls.append(address)
            machine.mem_write(sp + 8, b"\xcc" * 32)
            for reg in ["RCX", "RDX", "R8", "R9", "R10", "R11"]:
                machine.reg_write(getattr(x86, "UC_X86_REG_" + reg), 0xfedcba9876543210)
            for n in range(6):
                machine.reg_write(getattr(x86, f"UC_X86_REG_XMM{n}"), (1 << 128) - 1)
            machine.reg_write(x86.UC_X86_REG_RAX, 0xabcdef0000000000 | (index & 0xffffffff))
        uc.hook_add(UC_HOOK_CODE, on_code)
        registers = {}
        for n, reg in enumerate(["RBX", "RCX", "RDX", "RSI", "RBP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"]):
            registers[getattr(x86, "UC_X86_REG_" + reg)] = 0x100000000 + n
        for n in range(16):
            registers[getattr(x86, f"UC_X86_REG_XMM{n}")] = (1 << 120) + n
        registers[x86.UC_X86_REG_RDI] = root
        registers[x86.UC_X86_REG_RSP] = arena + 0x2F000
        start, end = base+patcher.SITE, base+patcher.RESUME
        if guest:
            registers[x86.UC_X86_REG_RCX] = root
            registers[x86.UC_X86_REG_RSP] += 8
            start, end = base+0x8d210, arena+0x2ff00
            q(registers[x86.UC_X86_REG_RSP], end)
        for reg, value in registers.items():
            uc.reg_write(reg, value)
        snapshot = bytes(uc.mem_read(arena, 0x20000))
        uc.emu_start(start, end, count=1500)
        self.assertEqual(uc.reg_read(x86.UC_X86_REG_RIP), end)
        if guest:
            for name in ["RCX", "RDX", "R8", "R9", "R10", "R11", *[f"XMM{n}" for n in range(6)]]:
                registers.pop(getattr(x86, "UC_X86_REG_"+name))
            registers[x86.UC_X86_REG_RSP] += 8
            self.assertEqual(receives, [0x14])
        for reg, value in registers.items():
            self.assertEqual(uc.reg_read(reg), value, f"register {reg}")
        if not guest:
            self.assertEqual(uc.reg_read(x86.UC_X86_REG_RAX), 0 if missing == "session" else session)
        expected = bytearray(snapshot)
        reset = (state & 7) == (1 if guest else 3) and marker in (3100800, 905180) and ready and index >= 0
        reset = reset and not defeated and not alt_defeated and missing is None and not reloading
        if reset:
            for pointer in set([temporary, alt_temporary] if alt else [temporary]):
                offset = pointer - arena
                expected[offset+100:offset+112] = b"\0" * 12
                expected[offset+112:offset+116] = struct.pack("<I", 0x0fffffff)
            offset = menu + slots[slot] - arena
            expected[offset:offset+8] = b"\xff" * 8
        if reset or (guest and not reloading):
            for address in [world+0x65c, world_wrapper+0x3c]:
                struct.pack_into("<I", expected, address-arena, 1)
        self.assertEqual(resets, [0x12345600, 0x12345601] if reset and not guest else [])
        self.assertEqual(bytes(uc.mem_read(arena, 0x20000)), expected)
        if repeat:
            uc.emu_start(base + patcher.SITE, base + patcher.RESUME, count=1500)
            self.assertEqual(bytes(uc.mem_read(arena, 0x20000)), expected)
            self.assertEqual(len(calls), 1)
            self.assertEqual(len(resets), 2)

    def test_reset_scope_and_abi(self):
        for base in [0x180000000, 0x7ff800000000]:
            for slot in range(3):
                for half in range(2):
                    for marker in (905180, 3100800):
                        with self.subTest(base=base, slot=slot, half=half, marker=marker):
                            self.run_hook(base=base, slot=slot, half=half, marker=marker)

    def test_guards(self):
        for options in [dict(marker=123456), dict(state=0), dict(state=1), dict(state=7),
                        dict(defeated=True), dict(alt_defeated=True), dict(ready=False), dict(index=-1), dict(reloading=True)]:
            with self.subTest(options=options):
                self.run_hook(**options)
        for missing in ["session", "menu_wrapper", "menu_global", "menu", "flag_wrapper",
                        "flag_global", "manager", "lookup", "primary",
                        "alternate", "permanent", "temporary", "alt_permanent", "alt_temporary",
                        "world_wrapper", "world_global", "world", "network", "peer_vector"]:
            with self.subTest(missing=missing):
                self.run_hook(missing=missing)

    def test_banks_and_repeat(self):
        for options in [dict(index=2), dict(alias=True), dict(alt=False), dict(repeat=True)]:
            with self.subTest(options=options):
                self.run_hook(**options)

    def test_guest_original_reload_receiver(self):
        for options in [dict(), dict(base=0x7ff800000000), dict(alt_defeated=True),
                        dict(marker=123456), dict(index=-1), dict(reloading=True)]:
            with self.subTest(options=options):
                self.run_hook(guest=True, state=1, **options)

    def test_guest_bonfire_full_original_handler(self):
        # Exercise the original guest/host/invader guards and validation lookup,
        # not just the arithmetic in an isolated stub. No live game is needed.
        for base in [0x180000000, 0x7ff800000000]:
            for state, destination, valid, game_loaded in [
                (1, 3102953, True, True), (1, 4002950, True, True),
                (1, 5102950, True, True), (1, 3102953, False, True),
                (1, -1, True, True), (3, 3102953, True, True),
                (5, 3102953, True, True), (1, 3102953, True, False)]:
                with self.subTest(base=base, state=state, destination=destination, valid=valid, game_loaded=game_loaded):
                    pe = pefile.PE(data=self.patched)
                    uc = Uc(UC_ARCH_X86, UC_MODE_64)
                    uc.mem_map(base, pe.OPTIONAL_HEADER.SizeOfImage)
                    uc.mem_write(base, pe.get_memory_mapped_image())
                    arena = 0x30000000
                    uc.mem_map(arena, 0x20000)
                    hook, root, state_ptr = arena, arena+0x1000, arena+0x2000
                    wrapper, game, global_ptr, repo, table = [arena+n*0x1000 for n in range(3, 8)]
                    flag_a, flag_b = arena+0x8000, arena+0x9000
                    def q(address, value):
                        uc.mem_write(address, struct.pack("<Q", value))
                    def d(address, value):
                        uc.mem_write(address, struct.pack("<I", value & 0xffffffff))
                    for address, value in [(hook+0x28, root), (root+0xb0, state_ptr),
                                           (root+0x28, wrapper), (wrapper+8, repo), (repo+8, table),
                                           (wrapper+0x10, global_ptr), (global_ptr, game if game_loaded else 0),
                                           (root+0x50, flag_a), (root+0x58, flag_b)]:
                        q(address, value)
                    q(state_ptr, state)
                    d(state_ptr+0x170, destination)
                    d(game+0xacc, 3102954)
                    uc.mem_write(base+0x69480, b"\xc3")
                    lookups = []
                    def callback(machine, address, size, _):
                        if address == base+0x69480:
                            self.assertEqual(machine.reg_read(x86.UC_X86_REG_RCX), table)
                            self.assertEqual(machine.reg_read(x86.UC_X86_REG_RSP) % 16, 8)
                            lookups.append(machine.reg_read(x86.UC_X86_REG_EDX))
                            machine.reg_write(x86.UC_X86_REG_RAX, 1 if valid else 0)
                    uc.hook_add(UC_HOOK_CODE, callback)
                    sp, end = arena+0x1e008, arena+0x1f000
                    q(sp, end)
                    uc.reg_write(x86.UC_X86_REG_RSP, sp)
                    uc.reg_write(x86.UC_X86_REG_RCX, hook)
                    preserved = [x86.UC_X86_REG_RBX, x86.UC_X86_REG_RBP, x86.UC_X86_REG_RDI,
                                 x86.UC_X86_REG_RSI, x86.UC_X86_REG_R14]
                    for reg in preserved:
                        uc.reg_write(reg, 0x12345000+reg)
                    uc.emu_start(base+0x4d0a0, end, count=400)
                    self.assertEqual(uc.reg_read(x86.UC_X86_REG_RIP), end)
                    self.assertEqual(uc.reg_read(x86.UC_X86_REG_RSP), sp+8)
                    for reg in preserved:
                        self.assertEqual(uc.reg_read(reg), 0x12345000+reg)
                    attempted = state == 1 and destination != -1
                    self.assertEqual(lookups, [destination-1000] if attempted else [])
                    restored = attempted and valid and game_loaded
                    self.assertEqual(struct.unpack("<I", uc.mem_read(game+0xacc, 4))[0], destination if restored else 3102954)
                    self.assertEqual(struct.unpack("<I", uc.mem_read(state_ptr+0x170, 4))[0], 0xffffffff if attempted else destination & 0xffffffff)

    def test_selective_patching(self):
        # 1. Both disabled: returns original unchanged
        none_patched = patcher.patch(self.original, enable_greatwood=False, enable_bonfire=False)
        self.assertEqual(none_patched, self.original)

        # 2. Greatwood only: bonfire site unchanged, greatwood sites patched
        gw_only = patcher.patch(self.original, enable_greatwood=True, enable_bonfire=False)
        pe_gw = pefile.PE(data=gw_only)
        bonfire_site = pe_gw.get_offset_from_rva(patcher.BONFIRE_SITE)
        self.assertEqual(gw_only[bonfire_site:bonfire_site+6], patcher.BONFIRE_EXPECTED)
        site = pe_gw.get_offset_from_rva(patcher.SITE)
        self.assertNotEqual(gw_only[site:site+7], patcher.EXPECTED)

        # 3. Bonfire only: greatwood sites unchanged, bonfire site patched
        bf_only = patcher.patch(self.original, enable_greatwood=False, enable_bonfire=True)
        pe_bf = pefile.PE(data=bf_only)
        site_bf = pe_bf.get_offset_from_rva(patcher.SITE)
        guest_site_bf = pe_bf.get_offset_from_rva(patcher.GUEST_SITE)
        self.assertEqual(bf_only[site_bf:site_bf+7], patcher.EXPECTED)
        self.assertEqual(bf_only[guest_site_bf:guest_site_bf+10], patcher.GUEST_EXPECTED)
        bonfire_site_bf = pe_bf.get_offset_from_rva(patcher.BONFIRE_SITE)
        self.assertNotEqual(bf_only[bonfire_site_bf:bonfire_site_bf+6], patcher.BONFIRE_EXPECTED)


if __name__ == "__main__":
    unittest.main(verbosity=2)
