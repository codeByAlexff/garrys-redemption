// The GMod end of the bridge without GMod: the link, the floating origin, the anchor, and
// RDR2's input turned into key events and mouse deltas. module.cpp is the Lua binding
// around this. Everything in and out is in Source units and angles; the conversion to the
// wire's RDR2 conventions happens here, through protocol/gr_units.h.
//
// Has no GMod dependency so that protocol/tests can run it against a host Link.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gr_joaat.h"
#include "gr_link.h"
#include "gr_protocol.h"
#include "gr_units.h"

namespace gr {

class Guest {
public:
    bool Open(uint64_t now_ms, const wchar_t* name = GR_SHM_NAME) noexcept {
        frame_ = 0;
        rebase_frame_ = 0;
        host_frames_read_ = 0;
        generation_ = 0;
        anchor_serial_ = 0;
        captured_ = false;
        host_ = GrHostFrame{};
        guest_ = GrGuestFrame{};
        guest_.player_health = -1.0f;
        fresh_session_ = true;
        anchor_health_ = -1.0f;
        std::memset(keys_told_, 0, sizeof(keys_told_));
        return link_.Open(Role::Guest, now_ms, name);
    }

    void Close() noexcept { link_.Close(); }

    // First call of a frame: heartbeat, and RDR2's frame if there is a new one.
    LinkState Tick(uint64_t now_ms) noexcept {
        ++frame_;
        const LinkState state = link_.Tick(now_ms);

        // RDR2 restarted, or went away: what was read from it is void. frame 0 means
        // "nothing from the host", which also un-captures the input below.
        if (link_.peer_generation() != generation_ || state != LinkState::Connected) {
            generation_ = link_.peer_generation();
            if (host_.frame != 0) host_ = GrHostFrame{};
            hurt_synced_ = false;
            fresh_session_ = true;
        }
        if (link_.Read(host_)) ++host_frames_read_;

        // The anchor only holds for the stretch of GMod ownership it was made under.
        if (anchor_serial_ != 0 && (host_.frame == 0 || (host_.flags & GR_HOSTF_RDR2_OWNS_PLAYER) != 0 ||
                                    host_.anchor_serial != anchor_serial_)) {
            anchor_serial_ = 0;
        }

        const bool captured = anchor_serial_ != 0 && (host_.input.flags & GR_INPUT_CAPTURED) != 0;
        if (captured && !captured_) {
            // The totals ran on while the input was not ours: start from where they are.
            mouse_x_ = host_.input.mouse_dx;
            mouse_y_ = host_.input.mouse_dy;
            wheel_ = host_.input.wheel;
        }
        captured_ = captured;
        return state;
    }

    // Last call of a frame.
    void Publish(uint32_t now_us) noexcept {
        guest_.frame = frame_;
        guest_.time_us = now_us;
        guest_.host_frame_seen = host_.frame;
        guest_.anchor_serial = anchor_serial_;
        if (terrain_) {
            guest_.flags |= GR_GUESTF_TERRAIN;
        } else {
            guest_.flags &= ~GR_GUESTF_TERRAIN;
        }
        if (anchor_serial_ != 0) {
            guest_.flags |= GR_GUESTF_PLAYER_VALID;
        } else {
            guest_.flags &= ~GR_GUESTF_PLAYER_VALID;
            // Proxies are placed through the anchor: without one, nothing is GMod's.
            guest_.driven_count = 0;
            guest_.possess = 0;
            guest_.weapon = 0;
            guest_.look_yaw_per_count = 0.0f;
            guest_.look_pitch_per_count = 0.0f;
        }
        // The mouse totals the view angles of this frame include: the ones last taken.
        guest_.look_mouse_x = mouse_x_;
        guest_.look_mouse_y = mouse_y_;
        link_.Publish(guest_);
    }

    // ---- anchor: which RDR2 position is Source (0,0,0)

    bool anchored() const noexcept { return anchor_serial_ != 0; }
    bool can_anchor() const noexcept {
        return link_.connected() && host_.frame != 0 && host_.anchor_serial != 0 &&
               (host_.flags & GR_HOSTF_RDR2_OWNS_PLAYER) == 0;
    }

    // Moves the origin so that the GMod player, standing at `player`, is where RDR2's
    // player ped is. Nothing in GMod moves. `ped_angles` is the way the ped faces.
    bool Anchor(const SrcVec& player, SrcAngle& ped_angles) noexcept {
        if (!can_anchor()) return false;
        origin_.x = static_cast<double>(host_.ped_pos.x) - static_cast<double>(player.x) * kMetresPerUnit;
        origin_.y = static_cast<double>(host_.ped_pos.y) - static_cast<double>(player.y) * kMetresPerUnit;
        origin_.z = static_cast<double>(host_.ped_pos.z) - static_cast<double>(player.z) * kMetresPerUnit;
        anchor_serial_ = host_.anchor_serial;
        // A new session (RDR2 or GMod started, or the plugin reloaded) starts at full health;
        // any later anchor takes the health the ped has after RDR2 had the player.
        anchor_health_ = fresh_session_ ? 1.0f : host_.ped_health;
        fresh_session_ = false;
        ped_angles = RdrRotToSource(host_.ped_rot);
        rebase_frame_ = 0;
        ++origin_serial_;
        return true;
    }

    // The health (0 to 1 of the maximum) the GMod player should have after the last anchor,
    // once; -1 when there is none to take.
    float TakeAnchorHealth() noexcept {
        const float h = anchor_health_;
        anchor_health_ = -1.0f;
        return h;
    }

    // The GMod player's health over its maximum (GrGuestFrame.player_health); -1 unknown.
    void SetPlayerHealth(float fraction) noexcept { guest_.player_health = fraction; }

    // Lets go of the anchor, as if RDR2 had taken the player: RDR2 shows its ped again,
    // and the next Anchor ties the two together afresh.
    void Unanchor() noexcept { anchor_serial_ = 0; }

    // Floating origin: moves the origin by `d` Source units, so whatever was at `d` is now
    // at (0,0,0). The caller moves the player and everything else GMod simulates by -d in
    // the same tick. `player` is where the player was before the move: SetPlayer uses it to
    // recognise a position from before the rebase (the client realm sees the move a frame
    // or so after the server makes it) and shifts it as well.
    void Rebase(const SrcVec& d, const SrcVec& player) noexcept {
        origin_.x += static_cast<double>(d.x) * kMetresPerUnit;
        origin_.y += static_cast<double>(d.y) * kMetresPerUnit;
        origin_.z += static_cast<double>(d.z) * kMetresPerUnit;
        ++origin_serial_;
        rebase_d_ = d;
        rebase_from_ = player;
        rebase_frame_ = frame_ == 0 ? 1 : frame_;
    }

    // Changes whenever Source (0,0,0) means a different RDR2 position: on every anchor and
    // rebase. Whatever GMod placed from RDR2 positions must then be placed again.
    uint32_t origin_serial() const noexcept { return origin_serial_; }

    // Whether the guest stands its player on the terrain chunks (GR_GUESTF_TERRAIN).
    void SetTerrain(bool on) noexcept { terrain_ = on; }
    bool terrain() const noexcept { return terrain_; }

    // `fov` is Source's number: the horizontal angle of a 4:3 picture. `flags` are the
    // GR_GUESTF_* bits other than PLAYER_VALID.
    void SetPlayer(const SrcVec& pos, const SrcVec& vel, const SrcVec& eye, const SrcAngle& angles, float fov,
                   uint32_t flags) noexcept {
        SrcVec shift{0.0f, 0.0f, 0.0f};
        if (StaleAfterRebase(pos)) shift = rebase_d_;
        guest_.pos = SourcePosToRdr({pos.x - shift.x, pos.y - shift.y, pos.z - shift.z}, origin_);
        guest_.vel = SourceVecToRdr(vel);
        guest_.eye_pos = SourcePosToRdr({eye.x - shift.x, eye.y - shift.y, eye.z - shift.z}, origin_);
        guest_.eye_rot = SourceAngToRdr(angles);
        guest_.fov = SourceFovToVertical(fov);
        guest_.flags = flags & ~GR_GUESTF_PLAYER_VALID;
    }

    // ---- entities near the player, and the ones GMod is simulating

    struct SrcEntity {
        int32_t handle;
        uint32_t model;
        uint32_t type;   // GR_ENT_*
        uint32_t flags;  // GR_ENTF_*
        SrcVec pos;
        SrcAngle angles;
        SrcVec vel;
        SrcVec mins;
        SrcVec maxs;
        float health;
    };

    // Only while anchored: the anchor is what places them.
    uint32_t entity_count() const noexcept {
        if (anchor_serial_ == 0) return 0;
        return host_.entity_count < GR_MAX_ENTITIES ? host_.entity_count : GR_MAX_ENTITIES;
    }

    // Entity `i` (0-based) in Source space, at the height the GMod player sees it: the
    // host's ground shift is taken off, as it is off the player.
    bool Entity(uint32_t i, SrcEntity& out) const noexcept {
        if (i >= entity_count()) return false;
        const GrEntity& e = host_.entities[i];
        out.handle = e.handle;
        out.model = e.model;
        out.type = e.type;
        out.flags = e.flags;
        out.pos = RdrPosToSource({e.pos.x, e.pos.y, e.pos.z - host_.ground_shift}, origin_);
        out.angles = RdrRotToSource(e.rot);
        out.vel = RdrVecToSource(e.vel);
        RdrBoundsToSource(e.bounds_min, e.bounds_max, out.mins, out.maxs);
        out.health = e.health;
        return true;
    }

    // The driven list is state: rebuilt in full whenever GMod's physics has ticked, and
    // published as it stands every frame.
    void ClearDriven() noexcept { guest_.driven_count = 0; }
    bool AddDriven(int32_t handle, uint32_t flags, const SrcVec& pos, const SrcAngle& angles,
                   const SrcVec& vel) noexcept {
        if (anchor_serial_ == 0 || guest_.driven_count >= GR_MAX_DRIVEN) return false;
        GrDriven& d = guest_.driven[guest_.driven_count++];
        d.handle = handle;
        d.flags = flags;
        d.pos = SourcePosToRdr(pos, origin_);
        d.rot = SourceAngToRdr(angles);
        d.vel = SourceVecToRdr(vel);
        return true;
    }

    // RDR2's world stopped the player: where it was held (Source units, under the current
    // origin; z is not meaningful), and the horizontal normal of what stopped it. `serial`
    // changes each time. false while not anchored or nothing has stopped it yet.
    bool Block(uint32_t& serial, SrcVec& pos, SrcVec& normal) const noexcept {
        if (anchor_serial_ == 0 || host_.block_serial == 0) return false;
        serial = host_.block_serial;
        pos = RdrPosToSource({host_.block_pos.x, host_.block_pos.y, host_.block_pos.z - host_.ground_shift}, origin_);
        normal = RdrVecToSource(host_.block_normal);
        return true;
    }

    // The light at the player (light_sync.h on the RDR2 side), direction in Source axes.
    // false while not anchored or RDR2 has not filled it.
    bool Light(SrcVec& dir, GrVec3& color, GrVec3& ambient, uint32_t& flags) const noexcept {
        if (anchor_serial_ == 0 || (host_.light_flags & GR_LIGHTF_VALID) == 0) return false;
        dir = RdrVecToSource(host_.light_dir);
        color = host_.light_color;
        ambient = host_.ambient_color;
        flags = host_.light_flags;
        return true;
    }

    // ---- events (GrEvent): hits and explosions, for RDR2 to make happen once

    // Positions in Source units under the current origin, `dir` a Source direction (any
    // length), damage in RDR2 health points, radius in units, force in units per second.
    // false while not anchored: positions mean nothing without the anchor.
    bool AddEvent(uint32_t type, uint32_t flags, int32_t handle, float damage, float radius, const SrcVec& from,
                  const SrcVec& pos, const SrcVec& dir, float force, uint32_t weapon = 0) noexcept {
        if (anchor_serial_ == 0) return false;
        GrEvent& e = NewEvent(type);
        e.model = weapon;
        e.flags = flags;
        e.handle = handle;
        e.damage = damage;
        e.radius = static_cast<float>(radius * kMetresPerUnit);
        e.from = SourcePosToRdr(from, origin_);
        e.pos = SourcePosToRdr(pos, origin_);
        const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        e.dir = length > 1e-6f ? GrVec3{dir.x / length, dir.y / length, dir.z / length} : GrVec3{0.0f, 0.0f, 0.0f};
        e.force = static_cast<float>(force * kMetresPerUnit);
        return true;
    }

    // Asks RDR2 for one entity of `model` standing on `pos` (Source units), facing along
    // `facing` (a Source direction). `id`, never 0, names it for RemoveSpawned and in the
    // spawn results. false while not anchored.
    bool AddSpawn(uint32_t id, uint32_t model, const SrcVec& pos, const SrcVec& facing) noexcept {
        if (anchor_serial_ == 0 || id == 0) return false;
        GrEvent& e = NewEvent(GR_EVENT_SPAWN);
        e.id = id;
        e.model = model;
        e.pos = SourcePosToRdr(pos, origin_);
        const float length = std::sqrt(facing.x * facing.x + facing.y * facing.y + facing.z * facing.z);
        e.dir = length > 1e-6f ? GrVec3{facing.x / length, facing.y / length, facing.z / length} : GrVec3{0, 1, 0};
        return true;
    }

    // Deletes the RDR2 entity `handle`, or if 0 the one spawned as `id`, or with
    // GR_EVENTF_ALL_SPAWNED everything spawned. Needs no anchor: nothing in it is a position.
    bool AddRemove(int32_t handle, uint32_t id, uint32_t flags) noexcept {
        if (!link_.connected()) return false;
        GrEvent& e = NewEvent(GR_EVENT_REMOVE);
        e.handle = handle;
        e.id = id;
        e.flags = flags;
        return true;
    }

    // The law forgets the player's crimes and bounty (GR_EVENT_CLEAR_WANTED).
    // RDR2 moves the player to (x, y, z), RDR2 world metres (GR_EVENT_TELEPORT).
    bool AddTeleport(float x, float y, float z) noexcept {
        if (!link_.connected()) return false;
        GrEvent& e = NewEvent(GR_EVENT_TELEPORT);
        e.pos = {x, y, z};
        return true;
    }

    bool AddClearWanted() noexcept {
        if (!link_.connected()) return false;
        NewEvent(GR_EVENT_CLEAR_WANTED);
        return true;
    }

    // The newest spawn result's seq (0: none yet), and result `seq` if it is still in the ring.
    uint32_t spawn_result_count() const noexcept { return host_.spawn_result_count; }
    bool SpawnResult(uint32_t seq, GrSpawnResult& out) const noexcept {
        if (seq == 0 || seq > host_.spawn_result_count) return false;
        out = host_.spawn_results[(seq - 1) % GR_MAX_SPAWN_RESULTS];
        return out.seq == seq;
    }

    // GMod health points RDR2 has taken from the player's ped since the last call. 0 while
    // not anchored; what built up before the anchor is never handed out.
    uint32_t TakePlayerHurt() noexcept {
        if (anchor_serial_ == 0 || host_.frame == 0) {
            hurt_synced_ = false;
            return 0;
        }
        if (!hurt_synced_) {
            hurt_seen_ = host_.player_hurt;
            hurt_synced_ = true;
            return 0;
        }
        const uint32_t hurt = host_.player_hurt - hurt_seen_;
        hurt_seen_ = host_.player_hurt;
        return hurt;
    }

    // ---- constraints (GrConstraint): what GMod's tools made, listed in full every tick

    void ClearConstraints() noexcept { guest_.constraint_count = 0; }

    // Source conventions in: pos_a in a's own space; pos_b in b's, or a world position under
    // the current origin when b is 0; length in units. false when the table is full, or a
    // world point is given while not anchored (it means nothing without the anchor).
    bool AddConstraint(uint32_t id, uint32_t type, int32_t a, int32_t b, const SrcVec& pos_a, const SrcVec& pos_b,
                       float length) noexcept {
        if (guest_.constraint_count >= GR_MAX_CONSTRAINTS) return false;
        if (b == 0 && type == GR_CON_ROPE && anchor_serial_ == 0) return false;
        GrConstraint& c = guest_.constraints[guest_.constraint_count++];
        c.id = id;
        c.type = type;
        c.a = a;
        c.b = b;
        c.pos_a = SourceLocalToRdr(pos_a);
        c.pos_b = b == 0 ? SourcePosToRdr(pos_b, origin_) : SourceLocalToRdr(pos_b);
        c.length = static_cast<float>(length * kMetresPerUnit);
        return true;
    }

    // ---- the possess tool (GrGuestFrame.possess)

    // `move` a Source direction whose length is the pace (0 stand, 1 walk, 2 run, 3 sprint).
    // Source's and RDR2's world axes point the same way, so it goes over as it is.
    void SetPossess(int32_t handle, const SrcVec& move, bool jump) noexcept {
        guest_.possess = handle;
        guest_.possess_flags = jump ? GR_POSSESSF_JUMP : 0u;
        guest_.possess_move = handle != 0 ? GrVec3{static_cast<float>(move.x), static_cast<float>(move.y), 0.0f}
                                          : GrVec3{0.0f, 0.0f, 0.0f};
    }

    // The RDR2 gun in the player's hand (GrGuestFrame.weapon), 0 for none.
    void SetWeapon(uint32_t hash) noexcept {
        guest_.weapon = hash;
        guest_.weapon_flags = 0;
    }
    // The same, held by the player's model: `hand` in Source units, `aim` Source angles.
    void SetWeaponAtHand(uint32_t hash, const SrcVec& hand, const SrcAngle& aim) noexcept {
        guest_.weapon = hash;
        guest_.weapon_flags = GR_WEAPONF_AT_HAND;
        guest_.weapon_pos = SourcePosToRdr(hand, origin_);
        guest_.weapon_rot = SourceAngToRdr(aim);
    }

    // Degrees one mouse count turns Source's view (yaw, pitch), 0 and 0 while the mouse is
    // not turning the view. Source's pitch grows downwards, RDR2's upwards.
    void SetMouseLook(float yaw_per_count, float pitch_per_count) noexcept {
        guest_.look_yaw_per_count = yaw_per_count;
        guest_.look_pitch_per_count = -pitch_per_count;
    }

    // ---- the view lock (GrHostFrame.view_pos)

    void SetViewHold(uint32_t frames) noexcept { guest_.view_hold = frames < GR_MAX_VIEW_HOLD ? frames : GR_MAX_VIEW_HOLD; }

    // The camera RDR2 is about to show, in Source terms. false while there is none.
    bool HostView(SrcVec& pos, SrcAngle& angles, uint32_t& serial) const noexcept {
        if (anchor_serial_ == 0 || host_.view_serial == 0) return false;
        pos = RdrPosToSource(host_.view_pos, origin_);
        angles = RdrRotToSource(host_.view_rot);
        serial = host_.view_serial;
        return true;
    }

    // ---- veils (GrVeilRequest): GMod's own things RDR2's foliage may hide, listed every frame

    void ClearVeils() noexcept { guest_.veil_count = 0; }

    // `pos` the thing's centre in Source units, `radius` in units. false when the table is full.
    bool AddVeil(uint32_t id, const SrcVec& pos, float radius) noexcept {
        if (guest_.veil_count >= GR_MAX_VEILS || anchor_serial_ == 0 || id == 0) return false;
        GrVeilRequest& v = guest_.veils[guest_.veil_count++];
        v.id = id;
        v.radius = static_cast<float>(radius * kMetresPerUnit);
        v.pos = SourcePosToRdr(pos, origin_);
        return true;
    }

    // The host's last look at `id`. false if it has not looked yet.
    bool VeilMask(uint32_t id, uint32_t& mask) const noexcept {
        for (const GrVeilResult& r : host_.veils) {
            if (r.id == id) {
                mask = r.mask;
                return true;
            }
        }
        return false;
    }

    // ---- probes (GrProbeRequest): stretches GMod's fast things are about to cover, listed every frame

    void ClearProbes() noexcept { guest_.probe_count = 0; }

    // From and to in Source units. false when the table is full.
    bool AddProbe(uint32_t id, const SrcVec& from, const SrcVec& to) noexcept {
        if (guest_.probe_count >= GR_MAX_PROBES || anchor_serial_ == 0 || id == 0) return false;
        GrProbeRequest& p = guest_.probes[guest_.probe_count++];
        p.id = id;
        p.from = SourcePosToRdr(from, origin_);
        p.to = SourcePosToRdr(to, origin_);
        return true;
    }

    // The host's last answer for `id`, its hit point in Source units. false if it has not
    // answered, or nothing is in the way.
    bool ProbeHit(uint32_t id, uint32_t& flags, SrcVec& pos, GrVec3& normal) const noexcept {
        if (anchor_serial_ == 0) return false;
        for (const GrProbeResult& r : host_.probes) {
            if (r.id == id && (r.flags & GR_PROBEF_HIT) != 0) {
                flags = r.flags;
                pos = RdrPosToSource(r.pos, origin_);
                normal = r.normal;
                return true;
            }
        }
        return false;
    }

    // ---- terrain chunks (GrChunk), mirrored by gr/terrain.lua

    // The height of RDR2's ground under a Source position, in Source units. false where no
    // chunk is built or no ground was found. Copies chunks to find the one: not for every frame.
    bool GroundZ(const SrcVec& p, float& z) noexcept {
        int32_t cx = 0;
        int32_t cy = 0;
        ChunkOf(p, cx, cy);
        const double size = GR_CHUNK_CELLS * GR_CHUNK_CELL_CM / 100.0;
        const double cell = GR_CHUNK_CELL_CM / 100.0;
        for (uint32_t slot = 0; slot < GR_MAX_CHUNKS; ++slot) {
            if (link_.ChunkSerial(slot) == 0 || !ReadChunk(slot)) continue;
            if (chunk_.cx != cx || chunk_.cy != cy) continue;
            const double mx = (p.x * kMetresPerUnit + origin_.x - cx * size) / cell;
            const double my = (p.y * kMetresPerUnit + origin_.y - cy * size) / cell;
            const uint32_t i = static_cast<uint32_t>(std::clamp(mx, 0.0, GR_CHUNK_CELLS - 1e-3));
            const uint32_t j = static_cast<uint32_t>(std::clamp(my, 0.0, GR_CHUNK_CELLS - 1e-3));
            const float fx = static_cast<float>(std::clamp(mx - i, 0.0, 1.0));
            const float fy = static_cast<float>(std::clamp(my - j, 0.0, 1.0));
            const float h00 = chunk_.heights[j * GR_CHUNK_VERTS + i];
            const float h10 = chunk_.heights[j * GR_CHUNK_VERTS + i + 1];
            const float h01 = chunk_.heights[(j + 1) * GR_CHUNK_VERTS + i];
            const float h11 = chunk_.heights[(j + 1) * GR_CHUNK_VERTS + i + 1];
            if (h00 <= GR_CHUNK_NO_GROUND_BELOW || h10 <= GR_CHUNK_NO_GROUND_BELOW ||
                h01 <= GR_CHUNK_NO_GROUND_BELOW || h11 <= GR_CHUNK_NO_GROUND_BELOW) {
                return false;
            }
            // The same two triangles as the mesh GMod builds (ChunkMesh splits each cell from
            // its (i, j) corner to its (i + 1, j + 1) corner), so the answer lies on that mesh.
            const float h = fx >= fy ? h00 + (h10 - h00) * fx + (h11 - h10) * fy
                                     : h00 + (h11 - h01) * fx + (h01 - h00) * fy;
            z = static_cast<float>((static_cast<double>(h) - origin_.z) * kUnitsPerMetre);
            return true;
        }
        return false;
    }

    // The serial of a slot as it stands, 0 for an empty one: whether it needs rebuilding.
    uint32_t ChunkSerial(uint32_t slot) const noexcept { return link_.ChunkSerial(slot); }

    // Copies a slot. The copy stays in chunk() until the next call. false if torn or empty.
    bool ReadChunk(uint32_t slot) noexcept { return link_.ReadChunk(slot, chunk_) && chunk_.serial != 0; }
    const GrChunk& chunk() const noexcept { return chunk_; }

    // Where a chunk's corner (its smallest x and y) at height base_z is in Source units
    // under the current origin.
    SrcVec ChunkPos(int32_t cx, int32_t cy, float base_z) const noexcept {
        const double size = GR_CHUNK_CELLS * GR_CHUNK_CELL_CM / 100.0;
        return {static_cast<float>((cx * size - origin_.x) * kUnitsPerMetre),
                static_cast<float>((cy * size - origin_.y) * kUnitsPerMetre),
                static_cast<float>((static_cast<double>(base_z) - origin_.z) * kUnitsPerMetre)};
    }

    // Which chunk a Source position is in.
    void ChunkOf(const SrcVec& p, int32_t& cx, int32_t& cy) const noexcept {
        const double size = GR_CHUNK_CELLS * GR_CHUNK_CELL_CM / 100.0;
        cx = static_cast<int32_t>(std::floor((p.x * kMetresPerUnit + origin_.x) / size));
        cy = static_cast<int32_t>(std::floor((p.y * kMetresPerUnit + origin_.y) / size));
    }

    // ---- input from RDR2

    // True while RDR2's keyboard and mouse are meant for the GMod player.
    bool captured() const noexcept { return captured_; }

    // The next key or button that changed since Lua was last told, as a Windows
    // virtual-key code. When the input stops being captured, every key that was down is
    // reported as released, so nothing stays stuck.
    bool NextKey(int& vk, bool& down) noexcept {
        static const uint8_t kNone[GR_KEY_BYTES] = {};
        const uint8_t* now = captured_ ? host_.input.keys : kNone;
        for (uint32_t byte = 0; byte < GR_KEY_BYTES; ++byte) {
            const uint8_t changed = static_cast<uint8_t>(now[byte] ^ keys_told_[byte]);
            if (!changed) continue;
            for (int bit = 0; bit < 8; ++bit) {
                const uint8_t mask = static_cast<uint8_t>(1u << bit);
                if (!(changed & mask)) continue;
                keys_told_[byte] ^= mask;
                vk = static_cast<int>(byte * 8) + bit;
                down = (now[byte] & mask) != 0;
                return true;
            }
        }
        return false;
    }

    // Mouse counts and wheel movement since the last call. Zero while not captured.
    void TakeMouse(int& dx, int& dy, int& wheel) noexcept {
        dx = dy = wheel = 0;
        if (!captured_) return;
        // The totals wrap; unsigned subtraction gives the right difference across it.
        dx = static_cast<int32_t>(static_cast<uint32_t>(host_.input.mouse_dx) - static_cast<uint32_t>(mouse_x_));
        dy = static_cast<int32_t>(static_cast<uint32_t>(host_.input.mouse_dy) - static_cast<uint32_t>(mouse_y_));
        wheel = static_cast<int32_t>(static_cast<uint32_t>(host_.input.wheel) - static_cast<uint32_t>(wheel_));
        mouse_x_ = host_.input.mouse_dx;
        mouse_y_ = host_.input.mouse_dy;
        wheel_ = host_.input.wheel;
    }

    // ---- for reporting

    Link& link() noexcept { return link_; }
    const GrHostFrame& host() const noexcept { return host_; }
    const GrGuestFrame& guest() const noexcept { return guest_; }
    const Origin& origin() const noexcept { return origin_; }
    uint32_t frame() const noexcept { return frame_; }
    uint32_t host_frames_read() const noexcept { return host_frames_read_; }
    // Where RDR2's player ped is, in Source units under the current origin.
    SrcVec ped_pos() const noexcept { return RdrPosToSource(host_.ped_pos, origin_); }

private:
    // The next slot of the event ring, zeroed, numbered and counted.
    GrEvent& NewEvent(uint32_t type) noexcept {
        const uint32_t seq = guest_.event_count + 1;
        GrEvent& e = guest_.events[(seq - 1) % GR_MAX_EVENTS];
        e = GrEvent{};
        e.seq = seq;
        e.type = type;
        guest_.event_count = seq;
        return e;
    }

    // For this many frames after a rebase, a player position nearer where the player was
    // than where it was moved to is from before the rebase.
    static constexpr uint32_t kRebaseWatchFrames = 120;

    bool StaleAfterRebase(const SrcVec& p) const noexcept {
        if (rebase_frame_ == 0 || frame_ - rebase_frame_ > kRebaseWatchFrames) return false;
        const SrcVec& a = rebase_from_;
        const SrcVec b{a.x - rebase_d_.x, a.y - rebase_d_.y, a.z - rebase_d_.z};
        const auto dist2 = [&p](const SrcVec& q) {
            return (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z);
        };
        return dist2(a) < dist2(b);
    }

    Link link_;
    Origin origin_;
    uint32_t frame_ = 0;
    uint32_t host_frames_read_ = 0;
    uint32_t generation_ = 0;
    uint32_t anchor_serial_ = 0;  // 0 = not anchored
    bool fresh_session_ = true;
    float anchor_health_ = -1.0f;
    bool captured_ = false;
    int32_t mouse_x_ = 0;
    int32_t mouse_y_ = 0;
    int32_t wheel_ = 0;
    uint8_t keys_told_[GR_KEY_BYTES] = {};
    bool terrain_ = false;
    bool hurt_synced_ = false;
    uint32_t hurt_seen_ = 0;
    uint32_t origin_serial_ = 1;
    SrcVec rebase_d_{};
    SrcVec rebase_from_{};
    uint32_t rebase_frame_ = 0;  // guest frame of the last rebase, 0 = none to watch for
    GrChunk chunk_{};
    // Members, not locals: 20 KB does not belong on the stack of a function called every frame.
    GrHostFrame host_{};
    GrGuestFrame guest_{};
};

}  // namespace gr
