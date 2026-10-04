# Garry's Redemption

Play Red Dead Redemption 2 as a Garry's Mod player: GMod's movement, HUD, weapons, physgun,
toolgun and Q menu inside RDR2's world, with RDR2's people, horses, wagons and props reacting
to GMod's physics.

It is a passthrough mod. Neither game is rewritten: Garry's Mod runs hidden next to RDR2 and
owns the player and the sandbox, RDR2 owns its world, AI and rendering, and two plugins
exchange state through shared memory every frame. [CLAUDE.md](CLAUDE.md) has the
architecture, [docs/DESIGN.md](docs/DESIGN.md) the decisions and measurements, and
[docs/NATIVES.md](docs/NATIVES.md) every RDR2 native used with its hash.

**Status: beta.** All nine planned milestones are built and have been played on one machine
(Windows 11, an NVIDIA card, RDR2 on Vulkan). It has not yet been installed on another PC.
See [Known issues](#known-issues).

## Made with Claude Opus 5.5

This project was built with Anthropic's Claude Opus 5.5, the latest model at the time, as a
test of what current AI can do at modding live game environments. The model wrote and
debugged the code and tested it in the running games itself: it launched and drove Red Dead
Redemption 2 and Garry's Mod, read their logs and screenshots, and measured the results.

What it took on:

- **Two games as one**: a shared-memory bridge between two separate game processes, with a
  versioned binary layout, lock-free single-writer frames (seqlocks) and a floating origin
  that maps kilometres of RDR2's world onto a small Source map.
- **Engine internals**: hooking Garry's Mod's Direct3D 9 Present to capture its frame on the
  GPU and composite it over RDR2; calling RDR2's own script functions (natives) through
  Script Hook RDR2, every one looked up and recorded with its hash; reading the game's entity
  pools; raycasting RDR2's world to rebuild its ground, walls and trees as GMod physics.
- **Reverse engineering by measurement**: undocumented native arguments, shape-test flags and
  input paths were worked out by experiment in the running game and written down in
  [docs/NATIVES.md](docs/NATIVES.md) and [docs/DESIGN.md](docs/DESIGN.md).

A person directed the work, played it and reported what felt wrong; [CLAUDE.md](CLAUDE.md) is
the brief the model worked from.

## What it does

- **Movement**: GMod's walk, sprint, jump, crouch and noclip (Shift flies fast) on RDR2's
  ground, stopped by RDR2's walls, trees, rocks and objects. First and third person.
- **Physgun and gravity gun** on RDR2's people, horses, animals (living or dead), wagons,
  boats and objects: held people are ragdolls, thrown ones land and get up, frozen ones hang
  where you left them. People sitting or busy in town come out of what they are doing.
- **Weapons**: GMod's guns hurt RDR2's people as RDR2 bullets (blood, reactions, witnesses
  and the law), mark RDR2's trees and walls, and are stopped by them. Grenades, the AR2's
  ball, rockets and thrown props bounce off RDR2's trees and houses. Explosions are real RDR2
  explosions. RDR2's enemies and lawmen hurt the GMod player.
- **RDR2's guns** as GMod weapons, shown with RDR2's own models and sounds.
- **Q menu**: a **Red Dead** tab that spawns RDR2's people, animals, vehicles and objects by
  name, with search, undo and cleanup; RDR2's guns; commands (god mode, heal, lose all
  bounties, teleport to any town).
- **Toolgun** on RDR2's entities: weld, rope, no-collide, thruster, wheels, balloons, hoist,
  winch, elastic, duplicator, remover, and **Possess** (drive an RDR2 person, horse or
  animal).
- **GMod's own props, NPCs and vehicles** in RDR2's world, lit by RDR2's sun or moon, hidden
  behind its ground, walls and trees, and running RDR2's people over.
- GMod's HUD, crosshair, viewmodels, spawn menu, context menu and chat drawn over RDR2.
- Health shared between the games; death and respawn as in GMod.

## Rules

- **Story mode only.** The RDR2 plugin turns itself off the moment it sees a network
  session, and Script Hook RDR2 closes the game if you go online.
- **You must own both games.** Nothing from either game is in this repository or in a
  release.
- Windows, 64-bit only.

## What you need

| | |
|---|---|
| Red Dead Redemption 2 | Story mode, a version Script Hook RDR2 supports (tested: 1.0.1491.x) |
| Script Hook RDR2 | From <http://www.dev-c.com/rdr2/scripthookrdr2/>. Copy `ScriptHookRDR2.dll` and `dinput8.dll` (the ASI loader) next to `RDR2.exe`. It may not be redistributed, so it is not included here |
| Garry's Mod | The **x86-64** branch (Steam, Properties, Betas), run as `gmod_win64.exe` |
| To build | Visual Studio 2022 or newer (or the Build Tools) with the C++ workload, and Python 3 |

## Install from a release (no building)

1. **RDR2**: install [Script Hook RDR2](http://www.dev-c.com/rdr2/scripthookrdr2/) by copying
   its `ScriptHookRDR2.dll` and `dinput8.dll` (the ASI loader) next to `RDR2.exe`. Then copy
   `RDR2\GarrysRedemption.asi` from the zip to the same folder.
2. **Garry's Mod**: in Steam, Properties, Betas, pick the **x86-64** branch. Copy the zip's
   `GarrysMod` folder over your Garry's Mod folder (the one that holds `gmod_win64.exe`). It
   adds `garrysmod\lua\bin\gmcl_gr_win64.dll`, `garrysmod\addons\garrys_redemption` and
   `play_gmod.bat`.
3. **RDR2's settings**: screen type **windowed borderless** (or windowed), at any resolution.
   Exclusive fullscreen hides GMod's HUD, which is drawn in a window on top of RDR2.
4. **If RDR2 fills the whole monitor** (borderless at the monitor's own resolution) on an
   NVIDIA card: NVIDIA Control Panel, Manage 3D settings, Program Settings, Red Dead
   Redemption 2, **Vulkan/OpenGL present method: Prefer layered on DXGI Swapchain**, then
   restart RDR2. Without it the driver shows RDR2's frames past Windows' compositor and
   GMod's HUD and guns are not drawn over it. (Or run RDR2 on DirectX 12, or in a window
   smaller than the monitor.)

## The launcher

`GarrysRedemptionPassthrough.exe` in the release folder does the steps above and below for
you. Keep it beside the release's `RDR2` and `GarrysMod` folders and run it:

- it finds both games (Steam, Rockstar and Epic installs) or lets you browse to them;
- a checklist says what is missing: the 64-bit Garry's Mod branch, Script Hook RDR2, the
  mod's files, RDR2's screen type, the NVIDIA present method;
- **Install / update mod files** copies the plugin, module and addon into the games
  (Windows asks for administrator rights if a game is under Program Files); **Uninstall**
  takes them out again;
- **Fix the NVIDIA setting** asks first, then sets "Vulkan/OpenGL present method" to
  "Prefer layered on DXGI Swapchain" for RDR2.exe only;
- **PLAY** starts RDR2 and, once its window is up, Garry's Mod. Load **story mode**. With
  the box ticked, closing either game closes the other.

It needs nothing installed (it is built with the C# compiler that is part of Windows:
`powershell -File launcher\build.ps1`).

## Play

1. Start RDR2 and load **story mode**. The top left says "Garry's Redemption: waiting for GMod".
2. Run `play_gmod.bat` in the Garry's Mod folder (the launcher's PLAY does both). Click RDR2's
   window again while GMod loads.
3. When the line says "connected to GMod, which has the player", you are the GMod player in
   RDR2's world. GMod's window is hidden while RDR2 runs and follows RDR2's resolution.

### Controls

Your own GMod binds work: move, jump, crouch, noclip, Q menu, C menu, weapons with the wheel
or number keys, undo, toolgun, numpad keys, `thirdperson` / `firstperson`. RDR2 keeps:

| Key | |
|---|---|
| Esc, P | RDR2's pause menu |
| M | RDR2's map |
| F9 | Hand the player to RDR2 (to ride, shop or play a mission) and back |

The list is in `rdr2/src/input_passthrough.h`. When RDR2 runs a scene, or you mount a horse
or a wagon, RDR2 takes the player until it ends.

Your health is GMod's, and it carries over to Arthur when RDR2 takes the player and back;
each session starts at full health. When you die you hear GMod's death beep and come back a
few seconds later where you fell.

As Arthur, stay out of New Austin (Armadillo, Tumbleweed, Benedict Point and around): the
game has him shot on sight there until the epilogue. The Q menu's teleport refuses those
towns for him.

## Settings

GMod console variables (all start with `gr_`; the defaults are what was tested):

| Variable | Default | |
|---|---|---|
| `gr_damage_scale` | 3 | RDR2 health taken per point of GMod damage |
| `gr_weapons` | 1 | GMod's hits and explosions happen in RDR2, and RDR2 hurts the GMod player |
| `gr_bullet_impacts` | 1 | GMod's bullets hit RDR2's trees, walls and rocks |
| `gr_surfaces` | 1 | Grenades, balls, rockets and thrown things bounce off RDR2's trees and houses |
| `gr_gmod_shots_in_rdr2` | 0 | RDR2's people also hear GMod's own guns |
| `gr_rdr2_gun_gmod_sound` | 0 | RDR2's guns also play a Half-Life 2 gunshot |
| `gr_physgun_range` | 16384 | Physgun reach in units while linked (GMod's own: 4096) |
| `gr_noclip_fast_speed` | 4000 | Noclip speed with Shift held |
| `gr_respawn_delay` | 3 | Seconds after death before you come back by yourself |
| `gr_thirdperson_distance` | 110 | Third-person camera distance |
| `gr_collide`, `gr_near_collide`, `gr_terrain` | 1 | RDR2's walls, nearby trees and objects, and ground are solid in GMod |
| `gr_tools` | 1 | Toolgun tools act on RDR2's entities |
| `gr_world`, `gr_world_shadow`, `gr_world_veil`, `gr_world_occlude` | 1 | GMod's props in RDR2's world: drawn, with shadows, behind RDR2's trees, behind RDR2's ground and walls |
| `gr_viewmodel_light`, `gr_light_match` | 1, 2.2 | The viewmodel and props lit by RDR2's light; brightness match |
| `gr_view_lock`, `gr_view_hold_frames`, `gr_look_predict` | 1, 0, 1 | GMod draws with the camera RDR2 shows; extra frames of hold if GMod's picture trails |
| `gr_overlay`, `gr_cursor_speed` | 1, 1 | GMod's picture over RDR2; menu cursor speed |
| `gr_hide_window`, `gr_match_resolution`, `gr_quit_with_rdr2` | 1 | GMod's window hidden, at RDR2's resolution, and closed with RDR2 |
| `gr_input`, `gr_keep_active` | 1 | RDR2's keyboard and mouse drive GMod; GMod keeps full speed in the background |

Commands: `gr_status`, `gr_proxies`, `gr_terrain_status`, `gr_weapons_status`,
`gr_tools_status`, `gr_surfaces_status`, `gr_near_status`, `gr_overlay_status`, `gr_spawn NAME`,
`gr_teleport NAME`, `gr_god`, `gr_heal`, `gr_clear_wanted`, `gr_possess`, `gr_unpossess`,
`gr_thirdperson`, `gr_firstperson`, `gr_vehicle_view`.

The RDR2 plugin reads an optional `GarrysRedemption.ini` next to the `.asi` (or in `%TEMP%`):

```ini
[GarrysRedemption]
status_text=1   ; 0 = no status line on screen
hide_hud=1      ; 0 = RDR2's minimap and health cores stay while GMod has the player
collide=1       ; 0 = RDR2's walls and objects do not stop the player
hold_rigid=0    ; 1 = people held by the physgun stay stiff instead of ragdolls
terrain_budget_us=1000 ; time per frame for sampling RDR2's ground
smooth=1        ; 0 = no extrapolation of the GMod player by frame age
verbose=0       ; 1 = log frame statistics
```

## Logs and troubleshooting

- RDR2 plugin: `GarrysRedemption.log` next to the `.asi`, or in `%TEMP%` when the game folder
  is not writable (it is not under `C:\Program Files`).
- GMod module: `GarrysRedemption_gmod.log` in `garrysmod/lua/bin/` (same fallback), and GMod's
  console (`garrysmod/console.log` with `-condebug`).
- **No HUD or guns over RDR2**: RDR2 is in exclusive fullscreen, or fills the monitor without
  the NVIDIA setting (see "Install from a release", step 4).
- **The status line says "version mismatch"**: the `.asi` and the GMod module are from
  different builds. Install both from the same release.
- **Nothing to grab in a town**: `gr_proxies` should list the people around you; if it says
  0, the plugin log's `world:` line says what RDR2 answered.
- **CTRL+R** in RDR2 reloads the plugin when an empty `ScriptHookRDR2.dev` file is next to
  `RDR2.exe`.

## Known issues

- Looking around in GMod mode can stutter on some setups (DESIGN.md, "The look, measured").
- GMod's picture is drawn in a window over RDR2, so exclusive fullscreen does not work, and a
  full-monitor RDR2 on NVIDIA needs the driver setting above. Drawing inside RDR2's own frame
  would fix both; not built.
- In towns, RDR2's own crates and barrels mostly have no stand-in in GMod and cannot be
  grabbed; people, animals and wagons can.
- A thrown RDR2 wagon passes through people.
- Typing in GMod's text boxes assumes a US keyboard layout; Esc stays with RDR2, so leave a
  text box by clicking elsewhere.
- Semi-transparent effects (smoke) only add light over RDR2's picture.
- Only tested with RDR2 on Vulkan and an NVIDIA card.

Not yet checked by hand: grenades, rockets and bolts against trees; wheels and thrusters on
wagons; hoists; ropes on people; possessing horses and animals; RDR2's long guns in view.
[docs/TESTING.md](docs/TESTING.md) has the in-game test steps for every feature.

## Build

```
powershell -File tools\build.ps1
```

This builds `protocol/` (tests and tools), `rdr2/` (`GarrysRedemption.asi`) and
`gmod/module/` (`gmcl_gr_win64.dll`), then runs the C++ and Python tests. CMake does not
need to be on `PATH`: the copy bundled with Visual Studio is found automatically.

The GMod module needs Facepunch's module headers, which are fetched once per machine:

```
git clone --depth 1 --branch development https://github.com/Facepunch/gmod-module-base gmod/module/third_party/gmod-module-base
```

The RDR2 plugin links without the Script Hook SDK (its import library is generated from
`rdr2/scripthook_imports.def`). If you unpack the SDK into `rdr2/sdk`, its own library is
used instead.

To build one project by hand, run CMake from inside its folder:

```
cd rdr2
cmake --preset default
cmake --build --preset release
```

## Install a build into the games

Set the two game folders once, then package:

```
setx GR_RDR2_DIR "<folder that holds RDR2.exe>"
setx GR_GMOD_DIR "<folder that holds gmod.exe and the garrysmod folder>"
powershell -File tools\package.ps1
```

| File | Goes to |
|---|---|
| `GarrysRedemption.asi` | next to `RDR2.exe` |
| `gmcl_gr_win64.dll` | `garrysmod/lua/bin/` (GMod only loads binary modules from there) |
| `gmod/addon/` | `garrysmod/addons/garrys_redemption/` |

Without the variables, `package.ps1` only fills `dist/` and writes `dist/GarrysRedemption.zip`.
If RDR2 is under `C:\Program Files` (where the Rockstar launcher puts it), Windows asks for
administrator rights for the one copy into that folder.

## Development tools

| | |
|---|---|
| `tools/fake_rdr2.py` | Stands in for the RDR2 plugin: lets the GMod side be tested alone |
| `tools/fake_gmod.py` | Stands in for the GMod module: lets the RDR2 side be tested alone |
| `tools/memdump.py` | Read-only dump of the shared memory (`--watch`, `--json`, `--all`) |
| `protocol/build/Release/gr_probe.exe` | Console host or guest running the real C++ link code |
| `rdr2/build/harness/Release/asi_runner.exe` | Runs the real `GarrysRedemption.asi` without RDR2, against a stand-in for Script Hook (`tools/fake_scripthook`) |
| `protocol/build/Release/gr_screen.exe` | Saves what the primary monitor really shows (half size BMP) (GDI screen grabs can return a stale picture of a full-screen RDR2) |
| `tools/overlay_dump.py` | Turns a `gr_overlay_dump` file into PNGs (colour and alpha) |
| `tools/gmod_live_test.py` | Starts GMod and tests the module in the real game against `fake_rdr2.py`, with nobody at the keyboard |

## Repository layout

| | |
|---|---|
| `protocol/` | `gr_protocol.h` (the shared memory layout both sides include), units, the link and seqlock, C++ tests |
| `common/`, `cmake/` | The logger and compiler settings both C++ sides share |
| `rdr2/` | The RDR2 plugin (`GarrysRedemption.asi`), C++20 |
| `gmod/module/` | The GMod binary module (`gmcl_gr_win64.dll`), C++20 |
| `gmod/addon/` | The GMod addon (Lua): `lua/gr/` one file per feature, `lua/entities/` |
| `launcher/` | The release's launcher (`Launcher.cs`) |
| `tools/` | Build, package and test scripts, stand-ins for each game |
| `docs/` | Design notes, the natives used, in-game test steps |

## Contributing

Issues and pull requests are welcome. Read [CLAUDE.md](CLAUDE.md) first: it has the rules
(story mode only, no game assets, every native verified, one protocol header) and the code
style. Test a change in the real games and put the steps in the commit message.

## Legal

Fan project. Not affiliated with or endorsed by Rockstar Games, Take-Two Interactive, Valve or
Facepunch Studios. Red Dead Redemption and Garry's Mod are trademarks of their owners. No
game assets are included; you must own both games. Code under the MIT licence: see
[LICENSE](LICENSE).

The Q menu's list of RDR2 model names is not in this repository: `tools/gen_models.py`
generates it at build time from [rdr3_discoveries](https://github.com/femga/rdr3_discoveries).
