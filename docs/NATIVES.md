# RDR2 natives used

Every native the RDR2 plugin calls, with its hash and what it was checked against. Nothing
is added here from memory: look it up, then add the row, then add the constant to
`rdr2/src/native_hashes.h`.

Source for all rows: alloc8or's RDR3 native database
(<https://alloc8or.re/rdr3/nativedb/>, data file
`https://raw.githubusercontent.com/alloc8or/rdr3-nativedb-data/master/natives.json`),
checked 2026-10-01. A name starting with an underscore is one the database marks as
unofficial: the hash is real, the name is the community's.

"In-game" means seen doing what the row says in RDR2 1.0.1491.50 with Script Hook RDR2
v1.0.1491.17.

## Called by the plugin

| Native | Hash | Signature | Used for |
|---|---|---|---|
| `NETWORK::NETWORK_IS_IN_SESSION` | `0xCA97246103B63917` | `BOOL ()` | Story-mode-only check |
| `NETWORK::NETWORK_IS_SESSION_STARTED` | `0x9DE624D2FC4B603F` | `BOOL ()` | Story-mode-only check |
| `NETWORK::NETWORK_IS_GAME_IN_PROGRESS` | `0x10FAB35428CCC9D7` | `BOOL ()` | Story-mode-only check |
| `MISC::GET_GAME_TIMER` | `0x4F67E8ECA7D3F667` | `int ()` | `GrHostFrame.game_time_ms` |
| `MISC::GET_FRAME_TIME` | `0x5E72022914CE3C38` | `float ()` | `GrHostFrame.dt` |
| `MISC::VAR_STRING` | `0xFA925AC00EB830B9` | `const char* (int flags, ...)` | Makes the string `_BG_DISPLAY_TEXT` needs |
| `MISC::GET_MODEL_DIMENSIONS` | `0xDCB8DDD5D054A7E7` | `void (Hash model, Vector3* min, Vector3* max)` | The ped's box, to find its feet. In-game: z -1.000 to 1.010 for the player |
| `MISC::GET_GROUND_Z_FOR_3D_COORD` | `0x24FA4267BB8D2431` | `BOOL (float x, float y, float z, float* groundZ, BOOL p4)` | Ground following; standing the ped on the ground at handback. In-game |
| `PLAYER::PLAYER_PED_ID` | `0x096275889B8E0EE0` | `Ped ()` | The ped GMod drives |
| `PLAYER::PLAYER_ID` | `0x217E9DC48139933D` | `Player ()` | For `IS_PLAYER_CONTROL_ON` |
| `PLAYER::IS_PLAYER_CONTROL_ON` | `0x7964097FCE4C244B` | `BOOL (Player player)` | A scripted scene has the player: RDR2 owns it |
| `ENTITY::GET_ENTITY_COORDS` | `0xA86D5F069399F44D` | `Vector3 (Entity, BOOL alive, BOOL realCoords)` | Ped position (its origin, about 1 m above the feet) |
| `ENTITY::GET_ENTITY_ROTATION` | `0xE09CAF86C32CB48F` | `Vector3 (Entity, int rotationOrder)` | Ped rotation, order 2 |
| `ENTITY::GET_ENTITY_HEADING` | `0xC230DD956E2F5507` | `float (Entity)` | Start-up convention check |
| `ENTITY::GET_ENTITY_FORWARD_VECTOR` | `0x2412D9C05BB09B97` | `Vector3 (Entity)` | Start-up convention check. In-game: dot 1.0000 with `gr::RdrForward` |
| `ENTITY::GET_ENTITY_MODEL` | `0xDA76A9F39210D365` | `Hash (Entity)` | For `GET_MODEL_DIMENSIONS`. In-game: `0x0D7114C9` for the player |
| `ENTITY::GET_ENTITY_HEIGHT_ABOVE_GROUND` | `0x0D3B5BAEA08F63E9` | `float (Entity)` | Start-up check that the origin is 1 m above the feet. In-game: 1.000 |
| `ENTITY::IS_ENTITY_DEAD` | `0x7D5B1F88E7504BBA` | `BOOL (Entity)` | Death: RDR2 owns the player |
| `ENTITY::SET_ENTITY_VISIBLE` | `0x1794B4FCC84D812F` | `void (Entity, BOOL)` | Hiding the ped while GMod drives. In-game |
| `ENTITY::FREEZE_ENTITY_POSITION` | `0x7D9EFB7AD6B19754` | `void (Entity, BOOL)` | Freezing the ped while GMod drives. In-game |
| `ENTITY::SET_ENTITY_HEADING` | `0xCF2B9C0645C4651B` | `void (Entity, float)` | Ped faces GMod's yaw. In-game |
| `ENTITY::SET_ENTITY_COORDS_NO_OFFSET` | `0x239A3351AC1DA385` | `void (Entity, float x, float y, float z, BOOL, BOOL, BOOL)` | Moving the ped to the GMod player. In-game |
| `PED::IS_PED_ON_MOUNT` | `0x460BC76A0E10655E` | `BOOL (Ped)` | On a horse: RDR2 owns the player |
| `PED::IS_PED_IN_ANY_VEHICLE` | `0x997ABD671D25CA0B` | `BOOL (Ped, BOOL atGetIn)` | In a wagon: RDR2 owns the player |
| `CAM::CREATE_CAM` | `0xE72CDBA7F0A02DD6` | `Cam (const char* camName, BOOL p1)` | The script camera. `"DEFAULT_SCRIPTED_CAMERA"`: in-game, see below |
| `CAM::DESTROY_CAM` | `0x4E67E0B6D7FD5145` | `void (Cam, BOOL p1)` | At handback and unload |
| `CAM::DOES_CAM_EXIST` | `0x153AD457764FD704` | `BOOL (Cam)` | |
| `CAM::SET_CAM_ACTIVE` | `0x87295BCA613800C8` | `void (Cam, BOOL)` | |
| `CAM::SET_CAM_COORD` | `0xF9EE7D419EE49DE6` | `void (Cam, float x, float y, float z)` | Camera at the GMod player's eyes. In-game |
| `CAM::SET_CAM_ROT` | `0x63DFA6810AD78719` | `void (Cam, float x, float y, float z, int rotationOrder)` | Camera along GMod's view, order 2. In-game |
| `CAM::SET_CAM_FOV` | `0x27666E5988D9D429` | `void (Cam, float fov)` | GMod's FOV, converted to vertical. Range 1 to 130 |
| `CAM::RENDER_SCRIPT_CAMS` | `0x33281167E4942E4F` | `void (BOOL render, BOOL ease, int easeTime, BOOL p3, BOOL p4, int flags)` | Switching to and from the script camera. In-game |
| `CAM::GET_GAMEPLAY_CAM_COORD` | `0x595320200B98596E` | `Vector3 ()` | Start-up convention check |
| `CAM::GET_GAMEPLAY_CAM_ROT` | `0x0252D2B5582957A6` | `Vector3 (int rotationOrder)` | Start-up convention check. In-game: dot 0.93 with the direction to the ped |
| `CAM::GET_FINAL_RENDERED_CAM_FOV` | `0x04AF77971E508F6A` | `float ()` | `GrHostFrame.cam_fov` |
| `ENTITY::DOES_ENTITY_EXIST` | `0xD42BD6EB2E0F1677` | `BOOL (Entity)` | Proxies: a listed or driven ped still exists. In-game |
| `ENTITY::GET_ENTITY_VELOCITY` | `0x4805D2B1D8CF94A9` | `Vector3 (Entity, int p1)` | `GrEntity.vel`. In-game |
| `ENTITY::SET_ENTITY_VELOCITY` | `0x1C99BB7B6E96D16F` | `void (Entity, float x, float y, float z)` | A driven ped follows its proxy. In-game |
| `ENTITY::GET_ENTITY_HEALTH` | `0x82368787EA73C0F7` | `int (Entity)` | `GrEntity.health`. In-game |
| `PED::IS_PED_HUMAN` | `0xB980061DA992779D` | `BOOL (Ped)` | Person or animal. In-game |
| `PED::_IS_THIS_MODEL_A_HORSE` | `0x772A1969F649E902` | `BOOL (Hash model)` | Horse. In-game: the camp's horses |
| `PED::IS_PED_RAGDOLL` | `0x47E4E977581C5B55` | `BOOL (Ped)` | `GR_ENTF_RAGDOLL`. In-game |
| `PED::SET_PED_TO_RAGDOLL` | `0xAE99FB955581844A` | `BOOL (Ped, int timeMin, int timeMax, int type, BOOL, BOOL, BOOL)` | Taking a ped for GMod (10 s), and 2 to 5 s more on release. In-game |
| `PED::RESET_PED_RAGDOLL_TIMER` | `0x9FA4664CF62E47E8` | `void (Ped)` | Keeps a held ped ragdolled. In-game: stays ragdolled while held and while frozen |
| `SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE` | `0x377906D8A31E5586` | `ScrHandle (float x1, y1, z1, x2, y2, z2, int flags, Entity ignore, int p8)` | Player collision rays (flags 1 map, 2 vehicles, 16 objects; p8 4). In-game: flags as in GTA V, see below |
| `SHAPETEST::GET_SHAPE_TEST_RESULT` | `0xEDE8AC7C5108FB1D` | `int (ScrHandle, BOOL* hit, Vector3* end, Vector3* normal, Entity* hit)` | Reads the ray. 2 = done; the synchronous probe is done at once. In-game |
| `CLOCK::GET_CLOCK_HOURS` | `0xC82CF208C2B19199` | `int ()` | Time of day for the viewmodel's light (light_sync.h). In-game |
| `CLOCK::GET_CLOCK_MINUTES` | `0x4E162231B823DBBF` | `int ()` | Same |
| `CLOCK::GET_CLOCK_SECONDS` | `0xB6101ABE62B5F080` | `int ()` | Same |
| `INTERIOR::GET_INTERIOR_FROM_ENTITY` | `0xB417689857646F61` | `Interior (Entity)`, 0 outside | Indoors: no sun on the viewmodel |
| `PAD::DISABLE_ALL_CONTROL_ACTIONS` | `0x5F4B6931816E599B` | `void (int control)` | RDR2 ignores the keys GMod gets. In-game: Tab and left click do nothing in RDR2 |
| `PAD::ENABLE_CONTROL_ACTION` | `0x351220255D64C155` | `void (int control, Hash action, BOOL enableRelated)` | Leaves the passthrough keys working. In-game: Esc and M open RDR2's menus |
| `HUD::IS_PAUSE_MENU_ACTIVE` | `0x535384D6067BA42E` | `BOOL ()` | `GR_HOSTF_PAUSED` |
| `MAP::DISPLAY_RADAR` | `0x1B3DA717B9AFF828` | `void (BOOL toggle)` | The minimap hidden while GMod has the player (ini `hide_hud`) |
| `HUD::_SHOW_PLAYER_CORES` | `0x50C803A4CD5932C5` | `void (BOOL state)` | The player's cores hidden while GMod has the player |
| `HUD::_SHOW_HORSE_CORES` | `0xD4EE21B7CC7FD350` | `void (BOOL state)` | The horse's cores, likewise |
| `UIDEBUG::_BG_DISPLAY_TEXT` | `0x16794E044C9EFB58` | `void (const char* text, float x, float y)` | Status line. Game build 1355+ |
| `UIDEBUG::_BG_SET_TEXT_SCALE` | `0xA1253A3C870B6843` | `void (float scaleX, float scaleY)` | Status line. Game build 1355+ |
| `UIDEBUG::_BG_SET_TEXT_COLOR` | `0x16FA5CE47F184F1E` | `void (int r, int g, int b, int a)` | Status line. Game build 1355+ |
| `MISC::SHOOT_SINGLE_BULLET_BETWEEN_COORDS` | `0x867654CBC7606F2C` | `void (float x1, y1, z1, x2, y2, z2, int damage, BOOL p7, Hash weapon, Ped owner, BOOL audible, BOOL invisible, float speed, BOOL p13)` | GMod's bullets (combat_sync.h), p7 1, speed -1, p13 0. In-game (checked 2026-10-02): a ped shot for 120 went from 75 health to dead, and the town put a bounty on the player |
| `PED::APPLY_DAMAGE_TO_PED` | `0x697157CED63F18D4` | `void (Ped, int damage, BOOL damageArmour, int boneId, Ped killer)` | GMod's blows (crowbar, thrown props). Called in-game (15 damage, right thigh); the health change not yet read back |
| `PED::GET_PED_BONE_COORDS` | `0x17C07FC640E86B4E` | `Vector3 (Ped, int boneId, float ox, oy, oz)` | Aiming the bullet at the bone nearest the hit. In-game: picked the upper arm for a chest-high hit |
| `PED::GET_PED_MAX_HEALTH` | `0x4700A416E8324EF3` | `int (Ped)` | The player's ped: 250 in-game |
| `ENTITY::SET_ENTITY_HEALTH` | `0xAC2767ED8BDFAB15` | `void (Entity, int health, Entity killedBy)` | Healing the player's ped after RDR2 hurts it. In-game: does not always reach the maximum (229 of 250 stayed 229) |
| `FIRE::ADD_OWNED_EXPLOSION` | `0xD84A917A64D4D016` | `void (Ped owner, float x, y, z, int type, float damageScale, BOOL audible, BOOL invisible, float cameraShake)` | GMod's explosions, as type 25 `EXP_TAG_DYNAMITE`. In-game: a dynamite blast where a GMod frag grenade went off |
| `STREAMING::REQUEST_MODEL` | `0xFA28FE3A6246FC30` | `void (Hash model, BOOL p1)` | Spawning (spawn_sync.h), asked again each frame until loaded. In-game (2026-10-02) |
| `STREAMING::HAS_MODEL_LOADED` | `0x1283B8B89DD5D1B6` | `BOOL (Hash model)` | Spawning. In-game: a cow, a person, two carts, a coach and crates, each within a few frames |
| `STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED` | `0x4AD96EF928BD4F9A` | `void (Hash model)` | After spawning |
| `STREAMING::IS_MODEL_IN_CDIMAGE` | `0xD6F3B6D7716CFF8E` | `BOOL (Hash model)` | Refusing unknown model names. In-game: false for a made-up name |
| `STREAMING::IS_MODEL_A_PED` | `0xC3F09DE9D6D17DDA` | `BOOL (Hash model)` | Ped, vehicle or object. In-game: true for `a_c_cow` |
| `STREAMING::IS_MODEL_A_VEHICLE` | `0x354F62672DE7DB0A` | `BOOL (Hash model)` | In-game: true for `cart01`, `wagon02x`, `coach2` |
| `PED::CREATE_PED` | `0xD49F9B0955C367DE` | `Ped (Hash model, float x, y, z, float heading, BOOL isNetwork, BOOL bScriptHostPed, BOOL p7, BOOL p8)` | Spawning, all four BOOLs false. In-game |
| `PED::_SET_RANDOM_OUTFIT_VARIATION` | `0x283978A15512B2FE` | `void (Ped, BOOL p1)` | A script-made ped is invisible without an outfit; p1 true. In-game: the cow and a townsman showed |
| `VEHICLE::CREATE_VEHICLE` | `0xAF35D0D2583051B0` | `Vehicle (Hash model, float x, y, z, float heading, BOOL isNetwork, BOOL bScriptHostVeh, BOOL bDontAutoCreateDraftAnimals, BOOL p8)` | Spawning, all false: wagons come with their team. In-game: `wagon02x` with two horses |
| `VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY` | `0x7263332501E07F52` | `BOOL (Vehicle, BOOL p1)` | After spawning a vehicle |
| `VEHICLE::_GET_PED_IN_DRAFT_HARNESS` | `0xA8BA0BAE0173457B` | `Ped (Vehicle, int harnessId)` | Deleting a spawned wagon's team with it. In-game: `coach2`'s horses went |
| `OBJECT::CREATE_OBJECT` | `0x509D5878EB39E842` | `Object (Hash model, float x, y, z, BOOL isNetwork, BOOL bScriptHostObj, BOOL dynamic, BOOL p7, BOOL p8)` | Spawning, dynamic true. In-game |
| `ENTITY::SET_ENTITY_AS_MISSION_ENTITY` | `0xDC19C288082E586E` | `void (Entity, BOOL scriptHostObject, BOOL grabFromOtherScript)` | Taking an entity over before deleting it |
| `ENTITY::DELETE_ENTITY` | `0x4CD38C78BD19A497` | `void (Entity* entity)` | Undo, cleanup, remover. In-game |
| `ENTITY::IS_ENTITY_A_PED` | `0xCF8176912DDA4EA5` | `BOOL (Entity)` | Ragdoll a driven ped, move anything else directly |
| `ENTITY::IS_ENTITY_A_VEHICLE` | `0xC3D96AF45FCCEC4C` | `BOOL (Entity)` | Deleting a wagon's team |
| `ENTITY::SET_ENTITY_ROTATION` | `0x9CC8314DFEDE441E` | `void (Entity, float pitch, float roll, float yaw, int rotationOrder, BOOL p5)` | A GMod-driven vehicle or object turns with its proxy, order 2, p5 true. In-game |
| `PHYSICS::ACTIVATE_PHYSICS` | `0x710311ADF0E20730` | `void (Entity)` | A new or resting object ignores `SET_ENTITY_VELOCITY` until woken (seen: a thrown crate only turned). In-game: after it, the crate flew 4 m up with its proxy |
| `LAW::CLEAR_WANTED_SCORE` | `0x062B4A4A3396351D` | `void (Player)` | `gr_clear_wanted` |
| `LAW::CLEAR_BOUNTY` | `0xC76F252371150D9A` | `void (Player)` | `gr_clear_wanted`. In-game: bounty 2500 (cents) to 0 |
| `LAW::GET_BOUNTY` | `0x54310AAB97B92816` | `int (Player)` | Logging the bounty cleared |
| `LAW::_SET_BOUNTY_HUNTER_PURSUIT_CLEARED` | `0x55F37F5F3F2475E1` | `void ()` | `gr_clear_wanted`: "force clears local player's wanted level" |

## Control actions

Not natives: the joaat hash of the control's name, the `Hash action` the `PAD` natives
take. Checked against the Controls list in femga/rdr3_discoveries (linked from the
database's `_SET_CONTROL_CONTEXT`) and recomputed with `gr_shm.joaat`.

| Control | Hash | Default key |
|---|---|---|
| `INPUT_FRONTEND_PAUSE` | `0xD82E0BD2` | P |
| `INPUT_FRONTEND_PAUSE_ALTERNATE` | `0x4A903C11` | Esc |
| `INPUT_MAP` | `0xE31C6A41` | M |

## Milestone 8 (tools), checked 2026-10-02

| Native | Hash | Signature | Used for |
|---|---|---|---|
| `ENTITY::ATTACH_ENTITY_TO_ENTITY` | `0x6B9BBD38AB0796DF` | `void (Entity e1, Entity e2, int boneIndex, float x, y, z, float rx, ry, rz, BOOL p9, BOOL softPinning, BOOL collision, BOOL isPed, int vertex, BOOL fixedRot, BOOL p15, BOOL p16)` | Weld. In-game: a welded crate followed the other through a throw |
| `ENTITY::DETACH_ENTITY` | `0x64CDE9D6BF8ECAD3` | `void (Entity, BOOL p1, BOOL collision)` | Undoing a weld |
| `ENTITY::IS_ENTITY_ATTACHED` | `0xEE6AD63ABF59C0B7` | `BOOL (Entity)` | WorldSync leaves welded entities to follow |
| `ENTITY::GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS` | `0x497C6B1A2C9AE69C` | `Vector3 (Entity, float x, y, z)` | Weld offset |
| `ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS` | `0x1899F328B0E12848` | `Vector3 (Entity, float x, y, z)` | Rope ends in the world |
| `ENTITY::SET_ENTITY_NO_COLLISION_ENTITY` | `0xE037BF068223C38D` | `void (Entity, Entity, BOOL thisFrameOnly)` | No-collide, every frame |
| `PHYSICS::_ADD_ROPE_2` | `0xE9C59F6809373A99` | `int (float x, y, z, float rx, ry, rz, float length, int type, BOOL networked, int p9, float p10)` | Rope. In-game: visible |
| `PHYSICS::_ATTACH_ENTITIES_TO_ROPE_2` | `0x462FF2A432733A44` | `void (int rope, Entity e1, Entity e2, float x1, y1, z1, float x2, y2, z2, const char* bone1, const char* bone2)` | Rope ends, in each entity's own space |
| `PHYSICS::DELETE_ROPE` | `0x52B4829281364649` | `void (int* rope)` | Undoing a rope |
| `PHYSICS::DOES_ROPE_EXIST` | `0xFD5448BE3111ED96` | `BOOL (int rope)` | A rope RDR2 removed is undone |

Tried and dropped: `PHYSICS::ADD_ROPE` (`0xE832D760399EB220`) with GTA V's arguments and
`ATTACH_ENTITIES_TO_ROPE` (`0x3D95EC8B6D940AC3`): handles, nothing visible.
`ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS` (`0x31DA7CEC5334DB37`, type 1, isForceRel) and
`SET_ENTITY_DYNAMIC` (`0xFBFC4473F66CE344`): a spawned crate did not move.
`p_cannonball01x` (`0x5A070AD3`, joaat checked) anchors ropes to the world.

## Holding a living ped (2026-10-03)

Hashes and signatures from alloc8or's rdr3-nativedb-data (`natives.json`).

| Native | Hash | Used for | In-game |
|---|---|---|---|
| `SET_PED_CAN_RAGDOLL(ped, toggle)` | `0xB128377056A54E2A` | off while the physgun holds a living ped | seen: a held ped stays upright |
| `SET_PED_GRAVITY(ped, toggle)` | `0x9FF447B6B6AD960A` | off while held | seen: with it off and the ped placed every frame, it stays animated (arms a little out) and does not flail |
| `CLEAR_PED_TASKS_IMMEDIATELY(ped, p1, resetCrouch)` | `0xAAA34F8A7CB32098` | ends a ragdoll when a ragdolled living ped is picked up; p1 false, resetCrouch true as community scripts pass | not seen on its own: TODO(verify) that it stands a ragdolled ped up |

`FREEZE_ENTITY_POSITION` on a held ped also stops the flailing but leaves the ped in its
bind pose (arms out, stiff), so it is not used for that.

## Townsfolk, pools and wagons (2026-10-03, protocol version 19)

Hashes and signatures from alloc8or's rdr3-nativedb-data (`natives.json`).

| Native | Hash | Used for | In-game |
|---|---|---|---|
| `IS_PED_USING_ANY_SCENARIO(ped)` | `0x57AB4A3080F85143` | a person GMod takes is let out of its scenario first | seen: Strawberry townsfolk logged "in a scenario", then followed the physgun |
| `DETACH_ENTITY` + `CLEAR_PED_TASKS_IMMEDIATELY` | see above | a person sitting (attached to a seat by RDR2) is freed for the physgun | seen: "attached to a seat or object", freed and lifted |
| `_GET_ENTITIES_NEAR_POINT(x, y, z, radius, itemSet, p5)` | `0x59B57C4B06531E1E` | the entity scan when Script Hook's pool walk answers nothing | p5 measured: 1 peds, 2 vehicles, 3 objects, 0 nothing; objects: 256 returned but 48 in the itemset |
| `CREATE_ITEMSET(p0)` | `0xA1AF16083320065A` | holds what the query above found; p0 true as community scripts pass (TODO(verify) its meaning) | seen working |
| `IS_ITEMSET_VALID` | `0xD30765D153EF5C76` | | |
| `_CLEAR_ITEMSET` | `0x20A4BF0E09BEE146` | emptied before and after each query | |
| `GET_ITEMSET_SIZE` | `0x55F2E375AC6018A9` | | |
| `GET_INDEXED_ITEM_IN_ITEMSET(index, itemset)` | `0x275A2E2C0FAB7612` | | |
| `IS_ENTITY_AN_OBJECT` | `0x0A27A546A375FDEF` | | |

Script Hook RDR2's `worldGetAllPeds` / `Vehicles` / `Objects` return 0 on most frames in a
town (Valentine, Strawberry: 97 peds for a few frames, then 0 for about a hundred), also with
a 16384-entry buffer. Out in the country they answered steadily.

## Weapons, bones and explosion types

Not natives. Weapon hashes are the joaat hash of the name, recomputed and checked against
femga/rdr3_discoveries `weapons/weapons.lua`: `WEAPON_REVOLVER_CATTLEMAN` `0x169F59F7`,
`WEAPON_SHOTGUN_PUMP` `0x31B7B9FE`. Bone ids from its `boneNames/mp_male__boneNames.lua`
(the same for every human): head 21030, neck0 14283, spine6 14416, spine3 14413, spine0
14410, pelvis 56200, thighs 65478 / 6884, calves 55120 / 43312, upper arms 37873 / 46065,
forearms 53675 / 54187. Explosion types from the database's `ADD_EXPLOSION` notes
(`eExplosionTag`, also femga `graphics/explosions/README.md`): 25 `EXP_TAG_DYNAMITE`.

## Looked up, not called yet

| Native | Hash | Signature |
|---|---|---|
| `NETWORK::NETWORK_IS_SESSION_ACTIVE` | `0xD83C2B94E7508980` | `BOOL ()` |
| `MISC::GET_FRAME_COUNT` | `0x77DFA958FCF100C1` | `int ()` |
| `MISC::GET_HASH_KEY` | `0xFD340785ADF8CFB7` | `Hash (const char*)`, case-insensitive joaat |
| `ENTITY::IS_ENTITY_VISIBLE` | `0xFFC96ECB7FA404CA` | `BOOL (Entity)` | WorldSync skips hidden objects |
| `ENTITY::GET_ENTITY_COLLISION_DISABLED` | `0xAA2FADD30F45A9DA` | `BOOL (Entity)` | WorldSync skips objects without collision. Checked against alloc8or's nativedb 2026-10-02 |
| `HUD::HIDE_HUD_AND_RADAR_THIS_FRAME` | `0x36CDD81627A6FCD2` | `void ()`. Tried for hiding the minimap: hides every part of the HUD, prompts and banners too |

## Notes and open checks

- `HUD::_DISPLAY_TEXT` (`0xD79334A4BB99BAD1`) and `_SET_TEXT_COLOR` do nothing since game
  build 1436 according to the database. That is why the `UIDEBUG::_BG_*` natives are used.
- `VAR_STRING(10, "LITERAL_STRING", text)` is the form community scripts use for raw text.
  It agrees with the database's notes (first flag bit clear, a text label as the extra
  argument) but the value 10 is not documented there. **Verified in-game 2026-10-01**:
  milestone 1's status line shows, drawn with `_BG_DISPLAY_TEXT` at (0.02, 0.02), scale 0.35.
- `CREATE_CAM("DEFAULT_SCRIPTED_CAMERA", false)`: the database documents the native but
  not the names it accepts. The name is the one the game's own scripts use. **Verified
  in-game 2026-10-01**: it returns a camera, and RDR2 renders from it.
- Rotation order 2 (x pitch, y roll, z yaw; yaw 0 faces +Y, positive pitch faces up) is
  RDR2's as well as GTA V's. **Verified in-game 2026-10-01** by the plugin's start-up
  check (`GET_ENTITY_FORWARD_VECTOR` against `gr::RdrForward`, dot 1.0000) and by mouse
  look: 500 counts right turned the view 20.2 degrees right, 200 counts down tilted it
  8.1 degrees down, as GMod's sensitivity predicts.
- RDR2's pause menu and map stop the script thread altogether: `IS_PAUSE_MENU_ACTIVE`
  is mostly seen from the other side, as a heartbeat that stops.
- Three network checks are OR-ed because none is documented well enough to trust alone. A
  false positive only costs a refused bridge.

- Shape test flags are not in the native database. Checked in-game (2026-10-02) with a
  waist-high ray at a tent wall 3.4 m ahead, one flag at a time: 1 hit the tent (the map,
  entity 183844), 4 hit a ped behind it (4.5 m), 8 the same ped's ragdoll bounds, 16 an
  object at 3.3 m, 2, 32, 64, 128 and 256 nothing; 511 and -1 the nearest. Same as GTA V.
  256 is foliage: checked in-game (2026-10-03) with rays carrying only that flag from the
  camera to a GMod prop 39 m away behind a pine: the rays through its branches were stopped
  (plugin log "veil: foliage found", mask 3270), those through gaps were not. It is used for
  the shade ray towards the sun and for hiding GMod's props behind trees and bushes
  (veil_sync.h). There is no native for the sun's direction: light_sync.h derives it from
  the clock.

## Script Hook RDR2 functions

Not natives, but the same rule applies. Signatures are from `inc/main.h` of Script Hook
RDR2 SDK v1.0.1207.73, declared in `rdr2/src/scripthook.h` and listed by decorated name in
`rdr2/scripthook_imports.def`: `scriptWait`, `scriptRegister`, `scriptUnregister`,
`nativeInit`, `nativePush64`, `nativeCall`, since milestone 3 `worldGetAllPeds`
(`?worldGetAllPeds@@YAHPEAHH@Z`) and since milestone 7 `worldGetAllVehicles`
(`?worldGetAllVehicles@@YAHPEAHH@Z`), both checked against the installed DLL with dumpbin. `tools/build.ps1` compares the decorated names
with the exports of the installed `ScriptHookRDR2.dll` when `GR_RDR2_DIR` is set: all six
match v1.0.1491.17, and the plugin loads and runs in the game.

`tools/fake_scripthook` (the test stand-in) answers exactly the natives in the first table.
A native added to the plugin must be added there too, or the harness tests fail with
`unknown_natives`.

## GMod functions (milestone 2)

Checked on the Facepunch wiki on 2026-10-01, in the realm they are used in: the
`CreateMove` and `OnPauseMenuShow` hooks; `CUserCmd:ClearMovement`, `SetButtons`,
`SetForwardMove`, `SetSideMove`, `SetUpMove`, `SetViewAngles`; `input.LookupKeyBinding`;
`Player:SetEyeAngles`, `Player:ConCommand`, `Player:GetFOV`, `Player:Crouching`,
`Player:IsOnGround`; `vgui.CursorVisible`; `gui.IsGameUIVisible`, `gui.HideGameUI`
(rate-limited by the engine); `math.AngleDifference`. The list of commands
`Player:ConCommand` refuses was read: none of the movement or menu binds is on it.

## GMod functions (milestones 3 and 4)

Checked on the Facepunch wiki on 2026-10-02, client realm: `render.Clear`,
`render.SetWriteDepthToDestAlpha`, `render.OverrideAlphaWriteEnable`, `render.OverrideBlend`
(the seven-argument form), the hooks `PreDrawSkyBox`, `PreDrawOpaqueRenderables`,
`PreDrawTranslucentRenderables`, `PreDrawViewModels`, `PreDrawHUD`, `DrawOverlay`;
`gui.InternalCursorMoved` (the wiki calls its arguments deltas; in-game it takes a screen
position), `gui.InternalMousePressed`, `gui.InternalMouseReleased`,
`gui.InternalMouseWheeled`, `gui.InternalKeyCodePressed`, `gui.InternalKeyCodeReleased`,
`gui.InternalKeyTyped`, `vgui.GetKeyboardFocus`, `surface.DrawPoly`, `draw.NoTexture`,
`render.SuppressEngineLighting`, `render.ResetModelLighting`, `render.SetModelLighting`,
`render.SetLocalModelLights` (LocalLight: type, color as an unrestricted Vector, dir), the
hooks `PreDrawViewModel`, `PostDrawViewModel`, `PreDrawPlayerHands`, `PostDrawPlayerHands`.
`mat_setvideomode` is on the list of commands Lua may not run, so GMod's resolution comes
from its launch options.

## The possess tool and RDR2's guns (2026-10-03)

Hashes and signatures from alloc8or's rdr3-nativedb-data (`natives.json`), downloaded 2026-10-03.

| Native | Hash | Used for | In-game |
|---|---|---|---|
| `TASK_GO_STRAIGHT_TO_COORD(ped, x, y, z, moveBlendSpeedY, timeBeforeTeleport, finalHeading, targetRadius, p8)` | `0xD76B57B44F1E6F8B` | walks the possessed ped; speed 1 to 3, -1 (never teleport), radius 0.5, p8 0 | seen: a farmer walked and ran along the direction given |
| `SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, toggle)` | `0x9F8AA94D6D97DBF4` | on while possessed | not judged on its own |
| `CLEAR_PED_TASKS(ped, p1, p2)` | `0xE1EF3C1216AFF2CD` | stops the walk; p1 and p2 true as community scripts pass | TODO(verify) what p1 and p2 mean |
| `TASK_STAND_STILL(ped, time)` | `0x919BE13EED931959` | stands when no key is held, time -1 | seen: he stops |
| `TASK_JUMP(ped, unused)` | `0x0AE4086104E067B1` | the jump key | TODO(verify): not tried in-game |
| `IS_WEAPON_VALID(weaponHash)` | `0x937C71165CF334B3` | a weapon hash from GMod is used only if RDR2 knows it | seen: the Cattleman's hash was fired |

Weapon hashes in `gr/sweps.lua` are from rdr3_discoveries `weapons/weapons.lua` (same date).

## Camera, health and collision (2026-10-03, protocol version 17)

Hashes and signatures checked against alloc8or's RDR3 nativedb (natives.json, 2026-10-03).

| Native | Hash | Used for | In-game |
|---|---|---|---|
| `SET_CAM_PARAMS(cam, x, y, z, rotX, rotY, rotZ, fov, p8, graphType1, graphType2, rotationOrder, p12, p13)` | `0xA47BBFFFB83D4D0A` | the camera each frame (the default, `cam_mode` 1): p8 0 (no blend), 1, 1, order 2, 0, 0 | seen: walking and strafing stay as sharp as standing (with SET_CAM_COORD/ROT/FOV they fell to DLSS's internal resolution). TODO(verify) p8 to p13 |
| `ATTACH_CAM_TO_ENTITY(cam, entity, x, y, z, isRelative)` | `0xFDC0DF7F6FB0A592` | `cam_mode` 2 only (test switch): camera on the hidden ped, world offset | seen: also sharp while moving |
| `DETACH_CAM(cam)` | `0x05B41DDBEB559556` | leaving `cam_mode` 2 | not judged |
| `GET_PED_MAX_HEALTH`, `GET_ENTITY_HEALTH`, `SET_ENTITY_HEALTH` | (above) | the ped gets GMod's health when RDR2 takes the player | seen: GMod 30 -> ped 75 of 250, back as 33 |

## The hidden ped as a target (2026-10-03)

| Native | Hash | Used for | In-game |
|---|---|---|---|
| `SET_ENTITY_ALPHA(entity, alphaLevel, skin)` | `0x0DF7692B1D9E7BA7` | `ped_mode` 1: the ped stays visible to RDR2 but is drawn see-through (0, skin false) | seen: wolves went for it |
| `RESET_ENTITY_ALPHA(entity)` | `0x744B9EF44779D9AB` | when RDR2 takes the player back | not judged |
| `_SET_PED_ALL_WEAPONS_VISIBILITY(ped, visible)` | `0x4F806A6CFED89468` | false every frame in `ped_mode` 1 (alpha does not reach the ped's weapon objects: the holstered gun and knife showed), true when RDR2 takes the player | seen: gone from first and third person |
| `GET_PLAYER_INVINCIBLE(player)` | `0x0CBBCB2CCFA7DC4E` | finding the game holding the player invincible | seen: 1 every frame, also with RDR2 in control |
| `SET_PLAYER_INVINCIBLE(player, toggle)` | `0xFEBEEBC9CBDF4B12` | false every frame while GMod drives | seen: reads 0 at the end of the plugin's frame, 1 again by the next |
| `_GET_ENTITY_CAN_BE_DAMAGED(entity)` | `0x75DF9E73F2F005FD` | as above | seen: 0 with it |
| `SET_ENTITY_CAN_BE_DAMAGED(entity, toggle)` | `0x0D06D522B90E861F` | true while GMod drives | as above |
| `SET_ENTITY_VELOCITY` | (above) | walking the unfrozen ped after the GMod player | seen |

Used only in tests and taken out again: `IS_PED_IN_COMBAT` 0x4859F1FC66A6278E (worked: counted
wolves and armed people fighting the player), `TASK_COMBAT_PED` 0xF166E48407BAC484 and
`GIVE_WEAPON_TO_PED` 0x5E3BDDBCB83F3D84 (arming a spawned farmer: not checked by eye),
`GET_MISSION_FLAG` 0xB15CD1CF58771DE1 (0), and the event queue (`GET_NUMBER_OF_EVENTS`
0x5CE8DE5909565748, `GET_EVENT_AT_INDEX` 0xA85E614430EFF816, `GET_EVENT_DATA` 0x57EC5FA4D4D6AFCA
with joaat("EVENT_ENTITY_DAMAGED") 0x18010D37: no event seen on the invincible player;
TODO(verify) the hash and layout before relying on them).

## Animal ragdolls (2026-10-03)

| Native | Hash | Used for | In-game |
|---|---|---|---|
| `SET_PED_TO_RAGDOLL` ragdollType | `0xAE99FB955581844A` | type 1 for non-human peds, 0 for people | seen: type 0 left animals stiff, 1 limp, 2 refused for animals |
| `RESURRECT_PED(ped)` | `0x71BC8E838B9C6035` | a dead animal while GMod has it | seen: dead 0, ragdolls |
| `SET_ENTITY_HEALTH(ped, 0)` | (above) | the revived animal dies again when let go | seen: falls on as a body |

Tried and not used on carcasses: `SET_ENTITY_DYNAMIC` 0xFBFC4473F66CE344 (no effect),
`_IS_ENTITY_FROZEN` 0x083D497D57B7400F (0), `SET_PED_TO_RAGDOLL` on the dead (returns false).

## Letting go of world entities (2026-10-03)

| Native | Hash | Used for | In-game |
|---|---|---|---|
| `SET_ENTITY_DYNAMIC(entity, toggle)` | `0xFBFC4473F66CE344` | true on take and release | no effect seen on spawned ones |
| `SET_ENTITY_HAS_GRAVITY(entity, toggle)` | `0x0CEDB728A1083FA7` | true on take and release | as above |
| `SET_BOAT_ANCHOR(vehicle, toggle)` | `0xAEAB044F05B92659` | false for vehicles | as above |
| `SET_ACTIVATE_OBJECT_PHYSICS_AS_SOON_AS_IT_IS_UNFROZEN(object, toggle)` | `0x406137F8EF90EAF5` | true for objects | as above |
| `IS_ENTITY_IN_WATER(entity)` | `0xDDE5C125AC446723` | the watchdog leaves floating boats alone | seen: boat on the river not pushed |
| `_IS_ENTITY_FROZEN(entity)` | `0x083D497D57B7400F` | logged by the watchdog | |

## RDR2's gun in the player's view (2026-10-03)

| Native | Hash | Used for | In-game |
|---|---|---|---|
| `_REQUEST_WEAPON_ASSET(weaponHash, p1, p2)` | `0x72D4CB5DB927009C` | loads the gun's model; p1 0, p2 true | seen: the object is made a moment later. TODO(verify) p1, p2 |
| `_HAS_WEAPON_ASSET_LOADED(weaponHash)` | `0xFF07CF465F48B830` | polled each frame until true | seen |
| `_REMOVE_WEAPON_ASSET(weaponHash)` | `0xC3896D03E2852236` | when the gun is put away | not judged |
| `_CREATE_WEAPON_OBJECT(weaponHash, ammoCount, x, y, z, showWorldModel, scale)` | `0x9888652B8BA77F73` | the gun in view; ammo 0, show true, scale 1 | seen: the Cattleman's model |
| `SET_ENTITY_COLLISION(entity, toggle, keepPhysics)` | `0xF66F820909453B8C` | off for that object | not judged on its own |
