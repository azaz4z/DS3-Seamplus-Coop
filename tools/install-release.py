"""Install a validated local release, backing up replaced files and preserving settings."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def install(game: Path, release: Path):
    game = game.resolve(strict=True)
    release = release.resolve(strict=True)
    if not (game / "DarkSoulsIII.exe").is_file():
        raise RuntimeError(f"DarkSoulsIII.exe does not exist in {game}")
    launcher_name = "TheAshenLink.exe" if (release / "TheAshenLink.exe").is_file() else "ds3sc_launcher.exe"
    mod_folder = "TheAshenLink" if (release / "TheAshenLink").is_dir() else ("SeamplusCoop" if (release / "SeamplusCoop").is_dir() else "SeamlessCoop")
    manifest_path = release / "manifest.json"
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        core_required = {launcher_name, f"{mod_folder}/ds3sc.dll", f"{mod_folder}/ds3sc_settings.ini"}
        if not core_required.issubset(manifest):
            missing = core_required - set(manifest.keys())
            raise RuntimeError(f"The manifest does not contain core essential files: {missing}")
        for relative, expected in manifest.items():
            source = (release / relative).resolve()
            if not source.is_relative_to(release):
                raise RuntimeError("Path outside of release in manifest")
            if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
                raise RuntimeError(f"Modified or incomplete file: {relative}")
    else:
        core_files = [release / launcher_name, release / mod_folder / "ds3sc.dll", release / mod_folder / "ds3sc_settings.ini"]
        for f in core_files:
            if not f.is_file():
                raise RuntimeError(f"Missing core essential file in release: {f}")
    processes = subprocess.check_output(
        ["tasklist", "/FI", "IMAGENAME eq DarkSoulsIII.exe", "/FO", "CSV", "/NH"], text=True)
    if '"DarkSoulsIII.exe"' in processes:
        raise RuntimeError("Close DS3 before installing release.")
    backup = ROOT / "build/backups" / ("installed-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    changed = []
    try:
        sources = [release / launcher_name, *sorted((release / mod_folder).rglob("*"))]
        for source in sources:
            if not source.is_file():
                continue
            relative = source.relative_to(release)
            target = (game / relative).resolve()
            if not target.is_relative_to(game):
                raise RuntimeError(f"Destination outside of Game directory: {target}")
            if target.exists() and source.suffix.lower() == ".ini":
                print(f"Preserved configuration: {relative}")
                continue
            saved = backup / relative
            if target.exists():
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(target, saved)
            changed.append((target, saved))
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            if hashlib.sha256(target.read_bytes()).digest() != hashlib.sha256(source.read_bytes()).digest():
                raise RuntimeError(f"Copy verification failed: {relative}")
    except Exception:
        for target, saved in reversed(changed):
            if saved.exists():
                shutil.copy2(saved, target)
            elif target.exists():
                target.unlink()
        raise
    print(f"Release installed and verified: {game}")
    if backup.exists():
        print(f"Previous backup: {backup}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--release", type=Path, default=ROOT / "release")
    args = parser.parse_args()
    install(args.game_dir, args.release)
