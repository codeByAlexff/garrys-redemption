// World sync, RDR2 side: lists the peds, horses, vehicles and objects (spawned or RDR2's
// own) near the player for GMod to mirror as proxies, and makes the ones GMod is simulating (held by the
// physgun, thrown, frozen) follow their proxies: peds as ragdolls, the rest as they are.
//
// A driven ped is ragdolled and pushed toward its proxy by setting its velocity every
// frame: the proxy's velocity plus a pull toward where the proxy is. RDR2's physics keeps
// it out of the terrain and Euphoria makes it limp. When GMod lets go, the ped keeps its
// last velocity and stays ragdolled for a while, then RDR2 stands it up (or it stays down,
// if the throw killed it). Its reactions, and everyone else's, are RDR2's own.
//
// A vehicle or object is put exactly where its proxy is, every frame, position and rotation
// (there is no native to set how fast it spins), with the proxy's velocity so it flies on
// when let go. Frozen, it is frozen in RDR2 too.
//
// Everything here calls natives: script thread only. No allocation: fixed arrays.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gr_log.h"
#include "gr_protocol.h"
#include "natives.h"
#include "spawn_sync.h"

namespace gr {

class WorldSync {
public:
    void Reset() noexcept {
        tracked_count_ = 0;
        object_count_ = 0;
        frames_to_object_scan_ = 0;
        std::memset(models_, 0, sizeof(models_));
    }

    // Before Scan: applies GMod's driven list. `shift` is PlayerSync's ground shift: the
    // guest's heights are without it. With `connected` false everything is let go.
    void Drive(bool connected, const GrGuestFrame& guest, native::Entity player, float shift) noexcept {
        const uint32_t count = connected ? std::min(guest.driven_count, GR_MAX_DRIVEN) : 0;

        // Let go of what GMod no longer lists, or that is gone from the game.
        for (uint32_t t = 0; t < tracked_count_;) {
            const native::Entity handle = tracked_[t].handle;
            bool listed = false;
            for (uint32_t i = 0; i < count; ++i) listed = listed || guest.driven[i].handle == handle;
            const bool exists = native::DoesEntityExist(handle);
            if (listed && exists) {
                ++t;
                continue;
            }
            if (exists) {
                if (tracked_[t].ped) {
                    if (tracked_[t].rigid) {
                        Loosen(tracked_[t]);
                    } else if (!tracked_[t].standing) {
                        // A last ragdoll so it lands and rolls rather than standing up mid-air.
                        native::SetPedToRagdoll(handle, kReleaseRagdollMinMs, kReleaseRagdollMaxMs);
                    }
                } else {
                    native::FreezeEntityPosition(handle, false);
                    native::MakeFallable(handle);
                    native::ActivatePhysics(handle);
                    native::SetEntityVelocity(handle, tracked_[t].vel);
                    Falling(handle);
                }
                // A dead animal revived while held dies again as it is let go, as a ragdoll.
                if (tracked_[t].revived) native::SetEntityHealth(handle, 0);
                const GrVec3 v = native::GetEntityVelocity(handle);
                GR_LOG("world: GMod let go of %d at %.1f m/s", handle, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
            }
            tracked_[t] = tracked_[--tracked_count_];
        }

        WatchFalling();
        for (uint32_t i = 0; i < count; ++i) {
            const GrDriven& d = guest.driven[i];
            if (d.handle == 0 || d.handle == player || !native::DoesEntityExist(d.handle)) continue;
            Tracked* t = Find(d.handle);
            if (!t) {
                if (tracked_count_ == GR_MAX_DRIVEN) continue;
                t = &tracked_[tracked_count_++];
                t->handle = d.handle;
                t->ped = native::IsEntityAPed(d.handle);
                t->flags = 0;
                t->vel = {0.0f, 0.0f, 0.0f};
                t->rigid = false;
                t->standing = false;
                t->revived = false;
                t->freed = false;
                t->refused = 0;
                if (!t->ped) native::MakeFallable(d.handle);
                GR_LOG("world: GMod took %s %d%s", t->ped ? "ped" : "entity", d.handle,
                       (d.flags & GR_DRIVE_FROZEN) ? " (frozen)" : "");
            }
            // Welded to another entity (milestone 8's weld tool): it goes where that one goes.
            // A person RDR2 itself attached (a seat, a bench) is freed instead, below.
            if (native::IsEntityAttached(d.handle) && (!t->ped || WeldedByGMod(guest, d.handle))) {
                t->flags = d.flags;
                continue;
            }
            if (t->ped && !native::IsEntityDead(d.handle)) Free(*t);
            const bool frozen = (d.flags & GR_DRIVE_FROZEN) != 0;
            const GrVec3 target{d.pos.x, d.pos.y, d.pos.z + shift};
            if (!t->ped) {
                const bool was_frozen = (t->flags & GR_DRIVE_FROZEN) != 0;
                t->flags = d.flags;
                if (frozen != was_frozen) native::FreezeEntityPosition(d.handle, frozen);
                native::SetEntityCoordsNoOffset(d.handle, target);
                native::SetEntityRotation(d.handle, d.rot);
                t->vel = frozen ? GrVec3{0.0f, 0.0f, 0.0f} : d.vel;
                if (!frozen) {
                    native::ActivatePhysics(d.handle);
                    native::SetEntityVelocity(d.handle, t->vel);
                }
                continue;
            }
            t->flags = d.flags;
            // A living ped in the physgun's beam, or frozen by it, is held as it is: put
            // exactly where its proxy is, turned as it is, frozen so that RDR2 does not see
            // it as falling. Works around: held as a ragdoll and pulled along by its
            // velocity, it tumbled and flailed in the beam as if falling the whole time
            // (user report). It becomes a ragdoll when it is let go (Loosen).
            // The user then asked for the opposite (2026-10-03): held rigid it stands in its
            // bind pose, arms out, and a ragdoll is wanted. So this is only with `hold_rigid=1`
            // in the ini; by default a held ped is a ragdoll pulled to its proxy (below).
            const bool held = (d.flags & GR_DRIVE_HELD) != 0;
            if (rigid_hold_ && (held || frozen) && !native::IsEntityDead(d.handle)) {
                if (!t->rigid) {
                    if (native::IsPedRagdoll(d.handle)) native::ClearPedTasksImmediately(d.handle);
                    native::SetPedCanRagdoll(d.handle, false);
                    native::SetPedGravity(d.handle, false);
                    t->rigid = true;
                    t->standing = false;
                }
                native::SetEntityCoordsNoOffset(d.handle, target);
                native::SetEntityRotation(d.handle, d.rot);
                t->vel = frozen ? GrVec3{0.0f, 0.0f, 0.0f} : d.vel;
                native::SetEntityVelocity(d.handle, t->vel);
                continue;
            }
            if (t->rigid) Loosen(*t);
            // Works around: a dead animal could not be picked up properly (user: "animals still
            // don't ragdoll properly"). Its carcass ignores velocity, forces, ACTIVATE_PHYSICS,
            // SET_ENTITY_DYNAMIC and SET_PED_TO_RAGDOLL (refused), so held it hung rigid and
            // thrown it stayed in mid-air (seen with a deer). Dead people are fine. While GMod
            // has it the animal is brought back to life, so it is a ragdoll like a living one,
            // and it dies again the moment it is let go, falling on as a body (see the release).
            if (!t->revived && native::IsEntityDead(d.handle) && !native::IsPedHuman(d.handle)) {
                native::ResurrectPed(d.handle);
                t->revived = true;
                GR_LOG("world: dead animal %d revived while GMod has it", d.handle);
            }
            // Put down gently: it stands, and RDR2 has it from here on.
            if (t->standing) continue;
            // Kept ragdolled for as long as GMod has it. The timer is reset rather than
            // the ragdoll restarted, which would make it twitch every frame.
            if (native::IsEntityDead(d.handle)) {
                // Works around: a body lying still has gone to sleep and ignored the velocity
                // it was given, so the dead could not be picked up (user report). Its physics
                // is woken every frame, and a body left far behind its proxy is put there.
                native::ActivatePhysics(d.handle);
                const GrVec3 at = native::GetEntityCoords(d.handle);
                const float dx = target.x - at.x, dy = target.y - at.y, dz = target.z - at.z;
                if (dx * dx + dy * dy + dz * dz > kBodySnap * kBodySnap) native::SetEntityCoordsNoOffset(d.handle, target);
            } else if (!native::IsPedRagdoll(d.handle)) {
                native::SetPedToRagdoll(d.handle, kHeldRagdollMs, kHeldRagdollMs);
                ++t->refused;
            } else {
                native::ResetPedRagdollTimer(d.handle);
                t->refused = 0;
            }
            Pull(d.handle, target, frozen ? GrVec3{0.0f, 0.0f, 0.0f} : d.vel);
        }
    }

    // Lists the nearest peds, vehicles and spawned objects around `center` (the player's
    // feet, real RDR2 height) into `host`, heights as RDR2 has them.
    void Scan(native::Entity player, const GrVec3& center, const SpawnSync& spawn, GrHostFrame& host) noexcept {
        candidate_count_ = 0;
        int peds = worldGetAllPeds(all_, kMaxPeds);
        if (peds <= 0) peds = NearPoint(kNearPeds, center, kRange, all_, kMaxPeds);
        AddCandidates(all_, peds < kMaxPeds ? peds : kMaxPeds, GR_ENT_PED, player, center);
        int vehicles = worldGetAllVehicles(all_, kMaxPeds);
        if (vehicles <= 0) vehicles = NearPoint(kNearVehicles, center, kRange, all_, kMaxPeds);
        AddCandidates(all_, vehicles < kMaxPeds ? vehicles : kMaxPeds, GR_ENT_VEHICLE, player, center);
        const uint32_t objects = spawn.Objects(all_, kMaxPeds);
        AddCandidates(all_, static_cast<int>(objects), GR_ENT_OBJECT, player, center);
        // Nearest first, so a crowd fills the list from the player outwards.
        const uint32_t keep = std::min<uint32_t>(candidate_count_, kMaxListed);
        std::partial_sort(candidates_, candidates_ + keep, candidates_ + candidate_count_,
                          [](const Candidate& a, const Candidate& b) { return a.d2 < b.d2; });

        // RDR2's own objects (crates, barrels, fences) after them, in slots of their own so
        // they never crowd out the people.
        RefreshObjects(player, center, spawn);
        uint32_t listed = keep;
        for (uint32_t k = 0; k < object_count_ && listed < kMaxListed + kMaxObjects; ++k) {
            const native::Entity h = objects_[k];
            if (!native::DoesEntityExist(h)) continue;
            candidates_[listed++] = {h, 0.0f, native::GetEntityCoords(h), GR_ENT_OBJECT};
        }

        for (uint32_t i = 0; i < listed; ++i) {
            const native::Entity ped = candidates_[i].ped;
            const uint32_t kind = candidates_[i].kind;
            GrEntity& e = host.entities[i];
            const uint32_t model = native::GetEntityModel(ped);
            const Model& m = Dimensions(model);
            e.handle = ped;
            e.model = model;
            uint16_t flags = 0;
            if (kind == GR_ENT_PED) {
                e.type = static_cast<uint16_t>(m.horse                    ? GR_ENT_HORSE
                                               : native::IsPedHuman(ped) ? GR_ENT_PED
                                                                         : GR_ENT_ANIMAL);
                if (native::IsEntityDead(ped)) flags |= GR_ENTF_DEAD;
                if (native::IsPedRagdoll(ped)) flags |= GR_ENTF_RAGDOLL;
            } else {
                e.type = static_cast<uint16_t>(kind);
            }
            if (spawn.IsSpawned(ped)) flags |= GR_ENTF_SPAWNED;
            if (const Tracked* t = Find(ped)) {
                flags |= GR_ENTF_DRIVEN;
                if (t->flags & GR_DRIVE_FROZEN) flags |= GR_ENTF_FROZEN;
            }
            e.flags = flags;
            e.pos = candidates_[i].pos;
            e.rot = native::GetEntityRotation(ped);
            e.vel = native::GetEntityVelocity(ped);
            e.bounds_min = m.min;
            e.bounds_max = m.max;
            e.health = static_cast<float>(native::GetEntityHealth(ped));
        }
        host.entity_count = listed;

        // Every 10 s: what RDR2's pools held and what was listed, so an empty list (seen: no
        // proxies at all after a teleport to Valentine) can be told from an empty town.
        const uint64_t now = NowMs();
        if (now >= next_report_ms_) {
            next_report_ms_ = now + 10000;
            GR_LOG("world: %d peds, %d vehicles and %d objects found, %u within %.0f m, %u objects kept; %u listed; "
                   "%u queries in 10 s had to ask the game for what is near (Script Hook's pools answered nothing)",
                   peds, vehicles, pool_objects_, candidate_count_, kRange, object_count_, listed, near_point_frames_);
            near_point_frames_ = 0;
        }
    }

    uint32_t driven_count() const noexcept { return tracked_count_; }
    // true: a living ped in the physgun's beam is held stiff as it stands, not as a ragdoll.
    void SetRigidHold(bool on) noexcept { rigid_hold_ = on; }
    // Whether GMod is moving this entity itself (held, thrown or frozen by the physgun).
    bool IsDriven(native::Entity handle) const noexcept { return Find(handle) != nullptr; }

    // Whether `h` is one of RDR2's objects listed for GMod: big enough, small enough and
    // solid (Solid). Bushes and ground debris are objects too, and are not.
    bool ListsObject(native::Entity h) const noexcept {
        for (uint32_t k = 0; k < object_count_; ++k) {
            if (objects_[k] == h) return true;
        }
        return false;
    }

private:
    static constexpr int kMaxPeds = 1024;              // per pool asked
    static constexpr int kMaxPoolObjects = 16384;      // objects asked (a town has thousands)
    static constexpr int kNearPeds = 1, kNearVehicles = 2, kNearObjects = 3;  // NearPoint's types
    static constexpr uint32_t kMaxCandidates = 2048;
    static constexpr uint32_t kMaxListed = 64;
    static constexpr float kRange = 60.0f;             // metres around the player
    static constexpr float kPullPerSecond = 12.0f;     // how hard a driven ped is pulled to its proxy
    static constexpr float kMaxSpeed = 40.0f;          // m/s, so a glitch cannot fling a ped across the map
    static constexpr float kBodySnap = 1.5f;           // metres a dead body may trail its proxy
    static constexpr float kGentleSpeed = 1.5f;        // m/s and ...
    static constexpr float kGentleHeight = 1.6f;       // ... metres of the origin above the ground: set down, not dropped
    static constexpr int kHeldRagdollMs = 10000;
    static constexpr int kReleaseRagdollMinMs = 2000;
    static constexpr int kReleaseRagdollMaxMs = 5000;
    static constexpr uint32_t kModelSlots = 256;       // power of two
    static constexpr uint32_t kMaxObjects = 128;       // RDR2 objects listed after the people
    static constexpr float kObjectRange = 40.0f;
    static constexpr int kObjectScanFrames = 15;
    static constexpr float kObjectMinSize = 0.3f;      // metres, longest side
    static constexpr float kObjectMaxSize = 8.0f;
    static constexpr float kObjectMaxVolume = 40.0f;   // cubic metres of box

    struct Model {
        uint32_t hash;
        bool horse;
        int8_t solid;  // objects: 1 a ray hits it, -1 rays pass through, 0 not known yet
        GrVec3 min;
        GrVec3 max;
    };

    // Works around: only map collision (terrain_sync.h's rays) reached GMod, so RDR2's
    // crates, barrels, fences and carts were not there for GMod's physics and thrown things
    // passed through them. The nearest solid objects become proxies like the spawned ones.
    // The pool can hold thousands, so it is gone through every kObjectScanFrames frames;
    // in between only the kept handles are read.
    void RefreshObjects(native::Entity player, const GrVec3& center, const SpawnSync& spawn) noexcept {
        if (frames_to_object_scan_-- > 0) return;
        frames_to_object_scan_ = kObjectScanFrames;
        int n = worldGetAllObjects(all_objects_, kMaxPoolObjects);
        if (n <= 0) n = NearPoint(kNearObjects, center, kObjectRange, all_objects_, kMaxPoolObjects);
        pool_objects_ = n;
        object_candidates_ = 0;
        for (int i = 0; i < n && i < kMaxPoolObjects; ++i) {
            const native::Entity h = all_objects_[i];
            if (h == 0 || h == player || spawn.IsSpawned(h)) continue;
            const GrVec3 p = native::GetEntityCoords(h);
            const float dx = p.x - center.x, dy = p.y - center.y, dz = p.z - center.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > kObjectRange * kObjectRange) continue;
            Model& m = Dimensions(native::GetEntityModel(h));
            const float sx = m.max.x - m.min.x, sy = m.max.y - m.min.y, sz = m.max.z - m.min.z;
            const float longest = std::max(sx, std::max(sy, sz));
            // Cups and bottles would only be clutter in GMod's physics. Tents and other big
            // pieces are mostly air in their box: walking into a tent would stop at its box.
            if (longest < kObjectMinSize || longest > kObjectMaxSize || sx * sy * sz > kObjectMaxVolume) continue;
            // Carried (a lantern, a gun), hidden or not solid: not something to bump into.
            if (native::IsEntityAttached(h) || !native::IsEntityVisible(h) || native::GetEntityCollisionDisabled(h)) {
                continue;
            }
            if (!Solid(h, p, m)) continue;
            if (object_candidates_ < kMaxCandidates) object_pool_[object_candidates_++] = {h, d2, p, GR_ENT_OBJECT};
        }
        object_count_ = std::min<uint32_t>(object_candidates_, kMaxObjects);
        std::partial_sort(object_pool_, object_pool_ + object_count_, object_pool_ + object_candidates_,
                          [](const Candidate& a, const Candidate& b) { return a.d2 < b.d2; });
        for (uint32_t k = 0; k < object_count_; ++k) objects_[k] = object_pool_[k].ped;
    }

    // Works around: bushes and ground debris are objects too, with boxes metres wide (seen:
    // 3.7 x 3.4 x 2.2 m bushes every few metres around the camp), and RDR2 lets the player
    // walk through them. An object counts if a ray against objects hits it: down through
    // the middle of its box, else along its longest side at half height. Decided once per
    // model; a ray that hits something else first leaves it undecided for the next scan.
    bool Solid(native::Entity h, const GrVec3& at, Model& m) noexcept {
        if (m.solid != 0) return m.solid > 0;
        const GrVec3 rot = native::GetEntityRotation(h);
        const float yaw = rot.z * 0.0174532925f;
        const float c = std::cos(yaw), sn = std::sin(yaw);
        auto world = [&](float x, float y, float z) {
            return GrVec3{at.x + x * c - y * sn, at.y + x * sn + y * c, at.z + z};
        };
        const float mx = (m.min.x + m.max.x) * 0.5f, my = (m.min.y + m.max.y) * 0.5f, mz = (m.min.z + m.max.z) * 0.5f;
        native::RayHit hit{};
        bool any = false;
        if (native::Raycast(world(mx, my, m.max.z + 0.05f), world(mx, my, m.min.z - 0.05f), native::kRayObjects, 0, hit)) {
            if (hit.entity == h) return (m.solid = 1) > 0;
            any = true;
        }
        const bool along_x = m.max.x - m.min.x >= m.max.y - m.min.y;
        const GrVec3 a = along_x ? world(m.min.x - 0.05f, my, mz) : world(mx, m.min.y - 0.05f, mz);
        const GrVec3 b = along_x ? world(m.max.x + 0.05f, my, mz) : world(mx, m.max.y + 0.05f, mz);
        if (native::Raycast(a, b, native::kRayObjects, 0, hit)) {
            if (hit.entity == h) return (m.solid = 1) > 0;
            any = true;
        }
        if (!any) m.solid = -1;
        return false;
    }

    // Works around: a boat or a tree thrown with the physgun stayed in the air once GMod let go
    // of it (user report: "it will start floating"). For kFallWatchMs after the release, an
    // entity that is in the air and not coming down is woken and given a push down. Logged,
    // so what had stopped it shows up.
    struct Fall {
        native::Entity handle;
        uint64_t until;
        int still;
        bool told;
    };
    static constexpr uint64_t kFallWatchMs = 4000;
    static constexpr int kFallStillFrames = 5;
    Fall falls_[8]{};

    void Falling(native::Entity handle) noexcept {
        Fall* slot = &falls_[0];
        for (Fall& f : falls_) {
            if (f.handle == 0 || NowMs() > f.until) {
                slot = &f;
                break;
            }
        }
        *slot = {handle, NowMs() + kFallWatchMs, 0, false};
    }

    void WatchFalling() noexcept {
        const uint64_t now = NowMs();
        for (Fall& f : falls_) {
            if (f.handle == 0) continue;
            if (now > f.until || !native::DoesEntityExist(f.handle) || Find(f.handle) != nullptr) {
                f.handle = 0;
                continue;
            }
            const GrVec3 v = native::GetEntityVelocity(f.handle);
            // A boat afloat is not hanging in the air. Nor is a wagon on its wheels: its origin
            // is about a metre up (seen: a stagecoach standing still taken for one hanging, and
            // pushed down), so the height counts from the bottom of its model's box.
            const float bottom = -Dimensions(native::GetEntityModel(f.handle)).min.z;
            const float lift = bottom > 0.0f ? bottom : 0.0f;
            const bool up_there =
                native::GetEntityHeightAboveGround(f.handle) > lift + 0.5f && !native::IsEntityInWater(f.handle);
            f.still = up_there && v.z > -0.2f && v.z < 0.2f && v.x * v.x + v.y * v.y < 0.04f ? f.still + 1 : 0;
            if (f.still < kFallStillFrames) continue;
            native::FreezeEntityPosition(f.handle, false);
            native::MakeFallable(f.handle);
            native::ActivatePhysics(f.handle);
            native::SetEntityVelocity(f.handle, {0.0f, 0.0f, -1.0f});
            f.still = 0;
            if (!f.told) {
                GR_LOG("world: falling watchdog: %d hung %.1f m up after GMod let go (frozen %d); pushed down", f.handle,
                       native::GetEntityHeightAboveGround(f.handle), native::IsEntityFrozenNow(f.handle) ? 1 : 0);
                f.told = true;
            }
        }
    }

    struct Tracked {
        native::Entity handle;
        uint32_t flags;
        bool ped;
        bool rigid;     // a living ped held as it is (frozen, placed every frame)
        bool standing;  // put down gently: left to RDR2 though GMod still lists it
        bool revived;
        bool freed;     // taken out of a seat, a vehicle or a scenario once already
        int refused;    // frames in a row SET_PED_TO_RAGDOLL has not made it a ragdoll
        GrVec3 vel;     // the velocity to leave it with when let go
    };

    // Works around the user's "it mostly fails when entities are doing an animation" (town
    // people): a person sitting on a bench, a chair or a wagon seat is attached to it by RDR2,
    // and the attachment was taken for a weld, so the person was never moved. Others in a
    // scenario (leaning, working, eating) or in a vehicle refused SET_PED_TO_RAGDOLL, and
    // velocities do nothing to an animated ped. So a living person GMod takes is first let
    // out of all of that: detached, and its tasks (scenario, seat) ended at once. Again if
    // the ragdoll is still refused after a few frames.
    static void Free(Tracked& t) noexcept {
        const native::Entity ped = t.handle;
        const bool attached = native::IsEntityAttached(ped);
        const bool stuck = t.refused >= kRefusedFrames;
        if (t.freed && !attached && !stuck) return;
        const bool busy = attached || native::IsPedInAnyVehicle(ped) || native::IsPedUsingAnyScenario(ped);
        if (!busy && !stuck) {
            t.freed = true;
            return;
        }
        if (attached) native::DetachEntity(ped);
        native::ClearPedTasksImmediately(ped);
        native::SetPedCanRagdoll(ped, true);
        t.freed = true;
        // One that still refuses is tried again only after about a second, not every 5 frames.
        t.refused = stuck ? -kRefusedFrames * 30 : 0;
        GR_LOG("world: %d freed for GMod (%s)", ped,
               attached ? "attached to a seat or object" : stuck ? "refused to ragdoll" : "in a scenario or vehicle");
    }
    static constexpr int kRefusedFrames = 5;

    // Works around: in towns Script Hook RDR2's pool walk (worldGetAll*) answered nothing on
    // most frames (seen in Valentine and Strawberry: 97 peds for a few frames, then 0 for a
    // hundred), so nothing there had a proxy and the physgun had nothing to take (user: "the
    // physgun stopped working"). The game's own query of what is near a point fills `out`
    // instead. Its `type`, measured: 1 peds, 2 vehicles, 3 objects (0 nothing).
    int NearPoint(int type, const GrVec3& center, float radius, int* out, int max) noexcept {
        if (itemset_ == 0 || !native::IsItemsetValid(itemset_)) itemset_ = native::CreateItemset();
        native::ClearItemset(itemset_);
        native::GetEntitiesNearPoint(center, radius, itemset_, type);
        int n = native::GetItemsetSize(itemset_);
        if (n > max) n = max;
        for (int i = 0; i < n; ++i) out[i] = native::GetIndexedItemInItemset(i, itemset_);
        native::ClearItemset(itemset_);
        ++near_point_frames_;
        return n;
    }
    int itemset_ = 0;
    uint32_t near_point_frames_ = 0;

    // Whether GMod lists a weld that holds `handle` to another entity (constraint_sync.h made
    // that attachment, so it is kept).
    static bool WeldedByGMod(const GrGuestFrame& guest, native::Entity handle) noexcept {
        const uint32_t n = std::min(guest.constraint_count, GR_MAX_CONSTRAINTS);
        for (uint32_t i = 0; i < n; ++i) {
            const GrConstraint& c = guest.constraints[i];
            if (c.type == GR_CON_WELD && c.a == handle && c.b != 0) return true;
        }
        return false;
    }

    // A held ped is let go. Thrown or dropped from a height it becomes a ragdoll with the
    // throw's velocity and Euphoria lands it and stands it up; set down slowly near the
    // ground it just stands.
    static void Loosen(Tracked& t) noexcept {
        t.rigid = false;
        native::SetPedGravity(t.handle, true);
        native::SetPedCanRagdoll(t.handle, true);
        const float speed = std::sqrt(t.vel.x * t.vel.x + t.vel.y * t.vel.y + t.vel.z * t.vel.z);
        const float height = native::GetEntityHeightAboveGround(t.handle);
        if (speed < kGentleSpeed && height < kGentleHeight) {
            t.standing = true;
            GR_LOG("world: %d set down (%.1f m/s, %.1f m up)", t.handle, speed, height);
            return;
        }
        native::SetPedToRagdoll(t.handle, kReleaseRagdollMinMs, kReleaseRagdollMaxMs);
        native::SetEntityVelocity(t.handle, t.vel);
        GR_LOG("world: %d let go as a ragdoll (%.1f m/s, %.1f m up)", t.handle, speed, height);
    }
    struct Candidate {
        native::Entity ped;  // or vehicle, or object
        float d2;
        GrVec3 pos;
        uint32_t kind;       // GR_ENT_PED (any ped), GR_ENT_VEHICLE or GR_ENT_OBJECT
    };

    void AddCandidates(const int* handles, int count, uint32_t kind, native::Entity player,
                       const GrVec3& center) noexcept {
        for (int i = 0; i < count && candidate_count_ < kMaxCandidates; ++i) {
            const native::Entity h = handles[i];
            if (h == 0 || h == player) continue;
            const GrVec3 p = native::GetEntityCoords(h);
            const float dx = p.x - center.x, dy = p.y - center.y, dz = p.z - center.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > kRange * kRange) continue;
            candidates_[candidate_count_++] = {h, d2, p, kind};
        }
    }

    // Sets the velocity that takes `handle` to `target` while moving at `vel`.
    static void Pull(native::Entity handle, const GrVec3& target, const GrVec3& vel) noexcept {
        const GrVec3 at = native::GetEntityCoords(handle);
        GrVec3 v{(target.x - at.x) * kPullPerSecond + vel.x, (target.y - at.y) * kPullPerSecond + vel.y,
                 (target.z - at.z) * kPullPerSecond + vel.z};
        const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (speed > kMaxSpeed) {
            const float k = kMaxSpeed / speed;
            v = {v.x * k, v.y * k, v.z * k};
        }
        native::SetEntityVelocity(handle, v);
    }

    Tracked* Find(native::Entity handle) noexcept {
        for (uint32_t t = 0; t < tracked_count_; ++t) {
            if (tracked_[t].handle == handle) return &tracked_[t];
        }
        return nullptr;
    }
    const Tracked* Find(native::Entity handle) const noexcept {
        return const_cast<WorldSync*>(this)->Find(handle);
    }

    // Model box and kind, looked up once per model. Open addressing; when full, the slot
    // is simply overwritten (it is only a cache).
    Model& Dimensions(uint32_t model) noexcept {
        uint32_t slot = (model * 2654435761u) & (kModelSlots - 1);
        for (uint32_t probe = 0; probe < 8; ++probe) {
            Model& m = models_[(slot + probe) & (kModelSlots - 1)];
            if (m.hash == model) return m;
            if (m.hash == 0) {
                slot = (slot + probe) & (kModelSlots - 1);
                break;
            }
        }
        Model& m = models_[slot];
        m.hash = model;
        m.horse = native::IsModelAHorse(model);
        m.solid = 0;
        native::GetModelDimensions(model, m.min, m.max);
        return m;
    }

    int all_[kMaxPeds] = {};
    int all_objects_[kMaxPoolObjects] = {};
    int pool_objects_ = 0;
    Candidate object_pool_[kMaxCandidates] = {};
    uint32_t object_candidates_ = 0;
    native::Entity objects_[kMaxObjects] = {};
    uint32_t object_count_ = 0;
    int frames_to_object_scan_ = 0;
    Candidate candidates_[kMaxCandidates] = {};
    uint32_t candidate_count_ = 0;
    bool rigid_hold_ = false;
    uint64_t next_report_ms_ = 0;
    Tracked tracked_[GR_MAX_DRIVEN] = {};
    uint32_t tracked_count_ = 0;
    Model models_[kModelSlots] = {};
};

}  // namespace gr
