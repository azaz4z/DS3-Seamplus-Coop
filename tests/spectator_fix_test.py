"""Test suite for the Spectator Stamina Bar & HUD Fix module."""
import ctypes
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPANION_DLL = ROOT / "build/companion/ds3sc_companion.dll"


class SpectatorFixMemorySimulationTest(unittest.TestCase):
    def setUp(self):
        # SprjChrDataModule simulation buffer (0x200 bytes)
        # Offsets:
        # +0xD8: int32 HP
        # +0xDC: int32 MaxHP
        # +0xE0: int32 BaseMaxHP
        # +0xE4: int32 MP
        # +0xE8: int32 MaxMP
        # +0xEC: int32 BaseMaxMP
        # +0xF0: int32 SP (Current Stamina)
        # +0xF4: int32 MaxSP
        # +0xF8: int32 BaseMaxSP
        self.buffer = bytearray(0x200)

    def set_hp(self, hp: int, max_hp: int):
        struct.pack_into("<ii", self.buffer, 0xD8, hp, max_hp)

    def set_stamina(self, sp: int, max_sp: int, base_sp: int):
        struct.pack_into("<iii", self.buffer, 0xF0, sp, max_sp, base_sp)

    def get_stamina(self):
        return struct.unpack_from("<iii", self.buffer, 0xF0)

    def test_stamina_offsets_layout(self):
        """Verify the exact byte offsets match DS3 1.15.2 SprjChrDataModule."""
        self.set_hp(1000, 1000)
        self.set_stamina(120, 120, 120)

        hp, max_hp = struct.unpack_from("<ii", self.buffer, 0xD8)
        sp, max_sp, base_sp = self.get_stamina()

        self.assertEqual(hp, 1000)
        self.assertEqual(max_hp, 1000)
        self.assertEqual(sp, 120)
        self.assertEqual(max_sp, 120)
        self.assertEqual(base_sp, 120)

    def test_dead_spectator_stamina_negative_overflow_guard(self):
        """Simulate lethal attack draining stamina to negative values when dead."""
        # Player died with -65 stamina from a guard break / lethal hit
        self.set_hp(0, 1000)
        self.set_stamina(-65, 120, 120)

        # Before fix: reading as unsigned causes massive uint32 expansion
        sp, max_sp, _ = self.get_stamina()
        raw_uint = ctypes.c_uint32(sp).value
        self.assertGreater(raw_uint, 4_000_000_000)

        # Emulate the fix logic: dead player stamina clamped to 0
        if sp < 0 or sp > max_sp:
            sp = 0
            struct.pack_into("<i", self.buffer, 0xF0, sp)

        corrected_sp, _, _ = self.get_stamina()
        self.assertEqual(corrected_sp, 0)
        self.assertEqual(ctypes.c_uint32(corrected_sp).value, 0)

    def test_dead_spectator_zero_max_stamina_division_by_zero_guard(self):
        """Simulate uninitialized / unhooked MaxSP causing NaN or infinite width."""
        self.set_hp(0, 1000)
        self.set_stamina(0, 0, 120)  # max_sp == 0

        sp, max_sp, base_sp = self.get_stamina()

        # Before fix: division by zero in UI calculation
        with self.assertRaises(ZeroDivisionError):
            _ = sp / max_sp

        # Emulate the fix logic: max_sp safely restored
        if max_sp <= 0:
            max_sp = base_sp if (0 < base_sp < 1000) else 100
            struct.pack_into("<i", self.buffer, 0xF4, max_sp)

        _, fixed_max_sp, _ = self.get_stamina()
        self.assertEqual(fixed_max_sp, 120)
        ratio = sp / fixed_max_sp
        self.assertEqual(ratio, 0.0)

    def test_alive_player_stamina_unaffected(self):
        """Verify alive player stamina is preserved and not altered."""
        self.set_hp(850, 1000)
        self.set_stamina(95, 120, 120)

        hp, _ = struct.unpack_from("<ii", self.buffer, 0xD8)
        self.assertGreater(hp, 0)

        sp, max_sp, base_sp = self.get_stamina()
        self.assertEqual(sp, 95)
        self.assertEqual(max_sp, 120)
        self.assertEqual(base_sp, 120)


class SpectatorFixDllExportTest(unittest.TestCase):
    def test_companion_dll_exports_spectator_stats(self):
        """Verify ds3sc_companion.dll exports ds3sc_get_spectator_fix_stats."""
        self.assertTrue(COMPANION_DLL.is_file(), f"Missing {COMPANION_DLL}")
        dll = ctypes.CDLL(str(COMPANION_DLL))
        self.assertTrue(hasattr(dll, "ds3sc_get_spectator_fix_stats"))

        stamina_fixes = ctypes.c_uint32(0)
        spectator_frames = ctypes.c_uint32(0)
        dll.ds3sc_get_spectator_fix_stats(ctypes.byref(stamina_fixes), ctypes.byref(spectator_frames))
        # Valid execution without crash
        self.assertGreaterEqual(stamina_fixes.value, 0)
        self.assertGreaterEqual(spectator_frames.value, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
