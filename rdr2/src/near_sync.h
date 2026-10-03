// Near sync, RDR2 side: what is solid right around the player, for GMod's collision
// (GrHostFrame.nearby).
//
// The terrain chunks hold RDR2's ground and the map's walls on a 1 m grid, map collision only.
// The GMod player walked through what they miss (seen: the camera inside a tree trunk and
// inside a wooden fence after a few steps): trees, posts, thin rocks, and RDR2's objects
// (crates, fences, carts' loads), whose proxies are not solid to the player (proxies.lua).
// PlayerSync::Collide only put the player back after the fact, and never at a tree.
//
// Here a ring of kDirs short horizontal rays goes out from the player's feet, half of the
// ring each frame, each slot keeping its last answer:
//   - map and objects at knee and chest height. Peds and vehicles are left out: their
//     proxies already stop the player. So are objects GMod spawned (proxies too), and objects
//     WorldSync does not list: bushes and ground debris are objects as well, with boxes
//     metres wide, and RDR2 lets the player walk through them.
//   - foliage, at 0.9 m and 1.8 m. A foliage hit only counts when both heights hit at about
//     the same distance with a face turned towards the player: a trunk. Bushes and the
//     branches of a pine are foliage as well, and RDR2 lets the player walk through those.
// Each hit becomes a panel standing on the surface, facing back along the ray.
//
// Everything here calls natives: script thread only. No allocation.

#pragma once

#include <cmath>
#include <initializer_list>

#include "gr_protocol.h"
#include "natives.h"
#include "gr_log.h"
#include "spawn_sync.h"
#include "world_sync.h"

namespace gr {

class NearSync {
public:
    void Reset() noexcept {
        for (Slot& s : slots_) s = Slot{};
        next_ = 0;
    }

    // One frame while GMod has the player. `feet` is the player's feet (RDR2 world).
    void Tick(native::Entity ped, const GrVec3& feet, bool noclip, const SpawnSync& spawn, const WorldSync& world,
              GrHostFrame& host) noexcept {
        host.nearby_count = 0;
        if (noclip) {
            Reset();
            return;
        }
        // A jump of more than this since the last frame (a teleport, a rebase, an anchor):
        // what was found is somewhere else now.
        const float mx = feet.x - last_feet_.x, my = feet.y - last_feet_.y, mz = feet.z - last_feet_.z;
        if (mx * mx + my * my + mz * mz > kForget * kForget) Reset();
        last_feet_ = feet;

        for (uint32_t k = 0; k < kDirs / 2; ++k) {
            const uint32_t i = next_;
            next_ = (next_ + 1) % kDirs;
            Probe(i, ped, feet, spawn, world);
        }
        const uint64_t now_ms = NowMs();
        if (now_ms >= report_ms_) {
            if (report_ms_ != 0 && map_ + objects_ + trunks_ + skipped_ > 0) {
                GR_LOG("near: in 10 s, %u map, %u object and %u trunk panels; %u objects left out (bushes, proxies)",
                       map_, objects_, trunks_, skipped_);
            }
            map_ = objects_ = trunks_ = skipped_ = 0;
            report_ms_ = now_ms + 10000;
        }
        for (const Slot& s : slots_) {
            if (!s.used || host.nearby_count >= GR_MAX_NEAR) continue;
            GrWall& w = host.nearby[host.nearby_count++];
            w = s.wall;
            // Heights follow the player: the panel stands from above its step height to
            // above its head, wherever its feet are now.
            w.z0 = feet.z + kPanelBottom;
            w.z1 = feet.z + s.top;
        }
    }

private:
    struct Slot {
        bool used = false;
        GrWall wall{};
        float top = 0.0f;
    };

    static constexpr uint32_t kDirs = 16;      // GR_MAX_NEAR: one panel per direction
    static constexpr float kReach = 1.6f;      // metres from the player's middle
    static constexpr float kForget = 3.0f;
    static constexpr float kPanelBottom = 0.4f;  // above GMod's step height (18 units, 0.34 m)
    static constexpr float kPanelTop = 2.0f;
    static constexpr float kHalfWidth = 0.3f;
    static constexpr float kTrunkHalfWidth = 0.2f;
    static constexpr float kTrunkTop = 2.5f;
    static constexpr float kTrunkSpread = 0.25f;  // metres the two foliage hits may differ by

    // A face turned at least this much towards the ray, and standing (not a slope).
    static bool Facing(const native::RayHit& hit, float dx, float dy, float& nx, float& ny) noexcept {
        const float n = std::sqrt(hit.normal.x * hit.normal.x + hit.normal.y * hit.normal.y);
        if (n < 0.6f) return false;
        nx = hit.normal.x / n;
        ny = hit.normal.y / n;
        return nx * dx + ny * dy < -0.3f;
    }

    void Probe(uint32_t i, native::Entity ped, const GrVec3& feet, const SpawnSync& spawn,
               const WorldSync& world) noexcept {
        static_assert(kDirs <= GR_MAX_NEAR);
        Slot& slot = slots_[i];
        slot.used = false;
        const float a = 6.2831853f * (static_cast<float>(i) + 0.5f) / static_cast<float>(kDirs);
        const float dx = std::cos(a), dy = std::sin(a);

        float best = kReach + 1.0f;
        for (const float h : {0.55f, 1.3f}) {
            const GrVec3 from{feet.x, feet.y, feet.z + h};
            const GrVec3 to{feet.x + dx * kReach, feet.y + dy * kReach, feet.z + h};
            native::RayHit hit{};
            if (!native::Raycast(from, to, native::kRayMap | native::kRayObjects, ped, hit)) continue;
            if (hit.entity != 0 && (spawn.IsSpawned(hit.entity) || !world.ListsObject(hit.entity))) {
                ++skipped_;
                continue;
            }
            float nx = 0.0f, ny = 0.0f;
            if (!Facing(hit, dx, dy, nx, ny)) continue;
            const float d = (hit.pos.x - feet.x) * dx + (hit.pos.y - feet.y) * dy;
            if (d >= best) continue;
            best = d;
            Set(slot, hit.pos, nx, ny, kHalfWidth, kPanelTop);
            ++(hit.entity != 0 ? objects_ : map_);
        }

        // A trunk: both foliage rays hit, close together.
        native::RayHit low{}, high{};
        const GrVec3 from_low{feet.x, feet.y, feet.z + 0.9f};
        const GrVec3 to_low{feet.x + dx * kReach, feet.y + dy * kReach, feet.z + 0.9f};
        if (!native::Raycast(from_low, to_low, native::kRayFoliage, ped, low)) return;
        const GrVec3 from_high{feet.x, feet.y, feet.z + 1.8f};
        const GrVec3 to_high{feet.x + dx * kReach, feet.y + dy * kReach, feet.z + 1.8f};
        if (!native::Raycast(from_high, to_high, native::kRayFoliage, ped, high)) return;
        const float d_low = (low.pos.x - feet.x) * dx + (low.pos.y - feet.y) * dy;
        const float d_high = (high.pos.x - feet.x) * dx + (high.pos.y - feet.y) * dy;
        if (std::fabs(d_low - d_high) > kTrunkSpread || d_low >= best) return;
        float nx = 0.0f, ny = 0.0f;
        if (!Facing(low, dx, dy, nx, ny)) return;
        Set(slot, low.pos, nx, ny, kTrunkHalfWidth, kTrunkTop);
        ++trunks_;
    }

    static void Set(Slot& slot, const GrVec3& at, float nx, float ny, float half, float top) noexcept {
        slot.used = true;
        slot.top = top;
        slot.wall.x = at.x;
        slot.wall.y = at.y;
        slot.wall.left = half;
        slot.wall.right = half;
        slot.wall.nx = nx;
        slot.wall.ny = ny;
    }

    Slot slots_[kDirs];
    uint32_t next_ = 0;
    uint32_t map_ = 0, objects_ = 0, trunks_ = 0, skipped_ = 0;
    uint64_t report_ms_ = 0;
    GrVec3 last_feet_{};
};

}  // namespace gr
