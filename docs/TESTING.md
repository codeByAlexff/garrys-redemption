# Testing in the games

Every feature is tested in the real games first: Red Dead Redemption 2 in story mode and
Garry's Mod (64-bit), linked, with nobody but you at the keyboard. The stand-ins in `tools/`
(`fake_rdr2.py`, `fake_gmod.py`, `fake_scripthook/`) are for regression tests afterwards and
know nothing of most features since milestone 5.

Each section is one feature: do the steps, see what is described. The plugin's log
(`GarrysRedemption.log`, next to the `.asi` or in `%TEMP%`) and GMod's console say more when
something is off. `git log` has the steps that were used when each change went in.

## Milestone 1 test

1. Start GMod (64-bit) and start a single player game on `gm_flatgrass`. The console says
   `[Garry's Redemption] bridge: module loaded.` and then `waiting for RDR2`.
2. Start RDR2 and load story mode. Top left of the screen: `Garry's Redemption: connected
   to GMod (guest frame N)` with N counting up. GMod's console says `connected to RDR2`.
3. In GMod's console, `gr_status` prints both sides' frame counters.
4. Quit GMod. RDR2's line changes to `waiting for GMod`. Start GMod again: it reconnects.
   The same works the other way round.

Either game can be started first.

## Milestone 2 test: player sync

GMod owns the player; RDR2 shows the world from the GMod player's eyes.

1. Both games running and connected as above. RDR2's line says `connected to GMod, which
   has the player (F9 hands it to RDR2)`. Arthur is hidden and the view is first person.
2. In RDR2: W/A/S/D walk, Shift sprints, Space jumps, Ctrl crouches, V toggles noclip, the
   mouse looks around. These are your GMod binds, so rebinding in GMod works.
3. Walk over a hill: the view follows RDR2's ground.
4. Tab, left click and the other RDR2 keys do nothing in RDR2.
5. Esc, P and M still open RDR2's pause menu and map. On closing, GMod takes the player back
   in the same place.
6. F9 hands the player to RDR2 (Arthur appears, standing on the ground, and RDR2's controls
   work) and back to GMod again.

Keys that stay with RDR2 (`rdr2/src/input_passthrough.h`): Esc, P (pause menu), M (map),
F9 (hand the player over). Everything else goes to GMod.

## Milestone 3 test: proxies and the physgun

1. GMod has the player. `gr_proxies` in GMod's console lists the RDR2 people and horses
   around you.
2. Aim the physgun at a person and hold left click: they ragdoll and follow the beam. The
   wheel pushes and pulls. Let go: they fly, land, and get up.
3. Right click while holding: they freeze in the air. Pick them up again to unfreeze.
4. Horses work the same way.

## Milestone 4 test: HUD, viewmodel and menus

RDR2 must run windowed or borderless, at any resolution: GMod follows it. Change RDR2's
resolution or screen type in its settings: within a few seconds the HUD fits again.

1. GMod has the player: GMod's crosshair, health and physgun viewmodel show over RDR2.
2. Physgun a person: the blue beam runs from the gun to them.
3. Hold Q: the spawn menu shows with a cursor. The mouse moves the cursor, clicks work,
   and you can type in the search box. Let go of Q to close it (after clicking away from a
   text box, as in GMod). C opens the context menu the same way.
4. Alt-tab away from RDR2: the overlay hides; it comes back with RDR2.
5. `gr_overlay_status` in GMod's console shows the path in use ("GPU path" in the module
   log) and frame rates; `gr_overlay 0` turns the overlay off.

## RDR2's walls stop the player (start of milestone 5)

1. Walk into a tent, a wagon or a building wall in RDR2: you stop at it instead of passing
   through, and GMod's player is held at the same spot.
2. Walk into it at an angle: you slide along it.
3. Noclip (V) still goes through everything.
4. `gr_collide 0` in GMod's console, or `collide=0` in the ini, turns it off.

## RDR2's ground in GMod (milestone 5)

1. Walk off a ledge or a cliff: you fall with GMod's gravity and land on RDR2's ground
   below (GMod's fall damage included), instead of snapping down to it.
2. Jump on a slope, walk up and down hills: the view follows the ground.
3. Walk more than about 150 m in one direction: nothing visible happens (GMod quietly moves
   its origin; `gr_terrain_status` counts the rebases).
4. Walk into a building's wall: GMod's player stops at it by itself (try `gr_collide 0`,
   which turns off the older push-back from RDR2's rays). Physgun a person into a wall:
   they hit it in GMod too.
5. `gr_terrain_debug 1` draws what GMod has over RDR2: the ground near you in green, walls
   in red.
6. `gr_terrain_status` in GMod's console: chunks built, walls, whether the ground under
   the player is built, rebases. `gr_terrain 0` goes back to the old flat ground draped
   over RDR2's.

## Weapons (milestone 6)

1. Take a gun in GMod (Q menu, Weapons) and shoot a person in RDR2: they react, bleed and
   fall as from an RDR2 bullet, and witnesses and the law treat it as your crime.
2. Hit someone with the crowbar or a thrown prop: they are hurt and knocked down.
3. Throw a grenade: RDR2 makes a real explosion where it goes off.
4. Get shot by RDR2's lawmen: GMod's health goes down. Die and you come back where you were.
5. `gr_weapons_status` in GMod's console; `gr_damage_scale` (default 3) sets how hard GMod's
   weapons hit in RDR2; `gr_weapons 0` turns it all off.

## Tools (milestone 8)

1. Weld two RDR2 crates (or a crate and a wagon) with the toolgun's weld; throw one with the
   physgun: the other goes with it in RDR2. Weld one to the ground: it stays put.
2. Rope two entities, or one to the ground: a real RDR2 rope joins them.
3. No-collide two entities: they pass through each other.
4. Put a thruster on a crate and fire it: the crate flies in RDR2.
5. Copy entities with the duplicator and paste: new RDR2 entities of the same models.
6. Undo (Z), the remover and cleanup undo all of these in RDR2 too. `gr_tools_status`;
   `gr_tools 0` turns it off.

## Balloons, wheels and hoists; possess; RDR2's guns

1. **Balloons** (toolgun) on a spawned person, a horse or a wagon: it rises in RDR2 (a
   person or animal as a ragdoll). Pop them or undo: it falls. **Hoist**, **winch** and
   **elastic** to the ground or a tree hold and lift it the same way; a plain **rope** on a
   person or animal ties it up (to a horse or wagon: it is dragged).
2. **Wheels** on a wagon: while they turn (their numpad keys) the wagon rolls on them.
   **Thrusters** on a wagon push it as they push a crate.
3. **Possess** (toolgun, Red Dead category, or `gr_possess` with the crosshair on it): left
   click a person, horse or animal to become it. W/A/S/D steer it relative to the camera,
   the sprint key runs or gallops, the walk key walks, jump jumps. Right click
   (`gr_unpossess`) lets go.
4. **RDR2's guns**: Q menu, **Red Dead** tab, **Weapons** (also the Weapons tab under "Red
   Dead Redemption 2"). Nobody starts with one; a picked gun is in GMod's weapon HUD. The gun on screen is
   RDR2's own model, and it fires in RDR2 as that very gun with RDR2's sound: the plugin log
   says `weapon: RDR2's model of weapon 169F59F7 is in the player's view` and `combat: shot
   heard, ... weapon 169F59F7` (the Cattleman). `gr_rdr2_gun_gmod_sound 1` adds a
   Half-Life 2 gunshot. GMod's own guns keep GMod's sounds and make none in RDR2
   (`gr_gmod_shots_in_rdr2 1` makes RDR2's people hear them too).

5. **GMod's vehicles** run RDR2's people and animals over: drive a jeep into one. It is
   thrown as a ragdoll and hurt in RDR2 (`combat: ... hit for N damage`), and the jeep drives
   on. RDR2's crates and wagons are knocked away by a vehicle or a thrown prop.
6. **Physgun**: a grabbed person is a ragdoll in the beam (`hold_rigid=1` in the plugin's ini
   holds them stiff instead), and the dead can be picked up.

## GMod's props and the player in RDR2's world (milestone 9)

1. Spawn props from the spawn menu's Source tabs: they stand on RDR2's ground, lit by RDR2's
   sun or moon. Push one into a slope or behind a wall: the part behind is hidden.
2. Press the key bound to GMod's `thirdperson` (L by default here; B for `firstperson`): the
   camera goes behind your GMod player in both games, and L again brings it back.
   `gr_thirdperson` does the same from the console; `gr_thirdperson_distance` sets how far.
3. `gr_world 0` stops drawing GMod's things; `gr_world_occlude 2` shows the surfaces that hide
   them (white).

## The viewmodel in RDR2's light

1. At night the physgun is dark and moonlit; in daylight it is lit warm from the sun's side.
2. Step into the shade of a building or under a roof: the gun darkens over a moment, and
   brightens again when you step out. Indoors it has no sunlight.
3. `gr_viewmodel_light 0` in GMod's console goes back to GMod's own lighting.
4. GMod's props and the viewmodel are lit to match RDR2's picture as measured off the
   screen: `gr_light_match` (2.2; higher is brighter, 0 uses RDR2's clock only). Props darken
   the ground under them (`gr_world_shadow 0` to stop) and RDR2's trees and bushes hide them
   (`gr_world_veil 0` to stop).

## The physgun in towns

1. In a town (Valentine, Strawberry), `gr_proxies` lists the people around you. The plugin
   log's `world:` line every 10 s counts what it found and how often it had to ask the game
   directly because Script Hook's pool walk answered nothing.
2. Physgun someone sitting on a bench or a porch: they come off the seat as a ragdoll
   (`world: N freed for GMod (attached to a seat or object)` in the plugin log).
3. Physgun someone leaning, working or eating: the same (`in a scenario or vehicle`).

## Wagons and stagecoaches

1. Spawn a stagecoach (Red Dead tab): it comes with its horses.
2. Physgun the coach, swing it and let go: it flies with its horses and lands; it does not
   shoot up into the air. Aiming at the coach catches the coach, not a horse.

## The Red Dead tab's commands

1. Q menu, Red Dead tab, Commands: God Mode, Heal Player, Lose All Bounties, and a teleport to
   each town. Each works from the console too (`gr_god`, `gr_heal`, `gr_clear_wanted`,
   `gr_teleport NAME`).
2. Playing as Arthur, Armadillo, Benedict Point, MacFarlane's Ranch and Tumbleweed are
   refused with a notice: RDR2 has Arthur shot on sight in New Austin.

## Weapons against RDR2's world

1. Shoot a tree or a house wall with a GMod gun: RDR2's bullet marks appear on it (the plugin
   log: `combat: GMod bullet N that hit nobody made an RDR2 bullet`).
2. Fire the AR2's energy ball at a tree or a house: it bounces off. Grenades bounce, rockets
   explode against it, a thrown prop stops at it. `gr_surfaces_status` counts the squares
   standing on RDR2's surfaces; the plugin log's `probe:` line counts RDR2's answers.
3. `gr_bullet_impacts 0` and `gr_surfaces 0` turn each off.

## Glowing effects

1. Fire the AR2's energy ball, a crossbow or a rocket: the glow shows over RDR2's picture
   with no black square around it.
