// Trees and bushes hide GMod's own props (GrVeilRequest, GrVeilResult).
//
// GMod draws its props over RDR2's picture and hides them behind its copy of RDR2's ground,
// walls and people. It has no copy of the foliage: a prop behind a bush was drawn over the
// bush (user report). There is no hook to read RDR2's depth buffer, so for each prop GMod
// lists, this casts GR_VEIL_GRID by GR_VEIL_GRID rays from the camera to a square just in
// front of it and reports which were stopped; GMod fades the prop out there
// (gr/world_render.lua).
//
// A few props a frame, in turn: kRaysPerFrame rays at 3 to 4 microseconds each.

#pragma once

#include <cmath>
#include <cstdint>

#include "gr_log.h"
#include "gr_protocol.h"
#include "natives.h"

namespace gr {

class VeilSync {
public:
    void Reset() noexcept {
        next_ = 0;
        logged_ = false;
        for (GrVeilResult& r : results_) r = {};
    }

    void Tick(bool driving, const GrGuestFrame& guest, native::Entity player_ped, GrHostFrame& host) noexcept {
        const uint32_t count = guest.veil_count < GR_MAX_VEILS ? guest.veil_count : GR_MAX_VEILS;
        if (!driving || count == 0) {
            for (GrVeilResult& r : results_) r = {};
            for (GrVeilResult& r : host.veils) r = {};
            return;
        }
        // Answers for things no longer listed go.
        for (GrVeilResult& r : results_) {
            bool listed = false;
            for (uint32_t i = 0; i < count; ++i) listed = listed || guest.veils[i].id == r.id;
            if (!listed) r = {};
        }
        for (uint32_t done = 0; done < kPropsPerFrame && done < count; ++done) {
            const GrVeilRequest& v = guest.veils[next_ % count];
            ++next_;
            Look(v, guest.eye_pos, player_ped);
        }
        for (uint32_t i = 0; i < GR_MAX_VEILS; ++i) host.veils[i] = results_[i];
    }

private:
    static constexpr uint32_t kPropsPerFrame = 4;

    void Look(const GrVeilRequest& v, const GrVec3& eye, native::Entity player_ped) noexcept {
        GrVec3 d{v.pos.x - eye.x, v.pos.y - eye.y, v.pos.z - eye.z};
        const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        uint32_t mask = 0;
        // Nothing stands between the camera and a thing it is inside or right at.
        if (dist > v.radius + 0.3f) {
            d = {d.x / dist, d.y / dist, d.z / dist};
            // The square: right is across the view and level, up is across both.
            GrVec3 right{d.y, -d.x, 0.0f};
            const float rl = std::sqrt(right.x * right.x + right.y * right.y);
            if (rl < 1e-4f) {
                right = {1.0f, 0.0f, 0.0f};
            } else {
                right = {right.x / rl, right.y / rl, 0.0f};
            }
            const GrVec3 up{right.y * d.z - right.z * d.y, right.z * d.x - right.x * d.z, right.x * d.y - right.y * d.x};
            const GrVec3 front{v.pos.x - d.x * v.radius, v.pos.y - d.y * v.radius, v.pos.z - d.z * v.radius};
            for (uint32_t row = 0; row < GR_VEIL_GRID; ++row) {
                for (uint32_t col = 0; col < GR_VEIL_GRID; ++col) {
                    const float a = (static_cast<float>(col) / (GR_VEIL_GRID - 1) * 2.0f - 1.0f) * v.radius;
                    const float b = (static_cast<float>(row) / (GR_VEIL_GRID - 1) * 2.0f - 1.0f) * v.radius;
                    const GrVec3 to{front.x + right.x * a + up.x * b, front.y + right.y * a + up.y * b,
                                    front.z + right.z * a + up.z * b};
                    native::RayHit hit{};
                    if (native::Raycast(eye, to, native::kRayFoliage, player_ped, hit)) {
                        mask |= 1u << (row * GR_VEIL_GRID + col);
                    }
                }
            }
        }
        if (mask != 0 && !logged_) {
            logged_ = true;
            GR_LOG("veil: foliage found in front of GMod thing %u (mask %04X, %.1f m away)", v.id, mask, dist);
        }
        // The thing's slot, or a free one.
        GrVeilResult* slot = nullptr;
        for (GrVeilResult& r : results_) {
            if (r.id == v.id) slot = &r;
        }
        if (!slot) {
            for (GrVeilResult& r : results_) {
                if (r.id == 0 && !slot) slot = &r;
            }
        }
        if (slot) *slot = {v.id, mask};
    }

    GrVeilResult results_[GR_MAX_VEILS]{};
    uint32_t next_ = 0;
    bool logged_ = false;
};

}  // namespace gr
