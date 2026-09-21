"""Compile and run native notification regression tests without launching DS3."""
from pathlib import Path
import argparse
import subprocess
from build_binaries import setup_msvc_environment

ROOT = Path(__file__).resolve().parents[1]
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-dll", type=Path)
    args = parser.parse_args()
    compiler = setup_msvc_environment()
    output = ROOT / "build/verbose-tests"
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX",
                    "/O2", "/MT", "/utf-8",
                    str(ROOT / "tests/verbose_connections_native_test.cpp"),
                    "/Fe:verbose_connections_test.exe"], cwd=output, check=True)
    subprocess.run([str(output / "verbose_connections_test.exe")], cwd=output,
                   check=True, timeout=15)
    if args.native_dll:
        minhook = ROOT / "tools/vendor/minhook-1.3.4/src"
        subprocess.run([compiler, "/nologo", "/O2", "/MT", "/c",
                        *[str(minhook / path) for path in
                          ("buffer.c", "hook.c", "trampoline.c", "hde/hde64.c")]],
                       cwd=output, check=True)
        subprocess.run([compiler, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX",
                        "/O2", "/MT", "/utf-8",
                        str(ROOT / "tests/verbose_connections_hooks_test.cpp"),
                        "buffer.obj", "hook.obj", "trampoline.obj", "hde64.obj",
                        "/Fe:verbose_connections_hooks_test.exe"], cwd=output, check=True)
        subprocess.run([str(output / "verbose_connections_hooks_test.exe"),
                        str(args.native_dll.resolve())], cwd=output, check=True, timeout=15)
