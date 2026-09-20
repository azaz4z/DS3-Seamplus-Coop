"""Build and run the read-only marker visibility regression."""
from pathlib import Path
import subprocess
from build_binaries import setup_msvc_environment

ROOT = Path(__file__).resolve().parents[1]

if __name__ == "__main__":
    compiler = setup_msvc_environment()
    output = ROOT / "build/marker-tests"
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/O2", "/MT",
                    str(ROOT / "tests/marker_visibility_test.cpp"), "/Fe:marker_visibility_test.exe"],
                   cwd=output, check=True)
    subprocess.run([str(output / "marker_visibility_test.exe")], cwd=output, timeout=15, check=True)
