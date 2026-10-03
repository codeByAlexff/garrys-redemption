// Combat sync, RDR2 side (milestone 6): what GMod's weapons do happens in RDR2, and what
// RDR2 does to the player's ped happens to the GMod player.
//
// GMod decides every hit: its weapons fire in GMod and their traces hit the proxies. Each hit
// arrives as a GrEvent, once, and is made real here:
//   - a bullet becomes a real RDR2 bullet along the same line, fired by the player's ped, aimed
//     at the bone of the ped nearest where the proxy was hit. RDR2 then does everything a
//     bullet does: blood, the hit reaction, headshots, death, and witnesses and the law
//     treating the player as the shooter.
//   - any other blow (a crowbar, a thrown prop) is damage applied to the ped and a push.
//   - an explosion in GMod is a real explosion at the same spot, owned by the player.
//
// The other way: the player's ped is hidden but still there, and RDR2's own guns, fire and
// explosions hurt it. Whatever health it loses is added to GrHostFrame.player_hurt (in GMod
// health points) for the GMod player to lose, and the ped is healed again, so it is GMod's
// health that decides when the player dies.
//
// Everything here calls natives: script thread only. No allocation.

#pragma once

#include <cmath>

#include "gr_log.h"
#include "gr_protocol.h"
#include "natives.h"
#include "spawn_sync.h"

namespace gr {

class CombatSync {
public:
    void Reset() noexcept {
        synced_ = false;
        last_seq_ = 0;
        hurt_total_ = 0;
        hurt_part_ = 0.0f;
        have_health_ = false;
        quiet_until_ms_ = 0;
    }

    // One frame. `driving`: GMod has the player. `guest_generation` changes when GMod restarts.
    // The event ring also carries the spawn menu's events: those go to `spawn`.
    void Tick(bool driving, uint32_t guest_generation, const GrGuestFrame& guest, native::Entity player,
              SpawnSync& spawn, GrHostFrame& host) noexcept {
        const uint64_t now_ms = NowMs();
        // Nothing read from GMod yet (frame 0): syncing to that would replay its whole ring
        // once its first real frame arrives (seen after a plugin reload).
        // A guest that restarted counts from 0 again; one never seen before may already be
        // far along, and what it did before this run of the plugin is not replayed.
        if (guest.frame == 0) {
            synced_ = false;
        } else if (!synced_ || guest_generation != generation_) {
            generation_ = guest_generation;
            last_seq_ = guest.event_count;
            spawn.ForgetIds();
            synced_ = true;
        }
        if (guest.event_count < last_seq_) last_seq_ = 0;
        if (!driving || !synced_) {
            last_seq_ = guest.event_count;
        } else {
            if (guest.event_count - last_seq_ > GR_MAX_EVENTS) {
                GR_LOG("combat: %u events from GMod were lost (more than %u in one frame)",
                       guest.event_count - last_seq_ - GR_MAX_EVENTS, GR_MAX_EVENTS);
                last_seq_ = guest.event_count - GR_MAX_EVENTS;
            }
            while (last_seq_ != guest.event_count) {
                ++last_seq_;
                const GrEvent& e = guest.events[(last_seq_ - 1) % GR_MAX_EVENTS];
                if (e.seq != last_seq_) continue;
                // The spawn menu's events (SPAWN, REMOVE, CLEAR_WANTED). SHOT has a higher number
                // but is a weapon's: sent to `spawn` with them, no GMod shot was ever heard.
                if (e.type >= GR_EVENT_SPAWN && e.type <= GR_EVENT_CLEAR_WANTED) {
                    spawn.Apply(e, now_ms);
                } else if (e.type == GR_EVENT_TELEPORT) {
                    teleport_ = e.pos;
                    has_teleport_ = true;
                } else {
                    Apply(e, player, now_ms);
                }
            }
        }

        if (watch_handle_ != 0 && now_ms >= watch_ms_) {
            const bool there = native::DoesEntityExist(watch_handle_);
            GR_LOG("combat: %d after the bullet: health %d -> %d%s", watch_handle_, watch_health_,
                   there ? native::GetEntityHealth(watch_handle_) : -1,
                   there && native::IsEntityDead(watch_handle_) ? ", dead" : "");
            if (there && watch_fallback_ > 0 && !native::IsEntityDead(watch_handle_) &&
                native::GetEntityHealth(watch_handle_) >= watch_health_) {
                native::ApplyDamageToPed(watch_handle_, watch_fallback_, 0, player);
                GR_LOG("combat: the bullet missed %d's body; %d damage applied directly", watch_handle_, watch_fallback_);
            }
            watch_handle_ = 0;
        }
        Hurt(driving, player, now_ms);
        host.player_hurt = hurt_total_;
    }

    // When the last GMod shot was fired (NowMs): weapon_view.h kicks the gun.
    uint64_t shot_ms() const noexcept { return shot_ms_; }

private:
    // An explosion GMod sent near the player hurts the GMod player in GMod already; the same
    // blast in RDR2 must not hurt it a second time.
    static constexpr uint64_t kQuietMs = 600;
    static constexpr float kQuietRadius = 15.0f;
    static constexpr float kMuzzleClear = 0.6f;   // past the player's own (hidden) body
    static constexpr float kBulletReach = 0.75f;  // beyond the bone, through the body
    static constexpr float kMaxPush = 25.0f;
    static constexpr uint64_t kOwnShotMs = 200;
    static constexpr int kOwnShotLoss = 2;

    void Apply(const GrEvent& e, native::Entity player, uint64_t now_ms) noexcept {
        switch (e.type) {
            case GR_EVENT_BULLET: Bullet(e, player); break;
            case GR_EVENT_HIT: Hit(e, player); break;
            case GR_EVENT_EXPLOSION: Explosion(e, player, now_ms); break;
            case GR_EVENT_SHOT: Shot(e, player); break;
            case GR_EVENT_IMPACT: Impact(e, player); break;
            default: break;
        }
    }

    // The RDR2 gun a shot is fired with: the one GMod named (gr/sweps.lua), if RDR2 knows it.
    static uint32_t Weapon(const GrEvent& e) noexcept {
        if (e.model != 0 && native::IsWeaponValid(e.model)) return e.model;
        return (e.flags & GR_EVENTF_BUCKSHOT) != 0 ? weapon_hash::WEAPON_SHOTGUN_PUMP
                                                   : weapon_hash::WEAPON_REVOLVER_CATTLEMAN;
    }

    // The bone of a human ped nearest `p`, and where it is. false for anything not human.
    static bool NearestBone(native::Entity ped, const GrVec3& p, int& bone, GrVec3& at) noexcept {
        static constexpr int kBones[] = {
            bone_id::SKEL_HEAD,       bone_id::SKEL_NECK0,      bone_id::SKEL_SPINE6,     bone_id::SKEL_SPINE3,
            bone_id::SKEL_SPINE0,     bone_id::SKEL_PELVIS,     bone_id::SKEL_L_THIGH,    bone_id::SKEL_R_THIGH,
            bone_id::SKEL_L_CALF,     bone_id::SKEL_R_CALF,     bone_id::SKEL_L_UPPERARM, bone_id::SKEL_R_UPPERARM,
            bone_id::SKEL_L_FOREARM,  bone_id::SKEL_R_FOREARM,
        };
        if (!native::IsPedHuman(ped)) return false;
        float best = 1e30f;
        for (const int b : kBones) {
            const GrVec3 c = native::GetPedBoneCoords(ped, b);
            const float d = (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y) + (c.z - p.z) * (c.z - p.z);
            if (d < best) {
                best = d;
                bone = b;
                at = c;
            }
        }
        // A bone over 1.5 m from the hit is not this ped's (the call failed): use the hit.
        return best < 1.5f * 1.5f;
    }

    void Bullet(const GrEvent& e, native::Entity player) noexcept {
        GrVec3 aim = e.pos;
        int bone = 0;
        const bool exists = e.handle != 0 && native::DoesEntityExist(e.handle);
        // Works around: animals and horses have none of the human bones, and RDR2's box for
        // some of their models is a thin slab off the body (seen: a deer's), so the bullet
        // aimed at GMod's hit point flew past (a deer's health 15 -> 15). Their root is in
        // the body; if the bullet still misses, the damage is applied directly (Tick).
        bool animal = false;
        if (exists && !NearestBone(e.handle, e.pos, bone, aim) && native::IsEntityAPed(e.handle)) {
            animal = true;
            aim = native::GetEntityCoords(e.handle);
        }
        GrVec3 d{aim.x - e.from.x, aim.y - e.from.y, aim.z - e.from.z};
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (length < 0.05f) return;
        d = {d.x / length, d.y / length, d.z / length};
        const float start = length > kMuzzleClear + 0.1f ? kMuzzleClear : 0.0f;
        const GrVec3 from{e.from.x + d.x * start, e.from.y + d.y * start, e.from.z + d.z * start};
        const GrVec3 to{aim.x + d.x * kBulletReach, aim.y + d.y * kBulletReach, aim.z + d.z * kBulletReach};
        const int damage = static_cast<int>(std::lround(e.damage < 1.0f ? 1.0f : e.damage));
        // Silent: the shot's sound comes from its GR_EVENT_SHOT, once however many pellets hit.
        native::ShootBullet(from, to, damage, Weapon(e), player, false);
        // What the bullet did is logged half a second later (Tick): the shot is not instant.
        if (exists && watch_handle_ == 0) {
            watch_handle_ = e.handle;
            watch_health_ = native::GetEntityHealth(e.handle);
            watch_ms_ = NowMs() + 500;
            watch_fallback_ = animal ? damage : 0;
        } else if (animal) {
            native::ApplyDamageToPed(e.handle, damage, 0, player);  // no slot to check it: apply now
        }
        GR_LOG("combat: bullet at %d (bone %d), %d damage, %.1f m", e.handle, bone, damage, length);
    }

    // Works around: GMod's gun sound and RDR2's bullet both played on a hit, and a miss made
    // no sound in RDR2 at all, so its people never heard it. GMod's firing sounds are muted
    // (weapons.lua) and every shot is one audible, harmless RDR2 bullet along the aim.
    void Shot(const GrEvent& e, native::Entity player) noexcept {
        GrVec3 d{e.pos.x - e.from.x, e.pos.y - e.from.y, e.pos.z - e.from.z};
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (length < kMuzzleClear + 0.1f) return;
        d = {d.x / length, d.y / length, d.z / length};
        const GrVec3 from{e.from.x + d.x * kMuzzleClear, e.from.y + d.y * kMuzzleClear, e.from.z + d.z * kMuzzleClear};
        const bool buckshot = (e.flags & GR_EVENTF_BUCKSHOT) != 0;
        native::ShootBullet(from, e.pos, 0, Weapon(e), player, true);
        shot_ms_ = NowMs();
        GR_LOG("combat: shot heard, %.1f m, weapon %08X%s", length, Weapon(e), buckshot ? ", shotgun" : "");
    }

    // A GMod bullet that hit none of RDR2's people: one harmless, silent RDR2 bullet along its
    // line, which stops at RDR2's own trees, walls and rocks and marks them as a shot does
    // (user: weapons should "collide with objects like trees, houses"). GMod's copy of RDR2's
    // world has no trees beyond the player's reach and no walls beyond the terrain chunks.
    void Impact(const GrEvent& e, native::Entity player) noexcept {
        GrVec3 d{e.pos.x - e.from.x, e.pos.y - e.from.y, e.pos.z - e.from.z};
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (length < kMuzzleClear + 0.1f) return;
        d = {d.x / length, d.y / length, d.z / length};
        const GrVec3 from{e.from.x + d.x * kMuzzleClear, e.from.y + d.y * kMuzzleClear, e.from.z + d.z * kMuzzleClear};
        native::ShootBullet(from, e.pos, 0, Weapon(e), player, false);
        if (impacts_++ % 100 == 0) {
            GR_LOG("combat: GMod bullet %u that hit nobody made an RDR2 bullet, %.1f m", impacts_, length);
        }
    }

    void Hit(const GrEvent& e, native::Entity player) noexcept {
        if (e.handle == 0 || !native::DoesEntityExist(e.handle)) return;
        const bool ped = native::IsEntityAPed(e.handle);
        int bone = 0;
        GrVec3 unused{};
        const int damage = static_cast<int>(std::lround(e.damage < 1.0f ? 1.0f : e.damage));
        if (ped) {
            NearestBone(e.handle, e.pos, bone, unused);
            native::ApplyDamageToPed(e.handle, damage, bone, player);
        }
        const float push = e.force < kMaxPush ? e.force : kMaxPush;
        if (push > 1.0f) {
            if (ped) native::SetPedToRagdoll(e.handle, 1000, 3000);
            const GrVec3 v = native::GetEntityVelocity(e.handle);
            native::SetEntityVelocity(e.handle, {v.x + e.dir.x * push, v.y + e.dir.y * push, v.z + e.dir.z * push + 1.0f});
        }
        GR_LOG("combat: %d hit for %d damage (bone %d), pushed %.1f m/s", e.handle, damage, bone, push);
    }

    void Explosion(const GrEvent& e, native::Entity player, uint64_t now_ms) noexcept {
        // RDR2's dynamite is about 5 m across; GMod's grenades and barrels are given as their
        // blast radius.
        float scale = e.radius / 5.0f;
        scale = scale < 0.3f ? 0.3f : scale > 3.0f ? 3.0f : scale;
        native::AddOwnedExplosion(player, e.pos, explosion_tag::EXP_TAG_DYNAMITE, scale, 1.0f);
        const GrVec3 me = native::GetEntityCoords(player);
        const float dx = me.x - e.pos.x, dy = me.y - e.pos.y, dz = me.z - e.pos.z;
        if (dx * dx + dy * dy + dz * dz < kQuietRadius * kQuietRadius) quiet_until_ms_ = now_ms + kQuietMs;
        GR_LOG("combat: explosion at (%.1f, %.1f, %.1f), radius %.1f m, scale %.2f", e.pos.x, e.pos.y, e.pos.z, e.radius,
               scale);
    }

    void Hurt(bool driving, native::Entity ped, uint64_t now_ms) noexcept {
        if (!driving || ped == 0 || native::IsEntityDead(ped)) {
            have_health_ = false;
            return;
        }
        const int max = native::GetPedMaxHealth(ped);
        const int health = native::GetEntityHealth(ped);
        if (max <= 0) return;
        // Against what the health was after last frame's refill, not against the maximum:
        // the refill does not always reach it (seen: 229 of 250, every frame).
        // Works around: the audible bullet of a GMod shot (Shot) starts at the player's own
        // hidden ped and costs it a point of health each time (seen: 1 of 250 a shot, and
        // GMod's health went 100 to 99 after a few). A loss that small right after a shot is
        // the shot's own.
        const bool own_shot = now_ms - shot_ms_ < kOwnShotMs && last_health_ - health <= kOwnShotLoss;
        if (have_health_ && health < last_health_ && now_ms >= quiet_until_ms_ && !own_shot) {
            hurt_part_ += static_cast<float>(last_health_ - health) * 100.0f / static_cast<float>(max);
            const uint32_t whole = static_cast<uint32_t>(hurt_part_);
            hurt_part_ -= static_cast<float>(whole);
            hurt_total_ += whole;
            GR_LOG("combat: RDR2 hurt the player's ped by %d of %d health: %u for GMod", last_health_ - health, max,
                   whole);
        }
        if (health < max) native::SetEntityHealth(ped, max);
        last_health_ = native::GetEntityHealth(ped);
        have_health_ = true;
    }

public:
    // A teleport GMod asked for since the last call (PlayerSync::RequestTeleport does it).
    bool TakeTeleport(GrVec3& to) noexcept {
        if (!has_teleport_) return false;
        has_teleport_ = false;
        to = teleport_;
        return true;
    }

private:
    GrVec3 teleport_{};
    bool has_teleport_ = false;
    bool synced_ = false;
    uint32_t generation_ = 0;
    uint32_t last_seq_ = 0;
    uint32_t hurt_total_ = 0;
    float hurt_part_ = 0.0f;
    bool have_health_ = false;
    int last_health_ = 0;
    native::Entity watch_handle_ = 0;
    int watch_health_ = 0;
    int watch_fallback_ = 0;  // damage to apply if the watched bullet did nothing (animals)
    uint64_t watch_ms_ = 0;
    uint64_t quiet_until_ms_ = 0;
    uint64_t shot_ms_ = 0;
    uint32_t impacts_ = 0;
};

}  // namespace gr
