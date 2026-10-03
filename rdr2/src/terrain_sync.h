// Terrain sync, RDR2 side: samples RDR2's ground around the player into heightfield chunks
// (GrChunk) that GMod builds into static physics meshes, so the GMod player walks, falls
// and lands on RDR2's ground with GMod's own movement, and thrown proxies land on it too.
//
// One column = one or two rays straight down, map collision only. A chunk is sampled
// against a reference height (the player's feet when it was started):
//   - the low ray starts 2 m above that and reaches 80 m down. Starting low is what finds
//     the floor under a roof, an awning or a tree, rather than their tops.
//   - only if it finds nothing, the high ray comes down from 150 m above to where the low
//     one started: the column is a hill higher than the player (a ray that starts inside
//     the ground finds nothing below it).
//
// Walls, fences and rock faces are what a heightfield cannot hold. Once a chunk's heights
// are in, a short horizontal ray runs along each 1 m edge of its grid, 0.5 m above the
// ground (1.5 m if that finds nothing: a low overhang, a window). A hit on something
// steeper than 45 degrees becomes a GrWall: a vertical panel up to 0.5 m either side of
// the hit, cut short where probes across the wall stop finding it (so doorways stay as
// wide as they are), from below the ground to the wall's top (a ray down just behind the
// face). Each chunk owns the edges on its low x and low y sides, so none is cast twice.
// PlayerSync::Collide still stops the player at objects and vehicles, which are not in
// here (map collision only).
//
// The rays run within a time budget per frame, nearest chunk first, measured from a point
// ahead of the player in the direction it moves. Slots are state: a chunk keeps its slot
// until the player is far from it, and GMod mirrors the table as it stands.
//
// Everything here calls natives: script thread only. No allocation: fixed arrays.

#pragma once

#include <cmath>
#include <cstring>
#include <initializer_list>

#include "gr_link.h"
#include "gr_log.h"
#include "gr_protocol.h"
#include "natives.h"

namespace gr {

class TerrainSync {
public:
    void Reset() noexcept {
        for (Slot& s : slots_) s = Slot{};
        // Never 0, and different from run to run, so a guest that still holds chunks from
        // an earlier run of the plugin sees every slot as new.
        serial_ = NowUs() | 1u;
        rays_ = 0;
        ray_us_ = 0;
        chunks_done_ = 0;
        next_report_ms_ = 0;
    }

    void SetBudgetUs(uint32_t us) noexcept { budget_us_ = us; }

    // One frame. `active`: GMod has the player and wants terrain. `feet` is where the
    // player is (RDR2 world), `vel` how it moves.
    void Tick(Link& link, bool active, native::Entity ped, const GrVec3& feet, const GrVec3& vel) noexcept {
        if (!active) return;
        const uint64_t now_ms = NowMs();
        const uint32_t start_us = NowUs();

        // Let go of chunks the player has left behind.
        for (uint32_t i = 0; i < GR_MAX_CHUNKS; ++i) {
            Slot& s = slots_[i];
            if (s.used && Distance(s.cx, s.cy, feet) > kDropRadius) Clear(link, i);
        }

        // A point a second ahead, capped: the chunks the player is heading for come first.
        float ax = vel.x;
        float ay = vel.y;
        const float speed = std::sqrt(ax * ax + ay * ay);
        if (speed > kLookAhead) {
            ax *= kLookAhead / speed;
            ay *= kLookAhead / speed;
        }
        const GrVec3 ahead{feet.x + ax, feet.y + ay, feet.z};

        for (int round = 0; round < 4; ++round) {
            const int i = Pick(feet, ahead, now_ms);
            if (i < 0) break;
            Slot& s = slots_[i];
            if (s.next == 0) Begin(s, feet.z);
            while (s.next < kWork && static_cast<uint32_t>(NowUs() - start_us) < budget_us_) {
                if (s.next < GR_CHUNK_SAMPLES) {
                    SampleColumn(s, ped);
                } else {
                    SampleEdge(s, ped);
                }
            }
            if (s.next < kWork) break;  // out of time this frame
            Finish(link, static_cast<uint32_t>(i), now_ms);
        }

        const uint32_t spent = NowUs() - start_us;
        frame_us_max_ = spent > frame_us_max_ ? spent : frame_us_max_;
        if (now_ms >= next_report_ms_) {
            if (next_report_ms_ != 0 && rays_ != 0) {
                uint32_t used = 0;
                uint32_t walls = 0;
                for (const Slot& s : slots_) {
                    used += s.used ? 1 : 0;
                    walls += s.used && s.sampled ? s.data.wall_count : 0;
                }
                GR_LOG("terrain: %u chunks in use with %u walls, %u sampled in the last 10 s, %u rays at %.1f us each, "
                       "worst frame %u us (budget %u us)",
                       used, walls, chunks_done_, rays_, static_cast<double>(ray_us_) / rays_, frame_us_max_,
                       budget_us_);
            }
            rays_ = 0;
            ray_us_ = 0;
            chunks_done_ = 0;
            frame_us_max_ = 0;
            next_report_ms_ = now_ms + 10000;
        }
    }

private:
    static constexpr float kChunkMetres = GR_CHUNK_CELLS * GR_CHUNK_CELL_CM / 100.0f;
    static constexpr float kCellMetres = GR_CHUNK_CELL_CM / 100.0f;
    // Chunks whose middle is this close are wanted; farther than kDropRadius they go. The
    // second must keep the count under GR_MAX_CHUNKS: pi * 64^2 / 16^2 is about 50.
    static constexpr float kWantRadius = 48.0f;
    static constexpr float kDropRadius = 64.0f;
    static constexpr float kLookAhead = 8.0f;
    static constexpr float kLowStart = 2.0f;
    static constexpr float kLowDepth = 80.0f;
    static constexpr float kHighStart = 150.0f;
    // A chunk with holes (collision not loaded yet when it was sampled) is tried again.
    static constexpr uint64_t kHoleRetryMs = 2000;
    static constexpr uint8_t kMaxTries = 6;
    // A chunk sampled from above or below the player is sampled again once the player is
    // near it: its low rays started at the wrong height. Seen at 12 m: chunks sampled
    // while the player came down in noclip had low roofs in the ground.
    static constexpr float kRefDrift = 2.5f;
    static constexpr float kRefDriftNear = 24.0f;
    // Walls.
    static constexpr uint32_t kEdges = 2 * GR_CHUNK_CELLS * GR_CHUNK_CELLS;
    static constexpr uint32_t kWork = GR_CHUNK_SAMPLES + kEdges;
    static constexpr float kWallLow = 0.5f;     // above the higher end of the edge
    static constexpr float kWallHigh = 1.5f;
    static constexpr float kWallSteep = 0.7f;   // horizontal part of the normal: about 45 degrees
    static constexpr float kWallHalf = 0.5f;    // half a cell: neighbouring panels meet
    static constexpr float kWallUnder = 0.3f;   // panels reach this far below the ground
    static constexpr float kWallMaxUp = 8.0f;   // and at most this far above the ray
    static constexpr float kWallProbe = 0.3f;   // probes start this far in front of the face

    struct Slot {
        bool used = false;
        bool sampled = false;  // finished at least once
        int32_t cx = 0;
        int32_t cy = 0;
        float zref = 0.0f;
        uint32_t next = 0;     // next column to sample; 0 = not started
        uint8_t tries = 0;
        uint32_t walls_dropped = 0;
        uint64_t done_ms = 0;
        GrChunk data{};
    };

    static int32_t ChunkOf(float metres) noexcept { return static_cast<int32_t>(std::floor(metres / kChunkMetres)); }

    static float Distance(int32_t cx, int32_t cy, const GrVec3& p) noexcept {
        const float dx = (static_cast<float>(cx) + 0.5f) * kChunkMetres - p.x;
        const float dy = (static_cast<float>(cy) + 0.5f) * kChunkMetres - p.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    int Find(int32_t cx, int32_t cy) const noexcept {
        for (int i = 0; i < static_cast<int>(GR_MAX_CHUNKS); ++i) {
            if (slots_[i].used && slots_[i].cx == cx && slots_[i].cy == cy) return i;
        }
        return -1;
    }

    // Whether a chunk already in a slot should be sampled (again).
    bool NeedsWork(const Slot& s, const GrVec3& feet, uint64_t now_ms) const noexcept {
        if (!s.sampled || s.next != 0) return true;
        if (s.data.holes != 0 && s.tries < kMaxTries && now_ms - s.done_ms > kHoleRetryMs) return true;
        return std::fabs(s.zref - feet.z) > kRefDrift && Distance(s.cx, s.cy, feet) < kRefDriftNear;
    }

    // The slot to work on: the wanted chunk nearest `ahead` that needs work, given a slot
    // if it has none. -1 when everything is done.
    int Pick(const GrVec3& feet, const GrVec3& ahead, uint64_t now_ms) noexcept {
        const int32_t reach = static_cast<int32_t>(std::ceil(kWantRadius / kChunkMetres)) + 1;
        const int32_t pcx = ChunkOf(feet.x);
        const int32_t pcy = ChunkOf(feet.y);
        int best_slot = -1;
        int32_t best_cx = 0;
        int32_t best_cy = 0;
        float best = 1e30f;
        for (int32_t cy = pcy - reach; cy <= pcy + reach; ++cy) {
            for (int32_t cx = pcx - reach; cx <= pcx + reach; ++cx) {
                if (Distance(cx, cy, feet) > kWantRadius) continue;
                const int slot = Find(cx, cy);
                if (slot >= 0 && !NeedsWork(slots_[slot], feet, now_ms)) continue;
                // The chunk under the player always first: without it GMod holds the player.
                const float score = (cx == pcx && cy == pcy) ? -1.0f : Distance(cx, cy, ahead);
                if (score < best) {
                    best = score;
                    best_slot = slot;
                    best_cx = cx;
                    best_cy = cy;
                }
            }
        }
        if (best >= 1e29f) return -1;
        if (best_slot >= 0) return best_slot;
        for (int i = 0; i < static_cast<int>(GR_MAX_CHUNKS); ++i) {
            if (slots_[i].used) continue;
            Slot& s = slots_[i];
            s = Slot{};
            s.used = true;
            s.cx = best_cx;
            s.cy = best_cy;
            return i;
        }
        return -1;  // all slots taken by chunks not yet out of kDropRadius
    }

    void Begin(Slot& s, float zref) noexcept {
        s.zref = zref;
        s.data.holes = 0;
        s.data.wall_count = 0;
        s.walls_dropped = 0;
    }

    float Height(const Slot& s, uint32_t i, uint32_t j) const noexcept {
        return s.data.heights[j * GR_CHUNK_VERTS + i];
    }

    // One grid edge: A to B, 1 m along x (the first half of the edges) or y.
    void SampleEdge(Slot& s, native::Entity ped) noexcept {
        const uint32_t e = s.next++ - GR_CHUNK_SAMPLES;
        const uint32_t half = kEdges / 2;
        const uint32_t k = e % half;
        const uint32_t ai = k % GR_CHUNK_CELLS;
        const uint32_t aj = k / GR_CHUNK_CELLS;
        const bool along_x = e < half;
        const uint32_t bi = along_x ? ai + 1 : ai;
        const uint32_t bj = along_x ? aj : aj + 1;
        float ha = Height(s, ai, aj);
        float hb = Height(s, bi, bj);
        const bool va = ha > GR_CHUNK_NO_GROUND_BELOW;
        const bool vb = hb > GR_CHUNK_NO_GROUND_BELOW;
        if (!va && !vb) return;
        if (!va) ha = hb;
        if (!vb) hb = ha;
        const float x0 = static_cast<float>(s.cx) * kChunkMetres + static_cast<float>(ai) * kCellMetres;
        const float y0 = static_cast<float>(s.cy) * kChunkMetres + static_cast<float>(aj) * kCellMetres;
        // A little past B, so a wall standing exactly on a sample is still crossed.
        const float x1 = x0 + (along_x ? kCellMetres + 0.05f : 0.0f);
        const float y1 = y0 + (along_x ? 0.0f : kCellMetres + 0.05f);
        const float ground = ha > hb ? ha : hb;
        const float low = ha < hb ? ha : hb;
        for (const float up : {kWallLow, kWallHigh}) {
            const float z = ground + up;
            // Both ways: a ray that starts inside a solid (many buildings are one closed
            // box) does not hit its faces, so one way alone missed every face turned
            // towards +x or +y (seen: whole stretches of wall without panels).
            native::RayHit hit{};
            if (!Ray({x0, y0, z}, {x1, y1, z}, ped, hit) &&
                !Ray({x1, y1, z}, {x0 - (along_x ? 0.05f : 0.0f), y0 - (along_x ? 0.0f : 0.05f), z}, ped, hit)) {
                continue;
            }
            const float nx = hit.normal.x;
            const float ny = hit.normal.y;
            const float n = std::sqrt(nx * nx + ny * ny);
            if (n < kWallSteep) continue;  // a slope or a step: the heightfield has it
            AddWall(s, ped, hit.pos, nx / n, ny / n, low, z);
            return;
        }
    }

    void AddWall(Slot& s, native::Entity ped, const GrVec3& at, float nx, float ny, float low, float z) noexcept {
        if (s.data.wall_count >= GR_MAX_CHUNK_WALLS) {
            ++s.walls_dropped;
            return;
        }
        GrWall& w = s.data.walls[s.data.wall_count++];
        w.x = at.x;
        w.y = at.y;
        w.nx = nx;
        w.ny = ny;
        w.left = Extent(ped, at, nx, ny, 1.0f, z);
        w.right = Extent(ped, at, nx, ny, -1.0f, z);
        // The probes found nothing either side (a curved or broken surface): no panel.
        if (w.left + w.right < 0.1f) {
            --s.data.wall_count;
            return;
        }
        w.z0 = low - kWallUnder;
        // The top: down onto the wall from above, just behind its face. Nothing there (a
        // plank thinner than that): a fence's height.
        native::RayHit top{};
        const float bx = at.x - nx * 0.01f;
        const float by = at.y - ny * 0.01f;
        w.z1 = Ray({bx, by, z + kWallMaxUp}, {bx, by, z}, ped, top) ? top.pos.z : z + 1.0f;
        if (w.z1 < z + 0.3f) w.z1 = z + 0.3f;
    }

    // How far the wall through `at` goes along (-ny, nx) * side, up to kWallHalf, to 6 cm:
    // probes across it, from in front of its face to behind it.
    float Extent(native::Entity ped, const GrVec3& at, float nx, float ny, float side, float z) noexcept {
        const float tx = -ny * side;
        const float ty = nx * side;
        const auto wall_at = [&](float o) {
            const float px = at.x + tx * o;
            const float py = at.y + ty * o;
            native::RayHit hit{};
            if (!Ray({px + nx * kWallProbe, py + ny * kWallProbe, z}, {px - nx * kWallProbe, py - ny * kWallProbe, z}, ped,
                     hit)) {
                return false;
            }
            return hit.normal.x * nx + hit.normal.y * ny > 0.9f;
        };
        if (wall_at(kWallHalf)) return kWallHalf;
        float lo = 0.0f;
        float hi = kWallHalf;
        for (int i = 0; i < 3; ++i) {
            const float mid = (lo + hi) * 0.5f;
            if (wall_at(mid)) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    void SampleColumn(Slot& s, native::Entity ped) noexcept {
        const uint32_t k = s.next++;
        const float x = static_cast<float>(s.cx) * kChunkMetres + static_cast<float>(k % GR_CHUNK_VERTS) * kCellMetres;
        const float y = static_cast<float>(s.cy) * kChunkMetres + static_cast<float>(k / GR_CHUNK_VERTS) * kCellMetres;
        native::RayHit hit{};
        float h = GR_CHUNK_NO_GROUND;
        // Objects too: some of RDR2's ground is objects (seen: the big rock beside the camp at
        // Horseshoe Overlook left a hole of 16 cells, and the GMod player standing on it fell
        // through again and again). Crates and barrels become bumps the player can stand on,
        // as in RDR2; their proxies are solid to GMod's props only (proxies.lua).
        if (Ray({x, y, s.zref + kLowStart}, {x, y, s.zref - kLowDepth}, ped, hit, kGroundRay) ||
            Ray({x, y, s.zref + kHighStart}, {x, y, s.zref + kLowStart}, ped, hit, kGroundRay)) {
            h = hit.pos.z;
        } else {
            ++s.data.holes;
        }
        s.data.heights[k] = h;
    }

    static constexpr int kGroundRay = native::kRayMap | native::kRayObjects;

    bool Ray(const GrVec3& from, const GrVec3& to, native::Entity ped, native::RayHit& hit,
             int flags = native::kRayMap) noexcept {
        const uint32_t t0 = NowUs();
        const bool found = native::Raycast(from, to, flags, ped, hit);
        ray_us_ += NowUs() - t0;
        ++rays_;
        return found;
    }

    void Finish(Link& link, uint32_t i, uint64_t now_ms) noexcept {
        Slot& s = slots_[i];
        float lowest = 1e30f;
        for (const float h : s.data.heights) {
            if (h > GR_CHUNK_NO_GROUND_BELOW && h < lowest) lowest = h;
        }
        s.data.base_z = lowest < 1e29f ? lowest : s.zref;
        s.data.cx = s.cx;
        s.data.cy = s.cy;
        if (++serial_ == 0) serial_ = 1;
        s.data.serial = serial_;
        s.next = 0;
        s.sampled = true;
        s.done_ms = now_ms;
        ++s.tries;
        ++chunks_done_;
        link.PublishChunk(i, s.data);
        GR_VERBOSE("terrain: chunk (%d, %d) in slot %u, lowest ground %.1f, %u holes, %u walls, try %u", s.cx, s.cy, i,
                   s.data.base_z, s.data.holes, s.data.wall_count, s.tries);
        if (s.walls_dropped != 0) {
            GR_LOG("terrain: chunk (%d, %d) has %u more walls than fit (GR_MAX_CHUNK_WALLS %u)", s.cx, s.cy,
                   s.walls_dropped, GR_MAX_CHUNK_WALLS);
        }
    }

    void Clear(Link& link, uint32_t i) noexcept {
        slots_[i] = Slot{};
        link.PublishChunk(i, slots_[i].data);
    }

    Slot slots_[GR_MAX_CHUNKS];
    uint32_t serial_ = 1;
    uint32_t budget_us_ = 1000;
    uint32_t rays_ = 0;
    uint64_t ray_us_ = 0;
    uint32_t chunks_done_ = 0;
    uint32_t frame_us_max_ = 0;
    uint64_t next_report_ms_ = 0;
};

}  // namespace gr
