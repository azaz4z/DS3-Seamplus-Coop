import os
import subprocess
import sys
from pathlib import Path

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

    out_dir = ROOT / "build/tests"
    out_dir.mkdir(parents=True, exist_ok=True)
    tests = [
        ("test_counters.exe", ROOT / "tests/test_counters.cpp"),
        ("test_contadores.exe", ROOT / "tests/test_contadores.cpp"),
        ("test_combat_stats.exe", ROOT / "tests/test_combat_stats.cpp")
    ]
    stats_src = ROOT / "src/extensions/counters/counters_extension.cpp"

    for exe_name, test_src in tests:
        print(f"Compiling {exe_name}...")
        cmd = (
            f'"{vcvars}" && cd /d "{out_dir}" && '
            f'cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /permissive- /MT '
            f'"{test_src}" "{stats_src}" /Fe:{exe_name} '
            f'/link User32.lib'
        )
        res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        if res.returncode != 0:
            print("STDERR:\n", res.stderr)
            print("STDOUT:\n", res.stdout)
            sys.exit(1)
        print(f"{exe_name} compiled successfully.")

        print(f"Running {exe_name}...")
        res_run = subprocess.run([str(out_dir / exe_name)], cwd=out_dir, capture_output=True, text=True)
        print(res_run.stdout)
        if res_run.returncode != 0:
            print("STDERR:\n", res_run.stderr)
            sys.exit(1)
    print("All tests passed successfully!")

if __name__ == "__main__":
    main()
