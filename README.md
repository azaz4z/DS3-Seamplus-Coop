# The Ashen Link: DS3 Coop

A modular multiplayer modification and enhancement suite for **Dark Souls III**, based on the Seamless Co-op experience as an independent standalone version.

---

## Key Features

- **LAN Co-op Transport & Runtime Switching:** Fully functional peer-to-peer LAN multiplayer using direct UDP sockets (default port `27015`), local subnet beacon discovery, automatic port release on session dissolution, and live runtime switching between Steam Matchmaking and LAN mode.
- **Custom Menu Integration:** Native Scaleform title menu button with game-accurate hitbox, plus in-game configuration modal accessible anytime via `F7`.
- **FPS Unlocker (Uncap 60 FPS):** Removes the engine's hardcoded 60 FPS limit with customizable target framerate (144, 165, 240+ FPS) and optional VSync.
- **Ally Outlines & Silhouettes:** Custom Direct3D 11 shader pass rendering real-time occluded silhouettes of party members behind walls and geometry.
- **Ally Diamond Markers:** 3D overhead position indicators above co-op allies with customizable height offset and distance scaling.

---

## Project Status, Roadmap & Bug Tracker

To keep this guide clear and organized, active task tracking and detailed bug reports are maintained in dedicated documents:

| Document | Description | Direct Link |
| :--- | :--- | :--- |
| 📋 **Project Roadmap & Tasks** | Active milestones, short/medium/long-term development priorities, and planned features. | **[Open TODO.md](TODO.md)** |
| 🐛 **Known Issues & Bug Tracker** | Detailed tracking of boss anomalies (Fire Demon, Dragonslayer Armour, Wolnir), hit sync notes, and reproduction details. | **[Open BUGS.md](BUGS.md)** |

---

## Visual Showcase & Screenshots

### Ally Outlines & Silhouettes
Real-time Direct3D 11 occluded silhouette rendering through walls and geometry:

![Ally Outlines & Silhouettes](github/2026092.JPG)

### Ally Diamond Markers
Real-time 3D overhead diamond indicators above co-op allies (configurable height offset, default `1.35m`):

![Ally Diamond Markers](github/2026091.JPG)

---

## Building & Packaging

### Prerequisites

- **Windows 10 / 11 (x64)**
- **Visual Studio 2022 / Build Tools** (with MSVC C++ x64 compiler and Windows SDK)
- **Python 3.8+** (with `PyQt6` for the release builder: `pip install PyQt6`)

---

### Primary Method: Modular Release Builder (`make_release.pyw`)

The primary way to configure, build, and package releases is via **`make_release.pyw`**. It provides an interactive graphical interface (PyQt6) to select active modules, patches, and compile ready-to-distribute release packages without opening a command prompt console:

```powershell
# Launch the graphical Release Manager (or simply double-click make_release.pyw)
python make_release.pyw
```

You can also run it headlessly via the command line:

```powershell
# Build release via CLI
python make_release.pyw --cli

# Build with custom modular flags
python make_release.pyw --cli --with-greatwood-patch --with-ally-markers --with-companion-spawner
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

### Step-by-Step Installation

1. Download the latest release `.zip` from the **[Releases](https://github.com/azaz4z/The-Ashen-Link/releases)** page.
2. Extract all contents directly into your Dark Souls III **`Game\`** folder (where `DarkSoulsIII.exe` is located, e.g. `C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS III\Game`):
   - `TheAshenLink.exe` -> `Game\TheAshenLink.exe`
   - `TheAshenLink\` -> `Game\TheAshenLink\` (contains `ds3sc.dll`, `ds3sc_companion.dll`, `ds3sc_settings.ini`, and `locale/`)
   *(Backwards compatibility with existing `Game\SeamlessCoop\` installations is automatically supported).*
3. Configure your co-op password and settings in `TheAshenLink\ds3sc_settings.ini` (all players in your party must use the same password).
4. Launch the game using **`TheAshenLink.exe`**.

### Controls & In-Game Hotkeys

| Key / Action | Function | Description |
| :--- | :--- | :--- |
| **Title Menu (Option 4)** | **The Ashen Link Settings** | Open the in-game settings modal directly from the main title screen. |
| **F7** | **Toggle Settings Menu** | Open / close the interactive mod settings menu anytime during gameplay. |

---

## License, Credits & Disclaimers

- **Project Origins & Independence:** This project is an independent standalone version based on the original **Dark Souls 3 Seamless Co-op** experience. It is an independent community effort and is in no way affiliated with, endorsed by, or supported by LukeYui.
- **Original Concept & Attribution:** All credit and appreciation go to **LukeYui / yuiamoroll** for pioneering the [Dark Souls 3 Seamless Co-op](https://github.com/yuiamoroll/DarkSouls3SeamlessCoopRelease) concept.
- **Clean-Room Reimplementation:** Because LukeYui's mod is closed-source and its source code was never publicly released, **The Ashen Link: DS3 Coop** is a 100% independent clean-room reimplementation developed from the ground up using reverse engineering, custom memory hooks, and community research. No proprietary binaries, decompiled assets, or closed-source code from LukeYui's original mod are copied, bundled, or redistributed.
- **AI-Assisted Development:** Modern AI developer tooling was utilized throughout development to assist with reverse-engineering analysis, debugging, refactoring, and documentation. The project is 100% open-source, fully transparent, and welcomes community audits, testing, and pull requests.
- **Third-Party Libraries:** Uses [MinHook](https://github.com/TsudaKageyu/minhook) for API redirection and runtime hooking.

