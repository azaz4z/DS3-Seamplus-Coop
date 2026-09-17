# DS3 Seamplus Coop

A modular companion modification and enhancement suite for **Dark Souls III Seamless Co-op**.

---

## Features & Roadmap Status

| Component / Feature | Category | Status | Notes / Known Issues |
| :--- | :--- | :--- | :--- |
| **Custom Menu Integration** | UI / Core | **Working / Stable** | Native Scaleform button hook (RVA `0xEE1150`) with exact game button hitbox. Also accessible in-game via `F7`. TrueType typography with INI synchronization. |
| **Companion Spawner (Ash Stone)** | Gameplay / AI | **Working** | Modular companion summon system (Ash Stone); functional companion NPC summoning. |
| **Guest Bonfires Restoration** | World Fix | **Working / Patched** | Fixes bonfire checkpoint write-back (`+0xacc`) for guest players so resting and respawns work properly. |
| **Combat Stat Counters** | Overlay / Stats | **Working / Stable** | Optional in-game HUD tracking session kills, deaths, and backstabs. |
| **Ally Diamond Markers** | Render / UI | **Working / In Progress** | Real-time 3D overhead position markers for co-op allies; undergoing polish and refinement. |
| **Ally Outlines & Silhouettes** | Render (D3D11) | **Working / In Progress** | Direct3D 11 shader pass for occluded silhouettes behind walls; functional, undergoing optimization. |
| **Hit Synchronization** | Network / Combat | **Buggy / In Progress** | Damage registration and weapon hit verification across co-op peers; still exhibits bugs and desynchronization. |
| **Curse-Rotted Greatwood Wipe Fix** | Boss Encounter | **Patched (Untested)** | Prevents infinite loading screen and host HUD lock on party wipe; multiplayer acceptance remains unverified. |
| **High Lord Wolnir** | Boss Encounter | **Buggy / Pending Fix** | Arena boundary desynchronization and phase transition issues in multiplayer. |
| **Fire Demon (Demon Ruins)** | Mini-Boss Encounter | **Buggy / Pending Fix** | Grab attack desynchronization and abnormal aggro drops in co-op sessions. |

---

## Building & Packaging

### Prerequisites

- **Windows 10 / 11 (x64)**
- **Visual Studio 2022 / Build Tools** (with MSVC C++ x64 compiler and Windows SDK)
- **Python 3.8+** (with `PyQt6` for the release builder: `pip install PyQt6`)

---

### Primary Method: Modular Release Builder (`make_release.py`)

The primary way to configure, build, and package releases is via **`make_release.py`**. It provides an interactive graphical interface (PyQt6) to select active modules, patches, and compile ready-to-distribute release packages:

```powershell
# Launch the graphical Release Manager
python make_release.py
```

You can also run it headlessly via the command line:

```powershell
# Build release via CLI
python make_release.py --cli

# Build with custom modular flags
python make_release.py --cli --with-greatwood-patch --with-ally-markers --with-companion-spawner
```

---

### Developer Method: Direct Binary Build (`tools/build_binaries.py`)

For rapid development and immediate testing without creating packaged distribution zips, you can compile the binaries directly:

```powershell
python tools/build_binaries.py
```

Feature toggles can be configured in `build_config.json`:

```json
{
  "greatwood_patch": true,
  "guest_bonfires": true,
  "ally_outline": true,
  "ally_markers": true,
  "companion_spawner": true,
  "hit_sync": true,
  "counters": true
}
```

Or overridden via command-line arguments:

```powershell
python tools/build_binaries.py --without-ally-outline --with-ally-markers --with-companion --with-hit-sync
```

---

## Installation & Usage

1. Copy the release files into your Dark Souls III `Game\` directory:
   - `ds3sc_launcher.exe` -> `Game\ds3sc_launcher.exe`
   - `SeamplusCoop\` -> `Game\SeamplusCoop\` (containing `ds3sc.dll`, `ds3sc_companion.dll`, `ds3sc_settings.ini`, etc.)
   *(Backwards compatibility with `Game\SeamlessCoop\` is also fully supported).*
2. Launch the game using `ds3sc_launcher.exe`.
3. Access the menu from the Title Screen or by pressing **F7** during gameplay.

---

## License & Credits

- Based on [Dark Souls 3 Seamless Co-op Release](https://github.com/yuiamoroll/DarkSouls3SeamlessCoopRelease) by **LukeYui / yuiamoroll**.
- Uses [MinHook](https://github.com/TsudaKageyu/minhook) for API redirection and runtime hooking.
