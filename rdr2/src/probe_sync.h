// Probes (GrProbeRequest, GrProbeResult): what of RDR2's world a GMod projectile is about to hit.
//
// GMod's copy of RDR2's world is the ground and the map's walls near the player (terrain
// chunks) and a ring of panels right around it (near_sync.h). Its bullets, grenades, the AR2's
// energy ball, rockets and thrown things flew through every tree, house and rock beyond that
// (user: "I want the weapons to be able to collide with objects like trees, houses"). For each
// fast thing GMod lists the stretch it is about to cover; this tests it against RDR2's world and
// answers with the first surface, which GMod stands a solid panel on (gr/probes.lua).
//
// What counts: the map (ground, buildings, rocks), RDR2's objects that have no proxy (a proxy
// is solid in GMod already), and tree trunks: foliage hit by two rays, the probe and one
// kTrunkRise above it, at about the same distance on a face that is not a floor. Branches and
// bushes are foliage too, and a grenade does not bounce off a bush. People and vehicles have
// proxies.
//
// Answered once per guest frame (GMod ticks slower than RDR2 draws). Everything here calls
// natives: script thread only. No allocation.

#pragma once

#include <cmath>
#include <cstdint>

#include "gr_log.h"
#include "gr_protocol.h"
#include "natives.h"
#include "spawn_sync.h"
#include "world_sync.h"

namespace gr {

class ProbeSync {
public:
    void Reset() noexcept {
        for (GrProbeResult& r : results_) r = {};
        last_frame_ = 0;
        asked_ = hits_ = trunks_ = 0;
        next_report_ms_ = 0;
    }

    void Tick(bool driving, const GrGuestFrame& guest, native::Entity player_ped, const SpawnSync& spawn,
              const WorldSync& world, GrHostFrame& host) noexcept {
        const uint32_t count = guest.probe_count < GR_MAX_PROBES ? guest.probe_count : GR_MAX_PROBES;
        const uint64_t now = NowMs();
        if (now >= next_report_ms_) {
            if (next_report_ms_ != 0 && asked_ != 0) {
                GR_LOG("probe: in 10 s %u probes from GMod, %u hit RDR2's world (%u of them tree trunks)", asked_, hits_,
                       trunks_);
            }
            next_report_ms_ = now + 10000;
            asked_ = hits_ = trunks_ = 0;
        }
        if (!driving || count == 0) {
            for (GrProbeResult& r : results_) r = {};
            for (GrProbeResult& r : host.probes) r = {};
            return;
        }
        if (guest.frame != last_frame_) {
            last_frame_ = guest.frame;
            for (uint32_t i = 0; i < count; ++i) {
                results_[i] = Test(guest.probes[i], player_ped, spawn, world);
                ++asked_;
            }
            for (uint32_t i = count; i < GR_MAX_PROBES; ++i) results_[i] = {};
        }
        for (uint32_t i = 0; i < GR_MAX_PROBES; ++i) host.probes[i] = results_[i];
    }

private:
    static constexpr float kTrunkRise = 0.5f;    // metres between the two foliage rays
    static constexpr float kTrunkSpread = 0.35f; // metres: the two hits' distances may differ by this
    static constexpr float kFloorNormal = 0.7f;  // a foliage face this flat is not a trunk's

    GrProbeResult Test(const GrProbeRequest& p, native::Entity player_ped, const SpawnSync& spawn,
                       const WorldSync& world) noexcept {
        GrProbeResult r{};
        r.id = p.id;
        if (p.id == 0) return r;
        const float dx = p.to.x - p.from.x, dy = p.to.y - p.from.y, dz = p.to.z - p.from.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 0.01f) return r;
        float best = length + 1.0f;

        native::RayHit hit{};
        if (native::Raycast(p.from, p.to, native::kRayMap | native::kRayObjects, player_ped, hit)) {
            // An object with a proxy is solid in GMod already.
            const bool proxied = hit.entity != 0 && (spawn.IsSpawned(hit.entity) || world.ListsObject(hit.entity));
            if (!proxied) {
                best = Along(p.from, hit.pos, dx, dy, dz, length);
                r.flags = GR_PROBEF_HIT | (hit.entity != 0 ? GR_PROBEF_OBJECT : 0u);
                r.pos = hit.pos;
                r.normal = hit.normal;
            }
        }

        native::RayHit low{};
        if (native::Raycast(p.from, p.to, native::kRayFoliage, player_ped, low) && std::fabs(low.normal.z) < kFloorNormal) {
            const float d_low = Along(p.from, low.pos, dx, dy, dz, length);
            if (d_low < best) {
                const GrVec3 from_high{p.from.x, p.from.y, p.from.z + kTrunkRise};
                const GrVec3 to_high{p.to.x, p.to.y, p.to.z + kTrunkRise};
                native::RayHit high{};
                if (native::Raycast(from_high, to_high, native::kRayFoliage, player_ped, high) &&
                    std::fabs(Along(from_high, high.pos, dx, dy, dz, length) - d_low) < kTrunkSpread) {
                    r.flags = GR_PROBEF_HIT | GR_PROBEF_TRUNK;
                    r.pos = low.pos;
                    r.normal = low.normal;
                    ++trunks_;
                }
            }
        }
        if (r.flags & GR_PROBEF_HIT) ++hits_;
        return r;
    }

    // How far along the probe (unit direction d / length) `at` is from `from`.
    static float Along(const GrVec3& from, const GrVec3& at, float dx, float dy, float dz, float length) noexcept {
        return ((at.x - from.x) * dx + (at.y - from.y) * dy + (at.z - from.z) * dz) / length;
    }

    GrProbeResult results_[GR_MAX_PROBES]{};
    uint32_t last_frame_ = 0;
    uint32_t asked_ = 0, hits_ = 0, trunks_ = 0;
    uint64_t next_report_ms_ = 0;
};

}  // namespace gr
