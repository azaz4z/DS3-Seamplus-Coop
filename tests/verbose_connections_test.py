#!/usr/bin/env python3
"""
Test suite for Verbose Connection Notifications (verbose_connections) module.
Validates:
1. Language JSON files syntax and presence of required MODMSG_HOSTING keys.
2. DLL exports and native C++ lobby events, delivery retries and hook failures.
3. Integration with build_config.json and make_release.pyw module catalog.
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def test_language_files_keys():
    """Verify spanish.json and english.json contain required hosting message keys."""
    spa_path = ROOT / "src/languages/spanish.json"
    eng_path = ROOT / "src/languages/english.json"

    assert spa_path.is_file(), f"Missing spanish.json: {spa_path}"
    assert eng_path.is_file(), f"Missing english.json: {eng_path}"

    with open(spa_path, "r", encoding="utf-8") as f:
        spa = json.load(f)
    with open(eng_path, "r", encoding="utf-8") as f:
        eng = json.load(f)

    required_keys = [
        "MODMSG_HOSTING_STEAM",
        "MODMSG_HOSTING_LAN",
        "MODMSG_HOSTING_SUCCESS_STEAM",
        "MODMSG_HOSTING_SUCCESS_LAN",
        "MODMSG_HOSTING_FAILED",
    ]

    for key in required_keys:
        assert key in spa, f"Missing key '{key}' in spanish.json"
        assert key in eng, f"Missing key '{key}' in english.json"
        assert len(spa[key].strip()) > 0, f"Empty value for key '{key}' in spanish.json"
        assert len(eng[key].strip()) > 0, f"Empty value for key '{key}' in english.json"

    # Specific string verification
    assert "Steam" in spa["MODMSG_HOSTING_STEAM"]
    assert "LAN" in spa["MODMSG_HOSTING_LAN"]
    assert "Steam" in eng["MODMSG_HOSTING_STEAM"]
    assert "LAN" in eng["MODMSG_HOSTING_LAN"]

def test_build_config_integration():
    """Verify verbose_connections is enabled in build_config.json."""
    cfg_path = ROOT / "build_config.json"
    assert cfg_path.is_file(), f"Missing build_config.json: {cfg_path}"

    with open(cfg_path, "r", encoding="utf-8") as f:
        cfg = json.load(f)

    assert "verbose_connections" in cfg, "verbose_connections key missing in build_config.json"
    assert cfg["verbose_connections"] is True, "verbose_connections should be enabled by default in build_config.json"

def test_make_release_module_catalog():
    """Verify verbose_connections appears in make_release.pyw --list-modules output."""
    proc = subprocess.run(
        [sys.executable, str(ROOT / "make_release.pyw"), "--list-modules"],
        cwd=ROOT,
        capture_output=True,
        text=True
    )
    assert proc.returncode == 0, f"make_release.pyw --list-modules failed: {proc.stderr}"
    assert "verbose_connections" in proc.stdout, "verbose_connections not found in make_release.pyw module list"
    assert "Verbose Connection Notifications" in proc.stdout

def test_dll_exports_and_functionality():
    """Inspect exports without executing a DLL worker in the Python process."""
    import pefile
    dll_path = ROOT / "build/companion/ds3sc_companion.dll"
    assert dll_path.is_file(), "Build ds3sc_companion.dll before running this test"
    pe = pefile.PE(str(dll_path))
    exports = {symbol.name for symbol in pe.DIRECTORY_ENTRY_EXPORT.symbols}
    for name in (
        b"ds3sc_trigger_verbose_host_notification",
        b"ds3sc_trigger_verbose_leave_notification",
        b"ds3sc_get_verbose_connection_stats",
        b"ds3sc_get_verbose_delivery_stats",
    ):
        assert name in exports, f"Missing export: {name}"


def test_native_lobby_events():
    """Exercise actual C++ detours and their banner sink, including failures."""
    subprocess.run([sys.executable, str(ROOT / "tools/run_verbose_connections_test.py")],
                   cwd=ROOT, check=True, timeout=60)

if __name__ == "__main__":
    print("[1/5] Testing language files...")
    test_language_files_keys()
    print("      OK: Language files valid.")

    print("[2/5] Testing build configuration...")
    test_build_config_integration()
    print("      OK: build_config.json valid.")

    print("[3/5] Testing make_release.pyw module catalog...")
    test_make_release_module_catalog()
    print("      OK: make_release.pyw catalog valid.")

    print("[4/5] Testing DLL exports...")
    test_dll_exports_and_functionality()
    print("      OK: ds3sc_companion.dll exports valid.")

    print("[5/5] Testing native lobby events and delivery...")
    test_native_lobby_events()
    print("All verbose_connections automated tests passed; in-game visual check still required.")
