// The possess tool, RDR2 side: the GMod player drives one of RDR2's peds, horses or animals.
//
// GMod names the ped and the way it should go (GrGuestFrame.possess, possess_move). The ped is
// not placed: it is given RDR2's own "go straight to" task towards a point well ahead in that
// direction, so it walks, runs or gallops with its own animation and RDR2's physics, and
// GMod's player and camera follow its proxy (gr/possess.lua). Its AI is kept from reacting to
// anything else for as long as it is possessed.
//
// Everything here calls natives: script thread only. No allocation.

#pragma once

#include <cmath>

#include "gr_link.h"
#include "gr_log.h"
#include "gr_protocol.h"
#include "natives.h"

namespace gr {

class PossessSync {
public:
    void Reset() noexcept {
        ped_ = 0;
        moving_ = false;
        jump_down_ = false;
    }

    // `driving`: GMod has the player. `player`: the player's own (hidden) ped.
    void Tick(bool driving, const GrGuestFrame& guest, native::Entity player) noexcept {
        native::Entity want = driving ? guest.possess : 0;
        if (want == player || (want != 0 && (!native::DoesEntityExist(want) || !native::IsEntityAPed(want) ||
                                             native::IsEntityDead(want)))) {
            want = 0;
        }
        if (want != ped_) {
            Release();
            if (want != 0) Take(want);
        }
        if (ped_ == 0) return;

        // The player's hidden ped stands where the GMod player is, which is inside this one.
        native::SetEntityNoCollisionEntity(player, ped_, true);
        native::SetEntityNoCollisionEntity(ped_, player, true);
        // Thrown or knocked over: nothing to steer until it is up again.
        if (native::IsPedRagdoll(ped_)) {
            moving_ = false;
            return;
        }

        const GrVec3 m = guest.possess_move;
        const float pace = std::sqrt(m.x * m.x + m.y * m.y);
        const uint64_t now = NowMs();
        if (pace < 0.1f) {
            if (moving_) {
                native::ClearPedTasks(ped_);
                native::TaskStandStill(ped_, -1);
                moving_ = false;
            }
        } else {
            const float dx = m.x / pace, dy = m.y / pace;
            // A new task only when the way or the pace changed, or the old target is getting
            // near: given every frame the ped would start its first step over and over.
            const bool turned = dx * dir_x_ + dy * dir_y_ < kSameWay;
            if (!moving_ || turned || std::fabs(pace - pace_) > 0.25f || now >= retask_ms_) {
                const GrVec3 at = native::GetEntityCoords(ped_);
                const GrVec3 to{at.x + dx * kAhead, at.y + dy * kAhead, at.z};
                // RDR2 headings: 0 faces +Y, counter-clockwise positive.
                const float heading = std::atan2(-dx, dy) * 57.29578f;
                native::TaskGoStraightToCoord(ped_, to, pace, heading < 0.0f ? heading + 360.0f : heading);
                moving_ = true;
                dir_x_ = dx;
                dir_y_ = dy;
                pace_ = pace;
                retask_ms_ = now + kRetaskMs;
            }
        }

        const bool jump = (guest.possess_flags & GR_POSSESSF_JUMP) != 0;
        if (jump && !jump_down_) {
            native::TaskJump(ped_);
            moving_ = false;  // the jump replaced the walk: give it again after
            retask_ms_ = now + kJumpMs;
        }
        jump_down_ = jump;
    }

    native::Entity ped() const noexcept { return ped_; }

private:
    static constexpr float kAhead = 25.0f;       // metres: far enough never to arrive between tasks
    static constexpr float kSameWay = 0.985f;    // cosine of about 10 degrees
    static constexpr uint64_t kRetaskMs = 2000;
    static constexpr uint64_t kJumpMs = 700;

    void Take(native::Entity ped) noexcept {
        ped_ = ped;
        moving_ = false;
        jump_down_ = false;
        native::SetBlockingOfNonTemporaryEvents(ped, true);
        native::ClearPedTasks(ped);
        native::TaskStandStill(ped, -1);
        GR_LOG("possess: GMod's player drives ped %d", ped);
    }

    void Release() noexcept {
        if (ped_ == 0) return;
        if (native::DoesEntityExist(ped_)) {
            native::SetBlockingOfNonTemporaryEvents(ped_, false);
            native::ClearPedTasks(ped_);
        }
        GR_LOG("possess: ped %d is RDR2's again", ped_);
        ped_ = 0;
        moving_ = false;
    }

    native::Entity ped_ = 0;
    bool moving_ = false;
    bool jump_down_ = false;
    float dir_x_ = 0.0f, dir_y_ = 0.0f, pace_ = 0.0f;
    uint64_t retask_ms_ = 0;
};

}  // namespace gr
