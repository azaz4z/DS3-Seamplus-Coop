"""Delegates to the main release application in the root directory."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from make_release import main

if __name__ == "__main__":
    main()
