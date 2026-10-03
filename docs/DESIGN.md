# Design notes

Decisions made while building, and answers to the open questions in `CLAUDE.md`. Each
answer says how it was measured, so it can be measured again.

## Bridge protocol (version 1)

**Wire format.** Everything in shared memory is in RDR2 conventions: metres, m/s, world
space, rotations as RDR2 "rotation order 2" Euler degrees (x pitch, y roll, z yaw). Only
the guest (GMod) converts, with `protocol/gr_units.h`. The floating origin never goes on
the wire: the guest holds it in doubles.

**Layout (21116 bytes).** `GrHeader` (64 bytes: two `GrPeer`), `GrHostFrame` (19572),
`GrGuestFrame` (1480). Host frame: frame counter, time, dt, FOV, player ped position,
`GrInput` (256-bit key bitmap, running mouse totals, cursor, screen size), up to 256
`GrEntity`. Guest frame: player position, velocity, eyes, FOV, up to 32 `GrDriven`.

**No shared header to race over.** Each side owns one `GrPeer` block and writes only its
own: magic, protocol version, shm size, pid, epoch, state, reason, heartbeat. `GrPeer` and
`GrHeader` are frozen for all versions so mismatched builds still recognise each other. A
side is connected when the other block has the magic, is not GONE, has a heartbeat that
was *seen to move* within 2 s, and reports the same version and size. A block left by a
dead process never moves, so it never counts.

**A version mismatch is reported, not hidden.** The mapping name carries no version. Both
sides stay mapped, publish `GR_PEER_REFUSED` with `GR_REASON_VERSION`, and exchange no
frames. If an older build created a smaller mapping, only the header is mapped; once that
build is gone the mapping is dropped and recreated at the right size (retry once a second).

**Refuse is latched.** `Link::Refuse(GR_REASON_MULTIPLAYER)` cannot be undone for the life
of the object. The RDR2 plugin keeps its heartbeat going so GMod can show the reason.

**Grab and release are state, not events.** A proxy GMod is simulating is listed in
`driven[]` for as long as GMod owns it. Leaving the list is the release, and the last
velocity sent is the throw velocity, so nothing is lost to a skipped frame. Events that
cannot be state (damage, spawn, remove) will need a ring with sequence ids: milestones 6
and 7, not in the protocol yet. Neither are collision chunks or the overlay frame (the
overlay is far too big for this block and gets its own mapping).

**`GR_GUESTF_PLAYER_VALID`.** Found by running the two fakes together: the host moved its
ped to the guest's position before the guest had anchored, the guest then anchored to
that, and both ended up at (0,0,0). The host ignores the guest's player fields until this
flag is set, and the guest sets it only after anchoring to `GrHostFrame.ped_pos`.

**Python reads the header instead of copying it.** `tools/gr_layout.py` parses the region
between `GR_LAYOUT_BEGIN` and `GR_LAYOUT_END`. `--emit-check` turns the parse into
static_asserts that the protocol build compiles, so a parser/compiler disagreement fails
the build. The Python `Link` in `tools/gr_shm.py` does duplicate the handshake logic of
`gr_link.h` (the fakes are Python); `PythonAgainstCpp` in `tools/tests/test_bridge.py`
keeps the two in step. Change one, change the other.

**Angle conversion.** Source yaw = RDR2 yaw + 90, Source pitch = -RDR2 pitch, roll
unchanged. `protocol/tests/test_units.cpp` proves this against rotation matrices built
separately for each convention. The RDR2 side assumes GTA V's order-2 convention
(`Rz(yaw) * Rx(pitch) * Ry(roll)`, heading 0 = +Y, positive pitch = up). **Not verified
in-game.** Milestone 2 must check it: face north and read the logged heading, look up and
read the logged pitch.

## Player sync (milestone 2, protocol version 2)

Version 2 added `time_us` and `anchor_serial` to both frames and made `ped_pos` the ped's
feet. Layout is now 21132 bytes.

- **GMod drives, RDR2 follows** (`rdr2/src/player_sync.h`): the player ped is hidden,
  frozen, moved to GMod's feet and turned to GMod's yaw every frame; a script camera
  (`CREATE_CAM("DEFAULT_SCRIPTED_CAMERA")`) renders from GMod's eyes with GMod's FOV. RDR2's
  controls are off except the passthrough keys (`rdr2/src/input_passthrough.h`).
- **Anchor, not teleport**: the guest makes the GMod player's spot the ped's feet by moving
  the floating origin (`gmod/module/src/guest.h`). Nothing moves in GMod, so no server-side
  module was needed.
- **Anchor serial**: never 0, new on every plugin start and every handback. The host only
  follows guest frames carrying the current one, so an anchor from before a handover can
  never move the ped after it.
- **Frame age**: both sides stamp `gr::NowUs()` (the performance counter, shared across
  processes); the host extrapolates GMod's position by the frame's age, capped at 50 ms
  (ini `smooth`). Not yet judged by eye.
- **Ground following until milestone 5** (ini `follow_ground`): GMod's flat ground is draped
  over RDR2's (`GET_GROUND_Z_FOR_3D_COORD`); only height above it (jump, noclip) carries
  over. Seen: GMod flat at 94.80 while the ped followed the terrain 94.79 to 95.16.
- **Handback stands the ped on the ground** first, or a ped left underground falls
  through the world.
- **Input**: keys by `GetAsyncKeyState`, mouse from RDR2's own raw input seen through a
  `WH_GETMESSAGE` hook; a key bitmap and running mouse totals go over the bridge.
  `input.lua` maps keys through the user's own GMod binds. Measured: 500 counts right
  turned 20.2 degrees (20.1 predicted), 200 down tilted 8.1 (8.05 predicted).
- **A torn read** leaves the caller's frame alone (`Link::Read` copies through scratch).
- **CTRL+R**: `InputCapture::Remove` waits on an in-hook counter, not the window thread
  (blocked during a reload, which pinned the DLL). Seen: one CTRL+R unloads and frees the
  DLL, the next reloads it and GMod anchors within a frame.

Found in the games:

- A kept-active GMod behind other windows turns its own view from the cursor (it drifted
  to straight down). `input.lua` holds the view and rebuilds the user command from the
  bridge alone while anchored.
- RDR2's pause menu and map stop its script thread. GMod drops to "waiting" and anchors
  again when they close; `player.lua` keeps the pitch when the ped still faces GMod's yaw.
- GMod's game menu pauses single player and is invisible. While anchored it is refused
  (`OnPauseMenuShow`) and closed (`gui.HideGameUI`, once a second, from `PreRender`).
- `gm_flatgrass` spawns on a platform 512 units above the grass (z -12800).
- RDR2's window class is `sgaWindow`; the player ped model `0x0D7114C9`, origin 1.000 m
  above the feet.

## Proxies (milestone 3, protocol version 3)

- `rdr2/src/world_sync.h` lists the nearest 64 peds within 60 m (`worldGetAllPeds`),
  typed person, animal or horse. `gmod/addon/lua/gr/proxies.lua` gives each an invisible
  proxy (8-sided prism for people, box for animals; mass 80, 60, 450).
- Idle proxies follow RDR2. Picked up or punted by the physgun or gravity gun, a proxy is
  GMod's: it is listed in `GrGuestFrame.driven` and RDR2 ragdolls the ped and sets its
  velocity to the proxy's plus 12/s times the distance between them (capped at 40 m/s).
  Dropped, it flies until slower than 40 u/s for 0.5 s, or until proxy and ped are 60
  units apart for 0.25 s (RDR2 hit something GMod does not have), or 8 s.
- `GrHostFrame.ground_shift` carries how far PlayerSync raised the player to follow RDR2's
  ground; entity heights go over the wire as real RDR2 heights and the guest subtracts it.
- One module image serves both GMod realms: `gmsv_gr_win64.dll` (the same build) loads the
  `gmcl_` file and hands it the server's Lua state. Both realms run on one thread in single
  player (logged).
- Measured: held ped within 0.05 m of its proxy, 2.9 m behind at the peak of a fast swing,
  caught up in 0.3 s; a ped lifted onto a wagon canopy; physgun freeze held within 0.01 m.

## Overlay (milestone 4)

GMod's HUD, menus, viewmodel and physgun beam over RDR2. Measured 2026-10-02, 1920x1080,
RDR2 at 135 fps: GMod 148 fps with the overlay on (150 off), capture 0.009 ms in GMod's
Present, overlay draw 0.05 ms, about 12 ms from GMod's Present to the overlay.

**What GMod draws** (`gmod/addon/lua/gr/overlay.lua`): while GMod has the player, no skybox
and no opaque entities; colour and depth are cleared to transparent as the translucent pass
begins, after the world's brushes and before the physgun beam. Depth-to-alpha is turned off
(the viewmodel's alpha was its depth). Halos are skipped (they redraw a copy of the frame).
The HUD and VGUI write no alpha of their own, so while they draw alpha writes are forced on
with an "over" alpha blend: from `PreDrawHUD` for the HUD, and from an invisible
full-screen panel kept behind all others for VGUI, whose paint pass resets render state
(an override set in `PreDrawHUD` did not reach the spawn menu). The frame is then
premultiplied colour with true alpha, additive glow included.

**Capture** (`gmod/module/src/d3d9_capture.h`): the module patches `Present` and `Reset`
in the vtable of GMod's own D3D9 device. That device has a vtable of its own on the heap: a
throwaway device's vtable was patched first and caught no call. The device is found
through the d3d9 objects `shaderapidx9.dll` keeps in its data (`GetDevice` on the first
one that is a resource, query or shader).

**GPU path** (`gpu_share.h`): Direct3D 11 makes three textures with shared handles; GMod's
plain D3D9 device opens them by passing the handle to `CreateTexture` (how OBS captures D3D9
games) and `StretchRect`s each frame into the next one; an event query says when a copy is
done. Nothing comes back to the CPU. The first version read the back buffer back to system
memory instead: the driver waited inside Present for each transfer, 8 to 26 ms, and GMod
fell to 35 to 100 fps. That path remains as a fallback if the handle cannot be opened.

**Display** (`overlay_window.h`): a thread in the module owns a click-through, topmost,
never-activated window laid over RDR2's client area, showing a premultiplied-alpha
DirectComposition swap chain; a pixel shader copies the frame in. Shown only while RDR2 is
the foreground window. Needs RDR2 windowed or borderless. If GMod's resolution differs from
RDR2's the visual is stretched; start GMod at RDR2's size (`-w 1920 -h 1080`), since Lua
may not run `mat_setvideomode`.

**Menus** (`input.lua`): while VGUI has the cursor, RDR2's mouse moves a cursor handed to
VGUI with `gui.InternalCursorMoved` every frame, buttons click with `gui.InternalMouse*`,
and keys go to a focused text box (`gui.InternalKeyCodePressed` and `InternalKeyTyped`, US
layout). The cursor is drawn into the frame by `overlay.lua`, and `gui.MousePos` and
`input.GetCursorPos` are wrapped to report it (tooltips use them).

**In step with RDR2 (2026-10-02).** The overlay used to show its view later than RDR2
showed the same view, so in fast turns GMod's things slid against RDR2's world.
`tools/overlay_lag.py` measures it: a steady turn with `gr_terrain_debug 1`, one frame grabbed
mid-turn and one still; the shift of RDR2's picture and of the red wall panels between them,
over the turn rate, is the lag. Noisy (take the first five samples), but clear: about 13 to
25 ms. Copies are now announced right after Present too and wake the overlay thread with an
event (capture to screen 6.5 ms average, from 6 to 8.5); raising GMod's GPU scheduling class
(`gr_overlay_gpu_priority`) and the overlay device's GPU thread priority changed nothing
measurable but cost nothing either. What fixed it: GMod renders its view `gr_view_lead_ms`
(20) ahead, extrapolated from the turn rate (`player.lua`, a `CalcView` hook), so it lands
where RDR2's view is by the time it is on screen. Measured trail: lead 0 about +13 ms, 20
about -1, 30 about -14. Holding RDR2's view back instead would have added the same to the
whole world's mouse lag. The viewmodel stays attached (seen mid-turn).

SkyCraft, for comparison, reads Minecraft's frame back asynchronously into shared memory
and draws it from a hook on Skyrim's DXGI Present; its host draws the menu cursor.

## RDR2 stops the player (protocol version 4)

Until GMod has RDR2's world as collision (milestone 5), RDR2 resolves the player's
collisions itself (`PlayerSync::Collide`): each frame, rays at 0.5, 1.0 and 1.6 m above the
feet go from where the player was held to where GMod put it, plus 0.3 m (GMod's hull is
0.61 m wide), against the map, vehicles and objects (not peds or foliage). On a hit with a
wall-like normal the move keeps only its part along the surface; if that is blocked too,
the player stays put. `GrHostFrame.block_serial/pos/normal` tell GMod, and
`gr/collide.lua` (server, `FinishMove`) pushes its player back out along the normal and
drops the velocity going into the surface. Noclip, and jumps of more than 3 m in a frame,
skip it. Measured: walking into a tent stopped 0.2 m from the wall with both players at
the same spot; walking at about 45 degrees slid 4.4 m along it. The rays are synchronous
(`START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE`): three per frame, six when sliding,
only while moving.

## The viewmodel in RDR2's light (protocol version 5)

GMod lit the viewmodel with gm_flatgrass's daylight, so at night in RDR2 the gun glowed as at
noon. `rdr2/src/light_sync.h` works out the light at the player and sends it in the host
frame (`light_dir`, `light_color`, `ambient_color`, `light_flags`); `overlay.lua` lights the
viewmodel and hands with it (`render.SuppressEngineLighting`, one directional local light,
an ambient cube brighter from above). RDR2 has no native for the sun's direction: it comes
from the clock (rises east at 6, sets west at 18 across the southern sky; the moon on the
opposite arc), an approximation of RDR2's sky. A ray towards the light every 6 frames
(map, vehicles, objects, foliage) says whether the player is in shade, which removes the
direct light and dims the ambient; an interior (`GET_INTERIOR_FROM_ENTITY`) does the same.
Colours settle over about a third of a second. Real shadows (a fence's shadow falling
across the gun) are not possible while GMod draws the gun: only RDR2's renderer knows where
they fall. `gr_viewmodel_light 0` turns it off.

Seen at 3 AM in camp: the gun went from daylight-bright to dark and moonlit, its glowing
parts still glowing. **Not yet seen in daylight**: the dawn, noon and sunset values are a
first guess (noon a near-white sun at 1.7; a low sun orange and down to 55 % strength, with
the sky's ambient turning warm; twilight fading to the moon between sun elevations -0.1
and 0.25). The shade ray's foliage flag (256) is also unverified.

RDR2's frame rate in the camp at 1920x1080 with all of this running: 111 to 114 fps with
the overlay on or off (no measurable difference).

## RDR2's ground in GMod (milestone 5, protocol version 6)

- **Chunks.** `rdr2/src/terrain_sync.h` samples heightfields of 16 x 16 one-metre cells
  (17 x 17 heights) with map-only rays straight down, and keeps the chunks whose middle is
  within 48 m of the player in 64 slots (`GrShared.chunks`, dropped beyond 64 m). Slots are
  state with a serial each and their own seqlock; the guest copies only the slots whose
  serial changed. `gr/terrain.lua` makes each one a `gr_chunk` (PhysicsFromMesh, static,
  server only, never transmitted); the module builds the triangle table in C++.
- **Two rays per column at most.** The low ray starts 2 m above the chunk's reference
  height (the player's feet when it was sampled) and reaches 80 m down: it finds the floor
  under roofs and trees. Only if it finds nothing does a ray come down from 150 m above:
  a hill higher than the player. Chunks with holes are sampled again after 2 s (collision
  not loaded yet), and chunks sampled 12 m or more off the player's height again once the
  player is within 24 m of them.
- **Measured: 3.1 to 4.2 us per synchronous ray** in RDR2 (`terrain:` lines in the plugin
  log), so the 1 ms budget is about 250 rays a frame and a chunk takes 1 to 2 frames. 28
  chunks around the player were built within a few seconds of anchoring. That answers the
  open question on shape tests for this use: synchronous is cheap enough; async not tried.
- **HOME.** gm_flatgrass has its own ground (grass at z -12800, the spawn platform's top
  at -12288, walls at +-15360, ceiling 15360, measured with traces). The GMod player is
  held at HOME (0, 0, 0), in the map's empty sky, while it waits to anchor, and anchors
  there. Until the chunk under it is built it may not fall.
- **Settle.** The first anchor was made with the client's copy of the position, 33 units
  above where the server held the player; the ground then sat inside the player's hull and
  Source called it stuck. The client now anchors at HOME itself, and after every anchor the
  server stands the player on the built ground (a trace 2 m up and down).
- **Floating origin.** More than 8000 units from HOME across or 6000 up or down, the
  server rebases: `Native.Rebase` moves the origin, the player and the GMod-owned proxies
  move back by the same amount and the chunks are placed again (the origin serial changed).
  The client realm sees the player's new position a frame or so later; the module
  recognises a position nearer the old spot than the new one for 120 frames and shifts it
  too, so RDR2's camera never jumps (`guest_rebase_keeps_rdr2_positions_and_fixes_a_stale_player`).
- **Rebase seen in the game:** in noclip, the player moved to 4000 then 8200 units east;
  RDR2 put the ped at x -89.14 then -9.13 (156.2 m from the start, as predicted), the
  rebase brought the GMod player back to (0, 0, 0) and RDR2's position did not move. Back
  again the same way. A false "fell through" rescue on the first try (the chunk under the
  player was the one from before the teleport) is fixed: it is looked up in FinishMove.
- **Ground draping is off** while the guest sets `GR_GUESTF_TERRAIN`; `ground_shift` is 0.
  PlayerSync::Collide still stops the player at walls until vertical surfaces are in.
- **Seen in the game:** walked off the ridge west of the camp, fell with GMod gravity down
  the slope (24 m in all), lost 20 health to GMod's fall damage, camera on the slope. The
  user confirmed cliffs and jumping work.

### Walls (protocol version 7)

- After a chunk's heights, one horizontal ray along each 1 m grid edge it owns (its low x
  and low y sides: 512 per chunk), 0.5 m above the higher end, or 1.5 m if that finds
  nothing. A hit steeper than 45 degrees becomes a `GrWall` (up to 96 a chunk): a vertical
  panel through the hit, up to 0.5 m each way along the surface, cut short to 6 cm by
  probes across it (perpendicular rays 0.3 m in front to 0.3 m behind) so a doorway keeps
  its width; from 0.3 m under the ground to the top found by a ray down 1 cm behind the
  face (1 m above the ray if none: a thin plank). Panels the probes find nothing for on
  either side (a curved or broken surface) are dropped. The module adds each as two
  triangles to the chunk's mesh.
- **Measured:** about 1400 rays a chunk with walls (Valentine), 3.2 to 3.4 us each; 28
  chunks with 478 walls in 10 s at the 1 ms budget.
- **Seen in Valentine** with `gr_terrain_debug 1` (the client draws the slots over RDR2
  through the overlay: ground green within 12 m, walls red): panels on the barn, the
  church, the houses, telegraph poles and tree trunks, tops at the roof lines. With
  `gr_collide 0` (GMod no longer pushed back by RDR2's rays) the GMod player stopped at the
  barn's wall on its own. A crate thrown at 1200 units/s at a wall 299 units away bounced
  back and lay 102 units along the throw.
- **Rays both ways.** Whole stretches of wall had no panels: RDR2's rays do not hit the
  faces of a solid they start inside, and many buildings are one closed box, so edge rays
  going only +x and +y missed every face turned that way. A miss is now cast back from B
  to A. Walls around the same spot went from 478 to 577 and the barn's long face is
  covered; about 2400 rays a chunk, 3.3 us each, 28 chunks in 10 s.
- **Not right yet:** interior walls are in too (they are real
  walls, but cost panels), and chunks sampled from 12 m above (flying in) had low roofs in
  the ground mesh: chunks are now sampled again once the player is 2.5 m off their
  reference height.

### RDR2's objects (2026-10-02)

- The scan also lists up to 128 of RDR2's own objects within 40 m (`worldGetAllObjects`, the
  pool gone through every 15 frames) after the people, as `GR_ENT_OBJECT` proxies: thrown
  GMod props and proxies stop at crates, barrels, fences and carts, and the physgun can pick
  them up. Left out: longest side under 0.3 m or over 8 m, boxes over 40 m^3, attached,
  invisible or collision-disabled objects, and models a ray against objects does not hit
  (down through the box's middle, else along its longest side; decided once per model).
  That last test removed bushes (3.7 x 3.4 x 2.2 m boxes every few metres around the camp)
  and ground debris.
- They are solid to props, not to the player (`COLLISION_GROUP_WEAPON`): RDR2's rays stop
  the player at them already, and a player inside a box would be stuck.
- **Ground rays hit objects too.** The big rock beside the camp is an object: map-only rays
  left a 16-cell hole there, and after a plugin reload the GMod player fell through it in a
  loop (170 rescues). Now crates and rocks are ground the player stands on, as in RDR2. A
  rescue whose "last stood" spot has no ground below it puts the player on top of the
  ground instead.
- Seen: 128 object proxies at the camp (boxes on the crates, tables, rock); a melon thrown
  at 800 units/s at a crate stopped at its box; the player stands on the rock.

## Weapons, damage and explosions (milestone 6, protocol version 8)

- **Events.** Hits and explosions are not state, so they travel in a ring of 32 `GrEvent`
  in the guest frame with a sequence number each; the host applies each number once
  (`rdr2/src/combat_sync.h`). It syncs to the ring only from a real guest frame: synced to
  the zeroed frame it had before its first read, a reloaded plugin replayed old bullets.
- **Bullets are RDR2's.** A GMod bullet that hits a proxy (`gr/weapons.lua`,
  `EntityTakeDamage`) becomes `SHOOT_SINGLE_BULLET_BETWEEN_COORDS` from the GMod eye
  (0.6 m out, past the hidden ped) to the bone of the ped nearest the hit and 0.75 m on,
  owned by the player's ped. RDR2 does the rest: blood, reaction, headshots, death, and the
  law. Seen: a ped 14 m away went from 75 health to dead; the minimap went red, then
  WANTED, $25 bounty. Damage is GMod's times `gr_damage_scale` (3).
- **Blows** (crowbar, fists, thrown props: not bullet, not blast) become
  `APPLY_DAMAGE_TO_PED` on the nearest bone and, when the blow carries force, a ragdoll and
  a push (GMod's damage force over the proxy's mass). Crush damage to a proxy GMod is
  throwing, or from the ground, is left to RDR2, which hurts its ragdoll itself.
- **Explosions.** Any GMod blast (`DMG_BLAST` reaching anything, or a grenade, rocket or
  satchel being removed) becomes one `ADD_OWNED_EXPLOSION` (dynamite, scaled by radius)
  where it went off; reports of one blast within 120 units and 0.2 s are merged. Proxies
  take no GMod blast damage: RDR2's explosion hurts them. Seen: a frag grenade 12 m ahead
  became a dynamite blast in the same spot.
- **The player's damage.** The hidden ped is still shot at (lawmen) and burnt. Each frame
  its health loss since last frame's refill becomes GMod points in
  `GrHostFrame.player_hurt`, and it is healed again; the GMod player takes the damage.
  First version compared against the maximum and killed the GMod player in a second: the
  refill does not always reach it (229 of 250). Losses within 0.6 s of a GMod explosion
  within 15 m are ignored (GMod already hurt its player). Seen: GMod health falling from
  100 to 69 under the lawmen's fire.
- **Respawn in place.** A dead GMod player respawns at gm_flatgrass's spawn, which the
  floating origin would have turned into a trip far under RDR2's ground; with the terrain
  on it comes back where it last stood.
- **Checked by hand (2026-10-02, clicks through the bridge, on a spawned townsman and deer):**
  crowbar 30 a swing; fists did nothing (GMod's fists only hurt what has health above 0: proxies
  of people, animals and horses now have 100, never used) and then 30 a punch; a shotgun blast
  at 4 m put two pellets in and killed. Animals: the bullet aimed at GMod's hit point missed
  (RDR2's box for the deer model is a thin slab off the body; health 15 -> 15). Animals and
  horses are now shot at their root, with the damage applied directly if the bullet still
  misses (seen: a deer 80 -> 58).
- **Sound (protocol 11).** GMod's gun sound and RDR2's audible bullet both played on a hit, and
  a miss made no sound in RDR2, so its people never heard it. Now every trigger pull sends
  `GR_EVENT_SHOT` (one per tick, however many pellets): one audible, harmless RDR2 bullet along
  the aim. Hit bullets are silent (`isAudible` false), and `gr/sounds.lua` mutes GMod's firing
  sounds (`Weapon_*.Single/Double/Burst`) while GMod has the player (`gr_mute_gmod_gunshots`).
  Not judged by ear.

## Spawning (milestone 7, protocol version 9)

- **The Red Dead tab** (`gr/spawn.lua`, client) lists people, horses, animals, vehicles and
  objects by model name, with a search over all of them; a name that is not in the list
  can be typed and spawned with Enter. No pictures: they would be the game's. The names
  come from femga's rdr3_discoveries lists, which carry no licence, so
  `tools/gen_models.py` makes `gr/models_data.lua` on the building machine and it is not
  committed. Objects are the `p_` models of the generic, interior, urban and vehicle_props
  folders (7390); the rest of that list is pieces of the map.
- **Source tabs hidden.** `spawnmenu.GetCreationTabs` is wrapped to return only Red Dead and
  Weapons (`gr_hide_source_tabs 0` brings the rest back). Source props, NPCs and vehicles
  cannot be seen over RDR2 until milestone 9.
- **Spawn and remove are events** in the same ring as hits (`GR_EVENT_SPAWN`,
  `GR_EVENT_REMOVE`). The server traces from the eye (through people and animals: a
  spawned person right in front once put a cart on the player), sends the model's joaat
  hash, the spot and the way to the player, and makes an undo entry for the spawn's id.
  `rdr2/src/spawn_sync.h` asks for the model each frame until it loads (10 s at most, no
  waiting), tells ped, vehicle and object apart by the model, stands its box on the spot,
  gives a ped a random outfit (invisible without), turns vehicles side on, and reports
  each outcome in `GrHostFrame.spawn_results` (unknown model, timed out, full at 128,
  failed). Up to 128 spawned entities are tracked and flagged `GR_ENTF_SPAWNED`.
- **Removing.** Undo removes by id. The remover marks the proxy (`CanTool`) and the real
  entity goes when the proxy does; so does a spawned entity's proxy removed by the cleanup
  menu. Proxies that leave because they are out of range carry `gr_dropping` and leave the
  entity alone. Cleaning up everything (or "RDR2 entities") also sends remove-all, which
  reaches spawned entities too far away to have a proxy. A spawned wagon's team is deleted
  with it.
- **Vehicles and objects as proxies.** The scan lists vehicles (Script Hook's
  `worldGetAllVehicles`) and the spawned objects with the peds, nearest 64 in 60 m. Their
  proxies are boxes; GMod-driven ones are set to the proxy's position and rotation every
  frame, with its velocity, and their physics woken (`ACTIVATE_PHYSICS`): pushed by
  velocity alone, a new crate only turned. Seen: a thrown crate rose 4 m in both games,
  RDR2 within a frame of GMod.
- **`gr_clear_wanted`**: an amnesty, `GR_EVENT_CLEAR_WANTED`. Seen: bounty $25 to $0.
- **Seen in the games (2026-10-02):** a cow and a townsman (outfits on), a cart, a
  wagon and a coach with teams, crates; undo three times; the remover; cleanup removing
  three entities and the coach's horses; a made-up name refused with a notification;
  the tab's categories, a click spawning, typing in the search.
- **Known limits:** (a plugin reload forgetting what was spawned is fixed, see "Surviving a
  plugin reload"); the remover on any proxy deletes that RDR2
  entity, story characters included.

## Toolgun tools (milestone 8, protocol version 10)

- **Constraints are state.** `gr/tools.lua` reads GMod's own constraint records
  (`constraint.GetTable`) on every proxy each tick and lists the ones joining two RDR2
  entities, or one and the world, in `GrGuestFrame.constraints` (64). `rdr2/src/constraint_sync.h`
  makes what is new and undoes what has gone, so undo, the remover, cleanup and a broken weld
  need no code of their own. Each constraint entity gets an id for as long as it exists.
- **Weld:** `ATTACH_ENTITY_TO_ENTITY` at the first entity's current offset in the second's
  space (`GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS`) and the difference of their rotations
  (exact for turns about the vertical); to the world, `FREEZE_ENTITY_POSITION`. A proxy taken
  by the physgun takes the proxies welded to it (an idle proxy's motion is off and GMod's weld
  would nail the taken one to it); RDR2 drives only the unattached one, the attached follow.
  Seen: two welded crates thrown, 91 units apart before and after in RDR2.
- **No-collide:** `SET_ENTITY_NO_COLLISION_ENTITY` both ways, every frame while listed.
- **Rope:** `_ADD_ROPE_2` (type 6, not networked, 31, -1) and `_ATTACH_ENTITIES_TO_ROPE_2`
  with each end in its entity's own space, as a working RedM script does
  (github.com/Big-Yoda/redm-hanging). The GTA V way (`ADD_ROPE`, `ATTACH_ENTITIES_TO_ROPE`
  with world points) made rope handles and nothing visible. A rope to the world ends on an
  invisible frozen `p_cannonball01x`. Seen: an RDR2 rope from a crate up to its anchor. RDR2's
  rope did not lift the crate though it was shorter than the gap; what holds a thrown proxy
  is GMod's own length constraint, and RDR2 follows the proxy.
- **Thruster:** not a constraint. Pushing RDR2's entity did nothing: a crate ignored
  `APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS` (type 1, scaled by mass; logged 54 m/s^2 for 2 s,
  speed 0) with or without `SET_ENTITY_DYNAMIC`, as milestone 7 found set velocities only
  turned one. So while a `gmod_thruster` welded to a proxy is on, GMod takes the proxy (the
  physgun's path), GMod's thruster moves it, and RDR2 follows. Seen: a crate rose 6 m in 2 s
  in RDR2. (A force-1500 thruster on a 175 kg crate gives 8 m/s^2: less than gravity in
  either game.)
- **Duplicator:** `duplicator.RegisterEntityClass("gr_proxy")`: a pasted proxy spawns its
  model (`gr_model`, from the entity list) in RDR2 through `Spawn.At`, with an undo entry;
  no GMod entity is made, so constraints between pasted proxies are not pasted. Seen: a
  copied welded pair pasted as two new crates.
- **Remover:** done in milestone 7.
- **Not checked yet by hand:** the tools were driven through GMod's own constraint functions
  (`constraint.Weld`, `Rope`, `NoCollide`, a scripted thruster) and duplicator calls, not by
  clicking the toolgun. Welds and ropes on people are untried. The attach and rope natives'
  undocumented flags are as community scripts pass them (`TODO(verify)` in `natives.h`).

## GMod's things in RDR2's world (milestone 9)

- **No hook in RDR2.** CLAUDE.md planned depth-aware compositing with RDR2's depth buffer
  (a Vulkan hook). Instead GMod's own copy of RDR2's surroundings is the depth: the terrain
  chunks with their walls (milestone 5) and a box per person, animal, horse, vehicle and
  object proxy. `gr/world_render.lua` runs right after the overlay clears GMod's frame
  (the opaque pass is skipped, as before): it draws those occluders into depth only, then
  every GMod entity with a model (props, NPCs, ragdolls, weapons lying about, a thruster on
  a crate), lit with the viewmodel's RDR2 light, then (in third person) the player.
- **Seen (night, Horseshoe Overlook):** an oil drum, a crate and a chair spawned in GMod
  stood on RDR2's ground beside RDR2's crates; a drum pushed 25 units into the ground showed
  only its top. GMod at about 200 fps.
- **Occluder material:** `render.SetColorMaterial` wrote no depth, and the chunk triangles
  were culled seen from above (`gr_world_occlude 2` shows the occluders: only slopes above
  the eye showed). An `UnlitGeneric` with `$nocull` does both.
- **Exposure:** with GMod's world wiped, its HDR auto-exposure climbed to its top
  (tone-mapping scale 6.0, logged) and props glowed at night. The overlay pins the scale to 1
  each frame: RDR2's light is what the light values already are. This also darkens the
  viewmodel against before; the day colours in `LightSync::Tick` were tuned (by guess)
  under the 6x exposure.
- **Third person** (`gr_thirdperson`, `gr/thirdperson.lua`): the camera behind the right
  shoulder (`gr_thirdperson_distance`, 110 units); `player.lua` sends that view as the eyes,
  so RDR2's camera goes there too; the player model is drawn scaled to Arthur's 1.85 m and
  the viewmodel is hidden. Seen: GMod's player with the physgun among RDR2's crates.
- **The Source tabs are back** in the spawn menu (`gr_hide_source_tabs` now defaults to 0).
- **Limits:** only what GMod has a copy of hides GMod's things: the ground and walls within
  about 50 m, and the proxies as boxes. RDR2's grass, bushes, trees (not map collision) and
  anything farther do not; a person's box is rough. GMod's things cast no shadows on RDR2
  and get none. The overlay is about 12 ms behind, so props swim a little in fast turns.
  The third-person camera has no collision (it can go into a wall).

## GMod in the background, at any resolution (2026-10-03)

The user set RDR2 to 3840x2160 borderless on a 3840x2160 monitor and lost GMod. Three
separate things were wrong, found in this order:

- **RDR2 makes a new window when its display mode changes** (seen: the handle changed on
  every switch). The plugin kept the dead handle, so RDR2 was never "in front": no input
  reached GMod and RDR2's pause menu stuck half open. `FindGameWindow` now lets go of a dead
  window and finds the new one (checked once a second).
- **GMod drew at the size it was started with** and the overlay stretched it. The module now
  compares RDR2's client size (`GrInput.screen_w/h`) with its back buffer and asks the engine
  for `mat_setvideomode W H 1` through `WM_COPYDATA` (`window_fit.h`). Seen: 1920x1080 to
  3840x2160 in under a second, capture and shared textures follow through `Reset`. GMod at
  3840x2160: 241 fps average. A size GMod refuses is asked for once only.
- **A borderless RDR2 that covers its monitor exactly is not composited**: the driver sends
  its Vulkan frames straight to the screen, and the overlay (a window on top) is never
  drawn. The user saw RDR2 with no HUD. GDI screen grabs showed the overlay over a stale
  RDR2 frame, which hid the problem; `tools/gr_screen` (desktop duplication) shows either
  that or the live game without the overlay.
  **Changing RDR2's window to stop this is a dead end** (`KeepComposited`, now off unless
  `keep_composited=1`), three versions tried:
  one pixel taller: composited, but RDR2 took 3840x2161 for its resolution, saved it, and
  fell to 1024x768 when the user opened its settings; moved up one pixel: composited, but
  Windows put the taskbar over the game; a one pixel border outside the screen (picture
  still exactly the monitor): the captures flip between composited and not.
  **The supported way is the driver's**: NVIDIA's "Vulkan/OpenGL present method: Prefer
  layered on DXGI Swapchain" for RDR2 makes its Vulkan frames an ordinary DXGI swap chain,
  which Windows composites with windows on top. **Confirmed** after the user set it and
  RDR2 was restarted: three `gr_screen` captures in a turn, each a live RDR2 at 3840x2160
  with GMod's HUD and physgun over it, RDR2's window untouched.

GMod's window is hidden while RDR2's window exists once the two have linked, and shown again
when RDR2 is gone. Exclusive fullscreen (RDR2's "fullscreen" screen type) still cannot show
the overlay.

## The launcher, and the two games as one (2026-10-03)

`launcher/Launcher.cs` is one C# 5 file compiled by the `csc.exe` of .NET Framework 4, so
neither building nor running it needs anything installed. It installs from the folders
beside it (the release layout of `tools/package.ps1`).

- The NVIDIA present method is read and set through NVAPI's driver settings
  (`nvapi64.dll`, `nvapi_QueryInterface`). Function ids, the setting id (`0x20D690F8`,
  1 = layered on DXGI) and the structures' sizes, versions and field offsets were taken from
  NVIDIA's public headers by compiling a program against them; buffers are filled by offset.
  Seen: it reads 1 on this machine after the user set it in the control panel. Writing was
  not run here (it is the user's driver).
- PLAY starts Garry's Mod only when RDR2's window exists, and puts it behind the other
  windows. It first minimised it: a minimised Garry's Mod presents no frames (seen: 0
  presents, no HUD), so the module also un-minimises before hiding.
- A layout in pixels was cut off at 150% display scaling; every position goes through `S()`.

The games as one: the overlay is only wanted while RDR2's frames are fresh (under 150 ms
old), so it is gone the moment RDR2 opens its pause menu or map instead of two seconds
later; Garry's Mod quits five seconds after RDR2's window is gone (`gr_quit_with_rdr2`);
the launcher closes the other game when one closes.

## Balloons, wheels, hoists and ropes on people (2026-10-03)

`tools.lua`: a constraint that pulls without holding rigid (rope, elastic, winch, hydraulic,
muscle, axis, motor, ball socket) makes GMod take the proxy for as long as something only
GMod can simulate acts through it: one of GMod's own entities on the other end that is not at
rest (a balloon never is; a wheel while it turns), a length-changing constraint to the world
(a hoist), or a rope on a person, horse or animal. RDR2 then follows the proxy as for the
physgun (a living ped as a ragdoll). Seen: four balloons of force 1500 on a spawned farmer
took him (`world: GMod took ped`) and carried him up at 3600 units a second. Not seen yet:
wheels on a wagon, a hoist, a rope dragging a person behind a horse.

## Vehicle third person and animal ragdolls (2026-10-03)

- **GMod vehicles in third person** (user request): `thirdperson.lua` uses GMod's own vehicle
  view (behind by the vehicle's size times 1 + its camera distance, which the wheel changes;
  Ctrl switches, `GM:VehicleMove`), now also sent to RDR2's camera, starting in third
  person on entering. The mod's CalcView hooks had kept GMod's vehicle view from ever
  running, and RDR2 was given the driver's eyes. The server's wall ray (gr_cam_free) leaves
  the vehicle out. Seen: a jeep from behind, driving, Ctrl back to first person.
- **Animals fell stiff and slow** (user report). Held in the physgun a bull hung upright in
  an animated pose; dropped, it landed on its feet and knelt down slowly, while a farmer
  dropped beside it fell limp. Its ragdoll (type 0) also ended after about 2 s. Ragdoll type
  1 for non-human peds: it hangs limp and lands with the farmer (`SetPedToRagdoll`).
- **Dead animals** (user, after that: "animals still don't ragdoll properly"). A carcass
  ignores velocity, forces, `ACTIVATE_PHYSICS`, `SET_ENTITY_DYNAMIC` and refuses
  `SET_PED_TO_RAGDOLL`; it is not frozen (`_IS_ENTITY_FROZEN` 0). Held it hung rigid with its
  legs straight out; thrown it stayed in mid-air while a dead farmer flew off. Dead people
  are fine. Now a dead animal is brought back to life (`RESURRECT_PED`) while GMod has it,
  so it is a ragdoll like a living one, and killed again (`SET_ENTITY_HEALTH` 0) the moment it
  is let go, so it falls on as a body. Seen: a dead deer hangs limp, flops through the air
  and lies on its side. Not checked: what this does to its pelt and loot. An animal's
  ragdoll ends after about 2 s whatever time is asked for; it is started again at once.

## Weapons against RDR2's trees, walls and rocks (2026-10-03, protocol version 20)

User: "I want the weapons to be able to collide with objects like trees, houses". GMod's copy
of RDR2's world is the terrain chunks (ground and the map's walls near the player) and the
near ring (trunks and objects within a few metres), so GMod's bullets and projectiles went
through everything else. People behind cover were already safe: the bullet that hurts them
is RDR2's own, fired along GMod's line, and RDR2 stops it.

- **Bullets** (`weapons.lua`, `GR_EVENT_IMPACT`): every bullet that hit none of RDR2's people
  and none of GMod's own things goes on in RDR2 as a harmless, silent RDR2 bullet along the
  same line, 312 m long. RDR2 stops it at its first tree, wall or rock and marks it (seen:
  splinters and a hole in a pine's bark). Single bullets of RDR2's guns, and of GMod's with
  `gr_gmod_shots_in_rdr2`, are marked by their audible SHOT bullet already. `gr_bullet_impacts`.
- **Projectiles and thrown things** (`gr/surfaces.lua`, `rdr2/src/probe_sync.h`): each tick
  the 16 nearest things moving faster than 150 units/s (grenades, the AR2 ball, rockets,
  bolts, props, ragdolls, GMod's vehicles, proxies GMod has) list the 0.25 s of travel ahead
  (`GrProbeRequest`). RDR2 tests each against the map and its objects without proxies, and
  foliage that two rays 0.5 m apart hit at about the same distance (a trunk, not branches),
  once per guest frame, and answers with the first surface (`GrProbeResult`). GMod stands a
  32-unit square, 24 deep, frozen physics (`gr_surface`) on it; it goes after 3 s unasked,
  and all of them when the origin moves. Measured: the AR2 ball found a pine 99 units ahead,
  bounced off 12 units from the bark (its radius), then off the rocky ground the same way.
  About 50 probes a second while one ball flies; RDR2's frame time unchanged.
- The pine in Strawberry was one of RDR2's objects, so the trunk test is not seen yet.

## The physgun in towns, wagons, and New Austin (2026-10-03, protocol version 19)

The user, after the thirteenth session: the physgun "stopped working", "mostly when entities
are doing an animation" (town people); wagons and stagecoaches "bugging and going flying in
the air with no physics"; "I randomly die after teleporting to a different city".

- **No proxies in towns.** Script Hook RDR2's pool walk (`worldGetAll*`) answered 0 on most
  frames in Valentine and Strawberry, so no one there had a proxy and the physgun had
  nothing to take. `WorldSync::NearPoint` asks the game itself (`_GET_ENTITIES_NEAR_POINT`)
  whenever a pool answers nothing. The plugin log's `world:` line every 10 s counts what
  was found and how often the fallback ran. Objects from the fallback are few (the itemset
  held 48 of 256), so RDR2's own crates and fences in towns are mostly not proxies.
- **Animated people.** Someone sitting is attached to the seat by RDR2, and attached
  entities were skipped as GMod welds: the person never moved. Others in a scenario refused
  the ragdoll. `WorldSync::Free` detaches (unless GMod lists a weld for it), ends the
  scenario or seat at once and allows the ragdoll; again if the ragdoll is refused for 5
  frames. Measured: a person freed from a scenario followed its proxy to within a unit over
  255 units of lift. Under a roof or tarp RDR2 stops the person while GMod's proxy goes on.
- **The physgun's wide grab** (thirteenth session) is for people only: on horses, wagons and
  objects it caught a coach's horse, or the bench next to a seated person, instead of the
  thing aimed at.
- **Wagons.** A wagon's box takes in its harnessed horses and the people on its seats. Taken
  by GMod, it started inside their proxies and was thrown out of them into the air, and RDR2
  copied the flight. People and animals that follow RDR2 are not solid to RDR2's wagons in
  GMod now (as with GMod's own vehicles). A thrown wagon therefore passes through people.
  The falling watchdog counted a wagon's height from its origin, a metre up on its wheels,
  and pushed standing wagons down; it counts from the bottom of the model's box now.
  Measured: a stagecoach lifted with its horses, swung and let go rolled down a slope and
  settled.
- **New Austin.** Arthur (`player_zero`, 0D7114C9) is shot dead on sight there in chapters 1
  to 6: one hit of 32 health, then dead, seconds after arriving at Armadillo and Benedict
  Point. Not the mod. `GR_HOSTF_ARTHUR` (protocol 19) tells GMod; the Q menu's teleport
  refuses Armadillo, Benedict Point, MacFarlane's Ranch and Tumbleweed for Arthur with a
  notice. Walking or flying there still gets him shot.

## The Red Dead tab's commands and the teleport (2026-10-03, protocol version 18)

User request: god mode, heal, lose all bounties and a teleport to each town, in the Q menu's
Red Dead tab ("Commands"). `gr/commands.lua`: `gr_god` (GMod's god mode: GMod's health
decides, so RDR2 cannot hurt the player either), `gr_heal`, `gr_clear_wanted` (existing),
`gr_teleport NAME`. Town positions from RDR2Mods.com's teleport list (Horseshoe Overlook
there matches the place earlier sessions reached); its Valentine entry is inside the
sheriff's office, so Valentine is the main-street spot used before.

The teleport is `GR_EVENT_TELEPORT` with an RDR2 position (not converted). RDR2 takes the
player (BusyReason "a teleport"), moves the ped and holds it frozen for 2.5 s while the place
loads, stands it on the ground and gives the player back; GMod anchors there afresh and
builds the new ground. Seen: to Valentine twice, standing on the new ground, health kept.

Found on the way: RDR2 caps the health it takes by the health core (250 asked, 150 kept), so
the health tie read back 60 % after a handover with no damage. `GiveHealth` now keeps what
was given and what was kept, and hands back GMod's own share scaled by what the ped lost.

## Thrown boats, trees and objects hanging in the air (2026-10-03)

User: "if I grab a boat and throw it in the air it will start floating, same with trees". Not
reproduced: spawned rowboats, a crate and two of RDR2's own objects, thrown up hard or let go
at rest in the air, over land and over the river, all fell (boats then floated on the water).
What can keep one of RDR2's world entities in the air and is now undone, when GMod takes it
and again when it lets go (`MakeFallable`): a moored boat's anchor (`SET_BOAT_ANCHOR` false),
a placed object's static state (`SET_ENTITY_DYNAMIC`,
`SET_ACTIVATE_OBJECT_PHYSICS_AS_SOON_AS_IT_IS_UNFROZEN`), gravity switched off
(`SET_ENTITY_HAS_GRAVITY`). For 4 s after the release a falling watchdog wakes and pushes
down anything still hanging above the ground and not in water, and logs it ("falling
watchdog: ... hung ... m up"): the next report should name the entity in the plugin log.

## Enemies attacking the GMod player (2026-10-03)

User: "enemies don't attack me in gmod mode". Two separate things, found with a spawned wolf
and `IS_PED_IN_COMBAT` counts (test code since removed):

1. **The hidden ped.** Invisible (`SET_ENTITY_VISIBLE` false), frozen and teleported every
   frame, it was a target RDR2's attackers went into combat with but never reached: wolves
   stood or circled a few metres off. With RDR2 in control the same wolf attacked Arthur at
   once. Taking away the hiding, the freeze, the script camera and the disabled controls one
   at a time changed nothing; with the ped visible, unfrozen and not teleported they
   attacked it. Now (`ped_mode` 1, the default) the ped is drawn with alpha 0 instead of
   hidden, is not frozen, and is walked after the GMod player with `SET_ENTITY_VELOCITY`
   (GMod's velocity plus 10/s of what it is behind), teleported only when it falls 0.75 m
   behind. Seen: wolves leaping at the player. `ped_mode 0` is the old way.
2. **Invincibility.** The game held the player invincible and undamageable
   (`GET_PLAYER_INVINCIBLE` 1, `_GET_ENTITY_CAN_BE_DAMAGED` 0) every frame, with GMod or RDR2
   in control, outside any mission (`GET_MISSION_FLAG` 0), with the installed NativeTrainer's
   INVINCIBLE and EVERYONE IGNORED off: wolves bit Arthur and his health stayed at 150 of
   250. Nothing in the plugin sets it. The plugin switches it off every frame while GMod
   drives, but it is set again before damage is applied (it reads 0 at the end of the
   plugin's frame and 1 at the start of the next), so in that state nothing can hurt the
   player. Earlier the same day RDR2's damage did reach GMod (1 of 250 logged), so it began
   during play. Not found: what sets it. Reloading the save is the thing to try; the log
   says "the game has made the player invincible" when it finds it.

## Third-person model opacity (2026-10-03)

User: "the opacity is too low and I can't see the head". The overlay shows GMod's frame by
its alpha, and a model writes its texture's alpha, which on skin and faces is a shine mask:
the head came out mostly see-through. `world_render.lua` now marks every pixel an opaque model
draws in the stencil buffer (alpha-tested hair that was cut away is not marked) and then
writes alpha 1 there, colour untouched, inside the model's on-screen box. Translucent
entities and ones coloured with alpha are left as they were. Seen: the head and hair solid,
no dark fringe. Applies to every GMod prop and NPC drawn over RDR2, not only the player.

## Pixelation while moving: the camera calls (2026-10-03)

The user: "when I stand still the resolution looks amazing ... when I move there is a lot of
pixelation". Full-size captures (`gr_screen OUT.bmp full`) of the same spot, standing and
strafing, showed it plainly: moving, the trees fell to blocky low-resolution edges, the look of
DLSS (and TAA) throwing away its history and showing its internal resolution.

The cause is how the script camera was moved: `SET_CAM_COORD`, `SET_CAM_ROT` and
`SET_CAM_FOV`, three calls every frame. Compared live at one spot, one strafe each (ini
`live_settings=1` re-reads `cam_mode` every 2 s so the player does not move between runs):
mode 0 (those three calls) blocky, twice; `SET_CAM_PARAMS` in one call (mode 1) and the
camera attached to the hidden ped (mode 2) as clean as standing. A still camera nudged 1 mm
every frame with mode 0 did not degrade, so it is not "any change of position". Why the
separate calls do it is not known. Mode 1 is the default (it also serves the third-person
camera); `cam_mode` in the ini (or %TEMP%'s) picks another. Checked afterwards in daylight and
at night: walking, strafing and turning stay sharp.

## Health and death (2026-10-03, protocol version 17)

GMod's health is the player's. RDR2's damage to the hidden ped comes off it (combat_sync.h)
and the ped is kept full while GMod drives. At the handovers they are tied:
`GrGuestFrame.player_health` goes to the ped when RDR2 takes the player (F9, a horse, a
scripted scene), and `GrHostFrame.ped_health` comes back when GMod anchors again (the module
latches it at the anchor: `TakeAnchorHealth`). The first anchor after the link connects (a game
started, the plugin reloaded) is a new session: full health.

Death: GMod's death beep is the HEV voice at `suitvolume` (0.25) on top of the game's volume,
inaudible from a hidden GMod; while linked the quiet one is left out (`PlayerDeathSound`) and
the client plays the same beep, beep, beep, flatline (`hl1/fvox`) at full volume. The player
comes back by itself `gr_respawn_delay` seconds later (3; a click is sooner), on the built
ground under the spot it died (`Terrain.RespawnPos`), or where it last stood.

## Collision with RDR2's world near the player (2026-10-03, protocol version 17)

The terrain chunks are map collision on a 1 m grid. The player walked through trees, posts,
thin rocks and RDR2's objects (whose proxies are not solid to the player), and
`PlayerSync::Collide` only snapped it back after the fact, never at a tree, and at bushes too
(bushes are objects). Now `near_sync.h` casts a ring of 16 rays (half each frame, 1.6 m) from
the player's feet: map and objects at 0.55 m and 1.3 m, objects only if WorldSync lists them
(its size and solidity filter leaves bushes out); foliage at 0.9 m and 1.8 m, counted as a
trunk only when both hit within 0.25 m of each other. Each hit is a panel in
`GrHostFrame.nearby`; `near.lua` stands an invisible `SOLID_OBB` box (`gr_blocker`, no physics
object) at each, so GMod's own movement stops and slides. A box that would start inside the
player stays non-solid that tick. Objects were taken out of `PlayerSync::Collide`'s rays.
Seen: walking into an oak stopped at its bark with 3 panels solid and no snap-backs, and
walking diagonally slid round it. Costs about 32 rays (~100 us) a frame. `gr_near_collide 0`
turns it off; `gr_near_status`.

Fast flight: noclip with Shift goes at `gr_noclip_fast_speed` (4000 units/s, ~76 m/s) while
linked (`fly.lua`, a shared `Move` hook); GMod's own Shift is 1500. Seen: 160 m in 2.5 s.

## The possess tool (2026-10-03, protocol version 14)

`GrGuestFrame.possess` names a ped and `possess_move` the way it should go (length: 1 walk,
2 run, 3 sprint). `possess_sync.h` gives the ped `TASK_GO_STRAIGHT_TO_COORD` to a point 25 m
ahead, again only when the way or pace changes or every 2 s, blocks its other AI events, and
keeps the player's hidden ped from colliding with it. GMod's player is in noclip at the
proxy; the camera is the third-person one around the proxy (its collision ray skips the
proxy: at first it hit it and the camera sat inside the head). Seen: a spawned farmer walked
and ran where W pointed, camera behind him. Not seen: horses, animals, jumping.

## RDR2's guns as GMod weapons (2026-10-03, protocol version 14)

`gr/sweps.lua` registers eleven weapons; `GrEvent.model` carries the RDR2 weapon hash of a
SHOT or BULLET, which `combat_sync.h` fires if `IS_WEAPON_VALID`.

The user wanted RDR2's guns themselves, picked from GMod's HUD, and GMod's own guns left
with their own sounds. So (protocol 15) GMod draws no viewmodel for these weapons and names
the gun in `GrGuestFrame.weapon`; `weapon_view.h` makes RDR2's weapon object of it
(`_CREATE_WEAPON_OBJECT`) and places it every frame 0.42 m ahead, 0.16 right and 0.17 below
the shown camera, turned yaw + 90 with the camera's pitch about its own Y (RDR2's gun models
point along +X: with the camera's yaw the barrel pointed right). Seen: the Cattleman in
view, pointing at the crosshair level and looking 30 degrees up. No hand, no reload
animation; a shot kicks it for 160 ms. Only these weapons send SHOT events now
(`gr_gmod_shots_in_rdr2` 0): GMod's guns make GMod's sound and no RDR2 gunshot.
Found on the way: SHOT events (type 7) were handed to the spawn code with the spawn menu's
events and dropped, so since protocol 11 no GMod shot was ever heard in RDR2 (the user's "no
gunshots"). Fixed; seen: `combat: shot heard, 34.2 m, weapon 169F59F7`.

## What the user changed after playing the twelfth session's build (2026-10-03)

- **Grabbed people are ragdolls again.** Held stiff (the section below) they stood in their
  bind pose, arms out, and the user wants a ragdoll. The stiff hold is now behind
  `hold_rigid=1` in the ini; by default a held ped is ragdolled and pulled to its proxy.
  Seen: entity flags RAGDOLL and DRIVEN, 1.3 units from the proxy.
- **The dead can be grabbed.** A body at rest ignored the velocity it was given. Its physics
  is woken every frame and it is put at the proxy if it trails by more than 1.5 m. Seen: a
  dead farmer followed its proxy 90 units up.
- **GMod's vehicles run people over.** A still proxy does not move, so a jeep that hit a
  person was thrown itself. People, horses and animals that still follow RDR2 are not solid
  to GMod's vehicles (`ShouldCollide`); every tick a moving vehicle takes the ones inside its
  box, throws them with its speed and sends RDR2 a HIT (`proxies.lua`, RunOver). Seen: a jeep
  at 900 units/s took a farmer, RDR2 logged 86 damage and a 6.6 m/s push, the jeep drove on.
  RDR2's objects and vehicles hit by any GMod thing are taken and pushed (`Proxies.Collide`).
- **A vehicle that falls off RDR2's ground** used to start an endless rebase (the player in
  it could not be moved back, the origin moved anyway): RDR2's player ended 900 km away.
  `terrain.lua` now moves the vehicle on a rebase, takes the player out of one that has
  fallen more than 6000 units, and never moves the origin unless the player moved.
- **The AR2's energy ball had a black square**: `world_render.lua` drew it as a prop.
  Glowing entities that draw themselves in the translucent pass are skipped there.
- **Third person: the gun hung under the arm**: the player is drawn scaled and the weapon
  was not. The weapon gets the same scale matrix.
- **RDR2's guns** are picked in the Red Dead tab's Weapons list; nobody is given one.

## The look, measured (2026-10-03, protocol version 16)

The user: "when looking around in GMod mode there is a kind of stutter, the movement is not
fluid". Measured in the plugin with a steady injected turn (2 counts every millisecond):

- `look:` lines (player_sync.h, Roughness): the spread of the camera's yaw step per RDR2 frame
  over its mean was 0.16 before and 0.14 with look prediction. Both are even: no frames
  without a turn followed by double ones.
- `pace:` lines (plugin.cpp): RDR2's frames are the same whoever has the player: 7.4 to 7.9 ms
  (about 128 fps at 3840x2160), longest 10 to 11 ms, none slow, with GMod; the same with RDR2.

So neither the camera nor RDR2's frame rate stutters. What differs in GMod mode is how the
picture reaches the screen: with the overlay window on top, Windows composites RDR2 at the
monitor's 240 Hz and RDR2's 128 unsynchronised frames a second land on it unevenly, with no
variable refresh; with RDR2 alone the driver shows each frame as it comes. Not measured (it
is on the monitor, not in either game). VSync in RDR2's settings makes the frames land
evenly; the lasting fix is to draw the overlay inside RDR2's own present (a Vulkan hook, or
a ReShade add-on since ReShade is installed here), so nothing is composited.

**Look prediction** was built on the way and kept (`gr_look_predict`): GMod reports which
of the host's mouse totals its angles include and the degrees per count; the host adds the
counts since. It takes a frame or two of mouse lag out of the camera.

**RDR2's gun in third person**: `GrGuestFrame.weapon_pos` is the right hand of the player's
model as drawn (scaled), and the gun is placed there along the aim. Seen from behind: the
Cattleman at the raised hand. RDR2 draws the gun and GMod's player is laid over RDR2's
picture, so the model always covers the gun where they overlap.

## Holding RDR2's people with the physgun (2026-10-03)

User report: RDR2's entities "fall" when grabbed. A held ped was a ragdoll whose velocity
was set towards its proxy every frame: it kept up with the beam (within 6 units carried at
150 u/s) but tumbled, hung upside down and flailed as Euphoria's falling behaviour, the
whole time it was held.

Now (`world_sync.h`): a **living** ped that is held (`GR_DRIVE_HELD`) or frozen is not a
ragdoll. It may not ragdoll, has no gravity, and is put at its proxy's position and angles
every frame, so it moves and turns exactly as the beam moves it, upright unless the player
turns it, still animated. Let go (`Loosen`): slower than 1.5 m/s with its origin under 1.6 m
above the ground it is set down and simply stands; otherwise it becomes a ragdoll with the
throw's velocity and follows its proxy's flight as before, until it settles and RDR2 stands
it up. Dead peds are handled as before (a ragdoll pulled along: a body hangs from the beam).

Seen in the game with a spawned `a_m_m_valfarmer_01`: held and lifted 90 units, upright
and steady; dropped from 4 m ("let go as a ragdoll"), fell and rolled down the slope; held
8 units up and released ("set down"), stood there. A deer ran out of view before the
pictures: the log shows the same path, not seen.

## The view lock (2026-10-03, protocol version 13)

User report: "when I move the camera in GMod mode, all of the objects placed from GMod
move". GMod drew its picture with its own newest view, moved ahead by a guess from the turn
rate (`gr_view_lead_ms`); RDR2 drew with the view it last received. The guess is wrong when
the turn speeds up or slows (one frame's mouse counts are lumpy), and the games draw at
different rates, so the two pictures were never of the same view and GMod's things slid.

Now the host publishes the camera it shows (`GrHostFrame.view_pos`, `view_rot`,
`view_serial`) and GMod's `CalcView` uses exactly those angles, first and third person
(`gr_view_lock`, default 1). Aim and bullets still use GMod's own newest angles. The host
can show a view `GrGuestFrame.view_hold` frames after publishing it (0 to 12,
`gr_view_hold_frames`), to give GMod's picture time to reach the screen.

Measured with `tools/overlay_lag.py 10 7` on two flat red props (unevenly spaced: a row of
evenly spaced ones aliased the tool's matching by a whole spacing), lead off:

| hold (RDR2 frames) | overlay trails RDR2 by (median of 7) |
|---|---|
| 0 | 0.0 ms (-11 to 12.5) |
| 3 | -17 ms |
| 6 | -39 ms |
| 9 | -55 ms |

**Only the angles are locked.** The first version drew from RDR2's camera position too, and
the user reported the character stuttering while moving. That position is the player's of a
few milliseconds ago, arriving at RDR2's frame rate, so whatever goes with GMod's player (its
model in third person, what the physgun holds) shook against the camera. Measured walking in
third person, the drawn player against the camera: 0.74 to 0.89 units rms, 5 to 7 units peak
to peak with the position locked; 0.05 rms, 0.7 peak to peak with GMod's camera on GMod's
own player (as before the lock). Walking leaves GMod's things a millisecond or two of travel
off RDR2's world, as it always did; it was turning that made them slide.

Checked for hitches while strafing (longest time without a new RDR2 frame, 6 s each, twice):
15 to 16 ms with GMod capped at 300 or 150 fps, scene probe on or off: neither adds hitches.
The 300 cap costs RDR2 about 5 % (163 against 171 fps).

So no hold, and no added mouse lag. This needs GMod's frames short: RDR2's view is one GMod
frame old when GMod draws with it (measured 10 ms at 95 fps). The user's `fps_max` was 150;
while linked it is set to 300 through `GR.Native.EngineSetting` (measured 253 fps) and put
back when the link goes. `gr_terrain_debug 1` halves GMod's frame rate: do not measure with it.

## GMod's things fitting into RDR2's picture (2026-10-03, protocol version 12)

User report: props from GMod did not fit RDR2's lighting, and trees and bushes did not hide
them. Three parts, none with a hook in RDR2:

- **Light matched to the picture.** `light_sync.h`'s light comes from the clock alone. The
  module's `scene_probe.h` copies a band of RDR2's window off the screen (5 to 60 % across,
  42 to 70 % down: ground level, left of the viewmodel), shrunk to 16 by 8, every 350 ms on its
  own thread, and keeps the average as a linear colour. `overlay.lua` scales RDR2's light so
  that ambient + half the direct light is `gr_light_match` (2.2) times the scene's luminance,
  eased over half a second, and gives the ambient half of the scene's tint. Measured at a
  cloudy noon on grass: scene (0.037, 0.064, 0.028), gain 0.48 (the clock's light was twice
  too bright). The viewmodel gets the same light.
- **Shadows.** Each thing within 24 units of the ground darkens it: a disc of black with
  alpha in the overlay, 1.7 times the thing's half width, full to 55 % of its radius and
  fading to the rim, pushed away from the sun, laid on the terrain with `GR.Native.GroundZ`
  (the chunk's own two triangles per cell, 3 units up so the depth copy of the ground does
  not cut it). A disc the size of the thing's foot was invisible under it.
- **Veils (trees and bushes).** GMod lists up to 16 of its things (`GrVeilRequest`: centre,
  radius). `veil_sync.h` casts 4 by 4 rays with the foliage flag (256) from the camera to a
  square at the front of each thing's sphere, 4 things a frame, and answers with a 16-bit
  mask. `world_render.lua` draws that square after the thing, multiplying what is in the
  frame (colour and alpha, premultiplied) by 1 minus each corner's eased share, furthest
  thing first. Seen: a cargo container 39 m away behind a pine hidden but for the gaps.
  Coarse (a quarter of the thing per cell), and only foliage: ground, walls and people are
  still hidden by depth.

Not built: a shade ray per thing (one at the player decides sun or shade for all), lamps
and fires as point lights, real cast shadows.

## GMod's sound (2026-10-02)

Since 2026-10-03 GMod's gunshots are heard again (`gr_mute_gunshots` 0): RDR2's bullet
between two points makes no gun sound, so with GMod's muted there was none (user report).

The user heard GMod's sound lag and sometimes cut off. GMod's window is never really in
front: `snd_mute_losefocus` (1) muted it whenever Windows said it lost focus (keep-active only
covers `WM_ACTIVATE`), and `snd_mixahead` (0.1) buffered 100 ms. While linked, `gr/sounds.lua`
sets them to 0 and 0.05 and puts the user's values back when the link goes. Lua may not set
either ("Command is blocked"), so `GR.Native.EngineSetting` hands GMod's own window the console
command as `WM_COPYDATA`, for those two names and plain numbers only. Seen: both set within a
second of linking. Not yet judged by ear.

## Surviving a plugin reload (2026-10-02)

CTRL+R used to make the plugin forget what it had made: spawned entities lost their flag
and object proxies, and welds and ropes were made again over the old ones. `rdr2/src/persist.h`
keeps the spawn list and the made constraints in a block on the process heap (a DLL unload
does not free it), found again through the `GR_PLUGIN_STATE` environment variable, with a
magic, version and size check. Written every frame (about 3.5 KB), restored at start.
GMod's side lost them too: with the link down for a moment every proxy was removed, and
its constraints with it. Proxies now stay while GMod is not anchored (30 s), and leave only
after RDR2 has not listed them for 1 s, 10 s for spawned or constrained ones. RDR2 deletes a
script's ropes when it unloads, so a rope is made again once after a reload. Seen: two
crates welded and roped to the world, two reloads, both games still have the weld and rope.

## RDR2's HUD in GMod mode

The user asked for RDR2's minimap and health, stamina and food cores to go while GMod has
the player and come back with RDR2, and every other part of RDR2's HUD to stay.
`HIDE_HUD_AND_RADAR_THIS_FRAME` was tried first: it hides everything, prompts and banners
too, which the user did not want. Now `DISPLAY_RADAR`, `_SHOW_PLAYER_CORES` and
`_SHOW_HORSE_CORES` (switches) are turned off every frame while GMod drives the ped and on
again once when RDR2 has it, and on at every plugin start (a CTRL+R while hidden cannot
call natives on the way out). Seen both ways with F9: in GMod mode no minimap or cores, the
WANTED banner stays; in RDR2 mode minimap, cores, banner and the "Pick up" prompt.

## Code shared by both sides

**Logger (`common/gr_log.h`).** `GR_LOG` formats into a fixed ring slot (no allocation, no
system call) and a writer thread does the file I/O. Full ring: the line is dropped and the
count is logged. `StopFromDllMain` exists because a thread cannot be joined under the
loader lock: the writer parks in `Sleep` and is terminated there.

**Repo layout additions.** `common/` (the logger), `cmake/GrCommon.cmake` (C++20, static
CRT, `/W4 /WX`, x64 check, include paths) and `tools/fake_scripthook/` are in addition to
the layout `CLAUDE.md` started with.

## RDR2 plugin

**Links without the Script Hook SDK.** The SDK may not be redistributed, so the build does
not need it: `rdr2/scripthook_imports.def` lists the six imported functions by decorated
name and the build makes an import library from it. No stand-in DLL is produced; at run
time the imports resolve against the real `ScriptHookRDR2.dll`. If the SDK is unpacked to
`rdr2/sdk`, its own `.lib` is used. `tools/build.ps1` compares the names with the exports
of the installed DLL when `GR_RDR2_DIR` is set.

**Test hook.** `plugin.cpp` reads the environment variable `GR_SHM_NAME_OVERRIDE` at
start-up so a test can point the plugin at its own mapping. Never set in normal use.

**Tested outside the game with a stand-in Script Hook.** `tools/fake_scripthook` builds
(as part of `rdr2/`) a `ScriptHookRDR2.dll` that exports the same six functions, compiled
from the plugin's own `scripthook.h`, and `asi_runner.exe`, which loads the real `.asi`
and plays the game: one script fiber resumed per frame, canned answers for the natives in
`docs/NATIVES.md`. It holds nothing of the real Script Hook or of the game, and it is built
into `rdr2/build/harness`, apart from the `.asi`, so it can never be packaged by mistake.
`RealAsiInHarness` in `tools/tests/test_bridge.py` uses it: the plugin connects to the fake
guest, reports a mismatched guest, and after a multiplayer session appears makes no further
native call at all (which is also why it cannot draw a message about refusing: the reason
goes to the log and to GMod).

**The game folder may not be writable.** The Rockstar launcher installs under
`C:\Program Files`, where a normal process cannot write. The plugin's log then goes to
`%TEMP%\GarrysRedemption.log` (seen working), and `tools/package.ps1` runs its one copy
into the game folder elevated, after a Windows prompt.

**Seen in the real game, 2026-10-01** (RDR2 1.0.1491.50, Rockstar launcher install, Script
Hook RDR2 v1.0.1491.17 loaded by its `dinput8.dll`): the whole milestone 1 test in
`README.md` passes. The status line draws; RDR2 first or GMod first both connect; killing
either side is noticed by the other within the 2 s heartbeat timeout and a restarted one
is connected to again; `gr_status` shows both counters moving with 0 torn reads.

Things learned about driving RDR2 from a script:

- Started as `RDR2.exe` from its folder, it goes straight into the last story save (about
  70 s to the first script frame). After being killed it stops at the main menu instead:
  the Story tab is already selected and Enter loads it.
- It does not close on `WM_CLOSE` within 20 s; tests kill it.
- Windowed and not in front, its script thread kept running at about 120 frames/s. Losing
  the foreground did not pause it.

## GMod side

**Module surface.** `require("gr")` loads `gmcl_gr_win64.dll`, which sets `GR.Native`:
`Tick()`, `State()`, `Stats()`, `SetVerbose(bool)`, `SetKeepActive(bool)`. Everything else
is Lua under `GR`. It compiled against the gmod-module-base headers as written and loads
in GMod 2026.09.17.

**The bridge only runs in a game this machine hosts**: single player, or as the listen
server host. Joined to someone else's server the module is not even loaded.

**The bridge ticks from `PreRender`, not `Think`.** See the measurements below: a paused
game stops calling `Think`.

## Open questions from CLAUDE.md

### Can GMod run fully hidden without throttling when unfocused? Yes, with one workaround.

Measured 2026-10-01 on GMod 2026.09.17 (main branch, 64-bit, `gmod_win64.exe`),
windowed 1280x720, single player on `gm_flatgrass`, with `gr_probe` (`gmod/addon/lua/gr/probe.lua`),
which counts calls of four hooks per second. The window was driven from outside with
`ShowWindow` and `SendMessage`.

| GMod's state | Think | Tick | PreRender | DrawOverlay | Game time |
|---|---|---|---|---|---|
| Engine believes it is active | 150/s | 67/s | 150/s | 150/s | runs |
| Engine believes it is inactive | 20/s | 20/s | 20/s | 20/s | runs |
| Game menu open (single player pauses) | **0** | **0** | keeps running | keeps running | **frozen** |

- **Window state does not matter.** Visible, behind other windows, minimised
  (`SW_MINIMIZE`) and hidden (`SW_HIDE`) all gave the same numbers. Nothing stalled and
  nothing paused by itself.
- **The throttle is a fixed 50 ms sleep per frame while the engine thinks it is
  inactive** (20 fps exactly), and that is too slow for player movement.
- **`engine_no_focus_sleep` does not exist in GMod** ("Unknown command" from the console
  and from the launch options), so the throttle cannot be turned off by a convar.
- **The engine's active flag is `WM_ACTIVATE` and nothing else.** Sending the game window
  `WM_ACTIVATE` with `WA_INACTIVE` drops it to 20 fps and makes `system.HasFocus()` false;
  `WA_ACTIVE` restores 150 fps and true, while the window is not the foreground window and
  while it is hidden. `WM_ACTIVATEAPP`, `WM_SETFOCUS` and `WM_KILLFOCUS` change nothing.
- The cursor clip rectangle stayed the whole desktop throughout, so an "active" GMod that
  is not really in front did not clip the cursor.

**Consequence, built and measured.** The GMod module subclasses the game window (class
`Valve001`) and, only while the bridge is connected, keeps the engine active: it swallows
`WM_ACTIVATE(WA_INACTIVE)` and sends `WM_ACTIVATE(WA_ACTIVE)` on connecting
(`gmod/module/src/keep_active.h`, tested in `protocol/tests/test_keep_active.cpp`). When
the link drops the engine is told the truth again, so GMod on its own behaves like GMod.
`gr_keep_active 0` switches it off. Measured with `tools/gmod_live_test.py`:

| GMod's state | Frames/s |
|---|---|
| Connected, keep-active on, `WM_ACTIVATE(WA_INACTIVE)` sent | 150 |
| Connected, keep-active on, minimised by Windows, another window in front | 150 |
| Connected, `gr_keep_active 0`, deactivated | 20 |
| Connected, `gr_keep_active 1` again | 150 |
| Not connected, not in front (keep-active off by design) | 20 |

**The kept-active engine leaves the cursor alone.** Nudging the cursor while GMod was kept
active but not the foreground window, and again while minimised, it stayed where it was
put: GMod did not recentre it. **Still to check in milestone 2:** whether it reads the
keyboard and mouse buttons itself in that state. If it does, its own input has to be shut
off, since all input is meant to arrive over the bridge.

**Consequence.** Single player is usable: the pause only stops `Think` and `Tick`, and
`gui.HideGameUI()` closes the menu from Lua. The bridge ticks from `PreRender`, which runs
in every state above. Whether the pause itself must be prevented (a paused GMod does not
simulate, so the player would freeze) is a milestone 2 question: either never let the game
menu open, or host with `maxplayers 2`, which does not pause.

Other things learned on the way:

- A convar created by Lua cannot be set from the launch options (`+gr_probe 1` is ignored:
  the convar does not exist yet when the command line is run).
- A running GMod accepts console commands through `WM_COPYDATA` (`dwData` 0, the command
  as an ANSI string), the mechanism Hammer uses. Useful for automated tests.
- The first launch after an update took about two minutes to reach the map; later ones
  four seconds.
- The frame rate was capped at 150 in every focused measurement, presumably by `fps_max`
  (not checked).

### Which API to hook first: Vulkan or DX12? Neither, for now.

This machine's RDR2 runs on Vulkan, borderless 1920x1080, with OptiScaler (as `dxgi.dll`)
and ReShade in its folder. The overlay (below) is a window of its own composited by
Windows over RDR2, so it hooks nothing in RDR2 and works whatever API RDR2 uses. A present
hook only becomes necessary for milestone 9 (depth-aware compositing needs RDR2's depth
buffer).

### Still open

- How many shape tests per frame RDR2 tolerates before stutter. Partly answered: a
  synchronous downward ray costs 3 to 4 us (milestone 5), so 250 a frame cost 1 ms.
- How to keep a ped ragdolled indefinitely while held without it dying or despawning.
  Partly answered: `RESET_PED_RAGDOLL_TIMER` every frame keeps it ragdolled while held (no
  long hold timed yet).
- Whether GMod's client and server Lua states (one process in single player) can share
  the one module or a `gmsv_` module is needed too: player teleports for origin rebasing
  are server-side. Decide in milestone 2.

## Verified facts about the tools

**Script Hook RDR2** (checked on dev-c.com 2026-10-01): current release v1.0.1491.17,
9 Feb 2023, "supported patches 1.0.1207.58/1491.17 and above". The installed game is
1.0.1491.50. From the SDK readme (v1.0.1207.73):

- Redistributing the SDK archive or `ScriptHookRDR2.dll` is not allowed. Link users to
  <http://www.dev-c.com/rdr2/scripthookrdr2/>. Never commit either.
- The SDK may only be used for scripts that work offline.
- Entities not set as mission entities can be deleted by the engine at any time: do not
  reuse their handles across frames without rechecking. This matters for milestone 3.
- An empty `ScriptHookRDR2.dev` file in the game folder enables CTRL+R to unload and
  reload scripts. It needs `scriptUnregister` in `DllMain`, which `dllmain.cpp` has.
- Also exported, and needed later: `worldGetAllPeds/Vehicles/Objects/Pickups(int* arr,
  int arrSize)`, `getScriptHandleBaseAddress`, `keyboardHandlerRegister`, `getGlobalPtr`,
  `getGameVersion`.

**GMod** (Facepunch wiki, checked 2026-10-01). Functions used by the addon, all confirmed
to exist in the realm they are used in: `hook.Add`, `include`, `AddCSLuaFile`, `MsgC`,
`Color`, `CreateClientConVar`, `GetConVar`, `ConVar:GetBool`, `ConVar:GetString`,
`cvars.AddChangeCallback`, `concommand.Add`, `util.IsBinaryModuleInstalled`, `require`,
`game.SinglePlayer`, `game.MaxPlayers`, `game.GetMap`, `LocalPlayer`,
`Player:IsListenServerHost`, `IsValid`, `system.HasFocus`, `SysTime`, `CurTime`,
`file.Append`, `os.date`, `gui.IsGameUIVisible`, `gui.ActivateGameUI`, `gui.HideGameUI`,
`jit.arch`, the globals `BRANCH`, `VERSIONSTR`, `CLIENT`, `SERVER`, and the hooks `Think`,
`Tick`, `PreRender`, `DrawOverlay`, `InitPostEntity`.

- `include` refuses a client file that was not `AddCSLuaFile`'d, except in `lua/autorun/`
  and a few other folders that are sent automatically. Hence the shared `gr_init.lua`.
- Binary modules must be in `garrysmod/lua/bin/`. They cannot ship inside an addon.
- `require` raises if the module fails to load: call it through `pcall`.
- `BRANCH` is `"unknown"` on the main branch. The main branch now ships a 64-bit
  executable (`gmod_win64.exe`), so no beta branch is needed.

**gmod-module-base** (`Facepunch/gmod-module-base`, branch `development`, headers read
2026-10-01): `GMOD_MODULE_OPEN()`, `GMOD_MODULE_CLOSE()`, `LUA_FUNCTION(name)`; `LUA` is a
`GarrysMod::Lua::ILuaBase*` with `PushSpecial(SPECIAL_GLOB)`, `GetField`, `SetField`,
`CreateTable`, `Push`, `Pop`, `PushCFunction`, `PushNumber`, `PushString`, `PushBool`,
`GetBool`, `IsType`, `CheckType`; type ids are `GarrysMod::Lua::Type::Table`, `::Bool` etc.
