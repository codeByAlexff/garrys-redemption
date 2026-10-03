# Garry's Redemption (working title)

Play Red Dead Redemption 2 as a Garry's Mod player: GMod movement, HUD, weapons, physgun,
toolgun and Q menu inside RDR2's world, with RDR2's peds, horses, wagons and props reacting
to GMod physics.

This is a **passthrough mod**, modelled on [SkyCraft](https://github.com/chasmlol/SkyCraft)
(Minecraft + Skyrim, MIT). Neither game is rewritten. GMod runs hidden and owns the player and
sandbox logic. RDR2 owns its world, AI and rendering. Two plugins exchange state through shared
memory every frame.

## Hard rules

- **Story mode only.** Never add anything that touches Red Dead Online. The RDR2 plugin must
  refuse to run if the session is multiplayer.
- **Ship no game assets.** No models, textures, sounds or decompiled code from either game go
  in the repo or the release. The user must own both games.
- **Do not guess native names or hashes.** Look every RDR2 native up in the native database
  (alloc8or's RDR3 nativedb) and record the hash next to the call. Same for GMod: check the
  Facepunch wiki before using a function. If you cannot verify one, say so and leave a `TODO(verify)`.
- **One protocol header is the source of truth.** Both C++ sides include
  `protocol/gr_protocol.h`. Never duplicate a struct. Bump `GR_PROTOCOL_VERSION` on any layout
  change; both sides refuse to connect on a mismatch.
- **Never block RDR2's script thread.** No waits, sleeps or file I/O in the per-frame path.

## Architecture

```
 RDR2 (host, visible)                         GMod (guest, hidden)
 ┌───────────────────────────┐  shared memory  ┌────────────────────────────┐
 │ rdr2/  GarrysRedemption.asi│◄──────────────►│ gmod/module  gmcl_gr_win64 │
 │  - world scan (peds, etc.) │  gr_protocol.h │  - maps the shared memory  │
 │  - collision sampling      │                │  - exposes it to Lua       │
 │  - applies physics results │                ├────────────────────────────┤
 │  - input capture           │                │ gmod/addon  (Lua)          │
 │  - graphics hook: overlay  │                │  - proxies, collision ents │
 │  - spawns RDR2 entities    │                │  - input apply, spawnmenu  │
 └───────────────────────────┘                 └────────────────────────────┘
```

### Who is authoritative

| Thing | Owner | Notes |
|---|---|---|
| Player position, velocity, view angles | GMod | RDR2 teleports the hidden player ped and a scripted camera to match |
| Terrain and building collision | RDR2 | Sampled and rebuilt in GMod as physics meshes |
| Peds, horses, wagons, RDR2 props | RDR2 | Mirrored into GMod as invisible proxies |
| A proxy while held, thrown or hit | GMod | RDR2 ragdolls the real entity and drives it to the proxy's velocity, then takes ownership back once released and settled |
| AI, reactions, law, quests, saves | RDR2 | Never simulated in GMod |
| HUD, Q menu, context menu, viewmodels | GMod | Captured and composited over RDR2's frame |
| Damage dealt by GMod weapons | GMod decides the hit, RDR2 applies it | |

### Data flow per frame

1. RDR2 plugin writes: input state, camera FOV, nearby entity list (handle, model hash, type,
   transform, velocity, bounds, health), new collision chunks, frame counter.
2. GMod reads it, applies input in `CreateMove`/`StartCommand`, updates proxies, runs its tick.
3. GMod writes: player transform and eye angles, per-proxy commands (grab, drive-to-velocity,
   release, freeze, damage, remove), spawn requests, overlay frame.
4. RDR2 plugin reads and applies it, then composites the overlay at present time.

Each direction is a single-writer block guarded by a sequence counter (seqlock). Readers retry
on a torn read. No mutexes across processes.

## Coordinates and units

- RDR2 uses metres, Z up. Source uses units, Z up. **1 Source unit = 0.01905 m**
  (1 m ≈ 52.49 units). Convert only in `protocol/` helpers, never inline.
- **Floating origin is mandatory.** A Source map is only about ±16384 units (roughly 312 m) from
  its centre, and RDR2's map is kilometres wide. GMod runs on a flat empty map and the bridge
  keeps an `origin` (RDR2 world position that maps to GMod 0,0,0). When the player passes
  ~8000 units from centre, rebase: shift origin, teleport player, proxies and collision chunks
  by the same offset in one tick.
- Angle conventions differ. Keep one tested conversion function per direction with unit tests.

## Features and how each one works

### Movement and collision
- GMod player movement is unchanged (walk, sprint, jump, crouch, noclip).
- RDR2 plugin samples the world around the player with shape-test raycasts on a grid and writes
  heightfield/triangle chunks. Budget raycasts per frame and prioritise chunks in the direction
  of travel.
- GMod builds each chunk as a scripted entity with `Entity:PhysicsFromMesh`, static and frozen.
  Chunks behind the player are recycled.
- Buildings, fences and wagons need side-on raycasts as well as top-down ones, or walls will be
  missing. Start with terrain only, add vertical sampling after.

### Proxies (the core of "the world reacts")
- Every nearby ped, horse, vehicle and dynamic prop gets an invisible GMod proxy sized from its
  RDR2 bounds. Peds get a capsule-like convex hull; ragdoll-accurate proxies are a later step.
- Idle proxies are kinematic and follow RDR2.
- When a proxy is grabbed by the physgun or gravity gun, hit by a prop, or shot, it becomes
  GMod-driven. RDR2 plugin then: set the ped to ragdoll, set or blend its velocity toward the
  proxy's, keep it ragdolled while held. On release it stays ragdolled until slow, then RDR2
  takes back ownership and lets Euphoria stand it up.
- Physgun freeze (right click) freezes the real entity in place.
- Do not suppress RDR2's own reactions: witnesses, fleeing, lawmen and spooked horses are the
  point.

### Weapons and damage
- GMod weapons fire in GMod. Traces hit proxies. The hit (damage, direction, force, bone guess
  from hit height) goes over the bridge; RDR2 applies damage and an impulse.
- Viewmodels come from the overlay, so no RDR2 weapon models are needed.
- Explosions: GMod sends position and radius, RDR2 spawns a real explosion.

### HUD, Q menu and overlay
- GMod's real HUD, spawn menu, context menu, chat and tool panels are used as-is.
- GMod renders only screen-space content plus viewmodels over a transparent clear, the module
  captures that frame, and the RDR2 plugin draws it on top of RDR2's frame from a hook on the
  Vulkan/DX12 present call. RDR2 can run on either API: support one first, pick by what the
  user runs, and state which.
- Match resolution and FOV between the games every frame.
- When a GMod menu is open, the RDR2 plugin forwards cursor position and clicks instead of
  camera movement, and blocks RDR2's own input.

### Spawning
- Add a **Red Dead** tab to the Q menu listing RDR2 peds, animals, vehicles and objects by model
  name, with search. Clicking sends a spawn request with model hash and a position from the
  GMod eye trace.
- RDR2 plugin requests the model, creates the entity, and it shows up as a proxy on the next
  scan. Spawned peds may need an outfit variation applied before they are visible
  (`TODO(verify)` the native).
- Track everything spawned so undo (Z) and cleanup work. Remover tool deletes the real entity.
- Native GMod props (Source models) are **phase 2**: they need depth-aware compositing of GMod's
  world render into RDR2's frame, with both depth buffers and matched view matrices. Until
  then the spawn menu's Source prop tabs are hidden.

### Toolgun
- Tools act on proxies and translate to RDR2: weld → entity attachment, rope → RDR2 rope,
  thruster → per-frame force, remover → delete, duplicator → respawn by model hash,
  no-collide → collision flags. Add one tool at a time, each with its own test steps.

### Input
- GMod is hidden and has no focus. The RDR2 plugin captures keyboard and mouse and writes them
  to shared memory. GMod applies them in `CreateMove` (buttons, move, view angles, impulse)
  and through the `gui.Internal*` functions for menus.
- A small set of keys stays with RDR2 (pause menu, map, an interact key for doors, shops and
  mounting). Keep the list in `rdr2/src/input_passthrough.h` and the README.
- When RDR2 runs a scripted scene or the player mounts a horse, hand control back to RDR2
  until it ends.

## Repo layout (create as you go)

```
protocol/   gr_protocol.h, unit conversion, version constant, link and seqlock, C++ tests
common/     Code both C++ sides share that is not protocol (the logger)
cmake/      GrCommon.cmake: compiler settings every project includes
rdr2/       ASI plugin, C++20, CMake. Script Hook RDR2 SDK in rdr2/sdk (optional, not committed)
gmod/module Binary module, C++20, CMake, Facepunch gmod-module-base headers in third_party (not committed)
gmod/addon  Lua addon: lua/autorun, entities, spawnmenu tab
launcher/   Launcher.cs: the release's GUI (find the games, install, NVIDIA setting, PLAY), build.ps1
tools/      build.ps1, package.ps1, fake_rdr2.py, fake_gmod.py, memdump.py, gmod_live_test.py,
            fake_scripthook/ (runs the real .asi without the game), tests/
            gr_screen/ (what the monitor really shows: GDI grabs of a full-screen RDR2 go stale)
docs/       DESIGN.md, NATIVES.md (every native used, with hash and what it was verified against),
            TESTING.md (the in-game test steps of every feature)
```

## Build and run

```
powershell -File tools\build.ps1      # protocol, rdr2, gmod/module, then every test
powershell -File tools\package.ps1    # dist\ and, with GR_RDR2_DIR / GR_GMOD_DIR set, into the games
```

- One project by hand: `cd rdr2`, `cmake --preset default`, `cmake --build --preset release`
  (`cmake --build --preset` has no `-S`: run it from inside the project folder).
- `python tools\gmod_live_test.py` tests the module in the real GMod without RDR2.

- RDR2 plugin output: `GarrysRedemption.asi`, deployed next to `RDR2.exe` with Script Hook RDR2
  and an ASI loader.
- GMod module output: `gmcl_gr_win64.dll` into `garrysmod/lua/bin`. GMod must be on the
  **x86-64 branch**. Addon folder goes in `garrysmod/addons/garrys_redemption`.
- Deploy paths come from `GR_RDR2_DIR` and `GR_GMOD_DIR`. Never hardcode a user path.
- Windows only, 64-bit only.

## Test in the real games

- **A change is tested in the real games first**: RDR2 in story mode and Garry's Mod, linked.
  The stand-ins (`fake_rdr2.py`, `fake_gmod.py`, `fake_scripthook/`) are for regression
  tests afterwards, never a substitute for the real test.
- `docs/TESTING.md` has the in-game test steps of every feature; add yours with the feature.
- Copying into a game folder under `C:\Program Files` needs administrator rights, and Script
  Hook RDR2 is downloaded by hand from its author.

## Milestones (do them in order, one at a time)

1. Protocol header, shared memory, both fakes, handshake and version check.
2. Player sync: GMod position drives the hidden RDR2 ped and camera on flat ground. Input forwarded.
3. Proxies and physgun on peds and horses. First shareable clip.
4. HUD and viewmodel overlay.
5. Terrain collision chunks, then floating origin, then vertical surfaces.
6. Weapons, damage, explosions.
7. Q menu with the Red Dead tab, spawning, undo and cleanup.
8. Toolgun tools, one by one.
9. Depth-composited Source props and third-person playermodel.

Do not start a milestone until the user confirms the previous one works in-game.

## Open questions (resolve by testing, record the answer in docs/DESIGN.md)

- Best way to capture GMod's frame with alpha: a render target read back in the module, or a
  D3D9 hook in the module. Measure the cost of each.
- Whether GMod can run fully hidden without throttling its frame rate when unfocused.
- How many shape tests per frame RDR2 tolerates before stutter, and whether async tests help.
- How to keep a ped ragdolled indefinitely while held without it dying or despawning.
- Which API (Vulkan or DX12) to hook first.

## Code style

- C++20, no exceptions across the shared-memory boundary, no heap allocation in per-frame paths.
- Lua: one file per feature under `lua/gr/`, everything in a single `GR` table, no globals.
- Comments explain why, not what. Every workaround names the problem it works around.
- Small commits, one feature each, with the in-game test steps in the commit body.

## Legal

Fan project. Not affiliated with Rockstar Games, Take-Two, Valve or Facepunch. MIT licensed
code only.
