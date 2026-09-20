"""The Ashen Link: DS3 Coop - Windowed Release Builder (No Console).

Double-clicking this file executes via pythonw.exe without opening a black command prompt.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

import make_release

if __name__ == "__main__":
    make_release.main()
