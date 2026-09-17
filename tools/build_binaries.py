"""Compile ds3sc_launcher.exe and modular ds3sc_companion.dll extensions via MSVC.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGE_ROOT = ROOT.parent

def parse_args():
    config_path = ROOT / "build_config.json"
    cfg = {}
    if config_path.is_file():
        try:
            with open(config_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
        except Exception as e:
            print(f"[Build Warning] Could not parse build_config.json: {e}")

    default_outline = cfg.get("ally_outline", True)
    default_markers = cfg.get("ally_markers", True)
    default_companion = cfg.get("companion_spawner", True)
    default_hit_sync = cfg.get("hit_sync", True)
    default_counters = cfg.get("counters", True)

    parser = argparse.ArgumentParser(description="DS3 Seamless Co-op Binary Builder")
    parser.add_argument("--with-ally-outline", dest="ally_outline", action="store_true", default=default_outline,
                        help="Include D3D11 ally outline extension")
    parser.add_argument("--without-ally-outline", dest="ally_outline", action="store_false",
                        help="Exclude D3D11 ally outline extension")
    parser.add_argument("--with-companion", dest="companion", action="store_true", default=default_companion,
                        help="Include companion spawner extension (ash stone)")
    parser.add_argument("--without-companion", dest="companion", action="store_false",
                        help="Exclude companion spawner extension")
    parser.add_argument("--with-hit-sync", dest="hit_sync", action="store_true", default=default_hit_sync,
                        help="Include hit synchronization and damage registration extension")
    parser.add_argument("--without-hit-sync", dest="hit_sync", action="store_false",
                        help="Exclude hit synchronization extension")
    parser.add_argument("--with-counters", "--with-contadores", "--with-combat-stats", dest="counters", action="store_true", default=default_counters,
                        help="Include stat counters module (deaths, kills, and backstabs)")
    parser.add_argument("--without-counters", "--without-contadores", "--without-combat-stats", dest="counters", action="store_false",
                        help="Exclude stat counters module")
    parser.add_argument("--with-ally-markers", "--with-markers", dest="ally_markers", action="store_true", default=default_markers,
                        help="Include ally diamond position markers")
    parser.add_argument("--without-ally-markers", "--without-markers", dest="ally_markers", action="store_false",
                        help="Exclude ally diamond position markers")

    parser.add_argument("--no-companion", dest="legacy_no_companion", action="store_true",
                        help="Alias to exclude companion spawner")
    parser.add_argument("--skip-launcher", action="store_true", help="Skip compilation of ds3sc_launcher.exe")
    parser.add_argument("--skip-install", action="store_true", help="Compile without modifying game installation")
    args, unknown = parser.parse_known_args()
    if args.legacy_no_companion:
        args.companion = False
    return args

def get_msvc_environment():
    vswhere = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if not os.path.isfile(vswhere):
        raise RuntimeError("vswhere.exe not found.")
    vs_path = subprocess.check_output(
        [vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        text=True
    ).strip()
    vcvars = os.path.join(vs_path, r"VC\Auxiliary\Build\vcvars64.bat")
    if not os.path.isfile(vcvars):
        raise RuntimeError("vcvars64.bat not found.")
    return vcvars

def setup_msvc_environment() -> str:
    """Configure MSVC environment once into os.environ and return path to cl.exe."""
    cl_exe = shutil.which("cl.exe")
    if cl_exe:
        return cl_exe

    vcvars = get_msvc_environment()
    raw = subprocess.check_output(f'call "{vcvars}" > nul && set', shell=True, text=True, errors="replace")
    for line in raw.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            os.environ[k] = v

    cl_exe = shutil.which("cl.exe")
    if not cl_exe:
        raise RuntimeError("Could not locate cl.exe after initializing vcvars64.bat.")
    return cl_exe

def build_extensions(cl_exe: str, enable_outline: bool, enable_markers: bool = True, enable_companion: bool = False, enable_hit_sync: bool = True, enable_counters: bool = True, enable_contadores: bool = None, skip_install: bool = False):
    if enable_contadores is not None:
        enable_counters = enable_contadores

    comp_out = ROOT / "build/companion"
    comp_out.mkdir(parents=True, exist_ok=True)

    if not enable_outline and not enable_markers and not enable_companion and not enable_hit_sync and not enable_counters:
        print("[Modular Build] No native extensions enabled. Skipping ds3sc_companion.dll.")
        # Clean previous DLL if it existed to avoid packaging by mistake
        stale_dll = comp_out / "ds3sc_companion.dll"
        if stale_dll.is_file():
            stale_dll.unlink()
        return

    defines = []
    sources = [
        ROOT / "src/extensions/extension_manager.cpp",
        ROOT / "src/extensions/extension_dll_main.cpp",
    ]
    libs = ["Kernel32.lib", "User32.lib", "Gdi32.lib"]

    needs_d3d11 = enable_outline or enable_markers or enable_counters or enable_companion
    needs_actor_tracker = enable_outline or enable_markers or needs_d3d11

    if needs_d3d11:
        sources.append(ROOT / "src/render/d3d11_hook.cpp")
        sources.append(ROOT / "src/render/title_menu.cpp")
        libs.extend(["d3d11.lib", "dxgi.lib", "d3dcompiler.lib"])

        # MinHook cache: only compile if .obj files are missing or older than sources
        minhook = ROOT / "tools/vendor/minhook-1.3.4"
        mh_sources = [minhook / "src" / name for name in
                      ["buffer.c", "hook.c", "trampoline.c", "hde/hde64.c"]]
        mh_objs = [comp_out / name for name in ["buffer.obj", "hook.obj", "trampoline.obj", "hde64.obj"]]

        need_rebuild_minhook = False
        for obj in mh_objs:
            if not obj.is_file():
                need_rebuild_minhook = True
                break
        if not need_rebuild_minhook:
            min_obj_mtime = min(obj.stat().st_mtime for obj in mh_objs)
            max_src_mtime = max(src.stat().st_mtime for src in mh_sources)
            if min_obj_mtime < max_src_mtime:
                need_rebuild_minhook = True

        if need_rebuild_minhook:
            print("      [MSVC] Compiling static MinHook library...")
            mh_cmd = [cl_exe, "/nologo", "/O2", "/MT", "/c", *[str(s) for s in mh_sources]]
            res_mh = subprocess.run(mh_cmd, cwd=comp_out, capture_output=True, text=True)
            if res_mh.returncode != 0:
                print("STDERR MinHook:\n", res_mh.stderr)
                raise RuntimeError("Failed to compile MinHook")
        else:
            print("      [Cache] MinHook already compiled, reusing .obj objects...")

        sources.extend(mh_objs)

    if needs_actor_tracker:
        sources.append(ROOT / "src/render/actor_tracker.cpp")

    if enable_outline:
        defines.append("/DDS3SC_FEATURE_ALLY_OUTLINE=1")
        sources.append(ROOT / "src/extensions/ally_outline/ally_outline_extension.cpp")
        sources.append(ROOT / "src/render/ally_outline.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_ALLY_OUTLINE=0")

    if enable_markers:
        defines.append("/DDS3SC_FEATURE_ALLY_MARKERS=1")
        sources.append(ROOT / "src/extensions/ally_markers/ally_markers_extension.cpp")
        sources.append(ROOT / "src/render/ally_marker.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_ALLY_MARKERS=0")

    if enable_companion:
        defines.append("/DDS3SC_FEATURE_COMPANION_SPAWNER=1")
        sources.append(ROOT / "src/extensions/companion_spawner/companion_spawner_extension.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_COMPANION_SPAWNER=0")

    if enable_hit_sync:
        defines.append("/DDS3SC_FEATURE_HIT_SYNC=1")
        sources.append(ROOT / "src/extensions/hit_sync/hit_sync_extension.cpp")
        sources.append(ROOT / "src/coop/hit_sync.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_HIT_SYNC=0")

    if enable_counters:
        defines.append("/DDS3SC_FEATURE_COUNTERS=1")
        defines.append("/DDS3SC_FEATURE_CONTADORES=1")
        defines.append("/DDS3SC_FEATURE_COMBAT_STATS=1")
        sources.append(ROOT / "src/extensions/counters/counters_extension.cpp")
        sources.append(ROOT / "src/render/stats_overlay.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_COUNTERS=0")
        defines.append("/DDS3SC_FEATURE_CONTADORES=0")
        defines.append("/DDS3SC_FEATURE_COMBAT_STATS=0")

    features_str = []
    if enable_outline: features_str.append("Ally Outline")
    if enable_markers: features_str.append("Ally Markers")
    if enable_companion: features_str.append("Companion Spawner")
    if enable_hit_sync: features_str.append("Hit Sync")
    if enable_counters: features_str.append("Stat Counters")
    print(f"[Modular Build] Compiling ds3sc_companion.dll with modules: [{', '.join(features_str)}]...")

    cmd_comp = [
        cl_exe, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/O2", "/utf-8",
        "/permissive-", "/MT", "/Zi", "/LD",
        *defines,
        *[str(s) for s in sources],
        "/Fe:ds3sc_companion.dll",
        "/link", "/DEBUG", "/MAP",
        *libs
    ]
    res = subprocess.run(cmd_comp, cwd=comp_out, capture_output=True, text=True)
    if res.returncode != 0:
        print("STDERR:\n", res.stderr)
        print("STDOUT:\n", res.stdout)
        raise RuntimeError("Failed to compile ds3sc_companion.dll")

    comp_dll = comp_out / "ds3sc_companion.dll"
    print(f"      [OK] ds3sc_companion.dll compiled ({comp_dll.stat().st_size:,} bytes)")
    for target_dir in [ROOT / "build/SeamplusCoop", ROOT / "SeamplusCoop", ROOT / "build/SeamlessCoop", ROOT / "SeamlessCoop"]:
        if target_dir.is_dir():
            target_coop = target_dir / "ds3sc_companion.dll"
            shutil.copy2(comp_dll, target_coop)
            print(f"      [OK] Copied to {target_coop}")

    game_dirs = [
        Path(r"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/SeamplusCoop"),
        Path(r"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/SeamlessCoop"),
    ]
    for game_coop_dir in game_dirs:
        if not skip_install and game_coop_dir.is_dir():
            target_dll = game_coop_dir / "ds3sc_companion.dll"
            try:
                shutil.copy2(comp_dll, target_dll)
                print(f"      [OK] Copied to game installation: {target_dll}")
            except PermissionError:
                try:
                    old_dll = game_coop_dir / "ds3sc_companion.dll.old"
                    if old_dll.is_file():
                        try: old_dll.unlink()
                        except Exception: pass
                    target_dll.rename(old_dll)
                    shutil.copy2(comp_dll, target_dll)
                    print(f"      [OK] In-use DLL renamed to .old and new DLL copied: {target_dll}")
                except Exception as ex:
                    print(f"      [WARNING] Could not copy to {target_dll}: {ex}")

def build_launcher(cl_exe: str):
    bin_out = ROOT / "build/bin"
    bin_out.mkdir(parents=True, exist_ok=True)
    launcher_src = ROOT / "src/launcher/main.cpp"
    launcher_exe = bin_out / "ds3sc_launcher.exe"

    if launcher_exe.is_file() and launcher_exe.stat().st_mtime >= launcher_src.stat().st_mtime:
        print(f"      [Cache] ds3sc_launcher.exe up to date ({launcher_exe.stat().st_size:,} bytes, main.cpp unchanged).")
        return

    print("Compiling native ds3sc_launcher.exe with MSVC...")
    cmd_launch = [
        cl_exe, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/O2", "/utf-8",
        "/permissive-", "/DUNICODE", "/D_UNICODE",
        str(launcher_src),
        "/Fe:ds3sc_launcher.exe",
        "/link", "Kernel32.lib", "Psapi.lib", "Advapi32.lib", "User32.lib"
    ]
    res = subprocess.run(cmd_launch, cwd=bin_out, capture_output=True, text=True)
    if res.returncode != 0:
        print("STDERR:\n", res.stderr)
        print("STDOUT:\n", res.stdout)
        raise RuntimeError("Failed to compile ds3sc_launcher.exe")
    print(f"      [OK] ds3sc_launcher.exe compiled ({launcher_exe.stat().st_size:,} bytes)")

    # Clean any residual launcher in root
    target_launcher = ROOT / "ds3sc_launcher.exe"
    if target_launcher.is_file():
        try:
            target_launcher.unlink()
        except OSError:
            pass

def main():
    args = parse_args()
    cl_exe = setup_msvc_environment()

    # Compile modular extensions
    build_extensions(cl_exe, enable_outline=args.ally_outline, enable_markers=args.ally_markers, enable_companion=args.companion, enable_hit_sync=args.hit_sync, enable_counters=args.counters, skip_install=args.skip_install)

    # Sync locale files in locale directories
    locale_srcs = [
        ROOT / "src/languages/english.json",
        ROOT / "build/SeamplusCoop/locale/english.json",
        ROOT / "SeamplusCoop/locale/english.json",
        ROOT / "build/SeamlessCoop/locale/english.json",
        ROOT / "SeamlessCoop/locale/english.json",
    ]
    english_src = next((f for f in locale_srcs if f.is_file()), None)

    spanish_src = ROOT / "src/languages/spanish.json"

    for target_dir in [ROOT / "build/SeamplusCoop", ROOT / "SeamplusCoop", ROOT / "build/SeamlessCoop", ROOT / "SeamlessCoop"]:
        if target_dir.is_dir():
            target_loc = target_dir / "locale"
            target_loc.mkdir(parents=True, exist_ok=True)
            if english_src and english_src.is_file():
                dest_eng = target_loc / "english.json"
                if english_src.resolve() != dest_eng.resolve():
                    shutil.copy2(english_src, dest_eng)
            if spanish_src.is_file():
                dest_spa = target_loc / "spanish.json"
                if spanish_src.resolve() != dest_spa.resolve():
                    shutil.copy2(spanish_src, dest_spa)

    # Compile launcher unless skipped
    if not args.skip_launcher:
        build_launcher(cl_exe)

    print("[OK] Modular build completed successfully.")

if __name__ == "__main__":
    main()

