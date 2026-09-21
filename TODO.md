# Project Roadmap & Task Tracker (TODO)

This document tracks planned improvements and development priorities for **The Ashen Link: DS3 Coop**.

---

## 🎨 UI & User Experience
- [ ] **Modernized Menu Interface (`F7`):**
  - Improve visual styling, layout responsiveness, and gamepad / controller navigation.

---

## ⚔️ Animation Synchronization
- [ ] **Independent Fog Gate Animations:**
  - Fix the bug where one player crossing a fog gate forces the traversal animation on all other players.
- [ ] **General Action & Animation Parity:**
  - Improve network synchronization of player actions (weapon arts, spell casts, item usage, gestures).
  - Smooth out remote player movement interpolation to avoid sliding or stutter under latency.

---

## ⚡ Performance Upgrades
- [ ] **Rendering Optimization (D3D11):**
  - Optimize shader passes for ally outlines and markers to reduce frame overhead.
- [ ] **Network & CPU Efficiency:**
  - Streamline packet handling and polling loops in `LanTransport` to minimize CPU tick usage.

---

## 🐉 Boss Encounters
- [ ] **Fire Demon (Demon Ruins):**
  - Fix fog gate entry triggering the traversal animation on all allies simultaneously.
- [ ] **Dragonslayer Armour:**
  - Fix boss health bar remaining permanently stuck on the HUD after a total party wipe.
- [ ] **High Lord Wolnir:**
  - Fix abyss fog kill boundaries and arena collision desynchronization in co-op.
- [ ] **Curse-Rotted Greatwood:**
  - Continue verifying stability of the phase 2 arena floor collapse fix during multiplayer sessions.
- [ ] **Dancer of the Boreal Valley:**
  - Fix fog gate lockout preventing co-op allies from traversing the mist into the arena ("Unable to enter").
  - Resolve crashes and memory instability occurring in the Dancer arena and High Wall church zone during multiplayer.
- [ ] **General Boss Wipe Reset:**
  - Ensure all boss encounters cleanly reset their state, health bars, and arenas when the whole party dies.

---

## ✅ Completed Milestones
- [x] **Local LAN Co-op Transport:** Fully functional UDP peer-to-peer transport on configurable port (`27015`).
- [x] **Runtime Mode Switching:** Seamless dynamic toggling between Steam Matchmaking and LAN mode without restarting the game.
- [x] **Automatic Port Release:** UDP socket closes and frees port `27015` immediately upon session dissolution.
- [x] **Dynamic INI Hot-Reload:** Automatic file monitoring detecting changes to `connection_mode`, `lan_port`, and passwords in real-time.
- [x] **Direct3D 11 Presentation Hook:** Dynamic fallback and runtime recovery for Steam overlay and Discord hooks.
- [x] **FPS Unlocker:** Unlimited framerate (144, 165, 240+ FPS) with VSync toggle and F7 integration.
- [x] **Ally Diamond Markers:** Real-time 3D overhead position indicators for co-op allies.
- [x] **Ally Outlines & Silhouettes:** Direct3D 11 occluded silhouette shader pass behind walls.
- [x] **Native Scaleform Title Menu:** Custom menu button on the title screen with game-matching button hitbox.
- [x] **Standalone Release Manager (`make_release.pyw`):** Standalone GUI builder with silent console launch and one-click packaging.
- [x] **Spectator Stamina Bar & HUD Fix (`spectator_fix`):** Eliminates stamina bar stretching, corruption and violent flickering while spectating co-op teammates after dying in boss fights.
