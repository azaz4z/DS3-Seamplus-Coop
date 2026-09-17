"""Exercise the real launcher against isolated game/DLL fixtures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "build/bin/ds3sc_launcher.exe"


class StartupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="launcher-fixture-", dir=ROOT / "build")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        shutil.copy2(ROOT / "build/fixture_game.exe", self.path / "DarkSoulsIII.exe")
        shutil.copy2(ROOT / "build/fixture_mod.dll", self.path / "fixture_mod.dll")
        self.result = self.path / "result.txt"
        self.env = {**os.environ, "DS3SC_TEST_RESULT": str(self.result), "DS3SC_TEST_MODE": ""}

    def run_launcher(self, *extra):
        return subprocess.run([str(LAUNCHER), "--no-gui", "--game-dir", str(self.path),
                               "--dll", "fixture_mod.dll", *extra], env=self.env,
                              capture_output=True, timeout=25)

    def test_preflight_does_not_start_game(self):
        result = self.run_launcher("--check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.result.exists())

    def test_invalid_explicit_game_dir_does_not_fall_back_to_steam(self):
        result = self.run_launcher("--game-dir", str(self.path / "missing"), "--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.result.exists())

    def test_missing_dll_fails_without_starting(self):
        (self.path / "fixture_mod.dll").unlink()
        result = self.run_launcher("--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.result.exists())

    def test_dll_loaded_before_game_main(self):
        result = self.run_launcher("--no-companion")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.result.read_text(), "PASS")

    def test_rejected_dll_never_runs_game_main(self):
        self.env["DS3SC_TEST_MODE"] = "reject"
        result = self.run_launcher("--no-companion")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.result.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
