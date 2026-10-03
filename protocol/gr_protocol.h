// Garry's Redemption bridge protocol.
//
// This header is the single source of truth for the shared memory layout. The RDR2 plugin
// (host) and the GMod module (guest) both include it. The Python tools do not carry a copy:
// tools/gr_layout.py parses the region between GR_LAYOUT_BEGIN and GR_LAYOUT_END, and
// protocol/tests cross-check that parse against the compiler's offsetof().
//
// Rules for the parsed region, so the parser can stay small:
//   - constants:  inline constexpr uint32_t NAME = <integer expression>;
//   - structs:    plain fields of fixed-width types, float, or earlier structs; arrays
//                 sized by an integer or a constant. No bitfields, pointers, unions,
//                 methods, alignas or #pragma pack.
//   - one field per line.
//
// Everything on the wire is in RDR2 conventions: metres, metres per second, Z up, and
// rotations as RDR2 "rotation order 2" Euler degrees (x = pitch, y = roll, z = yaw).
// The guest converts to Source units and angles with protocol/gr_units.h. Nothing else
// converts, and the floating origin never appears on the wire.
//
// Any change to the layout or meaning of a field: bump GR_PROTOCOL_VERSION.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// GR_LAYOUT_BEGIN

// 1: handshake and heartbeat frames only.
// 2: player sync. time_us and anchor_serial in both frames; ped_pos is the ped's feet;
//    input is filled.
// 3: entities and driven proxies. ground_shift in the host frame.
// 4: block_serial, block_pos and block_normal in the host frame (RDR2 stops the player).
// 5: light_dir, light_color, ambient_color and light_flags in the host frame.
// 6: terrain chunks (GrShared.chunks) and GR_GUESTF_TERRAIN.
// 7: walls in the terrain chunks (GrChunk.walls).
// 8: events from the guest (GrGuestFrame.events: hits and explosions) and player_hurt in the
//    host frame.
// 9: spawn, remove and clear-wanted events (GrEvent.model and .id), spawn results in the
//    host frame, GR_ENTF_SPAWNED, and vehicles and spawned objects in the entity list.
// 10: constraints from GMod's tools (GrGuestFrame.constraints: weld, no-collide, rope).
// 11: GR_EVENT_SHOT (every GMod shot is heard in RDR2; BULLET events are silent hits).
// 12: veils (GrGuestFrame.veils, GrHostFrame.veils): what of RDR2 stands between the camera
//     and each of GMod's own things (trees, bushes), found by rays.
// 13: the view lock (GrHostFrame.view_*, GrGuestFrame.view_hold): GMod draws each frame with
//     the very camera RDR2 is about to show, so its things do not slide in RDR2's world.
// 14: the possess tool (GrGuestFrame.possess*), and GrEvent.model names the RDR2 weapon of a
//     SHOT or BULLET (RDR2's guns as GMod weapons).
// 15: GrGuestFrame.weapon: the RDR2 gun in the GMod player's hand, shown by RDR2 itself.
// 16: look prediction (GrGuestFrame.look_*): the host turns the view by the mouse counts GMod
//     has not seen yet. weapon_flags, weapon_pos and weapon_rot: the RDR2 gun in the hand of
//     the third-person player model.
// 17: health tied between the games (GrHostFrame.ped_health, GrGuestFrame.player_health), and
//     GrHostFrame.nearby: solid things right around the player, for GMod's collision.
// 18: GR_EVENT_TELEPORT.
// 19: GR_HOSTF_ARTHUR.
// 20: probes (GrGuestFrame.probes, GrHostFrame.probes): what of RDR2's world GMod's
//     projectiles are about to hit; GR_EVENT_IMPACT for GMod bullets that hit no proxy.
inline constexpr uint32_t GR_PROTOCOL_VERSION = 20;

// "GRBR" when the bytes are read in memory order.
inline constexpr uint32_t GR_MAGIC = 0x52425247;

inline constexpr uint32_t GR_MAX_ENTITIES = 256;
inline constexpr uint32_t GR_MAX_DRIVEN = 32;
inline constexpr uint32_t GR_KEY_BYTES = 32;

// Terrain chunks: square heightfields of RDR2's ground, GR_CHUNK_CELLS cells of
// GR_CHUNK_CELL_CM centimetres a side, so GR_CHUNK_VERTS samples a side. Chunk (cx, cy)
// starts at RDR2 world (cx, cy) * GR_CHUNK_CELLS * GR_CHUNK_CELL_CM / 100 metres.
inline constexpr uint32_t GR_CHUNK_CELLS = 16;
inline constexpr uint32_t GR_CHUNK_CELL_CM = 100;
inline constexpr uint32_t GR_CHUNK_VERTS = GR_CHUNK_CELLS + 1;
inline constexpr uint32_t GR_CHUNK_SAMPLES = GR_CHUNK_VERTS * GR_CHUNK_VERTS;
inline constexpr uint32_t GR_MAX_CHUNKS = 64;
inline constexpr uint32_t GR_MAX_CHUNK_WALLS = 96;
inline constexpr uint32_t GR_MAX_EVENTS = 32;
inline constexpr uint32_t GR_MAX_CONSTRAINTS = 64;
inline constexpr uint32_t GR_MAX_SPAWN_RESULTS = 16;
inline constexpr uint32_t GR_MAX_VEILS = 16;
inline constexpr uint32_t GR_VEIL_GRID = 4;  // rays a side: GR_VEIL_GRID squared bits in a mask
inline constexpr uint32_t GR_MAX_VIEW_HOLD = 12;
inline constexpr uint32_t GR_MAX_NEAR = 16;
inline constexpr uint32_t GR_MAX_PROBES = 16;  // GrHostFrame.nearby: solid things right around the player

// A peer whose heartbeat has not moved for this long is treated as not there.
inline constexpr uint32_t GR_PEER_TIMEOUT_MS = 2000;

// GrPeer.state
inline constexpr uint32_t GR_PEER_ABSENT = 0;
inline constexpr uint32_t GR_PEER_ALIVE = 1;
inline constexpr uint32_t GR_PEER_REFUSED = 2;
inline constexpr uint32_t GR_PEER_GONE = 3;

// GrPeer.reason, meaningful while state is GR_PEER_REFUSED
inline constexpr uint32_t GR_REASON_NONE = 0;
inline constexpr uint32_t GR_REASON_VERSION = 1;
inline constexpr uint32_t GR_REASON_MULTIPLAYER = 2;

// GrEntity.type
inline constexpr uint32_t GR_ENT_NONE = 0;
inline constexpr uint32_t GR_ENT_PED = 1;
inline constexpr uint32_t GR_ENT_ANIMAL = 2;
inline constexpr uint32_t GR_ENT_HORSE = 3;
inline constexpr uint32_t GR_ENT_VEHICLE = 4;
inline constexpr uint32_t GR_ENT_OBJECT = 5;

// GrEntity.flags
inline constexpr uint32_t GR_ENTF_DEAD = 1u << 0;
inline constexpr uint32_t GR_ENTF_RAGDOLL = 1u << 1;
inline constexpr uint32_t GR_ENTF_DRIVEN = 1u << 2;
inline constexpr uint32_t GR_ENTF_FROZEN = 1u << 3;
// SPAWNED: GMod's spawn menu made it (a GR_EVENT_SPAWN). Cleanup removes these.
inline constexpr uint32_t GR_ENTF_SPAWNED = 1u << 4;

// GrHostFrame.flags
// FOCUSED: RDR2's window is the foreground window.
// PAUSED: RDR2's pause menu (or another of its full-screen menus) is open.
// RDR2_OWNS_PLAYER: RDR2 is moving the player ped itself (a scripted scene, a mount, death,
// or the user handed the player back). The host ignores the guest's player fields, the
// guest drops its anchor, and when the flag clears the guest anchors again to ped_pos.
inline constexpr uint32_t GR_HOSTF_FOCUSED = 1u << 0;
inline constexpr uint32_t GR_HOSTF_PAUSED = 1u << 1;
inline constexpr uint32_t GR_HOSTF_RDR2_OWNS_PLAYER = 1u << 2;
// ARTHUR: the player is Arthur (model player_zero). RDR2 has him shot on sight in New Austin
// (chapters 1 to 6), so the guest does not teleport him there.
inline constexpr uint32_t GR_HOSTF_ARTHUR = 1u << 3;

// GrHostFrame.light_flags
// VALID: the light fields are filled. SHADED: something stands between the player and the
// sun (or moon). INTERIOR: the player is inside a building. NIGHT: the light is the moon's.
inline constexpr uint32_t GR_LIGHTF_VALID = 1u << 0;
inline constexpr uint32_t GR_LIGHTF_SHADED = 1u << 1;
inline constexpr uint32_t GR_LIGHTF_INTERIOR = 1u << 2;
inline constexpr uint32_t GR_LIGHTF_NIGHT = 1u << 3;

// GrInput.flags
// CAPTURED: the keys and mouse in this block are meant for the GMod player. Clear while
// RDR2 is not in front, is paused, or owns the player: the guest then treats every key as
// up and ignores the mouse.
inline constexpr uint32_t GR_INPUT_CAPTURED = 1u << 0;

// GrGuestFrame.flags
// PLAYER_VALID: the player fields are meaningful. Clear until the guest has anchored
// itself to GrHostFrame.ped_pos. Without it the host would move its ped to the guest's
// not-yet-anchored (0,0,0) and the guest would then anchor to that. The host also wants
// GrGuestFrame.anchor_serial to match its own before it believes the player fields.
inline constexpr uint32_t GR_GUESTF_PLAYER_VALID = 1u << 0;
inline constexpr uint32_t GR_GUESTF_ON_GROUND = 1u << 1;
inline constexpr uint32_t GR_GUESTF_CROUCHING = 1u << 2;
inline constexpr uint32_t GR_GUESTF_NOCLIP = 1u << 3;
inline constexpr uint32_t GR_GUESTF_MENU_OPEN = 1u << 4;
// TERRAIN: the guest stands its player on the terrain chunks. The host samples them, and
// stops draping GMod's flat ground over RDR2's (ground_shift stays 0).
inline constexpr uint32_t GR_GUESTF_TERRAIN = 1u << 5;

// GrEvent.type
// BULLET: a GMod bullet hit the proxy of `handle` at `pos`, fired from `from`. The host fires
//   a real RDR2 bullet along the same line, owned by the player's ped.
// HIT: anything else that hurt the proxy (a crowbar, a fist, a thrown prop): damage applied
//   directly, and a push along `dir`.
// EXPLOSION: something exploded in GMod at `pos`. The host makes a real one there.
// SPAWN: make one entity of `model` standing on `pos`, facing along `dir`. Ped, vehicle or
//   object is the host's to tell from the model. `id` (never 0) names it from then on, and
//   its outcome comes back in GrHostFrame.spawn_results.
// REMOVE: delete `handle`, or if that is 0 the entity spawned as `id`, or with
//   GR_EVENTF_ALL_SPAWNED every entity GMod has spawned.
// CLEAR_WANTED: the sandbox's amnesty: the law forgets the player's crimes and bounty.
inline constexpr uint32_t GR_EVENT_NONE = 0;
inline constexpr uint32_t GR_EVENT_BULLET = 1;
inline constexpr uint32_t GR_EVENT_HIT = 2;
inline constexpr uint32_t GR_EVENT_EXPLOSION = 3;
inline constexpr uint32_t GR_EVENT_SPAWN = 4;
inline constexpr uint32_t GR_EVENT_REMOVE = 5;
inline constexpr uint32_t GR_EVENT_CLEAR_WANTED = 6;
// A GMod gun fired: RDR2 fires one harmless audible bullet from `from` to `pos`, so the shot
// is heard (RDR2's gun sound, and its people react to it) whether it hit or missed. The
// BULLET events of its hits are silent. flags: GR_EVENTF_BUCKSHOT for a shotgun's sound.
inline constexpr uint32_t GR_EVENT_SHOT = 7;
// The spawn menu's teleport: RDR2 takes the player, moves its ped to `pos` (RDR2 world
// metres, not converted: a place on RDR2's map, not in GMod's world), waits for the place to
// load, stands it on the ground and gives the player back, and GMod anchors there afresh.
inline constexpr uint32_t GR_EVENT_TELEPORT = 8;
// A GMod bullet that hit none of RDR2's people (a miss, or GMod's copy of the ground or a
// wall): RDR2 fires a harmless, silent bullet from `from` to `pos` along the same line, so it
// stops at RDR2's own trees, walls and rocks and leaves RDR2's marks there. flags:
// GR_EVENTF_BUCKSHOT for a shotgun pellet; model as for BULLET.
inline constexpr uint32_t GR_EVENT_IMPACT = 9;

// GrEvent.flags
// BUCKSHOT: one pellet of a shotgun.
inline constexpr uint32_t GR_EVENTF_BUCKSHOT = 1u << 0;
inline constexpr uint32_t GR_EVENTF_ALL_SPAWNED = 1u << 1;

// GrGuestFrame.possess_flags
// JUMP: the possessed ped jumps (acted on when the flag goes from clear to set).
inline constexpr uint32_t GR_POSSESSF_JUMP = 1u << 0;

// GrGuestFrame.weapon_flags
// AT_HAND: the gun is held by the player's model (third person): the host puts it at
// weapon_pos, turned weapon_rot, instead of in front of the camera.
inline constexpr uint32_t GR_WEAPONF_AT_HAND = 1u << 0;

// GrSpawnResult.status
inline constexpr uint32_t GR_SPAWN_OK = 1;
inline constexpr uint32_t GR_SPAWN_UNKNOWN_MODEL = 2;  // not in the game's files
inline constexpr uint32_t GR_SPAWN_TIMEOUT = 3;        // the model never finished loading
inline constexpr uint32_t GR_SPAWN_FULL = 4;           // too many spawned already
inline constexpr uint32_t GR_SPAWN_FAILED = 5;         // the game made nothing

// GrDriven.flags
inline constexpr uint32_t GR_DRIVE_HELD = 1u << 0;
inline constexpr uint32_t GR_DRIVE_FROZEN = 1u << 1;

struct GrVec3 {
    float x;
    float y;
    float z;
};

// One per side, written only by that side. The layout of GrPeer and GrHeader is frozen
// for all protocol versions: it is what lets two mismatched builds recognise each other
// and refuse cleanly instead of reading garbage.
struct GrPeer {
    uint32_t magic;             // GR_MAGIC once the fields below are valid. Written last.
    uint32_t protocol_version;  // GR_PROTOCOL_VERSION this side was built with
    uint32_t shm_size;          // GR_SHM_SIZE this side was built with
    uint32_t pid;
    uint32_t epoch;             // changes every time this side (re)opens the link
    uint32_t state;             // GR_PEER_*
    uint32_t reason;            // GR_REASON_*
    uint32_t heartbeat;         // incremented once per frame while this side is running
};

struct GrHeader {
    GrPeer host;   // RDR2 plugin
    GrPeer guest;  // GMod module
};

// Raw input captured by the RDR2 plugin. Keys that stay with RDR2
// (rdr2/src/input_passthrough.h) are never set in here.
struct GrInput {
    uint8_t keys[GR_KEY_BYTES];  // one bit per Windows virtual-key code, 1 = down
    int32_t mouse_dx;            // running totals, not per-frame deltas, so a reader that
    int32_t mouse_dy;            //   skips a frame loses nothing. They wrap.
    int32_t wheel;               // running total, in WHEEL_DELTA units
    int32_t cursor_x;            // pixels in RDR2's client area
    int32_t cursor_y;
    uint32_t screen_w;
    uint32_t screen_h;
    uint32_t flags;              // GR_INPUT_*
};

struct GrEntity {
    int32_t handle;     // RDR2 script handle. Never 0 for a live entry.
    uint32_t model;     // model hash
    uint16_t type;      // GR_ENT_*
    uint16_t flags;     // GR_ENTF_*
    GrVec3 pos;         // metres, world
    GrVec3 rot;         // degrees: x = pitch, y = roll, z = yaw
    GrVec3 vel;         // metres per second, world
    GrVec3 bounds_min;  // metres, model space (RDR2 model space: +Y forward, +X right)
    GrVec3 bounds_max;
    float health;
};

// What became of one GR_EVENT_SPAWN.
struct GrSpawnResult {
    uint32_t seq;     // 1, 2, 3 ... in the order the host finished them
    uint32_t id;      // GrEvent.id of the spawn
    int32_t handle;   // the new entity, 0 unless status is GR_SPAWN_OK
    uint32_t model;
    uint32_t status;  // GR_SPAWN_*
};

// One of GMod's own things (a prop, an NPC) that RDR2's trees and bushes may stand in front
// of. GMod has no copy of RDR2's foliage to hide it behind, so the host looks: GR_VEIL_GRID
// by GR_VEIL_GRID rays from the camera (GrGuestFrame.eye_pos) to a square facing it, `radius`
// in front of `pos` and `radius` to each side. State, like the constraints: listed every frame.
struct GrVeilRequest {
    uint32_t id;     // the guest's name for the thing
    float radius;    // metres
    GrVec3 pos;      // its centre, metres, world
};

// The host's answer: bit (row * GR_VEIL_GRID + column) is set where the ray was stopped. Row 0
// is the bottom, column 0 the left as the camera sees it. Kept until the host has looked again.
struct GrVeilResult {
    uint32_t id;     // 0 = no answer in this slot
    uint32_t mask;
};

// A stretch one of GMod's fast things (a grenade, the AR2's ball, a rocket, a thrown prop) is
// about to cover, for the host to test against RDR2's world (rdr2/src/probe_sync.h). GMod's
// copy of that world is only the ground and walls near the player, so without this they flew
// through trees and houses. State, like the veils: listed every frame.
struct GrProbeRequest {
    uint32_t id;     // the guest's name for the thing
    GrVec3 from;     // metres, world
    GrVec3 to;
};

// GrProbeResult.flags
// HIT: something of RDR2's is between from and to; pos is the first point of it and normal
// its face. OBJECT: it is one of RDR2's objects (a crate, a fence). TRUNK: a tree's trunk.
inline constexpr uint32_t GR_PROBEF_HIT = 1u << 0;
inline constexpr uint32_t GR_PROBEF_OBJECT = 1u << 1;
inline constexpr uint32_t GR_PROBEF_TRUNK = 1u << 2;

// The host's answer to the request with the same id, tested against the guest frame it last
// read. Slots without an answer have id 0.
struct GrProbeResult {
    uint32_t id;
    uint32_t flags;  // GR_PROBEF_*
    GrVec3 pos;      // metres, world
    GrVec3 normal;   // unit
};

// Host to guest. Written by the RDR2 plugin once per frame under the seqlock.
// A vertical panel of something the ground's heightfield cannot hold: a building's wall, a
// fence, the face of a rock. It stands on the line through (x, y) across `normal`, from
// `left` metres one way along the wall to `right` metres the other (left is along
// (-normal.y, normal.x)), and from z0 to z1. RDR2 world, metres.
struct GrWall {
    float x;
    float y;
    float left;
    float right;
    float z0;
    float z1;
    float nx;   // horizontal unit normal, facing the side the ray came from
    float ny;
};

struct GrHostFrame {
    uint32_t seq;           // seqlock counter, odd while a write is in progress
    uint32_t frame;         // host frame counter. 0 = nothing published yet.
    uint32_t time_us;       // when this frame was published: gr::NowUs(), a clock both
                            //   processes share, so `now - time_us` is the frame's age
    uint32_t game_time_ms;
    uint32_t flags;         // GR_HOSTF_*
    float dt;               // seconds
    float cam_fov;          // vertical, degrees: what RDR2's own camera last rendered with
    GrVec3 ped_pos;         // the player ped's feet (not the entity origin, which is about
                            //   a metre higher). The guest anchors its origin to this.
    GrVec3 ped_rot;         // x = pitch, y = roll, z = yaw
    uint32_t anchor_serial; // names the current stretch of GMod owning the player. Never
                            //   0. Changes whenever the guest has to anchor afresh: a new
                            //   host run, or RDR2 handing the player back. A guest frame
                            //   anchored under another serial is ignored, so an anchor
                            //   from before a handover can never be used after it.
    float ground_shift;     // metres the host is currently raising the player above where the
                            //   guest put it, to follow RDR2's ground (milestone 2's stand-in
                            //   for terrain collision; 0 without it). Entity heights here are
                            //   real RDR2 heights: the guest subtracts this to place them where
                            //   its player sees them, and driven heights come back without it.
    uint32_t block_serial;  // counts the frames on which RDR2's world stopped the player (a
                            //   wall, a wagon). The guest puts its player back to block_pos
                            //   whenever this changes, so the two players never part.
    GrVec3 block_pos;       // feet, metres, world: where the host held the player. Its height
                            //   includes ground_shift; the guest only uses x and y.
    GrVec3 block_normal;    // the surface that stopped the player, horizontal, unit length
    GrVec3 light_dir;       // unit, world: towards the sun, or the moon at night (from the clock)
    GrVec3 light_color;     // linear RGB of that light at the player: 0 in shade or indoors
    GrVec3 ambient_color;   // linear RGB of the sky's light at the player
    uint32_t light_flags;   // GR_LIGHTF_*
    uint32_t player_hurt;   // running total of the damage RDR2 did to the player's ped (shot,
                            //   burnt, blown up), in GMod health points. The guest hurts its
                            //   player by each increase; the host keeps the ped itself alive.
    float ped_health;       // the ped's health over its maximum, 0 to 1, while RDR2 has the
                            //   player; -1 while GMod drives (the ped is kept healed then) or
                            //   with no ped. The guest takes it as its player's health when
                            //   it anchors, so a fight on horseback carries over.
    // What is solid right around the player, found by a ring of short rays this frame and
    // the last: trees, rocks, fences, posts and RDR2's objects, which the terrain chunks
    // (map only, 1 m grid) miss or the GMod player was not stopped by. The guest stands an
    // invisible solid panel at each, so its own movement stops and slides there. z0 and z1
    // are RDR2 heights. Empty while RDR2 has the player.
    uint32_t nearby_count;
    GrWall nearby[GR_MAX_NEAR];
    // The last GR_MAX_SPAWN_RESULTS results, a ring like GrGuestFrame.events: result `seq` is
    // in spawn_results[(seq - 1) % GR_MAX_SPAWN_RESULTS], spawn_result_count the newest seq.
    // The view lock. The two games draw at different rates, so a GMod picture drawn with
    // GMod's newest view never shows the same view as the RDR2 picture under it, and GMod's
    // things slid in RDR2's world in every turn. Instead the host says here which camera it
    // is going to show, GrGuestFrame.view_hold of its own frames from now (it shows the one
    // it published that many frames ago), and GMod draws with exactly that: both pictures
    // are of the same view, and the hold gives GMod's the time to reach the screen.
    GrVec3 view_pos;        // metres, world
    GrVec3 view_rot;        // x = pitch, y = roll, z = yaw
    uint32_t view_serial;   // counts the views published. 0 = none (RDR2 has the player).
    GrVeilResult veils[GR_MAX_VEILS];
    GrProbeResult probes[GR_MAX_PROBES];
    uint32_t spawn_result_count;
    GrSpawnResult spawn_results[GR_MAX_SPAWN_RESULTS];
    GrInput input;
    uint32_t entity_count;
    GrEntity entities[GR_MAX_ENTITIES];
};

// A proxy that GMod is currently simulating. Listed for as long as GMod owns it. Leaving
// the list is the release, and the last velocity sent is the throw velocity, so no
// separate grab or release event can be lost.
struct GrDriven {
    int32_t handle;
    uint32_t flags;  // GR_DRIVE_*
    GrVec3 pos;
    GrVec3 rot;
    GrVec3 vel;
};

// Something that happened once in GMod and has to happen once in RDR2: not state, so it
// travels in a ring (GrGuestFrame.events) with a sequence number.
struct GrEvent {
    uint32_t seq;      // 1, 2, 3 ... in the order the guest made them
    uint32_t type;     // GR_EVENT_*
    uint32_t flags;    // GR_EVENTF_*
    int32_t handle;    // the RDR2 entity hit; 0 for an explosion
    float damage;      // RDR2 health points (the guest scales GMod's)
    float radius;      // EXPLOSION: metres
    GrVec3 from;       // BULLET: where it was fired from (the shooter's eyes)
    GrVec3 pos;        // where it hit, or the explosion's centre
    GrVec3 dir;        // unit: the way the bullet or the blow went
    float force;       // HIT: metres per second to add to the entity along dir
    uint32_t model;    // SPAWN: model hash. SHOT, BULLET: the RDR2 weapon's hash (0: the
                       //   host's default, a revolver, or a pump shotgun for BUCKSHOT)
    uint32_t id;       // SPAWN, REMOVE: the guest's name for a spawned entity
};

// Guest to host. Written by the GMod module once per frame under the seqlock.
// GrConstraint.type
// WELD: a is held to b as they are now (b 0: a is held where it is, welded to the world).
// NOCOLLIDE: a and b pass through each other.
// ROPE: a rope from pos_a on a to pos_b on b (b 0: pos_b is a point in the world), `length`
//   metres long.
// (Thrusters are not here: GMod takes a proxy while its thruster fires and RDR2 follows the
// proxy, see gr/tools.lua.)
inline constexpr uint32_t GR_CON_NONE = 0;
inline constexpr uint32_t GR_CON_WELD = 1;
inline constexpr uint32_t GR_CON_NOCOLLIDE = 2;
inline constexpr uint32_t GR_CON_ROPE = 3;

// Something a GMod tool made between RDR2 entities. State, not an event: the guest lists
// every constraint that exists, every frame; the host makes the ones it has not made yet and
// undoes the ones no longer listed (undo, the remover, cleanup, a broken weld).
struct GrConstraint {
    uint32_t id;     // the guest's name for it, never reused in one run of the guest
    uint32_t type;   // GR_CON_*
    int32_t a;       // RDR2 handles; b 0 = the world
    int32_t b;
    GrVec3 pos_a;    // metres in a's own space (RDR2 model axes)
    GrVec3 pos_b;    // metres in b's own space, or the world when b is 0
    float length;    // ROPE: metres
};

struct GrGuestFrame {
    uint32_t seq;
    uint32_t frame;            // guest frame counter
    uint32_t time_us;          // when this frame was published: gr::NowUs()
    uint32_t host_frame_seen;  // last GrHostFrame.frame the guest consumed
    uint32_t flags;            // GR_GUESTF_*
    uint32_t anchor_serial;    // GrHostFrame.anchor_serial of the frame the guest anchored to
    GrVec3 pos;                // player feet, metres, world. Valid with GR_GUESTF_PLAYER_VALID.
    GrVec3 vel;                // metres per second. The host extrapolates pos and eye_pos
                               //   along it by the frame's age.
    GrVec3 eye_pos;
    GrVec3 eye_rot;            // x = pitch, y = roll, z = yaw
    float fov;                 // vertical degrees GMod is rendering with. 0 = no preference.
    uint32_t driven_count;
    GrDriven driven[GR_MAX_DRIVEN];
    // The last GR_MAX_EVENTS events: event `seq` is in events[(seq - 1) % GR_MAX_EVENTS].
    // event_count is the seq of the newest (0: none yet). The host acts on each seq once;
    // one more than GR_MAX_EVENTS behind and it has missed some.
    uint32_t event_count;
    GrEvent events[GR_MAX_EVENTS];
    uint32_t constraint_count;
    GrConstraint constraints[GR_MAX_CONSTRAINTS];
    uint32_t veil_count;
    GrVeilRequest veils[GR_MAX_VEILS];
    uint32_t probe_count;
    GrProbeRequest probes[GR_MAX_PROBES];
    uint32_t view_hold;        // host frames between publishing a view (GrHostFrame.view_*) and
                               //   showing it, 0 to GR_MAX_VIEW_HOLD
    // The possess tool: the GMod player drives one of RDR2's peds, horses or animals. State,
    // listed for as long as it lasts. The host walks the ped along possess_move with RDR2's
    // own movement (so it is animated) and keeps its AI from doing anything else.
    int32_t possess;           // the ped's RDR2 handle, 0 = none
    uint32_t possess_flags;    // GR_POSSESSF_*
    GrVec3 possess_move;       // world direction to go, its length the pace: 0 stand, 1 walk,
                               //   2 run, 3 sprint
    uint32_t weapon;           // hash of the RDR2 gun the player holds in first person (0: none,
                               //   or one of GMod's own). The host shows RDR2's model of it in
                               //   front of the camera; GMod draws no viewmodel for it.
    uint32_t weapon_flags;     // GR_WEAPONF_*
    GrVec3 weapon_pos;         // AT_HAND: the hand, metres, world
    GrVec3 weapon_rot;         // AT_HAND: the way it points, as a camera rotation (x pitch, z yaw)
    // Look prediction. The two games' frames are not in step: a mouse count RDR2 captured
    // reaches GMod, turns its view, and comes back in eye_rot one or two RDR2 frames later,
    // and not always the same number of frames, so a steady turn of the mouse was an unsteady
    // turn of the camera. GMod says here which of the host's mouse totals (GrInput.mouse_dx,
    // mouse_dy) eye_rot already includes and how far a count turns the view; the host adds
    // the counts since then itself, so every RDR2 frame shows all the mouse movement so far.
    int32_t look_mouse_x;
    int32_t look_mouse_y;
    float look_yaw_per_count;    // degrees of eye_rot.z per count; both 0: do not predict (a
    float look_pitch_per_count;  //   menu has the mouse). Degrees of eye_rot.x per count.
    float player_health;       // the GMod player's health over its maximum, 0 to 1 (0: dead);
                               //   -1 unknown. When RDR2 takes the player the ped is given it.
};

// One slot of the terrain table. The host keeps the chunks around its player in these
// slots; the table is state, not a queue: a slot holds the same chunk until the host
// reuses it, and every new content gets a new serial. Each slot has its own seqlock, so
// the guest copies only the slots whose serial changed.
struct GrChunk {
    uint32_t seq;
    uint32_t serial;     // changes with every new content. 0 = the slot is empty.
    int32_t cx;          // which chunk (see GR_CHUNK_CELLS)
    int32_t cy;
    uint32_t holes;      // samples where no ground was found
    float base_z;        // metres: the lowest height in heights[]
    float heights[GR_CHUNK_SAMPLES];  // RDR2 world z in metres, row by row (y), x
                                      //   fastest. GR_CHUNK_NO_GROUND where none was found.
    uint32_t wall_count;
    GrWall walls[GR_MAX_CHUNK_WALLS];
};

struct GrShared {
    GrHeader header;
    GrHostFrame host;
    GrGuestFrame guest;
    GrChunk chunks[GR_MAX_CHUNKS];
};

inline constexpr uint32_t GR_HEADER_SIZE = sizeof(GrHeader);
inline constexpr uint32_t GR_SHM_SIZE = sizeof(GrShared);

// GR_LAYOUT_END

// Session-local so the two games must run in the same Windows session. The version is
// deliberately not part of the name: mismatched builds should meet and say so.
#define GR_SHM_NAME L"Local\\GarrysRedemptionBridge"

// GrChunk.heights for a sample with no ground under it (nothing loaded there, or a hole).
// Anything below GR_CHUNK_NO_GROUND_BELOW counts as none.
inline constexpr float GR_CHUNK_NO_GROUND = -1.0e9f;
inline constexpr float GR_CHUNK_NO_GROUND_BELOW = -1.0e8f;

// The frozen part. If one of these fires, you changed something that must never change.
static_assert(sizeof(GrPeer) == 32);
static_assert(sizeof(GrHeader) == 64);
static_assert(offsetof(GrPeer, magic) == 0);
static_assert(offsetof(GrPeer, protocol_version) == 4);
static_assert(offsetof(GrPeer, shm_size) == 8);
static_assert(offsetof(GrPeer, heartbeat) == 28);
static_assert(offsetof(GrShared, header) == 0);

// The versioned part. If one of these fires, the layout changed: update the number here
// and bump GR_PROTOCOL_VERSION in the same commit.
static_assert(sizeof(GrVec3) == 12);
static_assert(sizeof(GrInput) == 64);
static_assert(sizeof(GrEntity) == 76);
static_assert(sizeof(GrDriven) == 44);
static_assert(sizeof(GrEvent) == 72);
static_assert(sizeof(GrSpawnResult) == 20);
static_assert(sizeof(GrVeilRequest) == 20);
static_assert(sizeof(GrVeilResult) == 8);
static_assert(sizeof(GrProbeRequest) == 28);
static_assert(sizeof(GrProbeResult) == 32);
static_assert(sizeof(GrHostFrame) == 19696 + 32 * GR_MAX_NEAR + 20 * GR_MAX_SPAWN_RESULTS + 8 * GR_MAX_VEILS +
                                         32 * GR_MAX_PROBES);
static_assert(sizeof(GrConstraint) == 44);
static_assert(sizeof(GrGuestFrame) == 1580 + 72 * GR_MAX_EVENTS + 44 * GR_MAX_CONSTRAINTS + 20 * GR_MAX_VEILS +
                                          28 * GR_MAX_PROBES);
static_assert(sizeof(GrWall) == 32);
static_assert(sizeof(GrChunk) == 24 + 4 * GR_CHUNK_SAMPLES + 4 + 32 * GR_MAX_CHUNK_WALLS);
static_assert(sizeof(GrShared) == 21340 + 32 * GR_MAX_NEAR + 20 * GR_MAX_SPAWN_RESULTS + 72 * GR_MAX_EVENTS +
                                       44 * GR_MAX_CONSTRAINTS + 28 * GR_MAX_VEILS + 60 * GR_MAX_PROBES +
                                       GR_MAX_CHUNKS * sizeof(GrChunk));

// Both frames start with the seqlock counter, which gr_seqlock.h relies on.
static_assert(offsetof(GrHostFrame, seq) == 0);
static_assert(offsetof(GrGuestFrame, seq) == 0);
static_assert(offsetof(GrChunk, seq) == 0);

// Shared memory holds bytes, not objects. Nothing in here may need a constructor.
static_assert(std::is_trivially_copyable_v<GrShared>);
static_assert(std::is_standard_layout_v<GrShared>);
