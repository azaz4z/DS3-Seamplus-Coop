"""Compile TheAshenLink.exe and modular ds3sc_companion.dll extensions via MSVC.
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
    default_player_outline = cfg.get("player_outline", False)
    default_markers = cfg.get("ally_markers", True)
    default_companion = cfg.get("companion_spawner", True)
    default_hit_sync = cfg.get("hit_sync", True)
    default_counters = cfg.get("counters", True)
    default_fps_unlock = cfg.get("fps_unlock", True)
    default_anim_fix = cfg.get("anim_fix", True)
    default_cutscene_fix = cfg.get("cutscene_fix", True)
    default_lan_coop = cfg.get("lan_coop", True)
    default_spectator_fix = cfg.get("spectator_fix", True)
    default_verbose_connections = cfg.get("verbose_connections", True)

    parser = argparse.ArgumentParser(description="The Ashen Link: DS3 Coop Binary Builder")
    parser.add_argument("--with-ally-outline", dest="ally_outline", action="store_true", default=default_outline,
                        help="Include D3D11 ally outline extension")
    parser.add_argument("--without-ally-outline", dest="ally_outline", action="store_false",
                        help="Exclude D3D11 ally outline extension")
    parser.add_argument("--with-player-outline", dest="player_outline", action="store_true", default=default_player_outline,
                        help="Include local player outline and silhouette diagnostics")
    parser.add_argument("--without-player-outline", dest="player_outline", action="store_false",
                        help="Exclude local player outline and silhouette diagnostics")
    parser.add_argument("--with-companion", dest="companion", action="store_true", default=default_companion,
                        help="Include companion spawner extension (ash stone)")
    parser.add_argument("--without-companion", dest="companion", action="store_false",
                        help="Exclude companion spawner extension")
    parser.add_argument("--with-hit-sync", dest="hit_sync", action="store_true", default=default_hit_sync,
                        help="Include hit registration & damage synchronization extension")
    parser.add_argument("--without-hit-sync", dest="hit_sync", action="store_false",
                        help="Exclude hit registration & damage synchronization extension")
    parser.add_argument("--with-counters", "--with-contadores", "--with-combat-stats", dest="counters", action="store_true", default=default_counters,
                        help="Include stat counters module (deaths, kills, and backstabs)")
    parser.add_argument("--without-counters", "--without-contadores", "--without-combat-stats", dest="counters", action="store_false",
                        help="Exclude stat counters module")
    parser.add_argument("--with-ally-markers", "--with-markers", dest="ally_markers", action="store_true", default=default_markers,
                        help="Include ally diamond position markers")
    parser.add_argument("--without-ally-markers", "--without-markers", dest="ally_markers", action="store_false",
                        help="Exclude ally diamond position markers")
    parser.add_argument("--with-fps-unlock", "--with-fps", dest="fps_unlock", action="store_true", default=default_fps_unlock,
                        help="Include 60 FPS uncap / framerate unlocker extension")
    parser.add_argument("--without-fps-unlock", "--without-fps", dest="fps_unlock", action="store_false",
                        help="Exclude 60 FPS uncap / framerate unlocker extension")
    parser.add_argument("--with-anim-fix", "--with-fix-anim", dest="anim_fix", action="store_true", default=default_anim_fix,
                        help="Include animation and locomotion repair module")
    parser.add_argument("--without-anim-fix", "--without-fix-anim", dest="anim_fix", action="store_false",
                        help="Exclude animation and locomotion repair module")
    parser.add_argument("--with-cutscene-fix", "--with-cutscenes", dest="cutscene_fix", action="store_true", default=default_cutscene_fix,
                        help="Include cutscene ally isolation module")
    parser.add_argument("--without-cutscene-fix", "--without-cutscenes", dest="cutscene_fix", action="store_false",
                        help="Exclude cutscene ally isolation module")
    parser.add_argument("--with-lan-coop", "--with-lan", dest="lan_coop", action="store_true", default=default_lan_coop,
                        help="Include LAN Co-op transport and Steamworks interception module")
    parser.add_argument("--without-lan-coop", "--without-lan", dest="lan_coop", action="store_false",
                        help="Exclude LAN Co-op transport module")
    parser.add_argument("--with-spectator-fix", "--with-spectator", dest="spectator_fix", action="store_true", default=default_spectator_fix,
                        help="Include spectator stamina bar and HUD fix module")
    parser.add_argument("--without-spectator-fix", "--without-spectator", dest="spectator_fix", action="store_false",
                        help="Exclude spectator stamina bar and HUD fix module")
    parser.add_argument("--with-verbose-connections", "--with-verbose", dest="verbose_connections", action="store_true", default=default_verbose_connections,
                        help="Include verbose connection notifications module")
    parser.add_argument("--without-verbose-connections", "--without-verbose", dest="verbose_connections", action="store_false",
                        help="Exclude verbose connection notifications module")


    parser.add_argument("--no-companion", dest="legacy_no_companion", action="store_true",
                        help="Alias to exclude companion spawner")
    parser.add_argument("--skip-launcher", action="store_true", help="Skip compilation of TheAshenLink.exe")
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
    # `set` can contain both PATH and Path on the host.  Treat environment
    # names case-insensitively; otherwise the later stale `Path` entry can
    # overwrite the toolchain PATH and make cl.exe appear to be missing.
    msvc_env = {}
    for line in raw.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            msvc_env.setdefault(k.upper(), v)
    for k, v in msvc_env.items():
        os.environ[k] = v
    if "PATH" in msvc_env:
        os.environ["PATH"] = msvc_env["PATH"]
        os.environ["Path"] = msvc_env["PATH"]

    cl_exe = shutil.which("cl.exe")
    if not cl_exe:
        raise RuntimeError("Could not locate cl.exe after initializing vcvars64.bat.")
    return cl_exe

def build_extensions(cl_exe: str, enable_outline: bool, enable_player_outline: bool = False, enable_markers: bool = True, enable_companion: bool = False, enable_hit_sync: bool = True, enable_counters: bool = True, enable_fps_unlock: bool = True, enable_anim_fix: bool = True, enable_cutscene_fix: bool = True, enable_lan_coop: bool = True, enable_spectator_fix: bool = True, enable_verbose_connections: bool = True, enable_contadores: bool = None, skip_install: bool = False):
    if enable_contadores is not None:
        enable_counters = enable_contadores

    comp_out = ROOT / "build/companion"
    comp_out.mkdir(parents=True, exist_ok=True)

    if not enable_outline and not enable_player_outline and not enable_markers and not enable_companion and not enable_hit_sync and not enable_counters and not enable_fps_unlock and not enable_anim_fix and not enable_cutscene_fix and not enable_lan_coop and not enable_spectator_fix and not enable_verbose_connections:
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
        ROOT / "src/network/lan_transport.cpp",
    ]
    libs = ["Kernel32.lib", "User32.lib", "Gdi32.lib", "Ws2_32.lib", "Advapi32.lib"]

    needs_d3d11 = enable_outline or enable_player_outline or enable_markers or enable_counters or enable_companion or enable_fps_unlock or enable_anim_fix or enable_cutscene_fix
    needs_actor_tracker = enable_outline or enable_player_outline or enable_markers or enable_counters or enable_companion or enable_anim_fix or enable_cutscene_fix
    needs_minhook = needs_d3d11 or enable_lan_coop or enable_verbose_connections


    if needs_d3d11:
        defines.append("/DDS3SC_HAS_D3D11_HOOK=1")
        sources.append(ROOT / "src/render/d3d11_hook.cpp")
        sources.append(ROOT / "src/render/title_menu.cpp")
        libs.extend(["d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "dinput8.lib", "dxguid.lib"])

    if needs_minhook:
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

    if enable_outline or enable_player_outline:
        defines.append("/DDS3SC_FEATURE_PLAYER_OUTLINE=1" if enable_player_outline else "/DDS3SC_FEATURE_PLAYER_OUTLINE=0")
    else:
        defines.append("/DDS3SC_FEATURE_PLAYER_OUTLINE=0")

    if enable_outline:
        defines.append("/DDS3SC_FEATURE_ALLY_OUTLINE=1")
        sources.append(ROOT / "src/extensions/ally_outline/ally_outline_extension.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_ALLY_OUTLINE=0")

    if enable_outline or enable_player_outline:
        sources.append(ROOT / "src/render/ally_outline.cpp")

    if enable_player_outline:
        sources.append(ROOT / "src/extensions/player_outline/player_outline_extension.cpp")

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

    if enable_fps_unlock:
        defines.append("/DDS3SC_FEATURE_FPS_UNLOCK=1")
        sources.append(ROOT / "src/extensions/fps_unlock/fps_unlock_extension.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_FPS_UNLOCK=0")

    if enable_anim_fix:
        defines.append("/DDS3SC_FEATURE_ANIM_FIX=1")
        sources.append(ROOT / "src/extensions/anim_fix/anim_fix_extension.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_ANIM_FIX=0")

    if enable_cutscene_fix:
        defines.append("/DDS3SC_FEATURE_CUTSCENE_FIX=1")
        sources.append(ROOT / "src/extensions/cutscene_fix/cutscene_fix_extension.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_CUTSCENE_FIX=0")

    if enable_lan_coop:
        defines.append("/DDS3SC_FEATURE_LAN_COOP=1")
        sources.append(ROOT / "src/extensions/lan_coop/lan_coop_extension.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_LAN_COOP=0")

    if enable_spectator_fix:
        defines.append("/DDS3SC_FEATURE_SPECTATOR_FIX=1")
        sources.append(ROOT / "src/extensions/spectator_fix/spectator_fix_extension.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_SPECTATOR_FIX=0")

    if enable_verbose_connections:
        defines.append("/DDS3SC_FEATURE_VERBOSE_CONNECTIONS=1")
        sources.append(ROOT / "src/extensions/verbose_connections/verbose_connections_extension.cpp")
    else:
        defines.append("/DDS3SC_FEATURE_VERBOSE_CONNECTIONS=0")

    features_str = []
    if enable_outline: features_str.append("Ally Outline")
    if enable_player_outline: features_str.append("Player Outline & Silhouette")
    if enable_markers: features_str.append("Ally Markers")
    if enable_companion: features_str.append("Companion Spawner")
    if enable_hit_sync: features_str.append("Hit Sync")
    if enable_counters: features_str.append("Stat Counters")
    if enable_fps_unlock: features_str.append("FPS Unlocker")
    if enable_anim_fix: features_str.append("Anim Fix")
    if enable_cutscene_fix: features_str.append("Cutscene Ally Isolation")
    if enable_lan_coop: features_str.append("LAN Co-op Transport & Matchmaking")
    if enable_spectator_fix: features_str.append("Spectator Stamina Fix")
    if enable_verbose_connections: features_str.append("Verbose Connections")
    print(f"[Modular Build] Compiling ds3sc_companion.dll with modules: [{', '.join(features_str)}]...")

    cmd_comp = [
        cl_exe, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/O2", "/utf-8",
        "/permissive-", "/MT", "/Zi", "/FS", "/LD",
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
    for target_dir in [ROOT / "build/TheAshenLink", ROOT / "TheAshenLink", ROOT / "build/SeamplusCoop", ROOT / "SeamplusCoop", ROOT / "build/SeamlessCoop", ROOT / "SeamlessCoop"]:
        if target_dir.is_dir():
            target_coop = target_dir / "ds3sc_companion.dll"
            shutil.copy2(comp_dll, target_coop)
            print(f"      [OK] Copied to {target_coop}")

    game_dirs = [
        Path(r"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/TheAshenLink"),
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

def build_launcher(cl_exe: str, skip_install: bool = False):
    bin_out = ROOT / "build/bin"
    bin_out.mkdir(parents=True, exist_ok=True)
    launcher_src = ROOT / "src/launcher/main.cpp"
    launcher_exe = bin_out / "TheAshenLink.exe"

    if launcher_exe.is_file() and launcher_exe.stat().st_mtime >= launcher_src.stat().st_mtime:
        print(f"      [Cache] TheAshenLink.exe up to date ({launcher_exe.stat().st_size:,} bytes, main.cpp unchanged).")
    else:
        print("Compiling native TheAshenLink.exe with MSVC...")
        cmd_launch = [
            cl_exe, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/O2", "/utf-8",
            "/permissive-", "/DUNICODE", "/D_UNICODE",
            str(launcher_src),
            "/Fe:TheAshenLink.exe",
            "/link", "Kernel32.lib", "Psapi.lib", "Advapi32.lib", "User32.lib"
        ]
        res = subprocess.run(cmd_launch, cwd=bin_out, capture_output=True, text=True)
        if res.returncode != 0:
            print("STDERR:\n", res.stderr)
            print("STDOUT:\n", res.stdout)
            raise RuntimeError("Failed to compile TheAshenLink.exe")
        print(f"      [OK] TheAshenLink.exe compiled ({launcher_exe.stat().st_size:,} bytes)")

    # Clean any residual launcher in root
    for target_name in ["TheAshenLink.exe", "ds3sc_launcher.exe"]:
        target_launcher = ROOT / target_name
        if target_launcher.is_file():
            try:
                target_launcher.unlink()
            except OSError:
                pass

    # Copy to game installation if available
    game_exe_dir = Path(r"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game")
    if not skip_install and (game_exe_dir / "DarkSoulsIII.exe").is_file():
        target_game_launcher = game_exe_dir / "TheAshenLink.exe"
        try:
            shutil.copy2(launcher_exe, target_game_launcher)
            print(f"      [OK] Copied launcher to game installation: {target_game_launcher}")
        except Exception as ex:
            print(f"      [WARNING] Could not copy launcher to game installation: {ex}")

def main():
    args = parse_args()
    cl_exe = setup_msvc_environment()

    # Compile modular extensions
    build_extensions(cl_exe, enable_outline=args.ally_outline, enable_player_outline=args.player_outline, enable_markers=args.ally_markers, enable_companion=args.companion, enable_hit_sync=args.hit_sync, enable_counters=args.counters, enable_fps_unlock=args.fps_unlock, enable_anim_fix=args.anim_fix, enable_cutscene_fix=args.cutscene_fix, enable_lan_coop=args.lan_coop, enable_spectator_fix=args.spectator_fix, enable_verbose_connections=args.verbose_connections, skip_install=args.skip_install)

    # Sync locale files in locale directories
    locale_srcs = [
        ROOT / "src/languages/english.json",
        ROOT / "build/TheAshenLink/locale/english.json",
        ROOT / "TheAshenLink/locale/english.json",
        ROOT / "build/SeamplusCoop/locale/english.json",
        ROOT / "SeamplusCoop/locale/english.json",
        ROOT / "build/SeamlessCoop/locale/english.json",
        ROOT / "SeamlessCoop/locale/english.json",
    ]
    english_src = next((f for f in locale_srcs if f.is_file()), None)

    spanish_src = ROOT / "src/languages/spanish.json"

    target_dirs = [
        ROOT / "build/TheAshenLink", ROOT / "TheAshenLink",
        ROOT / "build/SeamplusCoop", ROOT / "SeamplusCoop",
        ROOT / "build/SeamlessCoop", ROOT / "SeamlessCoop"
    ]
    if not args.skip_install:
        target_dirs.extend([
            Path(r"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/TheAshenLink"),
            Path(r"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/SeamplusCoop"),
            Path(r"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/SeamlessCoop"),
        ])

    for target_dir in target_dirs:
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
        build_launcher(cl_exe, skip_install=args.skip_install)

    print("[OK] Modular build completed successfully.")

if __name__ == "__main__":
    main()

