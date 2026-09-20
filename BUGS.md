# Known Issues & Bug Tracker

This document tracks identified bugs, multiplayer synchronization issues, and encounter anomalies in **The Ashen Link: DS3 Coop**.

---

## 🐉 Boss & Encounter Issues

### 1. Fire Demon (Demon Ruins - Mini-Boss)
* **Category:** Mini-Boss Encounter / Animation
* **Status:** ⚠️ **Pending Fix**
* **Known Anomaly:**
  * **Shared Fog Gate Animation Trigger:** When a single player interacts with and passes through the fog gate to enter the fight, all other players in the session are forcibly locked into the fog-crossing animation simultaneously, regardless of where they are in the map.

---

### 2. Dragonslayer Armour
* **Category:** Boss Encounter / HUD
* **Status:** ⚠️ **Pending Fix**
* **Known Anomaly:**
  * **Persistent Boss Health Bar on Wipe:** If all members of the co-op session die during the fight (total party wipe), the Dragonslayer Armour's boss health bar remains permanently stuck and visible on the HUD after respawning at the bonfire.

---

### 3. High Lord Wolnir
* **Category:** Boss Encounter / World Geometry
* **Status:** ⚠️ **Pending Fix**
* **Known Anomaly:**
  * **Arena Boundary & Fog Desync:** The lethal abyss fog edge and boundary collision can desynchronize between peers, causing invisible barriers or damage in areas that appear clear on client screens.

---

### 4. Curse-Rotted Greatwood
* **Category:** Boss Encounter / Arena Floor
* **Status:** 🟡 **Patched (Testing In Progress)**
* **Known Anomaly:**
  * **Phase 2 Floor Collapse Wipe Lock:** Previous builds could enter an infinite loading screen if the party wiped as the floor collapsed. A memory patch is applied and currently being verified in multiplayer sessions.

---

### 5. Dancer of the Boreal Valley
* **Category:** Boss Encounter / Fog Gate & Stability
* **Status:** ⚠️ **Pending Fix**
* **Known Anomalies:**
  * **Unable to Enter Through Fog Gate:** When the fight begins or upon returning to the arena, guest players are frequently unable to traverse the fog gate ("Unable to enter" or interaction prompt missing), leaving them locked outside while the host fights inside.
  * **Crash-Prone Arena & Transition Zone:** The Dancer's arena and the High Wall church area are highly prone to sudden crashes (CTD) for both host and guests, especially when triggering the fight, during Emma's death/cutscene sequence, or upon co-op entry into the room.

---

## ⚔️ Combat & Networking Issues

### 1. Hit Synchronization
* **Category:** Combat / Netcode
* **Status:** 🔧 **In Progress**
* **Known Anomaly:**
  * **Damage & Stagger Registration:** Under latency or packet jitter, fast weapon hits may show visual blood effects without registering damage or poise stagger on co-op peers.
