import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def main():
    vswhere = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if not os.path.isfile(vswhere):
        raise RuntimeError("vswhere.exe not found.")
    vs_path = subprocess.check_output(
        [vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        text=True
    ).strip()
    vcvars = os.path.join(vs_path, r"VC\Auxiliary\Build\vcvars64.bat")

    out_dir = ROOT / "build/outline"
    out_dir.mkdir(parents=True, exist_ok=True)
    test_src = ROOT / "tests/ally_outline_test.cpp"
    outline_src = ROOT / "src/render/ally_outline.cpp"
    actor_tracker_src = ROOT / "src/render/actor_tracker.cpp"

    print("Compiling ally_outline_test.exe...")
    cmd = (
        f'"{vcvars}" && cd /d "{out_dir}" && '
        f'cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /permissive- /MT '
        f'"{test_src}" "{outline_src}" "{actor_tracker_src}" /Fe:ally_outline_test.exe '
        f'/link d3d11.lib dxgi.lib d3dcompiler.lib User32.lib'
    )
    res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if res.returncode != 0:
        print("STDERR:\n", res.stderr)
        print("STDOUT:\n", res.stdout)
        sys.exit(1)
    print("ally_outline_test.exe compiled successfully.")

    subprocess.run([str(out_dir / "ally_outline_test.exe")], cwd=out_dir, check=True)

    # 2. Compile renderer test
    renderer_test_src = ROOT / "tests/ally_outline_renderer_test.cpp"
    print("Compiling ally_outline_renderer_test.exe...")
    cmd_renderer = (
        f'"{vcvars}" && cd /d "{out_dir}" && '
        f'cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /permissive- /MT '
        f'"{renderer_test_src}" "{outline_src}" "{actor_tracker_src}" /Fe:ally_outline_renderer_test.exe '
        f'/link d3d11.lib dxgi.lib d3dcompiler.lib User32.lib'
    )
    res_rend = subprocess.run(cmd_renderer, shell=True, capture_output=True, text=True)
    if res_rend.returncode != 0:
        print("STDERR:\n", res_rend.stderr)
        print("STDOUT:\n", res_rend.stdout)
        sys.exit(1)
    print("ally_outline_renderer_test.exe compiled successfully.")

    # Run renderer test
    print("Running ally_outline_renderer_test.exe...")
    res_run = subprocess.run(str(out_dir / "ally_outline_renderer_test.exe"), cwd=out_dir, capture_output=True, text=True)
    print(res_run.stdout)
    if res_run.returncode != 0:
        print(res_run.stderr)
        sys.exit(1)

    live_cmd = (f'"{vcvars}" && cd /d "{out_dir}" && '
                f'cl /nologo /std:c++20 /O2 /EHsc /W4 /WX /MT /utf-8 '
                f'"{ROOT / "tests/live_outline_test.cpp"}" "{outline_src}" "{actor_tracker_src}" '
                '/Fe:live_outline_test.exe /link d3d11.lib dxgi.lib d3dcompiler.lib User32.lib')
    subprocess.run(live_cmd, shell=True, check=True)
    subprocess.run([str(out_dir / "live_outline_test.exe")], cwd=out_dir, timeout=60, check=True)

    manager_cmd = (f'"{vcvars}" && cd /d "{out_dir}" && '
                   f'cl /nologo /std:c++20 /O2 /EHsc /W4 /WX /MT /utf-8 '
                   f'"{ROOT / "tests/extension_manager_test.cpp"}" '
                   f'"{ROOT / "src/extensions/extension_manager.cpp"}" /Fe:extension_manager_test.exe')
    subprocess.run(manager_cmd, shell=True, check=True)
    subprocess.run([str(out_dir / "extension_manager_test.exe")], cwd=out_dir, check=True)

    # Regression coverage for live function hooks (shared COM vtables stay intact).
    hook_files = [ROOT / "tests/d3d11_hook_test.cpp"] + [
        ROOT / "src/render" / name for name in
        ["d3d11_hook.cpp", "ally_outline.cpp", "actor_tracker.cpp"]] + [
        ROOT / "build/companion" / name for name in
        ["buffer.obj", "hook.obj", "trampoline.obj", "hde64.obj"]]
    hook_args = " ".join(f'"{path}"' for path in hook_files)
    hook_cmd = (f'"{vcvars}" && cd /d "{out_dir}" && '
                f'cl /nologo /std:c++20 /O2 /EHsc /W4 /WX /MT /utf-8 {hook_args} '
                '/Fe:d3d11_hook_test.exe /link d3d11.lib dxgi.lib d3dcompiler.lib User32.lib')
    subprocess.run(hook_cmd, shell=True, check=True)
    subprocess.run([str(out_dir / "d3d11_hook_test.exe")], timeout=30, check=True)

if __name__ == "__main__":
    main()

