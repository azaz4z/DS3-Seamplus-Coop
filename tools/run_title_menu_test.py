"""Compile the real menu/extension settings regression without touching DS3."""
from pathlib import Path
import subprocess
from build_binaries import setup_msvc_environment

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = setup_msvc_environment()
    output = ROOT / "build/menu-tests"
    output.mkdir(parents=True, exist_ok=True)
    sources = [
        "tests/title_menu_test.cpp", "src/render/title_menu.cpp",
        "src/extensions/ally_outline/ally_outline_extension.cpp",
        "src/extensions/player_outline/player_outline_extension.cpp",
        "src/extensions/ally_markers/ally_markers_extension.cpp",
    ]
    minhook = ROOT / "tools/vendor/minhook-1.3.4/src"
    subprocess.run([compiler, "/nologo", "/O2", "/MT", "/c",
                    *[str(minhook / name) for name in ("buffer.c", "hook.c", "trampoline.c", "hde/hde64.c")]],
                   cwd=output, check=True)
    command = [compiler, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/O2", "/MT", "/utf-8",
               "/DDS3SC_STANDALONE_TEST", "/DDS3SC_FEATURE_ALLY_OUTLINE=1",
               "/DDS3SC_FEATURE_PLAYER_OUTLINE=1", "/DDS3SC_FEATURE_ALLY_MARKERS=1",
               *[str(ROOT / source) for source in sources],
               "buffer.obj", "hook.obj", "trampoline.obj", "hde64.obj", "/Fe:title_menu_test.exe",
               "/link", "d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "User32.lib", "Gdi32.lib",
               "dinput8.lib", "dxguid.lib"]
    subprocess.run(command, cwd=output, check=True)
    subprocess.run([str(output / "title_menu_test.exe")], cwd=output, timeout=30, check=True)


if __name__ == "__main__":
    main()
