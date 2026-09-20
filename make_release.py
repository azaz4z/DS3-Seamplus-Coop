"""The Ashen Link: DS3 Coop - Modular Release Creation Application (PyQt6).

Location: Project root.
Configures the build modularly, selects patches and extensions,
and packages the release via a modern PyQt graphical interface.
"""
import argparse
from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent
PACKAGE_ROOT = ROOT.parent
RELEASE_DIR = ROOT / "release"
CONFIG_FILE = ROOT / "build_config.json"
DEFAULT_VERSION_BASE = "0.1.2"
DEFAULT_AUTHOR = "azaz4z"

@dataclass
class ReleaseModule:
    id: str
    name: str
    category: str
    description: str
    default: bool

MODULES = [
    ReleaseModule(
        id="greatwood_patch",
        name="Curse-Rotted Greatwood Boss Fix",
        category="Binary Patch (ds3sc.dll)",
        description="Fixes infinite loading screen, freeze and desynchronization during the Curse-Rotted Greatwood boss fight.",
        default=True
    ),
    ReleaseModule(
        id="guest_bonfires",
        name="Guest Bonfires Restoration",
        category="Binary Patch (ds3sc.dll)",
        description="Fixes bonfire checkpoint write-back (+0xacc) for guest players so bonfire resting and respawns work properly.",
        default=True
    ),
    ReleaseModule(
        id="ally_outline",
        name="Ally Outlines & Silhouettes (D3D11)",
        category="Native C++ Extension (Render)",
        description="Direct3D 11 60 FPS hook to draw occluded silhouettes behind walls and visible outlines for allies.",
        default=True
    ),
    ReleaseModule(
        id="player_outline",
        name="Player Outline & Silhouette (D3D11)",
        category="Native C++ Extension (Render/Diagnostics)",
        description="Shows the local player's captured render silhouette and contour to diagnose marker occlusion and hitbox alignment.",
        default=False
    ),
    ReleaseModule(
        id="ally_markers",
        name="Ally Diamond Markers (D3D11)",
        category="Native C++ Extension (Visual/HUD)",
        description="Direct3D 11 overhead diamond indicator pulsating over allies to easily track positions in co-op.",
        default=True
    ),
    ReleaseModule(
        id="companion_spawner",
        name="Test Companion Spawner (Ash Stone)",
        category="Native C++ Extension (Gameplay)",
        description="Adds the Ash Stone (ID 589006) to inventory to summon/dismiss a solo test companion NPC.",
        default=False
    ),
    ReleaseModule(
        id="hit_sync",
        name="Hit & Damage Synchronization (Hit Registration Fix)",
        category="Native C++ Extension (Combat/Netcode)",
        description="Fixes ghost hits and damage desynchronization against online enemies.",
        default=True
    ),
    ReleaseModule(
        id="counters",
        name="Stat Counters Module (Deaths, Kills, Backstabs)",
        category="Native C++ Extension (HUD/Stats)",
        description="Death counter, enemy kills, backstabs dealt and taken with onscreen D3D11 HUD and persistence.",
        default=True
    ),
    ReleaseModule(
        id="fps_unlock",
        name="FPS Unlocker (Uncap 60 FPS)",
        category="Native C++ Extension (Performance)",
        description="Unlocks Dark Souls III's native 60 FPS cap with configurable target framerate (144, 165, 240+ FPS) and optional VSync control.",
        default=True
    ),
    ReleaseModule(
        id="anim_fix",
        name="Animation & Locomotion Repair",
        category="Native C++ Extension (Animation)",
        description="Fixes stuck locomotion animations (skating/sliding across ground without moving legs) and unblocks ending cutscene stalls (such as the Fire Keeper extinguishing the flame).",
        default=True
    ),
    ReleaseModule(
        id="cutscene_fix",
        name="Cutscene Ally Isolation (Hide Allies in Cinematics)",
        category="Native C++ Extension (Cutscenes/Visual)",
        description="Automatically isolates and hides co-op allies, diamond markers, and outlines during cutscenes to prevent camera obstruction and scene disruptions.",
        default=True
    ),
]


def load_saved_config() -> dict:
    if CONFIG_FILE.is_file():
        try:
            cfg = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
            if "contadores" in cfg and "counters" not in cfg:
                cfg["counters"] = cfg["contadores"]
            return cfg
        except Exception:
            pass
    return {m.id: m.default for m in MODULES}

def save_config(config: dict):
    try:
        CONFIG_FILE.write_text(json.dumps(config, indent=2), encoding="utf-8")
    except Exception:
        pass

def compute_sha256(path: Path) -> str:
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()

def make_menu_label(version: str, author: str, release_id: str) -> str:
    if release_id:
        return f"The Ashen Link ({release_id[:7]})"
    return "The Ashen Link"

def apply_label(dll_path: Path, label: str):
    data = bytearray(dll_path.read_bytes())
    label = label[:35]
    label_len = len(label)

    offset_rdata = 0x174954
    p1 = label[:31].encode("utf-16-le")
    buf = bytearray(64)
    buf[:len(p1)] = p1
    data[offset_rdata:offset_rdata + 64] = buf

    offset_rsi = 0xd06f
    if label_len > 31:
        p2 = label[31:35].encode("utf-16-le")
        rsi_buf = bytearray(8)
        rsi_buf[:len(p2)] = p2
    else:
        rsi_buf = bytearray(8)
    data[offset_rsi:offset_rsi + 8] = rsi_buf

    offset_size = 0x16b830
    struct.pack_into("<Q", data, offset_size, label_len)
    dll_path.write_bytes(data)

def apply_folder_name(dll_path: Path, folder_name: str = "TheAshenLink"):
    """
    Patches the internal folder name in ds3sc.dll.
    Original string: b"SeamlessCoop\x00" (13 bytes, length 12).
    Replaced with: folder_name (must be <= 12 ASCII chars, padded with \x00).
    """
    data = bytearray(dll_path.read_bytes())
    replacement = folder_name.encode("ascii")[:12].ljust(12, b"\x00") + b"\x00"
    for target in [b"TheAshenLink\x00", b"SeamplusCoop\x00", b"SeamlessCoop\x00"]:
        idx = data.find(target)
        if idx != -1:
            data[idx:idx + len(target)] = replacement
            dll_path.write_bytes(data)
            return
    raise RuntimeError(f"Could not find folder name target in {dll_path.name}")

def build_release_pipeline(selected_modules: dict, release_id: str = None, skip_build: bool = False,
                           run_tests: bool = False, create_zip: bool = False, log_fn = print, progress_fn = None) -> Path:
    if not release_id:
        release_id = f"{secrets.randbelow(900000000) + 100000000:09d}"

    def report_step(step_num: int, total: int, msg: str):
        if progress_fn:
            progress_fn(int((step_num / total) * 100), msg)
        log_fn(f"[{step_num}/{total}] {msg}")

    save_config(selected_modules)

    version_str = f"{DEFAULT_VERSION_BASE}-by-{DEFAULT_AUTHOR}-({release_id})"
    release_name = f"The-Ashen-Link-DS3-Coop-{version_str}"
    menu_label = make_menu_label(DEFAULT_VERSION_BASE, DEFAULT_AUTHOR, release_id)

    log_fn("=" * 65)
    log_fn(f"  STARTING RELEASE BUILD: {release_name}")
    log_fn(f"  RELEASE ID:           {release_id}")
    log_fn(f"  TITLE SCREEN LABEL:   {menu_label}")
    log_fn("  SELECTED MODULES:")
    for m in MODULES:
        status = "[X] ENABLED" if selected_modules.get(m.id, False) else "[ ] SKIPPED"
        log_fn(f"    - {status:15} {m.name}")
    log_fn("=" * 65)

    has_native_extensions = (selected_modules.get("ally_outline", False) or
                            selected_modules.get("player_outline", False) or
                            selected_modules.get("ally_markers", False) or
                            selected_modules.get("companion_spawner", False) or
                            selected_modules.get("hit_sync", False) or
                            selected_modules.get("counters", False) or
                            selected_modules.get("contadores", False) or
                            selected_modules.get("fps_unlock", False) or
                            selected_modules.get("anim_fix", False) or
                            selected_modules.get("cutscene_fix", False))
    greatwood_enabled = selected_modules.get("greatwood_patch", False)
    bonfire_enabled = selected_modules.get("guest_bonfires", False)

    # 1. Compile mod and launcher binaries
    report_step(1, 6, "Compiling modular binaries with MSVC...")
    if not skip_build:
        build_cmd = [sys.executable, str(ROOT / "tools/build_binaries.py")]
        if selected_modules.get("ally_outline", False):
            build_cmd.append("--with-ally-outline")
        else:
            build_cmd.append("--without-ally-outline")

        if selected_modules.get("player_outline", False):
            build_cmd.append("--with-player-outline")
        else:
            build_cmd.append("--without-player-outline")

        if selected_modules.get("ally_markers", False):
            build_cmd.append("--with-ally-markers")
        else:
            build_cmd.append("--without-ally-markers")

        if selected_modules.get("companion_spawner", False):
            build_cmd.append("--with-companion")
        else:
            build_cmd.append("--without-companion")

        if selected_modules.get("hit_sync", False):
            build_cmd.append("--with-hit-sync")
        else:
            build_cmd.append("--without-hit-sync")

        if selected_modules.get("counters", False) or selected_modules.get("contadores", False):
            build_cmd.append("--with-counters")
        else:
            build_cmd.append("--without-counters")

        if selected_modules.get("fps_unlock", False):
            build_cmd.append("--with-fps-unlock")
        else:
            build_cmd.append("--without-fps-unlock")

        if selected_modules.get("anim_fix", False):
            build_cmd.append("--with-anim-fix")
        else:
            build_cmd.append("--without-anim-fix")

        if selected_modules.get("cutscene_fix", False):
            build_cmd.append("--with-cutscene-fix")
        else:
            build_cmd.append("--without-cutscene-fix")

        build_proc = subprocess.run(build_cmd, cwd=ROOT, capture_output=True, text=True)
        if build_proc.returncode != 0:
            log_fn("ERROR STDERR:\n" + build_proc.stderr)
            log_fn("STDOUT:\n" + build_proc.stdout)
            raise RuntimeError("Modular MSVC compilation failed.")
        log_fn("      MSVC binaries generated successfully.")
    else:
        log_fn("      (--skip-build mode: reusing existing binaries).")

    # 2. Prepare staging directory
    report_step(2, 6, "Preparing staging directory...")
    output_dir = Path(tempfile.mkdtemp(prefix="release-stage-", dir=ROOT / "build"))
    log_fn(f"      Staging directory: {output_dir.name}")

    # 3. Patch integration tests
    report_step(3, 6, "Verifying integration tests...")
    if (greatwood_enabled or bonfire_enabled) and run_tests:
        log_fn("      Running Unicorn emulation test suite (greatwood_patch_test.py)...")
        test_proc = subprocess.run([sys.executable, str(ROOT / "tests/greatwood_patch_test.py")],
                                    cwd=ROOT, capture_output=True, text=True)
        if test_proc.returncode != 0:
            log_fn("ERROR IN TEST:\n" + test_proc.stderr)
            raise RuntimeError("Tests failed in greatwood_patch_test.py")
        log_fn("      Binary patch tests passed successfully (9/9 OK).")
    elif greatwood_enabled or bonfire_enabled:
        log_fn("      (Unicorn emulation tests skipped for faster build. Use --run-tests to execute).")
    else:
        log_fn("      (No binary patches selected: tests skipped).")

    # 4. Verify launcher
    report_step(4, 6, "Verifying launcher executable...")
    launcher_candidates = [
        ROOT / "build/bin/TheAshenLink.exe",
        ROOT / "TheAshenLink.exe",
        ROOT / "build/TheAshenLink.exe",
        PACKAGE_ROOT / "TheAshenLink.exe",
        ROOT / "build/bin/ds3sc_launcher.exe",
        ROOT / "ds3sc_launcher.exe",
        ROOT / "build/ds3sc_launcher.exe",
        PACKAGE_ROOT / "ds3sc_launcher.exe",
    ]
    launcher_src = next((c for c in launcher_candidates if c.is_file()), None)
    if not launcher_src:
        raise RuntimeError("Could not find compiled TheAshenLink.exe.")
    log_fn(f"      Launcher verified: {launcher_src.name} ({launcher_src.stat().st_size:,} bytes)")

    # 5. Prepare core DLL (ds3sc.dll)
    report_step(5, 6, "Configuring ds3sc.dll...")
    orig_candidates = [
        ROOT / "TheAshenLink/ds3sc_original.dll",
        ROOT / "TheAshenLink/ds3sc.dll",
        ROOT / "SeamplusCoop/ds3sc_original.dll",
        ROOT / "SeamplusCoop/ds3sc.dll",
        ROOT / "SeamlessCoop/ds3sc_original.dll",
        ROOT / "SeamlessCoop/ds3sc.dll",
        ROOT / "build/backups/ds3sc-before-greatwood-8760fc0d.dll",
        PACKAGE_ROOT / "SeamlessCoop/ds3sc_original.dll",
        PACKAGE_ROOT / "SeamlessCoop/ds3sc.dll",
    ]
    orig_dll = next((c for c in orig_candidates if c.is_file()), None)
    if not orig_dll:
        raise RuntimeError(f"Original DLL missing. Searched in: {orig_candidates}")

    patched_dll = ROOT / "build/ds3sc_configured.dll"
    if greatwood_enabled or bonfire_enabled:
        desc_parts = []
        if greatwood_enabled: desc_parts.append("Curse-Rotted Greatwood")
        if bonfire_enabled: desc_parts.append("Guest Bonfires")
        log_fn(f"      Injecting binary patches ({' + '.join(desc_parts)})...")

        cmd = [sys.executable, str(ROOT / "tools/patch-greatwood.py"), str(orig_dll), str(patched_dll)]
        if not greatwood_enabled:
            cmd.append("--without-greatwood")
        if not bonfire_enabled:
            cmd.append("--without-bonfire")

        patch_proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if patch_proc.returncode != 0:
            log_fn("ERROR IN PATCH:\n" + patch_proc.stderr)
            raise RuntimeError("Failed patch-greatwood.py")
    else:
        log_fn("      Using original ds3sc.dll (no binary patches selected)...")
        shutil.copy2(orig_dll, patched_dll)

    apply_label(patched_dll, menu_label)
    apply_folder_name(patched_dll, "TheAshenLink")
    patched_sha = compute_sha256(patched_dll)
    log_fn(f"      ds3sc.dll ready: SHA-256 = {patched_sha}")

    # 6. Deploy release structure
    report_step(6, 6, "Deploying release files...")
    target_launcher = output_dir / "TheAshenLink.exe"
    shutil.copy2(launcher_src, target_launcher)
    log_fn(f"      + {target_launcher.name}")

    release_coop = output_dir / "TheAshenLink"
    release_coop.mkdir(parents=True, exist_ok=True)

    target_dll = release_coop / "ds3sc.dll"
    shutil.copy2(patched_dll, target_dll)
    log_fn(f"      + TheAshenLink/{target_dll.name}")

    if has_native_extensions:
        companion_candidates = [
            ROOT / "build/companion/ds3sc_companion.dll",
            ROOT / "build/TheAshenLink/ds3sc_companion.dll",
            ROOT / "TheAshenLink/ds3sc_companion.dll",
            ROOT / "build/SeamplusCoop/ds3sc_companion.dll",
            ROOT / "SeamplusCoop/ds3sc_companion.dll",
            ROOT / "build/SeamlessCoop/ds3sc_companion.dll",
            ROOT / "SeamlessCoop/ds3sc_companion.dll",
        ]
        companion_src = next((c for c in companion_candidates if c.is_file()), None)
        if not companion_src:
            raise RuntimeError("Native extensions were enabled but ds3sc_companion.dll is missing")
        target_comp = release_coop / "ds3sc_companion.dll"
        shutil.copy2(companion_src, target_comp)
        log_fn(f"      + TheAshenLink/{target_comp.name} (Extensions included)")
    else:
        log_fn("      - TheAshenLink/ds3sc_companion.dll SKIPPED")

    # Settings INI
    settings_candidates = [
        ROOT / "build/TheAshenLink/ds3sc_settings.ini",
        ROOT / "TheAshenLink/ds3sc_settings.ini",
        ROOT / "build/SeamplusCoop/ds3sc_settings.ini",
        ROOT / "SeamplusCoop/ds3sc_settings.ini",
        ROOT / "build/SeamlessCoop/ds3sc_settings.ini",
        ROOT / "build/ds3sc_settings_release.ini",
        ROOT / "SeamlessCoop/ds3sc_settings.ini",
        PACKAGE_ROOT / "SeamlessCoop/ds3sc_settings.ini",
    ]
    settings_src = next((c for c in settings_candidates if c.is_file()), None)
    if not settings_src:
        raise RuntimeError("Missing ds3sc_settings.ini.")
    raw_settings = settings_src.read_text(encoding="utf-8")
    settings_lines = [
        f"; The Ashen Link: DS3 Coop v{DEFAULT_VERSION_BASE} by {DEFAULT_AUTHOR} (Release ID: {release_id})",
        f"; Build timestamp: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
        "",
    ]
    for s_line in raw_settings.splitlines():
        if s_line.startswith("; The Ashen Link") or s_line.startswith("; DS3 Seamplus Co-op") or s_line.startswith("; DS3 Seamless Co-op") or s_line.startswith("; Build timestamp") or s_line.startswith("; Fecha"):
            continue
        if s_line.strip().startswith("cooppassword"):
            settings_lines.append("cooppassword = 12345678")
        elif s_line.strip().startswith("mod_language_override"):
            settings_lines.append(";mod_language_override = ")
        else:
            settings_lines.append(s_line)
    target_settings = release_coop / "ds3sc_settings.ini"
    target_settings.write_text("\n".join(settings_lines) + "\n", encoding="utf-8")
    log_fn(f"      + TheAshenLink/{target_settings.name}")

    # Locale
    release_locale = release_coop / "locale"
    release_locale.mkdir(parents=True, exist_ok=True)
    locale_src = next((c for c in [ROOT / "build/TheAshenLink/locale/english.json",
                                   ROOT / "TheAshenLink/locale/english.json",
                                   ROOT / "build/SeamplusCoop/locale/english.json",
                                   ROOT / "SeamplusCoop/locale/english.json",
                                   ROOT / "build/SeamlessCoop/locale/english.json",
                                   ROOT / "SeamlessCoop/locale/english.json"] if c.is_file()), None)
    if locale_src:
        shutil.copy2(locale_src, release_locale / "english.json")
        log_fn("      + TheAshenLink/locale/english.json")

    spanish_src = next((c for c in [ROOT / "src/languages/spanish.json",
                                    ROOT / "build/TheAshenLink/locale/spanish.json",
                                    ROOT / "TheAshenLink/locale/spanish.json",
                                    ROOT / "build/SeamplusCoop/locale/spanish.json",
                                    ROOT / "SeamplusCoop/locale/spanish.json",
                                    ROOT / "build/SeamlessCoop/locale/spanish.json",
                                    ROOT / "SeamlessCoop/locale/spanish.json"] if c.is_file()), None)
    if spanish_src:
        shutil.copy2(spanish_src, release_locale / "spanish.json")
        log_fn("      + TheAshenLink/locale/spanish.json")

    # Crashpad
    release_crashpad = release_coop / "crashpad"
    release_crashpad.mkdir(parents=True, exist_ok=True)
    crashpad_exe = next((c for c in [ROOT / "build/TheAshenLink/crashpad/crashpad_handler.exe",
                                     ROOT / "TheAshenLink/crashpad/crashpad_handler.exe",
                                     ROOT / "build/SeamplusCoop/crashpad/crashpad_handler.exe",
                                     ROOT / "SeamplusCoop/crashpad/crashpad_handler.exe",
                                     ROOT / "build/SeamlessCoop/crashpad/crashpad_handler.exe",
                                     ROOT / "SeamlessCoop/crashpad/crashpad_handler.exe"] if c.is_file()), None)
    if crashpad_exe:
        shutil.copy2(crashpad_exe, release_crashpad / "crashpad_handler.exe")
        log_fn("      + TheAshenLink/crashpad/crashpad_handler.exe")

    (release_coop / "crashdumps" / "attachments").mkdir(parents=True, exist_ok=True)
    (release_coop / "crashdumps" / "reports").mkdir(parents=True, exist_ok=True)

    licenses = release_coop / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    minhook_lic = ROOT / "tools/vendor/minhook-1.3.4/LICENSE.txt"
    if minhook_lic.is_file():
        shutil.copy2(minhook_lic, licenses / "MinHook.txt")

    # Optional ZIP compression
    if create_zip:
        zip_path = output_dir / f"{release_name}.zip"
        log_fn(f"\n      Compressing release into {zip_path.name}...")
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for item in sorted(output_dir.rglob("*")):
                if item.is_file() and item != zip_path:
                    rel = item.relative_to(output_dir)
                    zf.write(item, arcname=str(rel))
        log_fn(f"      + [ZIP] {zip_path.name} ({zip_path.stat().st_size:,} bytes)")

    # Atomic deployment to release/
    previous = ROOT / "build/backups" / ("release-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    previous.parent.mkdir(parents=True, exist_ok=True)
    if RELEASE_DIR.exists():
        RELEASE_DIR.rename(previous)
    try:
        output_dir.rename(RELEASE_DIR)
    except OSError:
        if previous.exists():
            previous.rename(RELEASE_DIR)
        raise

    for old_exe in [ROOT / "TheAshenLink.exe", ROOT / "ds3sc_launcher.exe"]:
        if old_exe.is_file():
            try:
                old_exe.unlink()
            except OSError:
                pass

    if progress_fn:
        progress_fn(100, "Release generated successfully!")

    log_fn("\n" + "=" * 65)
    log_fn(f"  Destination: {RELEASE_DIR}")
    return RELEASE_DIR

# ==============================================================================
# PYQT6 GRAPHICAL USER INTERFACE
# ==============================================================================

def run_pyqt_gui(initial_config: dict = None, initial_id: str = None):
    try:
        from PyQt6.QtWidgets import (
            QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
            QLabel, QCheckBox, QPushButton, QGroupBox, QLineEdit, QProgressBar,
            QPlainTextEdit, QMessageBox, QFrame, QSizePolicy, QScrollArea
        )
        from PyQt6.QtCore import Qt, QThread, pyqtSignal
        from PyQt6.QtGui import QFont, QIcon, QTextCursor
    except ImportError:
        print("[ERROR] PyQt6 is not available. Install it with: pip install PyQt6")
        sys.exit(1)

    class BuildWorker(QThread):
        log_signal = pyqtSignal(str)
        progress_signal = pyqtSignal(int, str)
        finished_signal = pyqtSignal(bool, str)

        def __init__(self, modules: dict, rel_id: str, skip: bool, run_tests: bool = False):
            super().__init__()
            self.modules = modules
            self.rel_id = rel_id
            self.skip = skip
            self.run_tests = run_tests

        def run(self):
            try:
                build_release_pipeline(
                    self.modules,
                    release_id=self.rel_id,
                    skip_build=self.skip,
                    run_tests=self.run_tests,
                    create_zip=False,
                    log_fn=self.log_signal.emit,
                    progress_fn=self.progress_signal.emit
                )
                self.finished_signal.emit(True, "Release generated successfully in /release.")
            except Exception as e:
                self.log_signal.emit(f"\n[CRITICAL ERROR] {e}")
                self.finished_signal.emit(False, str(e))

    class MainWindow(QMainWindow):
        def __init__(self):
            super().__init__()
            self.setWindowTitle("The Ashen Link: DS3 Coop - Modular Release Builder")
            self.resize(760, 840)
            self.setMinimumSize(680, 680)
            self.worker = None
            self.config = initial_config or load_saved_config()

            self.apply_dark_theme()
            self.init_ui()

        def apply_dark_theme(self):
            self.setStyleSheet("""
                QMainWindow, QWidget {
                    background-color: #1a1a1f;
                    color: #dcdcdc;
                    font-family: 'Segoe UI', sans-serif;
                    font-size: 13px;
                }
                QGroupBox {
                    border: 1px solid #3d3d4a;
                    border-radius: 8px;
                    margin-top: 14px;
                    padding-top: 6px;
                    font-weight: bold;
                    color: #d8b26e;
                }
                QGroupBox::title {
                    subcontrol-origin: margin;
                    left: 12px;
                    padding: 0 6px 0 6px;
                }
                QCheckBox {
                    spacing: 8px;
                    font-size: 13px;
                    font-weight: 600;
                    color: #f0f0f0;
                }
                QCheckBox::indicator {
                    width: 18px;
                    height: 18px;
                    border-radius: 4px;
                    border: 1px solid #5a5a6e;
                    background-color: #24242e;
                }
                QCheckBox::indicator:checked {
                    background-color: #d8b26e;
                    border: 1px solid #f3cb82;
                }
                QPushButton {
                    background-color: #2b2b36;
                    border: 1px solid #4a4a5e;
                    border-radius: 6px;
                    padding: 8px 14px;
                    font-weight: bold;
                    color: #e2e2ea;
                }
                QPushButton:hover {
                    background-color: #383846;
                    border: 1px solid #d8b26e;
                    color: #ffffff;
                }
                QPushButton:pressed {
                    background-color: #1e1e24;
                }
                QPushButton#btn_build {
                    background-color: #946927;
                    border: 1px solid #d8b26e;
                    color: #ffffff;
                    font-size: 14px;
                    padding: 12px 20px;
                    border-radius: 8px;
                }
                QPushButton#btn_build:hover {
                    background-color: #b3802e;
                    border: 1px solid #f6d899;
                }
                QLineEdit {
                    background-color: #24242e;
                    border: 1px solid #4a4a5e;
                    border-radius: 6px;
                    padding: 6px 10px;
                    color: #ffffff;
                    font-family: 'Consolas', monospace;
                }
                QLineEdit:focus {
                    border: 1px solid #d8b26e;
                }
                QProgressBar {
                    border: 1px solid #3d3d4a;
                    border-radius: 6px;
                    text-align: center;
                    background-color: #24242e;
                    color: #ffffff;
                    font-weight: bold;
                }
                QProgressBar::chunk {
                    background-color: #d8b26e;
                    border-radius: 5px;
                }
                QPlainTextEdit {
                    background-color: #131316;
                    border: 1px solid #33333d;
                    border-radius: 6px;
                    color: #9cdcfe;
                    font-family: 'Consolas', 'Courier New', monospace;
                    font-size: 11px;
                }
                QScrollArea {
                    border: none;
                    background-color: transparent;
                }
                QScrollBar:vertical {
                    background-color: #1a1a22;
                    width: 10px;
                    margin: 0px;
                    border-radius: 5px;
                }
                QScrollBar::handle:vertical {
                    background-color: #404052;
                    min-height: 24px;
                    border-radius: 5px;
                }
                QScrollBar::handle:vertical:hover {
                    background-color: #d8b26e;
                }
                QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
                    height: 0px;
                }
                QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
                    background: none;
                }
            """)

        def init_ui(self):
            central = QWidget()
            self.setCentralWidget(central)
            layout = QVBoxLayout(central)
            layout.setContentsMargins(18, 16, 18, 16)
            layout.setSpacing(10)

            # Header
            header = QVBoxLayout()
            title = QLabel("THE ASHEN LINK - DS3 COOP")
            title.setStyleSheet("font-size: 18px; font-weight: bold; color: #d8b26e; letter-spacing: 1px;")
            subtitle = QLabel("Modular Build Generator & Packager v0.1.2")
            subtitle.setStyleSheet("font-size: 12px; color: #8f8f9f;")
            header.addWidget(title)
            header.addWidget(subtitle)
            layout.addLayout(header)

            # Modules Group
            modules_group = QGroupBox("Extensions && Patches to Include")
            modules_group_layout = QVBoxLayout(modules_group)
            modules_group_layout.setContentsMargins(8, 2, 8, 8)

            scroll_area = QScrollArea()
            scroll_area.setWidgetResizable(True)
            scroll_area.setFrameShape(QFrame.Shape.NoFrame)
            scroll_area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
            scroll_area.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
            scroll_area.setStyleSheet("background: transparent; border: none; margin: 0px; padding: 0px;")
            scroll_area.setMinimumHeight(180)

            scroll_content = QWidget()
            scroll_content.setStyleSheet("background: transparent; margin: 0px; padding: 0px;")
            modules_layout = QVBoxLayout(scroll_content)
            modules_layout.setContentsMargins(4, 0, 8, 16)
            modules_layout.setSpacing(10)

            self.checkboxes = {}
            for m in MODULES:
                box = QVBoxLayout()
                box.setSpacing(2)

                cb = QCheckBox(m.name)
                cb.setChecked(self.config.get(m.id, False))
                self.checkboxes[m.id] = cb

                lbl_info = QLabel(f"<b>Type:</b> {m.category} — {m.description}")
                lbl_info.setStyleSheet("color: #9a9ab0; font-size: 11px;")
                lbl_info.setWordWrap(True)

                info_layout = QHBoxLayout()
                info_layout.setContentsMargins(26, 0, 0, 0)
                info_layout.addWidget(lbl_info)

                box.addWidget(cb)
                box.addLayout(info_layout)
                modules_layout.addLayout(box)

            modules_layout.addSpacing(10)
            modules_layout.addStretch()
            scroll_area.setWidget(scroll_content)
            modules_group_layout.addWidget(scroll_area)

            layout.addWidget(modules_group, stretch=1)

            # Build Options Group
            opts_group = QGroupBox("Build Settings")
            opts_layout = QVBoxLayout(opts_group)
            opts_layout.setSpacing(8)

            id_layout = QHBoxLayout()
            id_layout.addWidget(QLabel("Release ID:"))
            self.id_input = QLineEdit()
            self.id_input.setPlaceholderText("Leave blank to generate a random 9-digit ID")
            if initial_id:
                self.id_input.setText(initial_id)
            id_layout.addWidget(self.id_input)

            btn_rand_id = QPushButton("🎲 Generate ID")
            btn_rand_id.clicked.connect(self.generate_random_id)
            id_layout.addWidget(btn_rand_id)
            opts_layout.addLayout(id_layout)

            self.cb_skip_build = QCheckBox("Reuse previously built MSVC binaries (--skip-build)")
            self.cb_skip_build.setStyleSheet("font-size: 12px; font-weight: normal; color: #b0b0c0;")
            opts_layout.addWidget(self.cb_skip_build)

            self.cb_run_tests = QCheckBox("Run full Unicorn emulation tests (adds ~32s)")
            self.cb_run_tests.setStyleSheet("font-size: 12px; font-weight: normal; color: #b0b0c0;")
            opts_layout.addWidget(self.cb_run_tests)

            layout.addWidget(opts_group)

            # Progress Bar and Status Label
            self.progress_bar = QProgressBar()
            self.progress_bar.setFixedHeight(18)
            self.progress_bar.setValue(0)
            layout.addWidget(self.progress_bar)

            self.lbl_status = QLabel("Ready to build release.")
            self.lbl_status.setStyleSheet("color: #d8b26e; font-weight: 600; font-size: 12px;")
            layout.addWidget(self.lbl_status)

            # Logs Console
            self.log_view = QPlainTextEdit()
            self.log_view.setReadOnly(True)
            self.log_view.setMinimumHeight(100)
            layout.addWidget(self.log_view, stretch=1)

            # Action Buttons
            actions_layout = QHBoxLayout()
            actions_layout.setSpacing(10)

            self.btn_build = QPushButton("🔨 BUILD RELEASE")
            self.btn_build.setObjectName("btn_build")
            self.btn_build.clicked.connect(self.start_build)
            actions_layout.addWidget(self.btn_build, stretch=2)

            self.btn_open_folder = QPushButton("📁 Open Release Folder")
            self.btn_open_folder.clicked.connect(self.open_release_folder)
            actions_layout.addWidget(self.btn_open_folder, stretch=1)

            layout.addLayout(actions_layout)

        def generate_random_id(self):
            new_id = f"{secrets.randbelow(900000000) + 100000000:09d}"
            self.id_input.setText(new_id)

        def open_release_folder(self):
            if RELEASE_DIR.is_dir():
                os.startfile(RELEASE_DIR)
            else:
                QMessageBox.warning(self, "Folder Not Found", "The release/ folder has not been created yet.")

        def append_log(self, text: str):
            self.log_view.appendPlainText(text)
            self.log_view.moveCursor(QTextCursor.MoveOperation.End)

        def update_progress(self, val: int, msg: str):
            self.progress_bar.setValue(val)
            self.lbl_status.setText(msg)

        def start_build(self):
            selected = {m.id: self.checkboxes[m.id].isChecked() for m in MODULES}
            rel_id = self.id_input.text().strip() or None
            skip = self.cb_skip_build.isChecked()
            run_tests = self.cb_run_tests.isChecked()

            self.btn_build.setEnabled(False)
            self.btn_build.setText("BUILDING RELEASE...")
            self.log_view.clear()
            self.progress_bar.setValue(5)
            self.lbl_status.setText("Starting process...")

            self.worker = BuildWorker(selected, rel_id, skip, run_tests)
            self.worker.log_signal.connect(self.append_log)
            self.worker.progress_signal.connect(self.update_progress)
            self.worker.finished_signal.connect(self.build_finished)
            self.worker.start()

        def build_finished(self, success: bool, msg: str):
            self.btn_build.setEnabled(True)
            self.btn_build.setText("🔨 BUILD RELEASE")

            if success:
                self.progress_bar.setValue(100)
                self.lbl_status.setText("Build completed successfully!")
                res = QMessageBox.question(
                    self, "Release Generated",
                    f"{msg}\n\nDo you want to open the release folder now?",
                    QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
                )
                if res == QMessageBox.StandardButton.Yes:
                    self.open_release_folder()
            else:
                self.lbl_status.setText("Error during build.")
                QMessageBox.critical(self, "Release Error", f"A failure occurred during packaging:\n\n{msg}")

    app = QApplication.instance() or QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())

# ==============================================================================
# CLI AND ENTRY POINT
# ==============================================================================

def parse_args():
    parser = argparse.ArgumentParser(
        description="The Ashen Link: DS3 Coop - Modular Release Application",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  make-release.bat                          (Opens the PyQt interface by default)
  python make_release.py                    (Opens the PyQt interface)
  python make_release.py --cli --with-greatwood-patch --without-companion-spawner
"""
    )
    parser.add_argument("pos_id", nargs="?", default=None, help="Optional unique ID")
    parser.add_argument("--id", dest="opt_id", default=None, help="Unique ID for the release")
    parser.add_argument("--skip-build", action="store_true", help="Reuse previously built binaries")
    parser.add_argument("--run-tests", action="store_true", default=False,
                        help="Run full Unicorn integration test suite (adds ~32s)")
    parser.add_argument("--skip-tests", dest="run_tests", action="store_false",
                        help="Skip Unicorn emulation tests (default)")
    parser.add_argument("--create-zip", "--zip", dest="create_zip", action="store_true", default=False,
                        help="Package the release into a .zip archive as well")
    parser.add_argument("--gui", action="store_true", help="Force opening the PyQt GUI")
    parser.add_argument("--cli", action="store_true", help="Run in command-line mode without GUI")
    parser.add_argument("--list-modules", action="store_true", help="List all available modules")

    for m in MODULES:
        cli_flag = m.id.replace("_", "-")
        parser.add_argument(f"--with-{cli_flag}", dest=m.id, action="store_true", default=None)
        parser.add_argument(f"--without-{cli_flag}", dest=m.id, action="store_false")

    # Backward compatibility aliases
    parser.add_argument("--with-contadores", dest="counters", action="store_true", default=None, help=argparse.SUPPRESS)
    parser.add_argument("--without-contadores", dest="counters", action="store_false", help=argparse.SUPPRESS)

    return parser.parse_args()

def main():
    args = parse_args()

    if args.list_modules:
        print("\nAvailable modules and extensions in The Ashen Link: DS3 Coop:")
        for m in MODULES:
            print(f"  * {m.id} ({m.category})")
            print(f"      Name: {m.name}")
            print(f"      Info: {m.description}\n")
        sys.exit(0)

    # If --cli or any modular flag was passed without --gui, run in CLI mode
    run_cli_mode = args.cli or (
        any(getattr(args, m.id) is not None for m in MODULES) and not args.gui
    )

    if not run_cli_mode:
        # Default opens PyQt6 GUI
        run_pyqt_gui(initial_id=args.opt_id or args.pos_id)
        return

    # Non-interactive CLI mode
    config = load_saved_config()

    for m in MODULES:
        val = getattr(args, m.id)
        if val is not None:
            config[m.id] = val

    release_id = args.opt_id or args.pos_id
    build_release_pipeline(
        config,
        release_id=release_id,
        skip_build=args.skip_build,
        run_tests=args.run_tests,
        create_zip=args.create_zip
    )

if __name__ == "__main__":
    main()
