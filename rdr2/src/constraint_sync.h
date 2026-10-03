// Constraint sync, RDR2 side (milestone 8): what GMod's tools make between RDR2 entities,
// made in RDR2 too.
//
// GMod lists every constraint that exists between proxies in GrGuestFrame.constraints, every
// tick (gr/tools.lua). This keeps what it has made in step with that list: a new id is made,
// an id that has gone (undo, the remover, cleanup, a weld that broke) is undone. Nothing is
// an event, so nothing can be lost or done twice.
//   - weld: the first entity is attached to the second where it is now
//     (ATTACH_ENTITY_TO_ENTITY); welded to the world it is frozen in place.
//   - no-collide: SET_ENTITY_NO_COLLISION_ENTITY every frame, both ways, while listed.
//   - rope: a real RDR2 rope between the two points; to the world, the far end is held by
//     a small invisible frozen prop, as ropes only join entities.
// Thrusters are not constraints: GMod takes a proxy while its thruster fires (gr/tools.lua).
//
// Everything here calls natives: script thread only. No allocation.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gr_log.h"
#include "gr_protocol.h"
#include "natives.h"

namespace gr {

class ConstraintSync {
public:
    void Reset() noexcept {
        count_ = 0;
        restored_frames_ = 0;
    }

    // `driving`: GMod has the player. While RDR2 has it, what was made stays as it is.
    void Tick(bool driving, const GrGuestFrame& guest) noexcept {
        if (!driving) return;
        const uint32_t listed = std::min(guest.constraint_count, GR_MAX_CONSTRAINTS);

        for (uint32_t i = 0; i < count_;) {
            const GrConstraint* c = Listed(guest, listed, made_[i].id);
            // Just after a reload GMod's list can be short for a frame or two (seen: a rope
            // undone and made again): what was restored waits a little for it.
            if ((c || restored_frames_ > 0) && Alive(made_[i])) {
                ++i;
                continue;
            }
            Undo(made_[i], c != nullptr);
            made_[i] = made_[--count_];
        }

        for (uint32_t i = 0; i < listed; ++i) {
            const GrConstraint& c = guest.constraints[i];
            if (c.id == 0 || c.type == GR_CON_NONE || Find(c.id) || count_ == kMax) continue;
            Made m{};
            if (Make(c, m)) made_[count_++] = m;
        }

        if (restored_frames_ > 0) --restored_frames_;
        for (uint32_t i = 0; i < count_; ++i) {
            const Made& m = made_[i];
            if (m.type == GR_CON_NOCOLLIDE) {
                native::SetEntityNoCollisionEntity(m.a, m.b, true);
                native::SetEntityNoCollisionEntity(m.b, m.a, true);
            }
        }
    }

    uint32_t count() const noexcept { return count_; }

    // persist.h: what was made, kept across a plugin reload so it is not made twice.
    static constexpr uint32_t kSaveBytes = 4 + 24 * GR_MAX_CONSTRAINTS;  // 24: sizeof(Made), asserted below
    void Save(uint8_t* to) const noexcept {
        std::memcpy(to, &count_, 4);
        std::memcpy(to + 4, made_, sizeof(Made) * count_);
    }
    void Restore(const uint8_t* from) noexcept {
        uint32_t n = 0;
        std::memcpy(&n, from, 4);
        if (n > kMax) return;
        std::memcpy(made_, from + 4, sizeof(Made) * n);
        count_ = n;
        restored_frames_ = n ? kRestoreGraceFrames : 0;
        if (n) GR_LOG("tools: %u welds, ropes and no-collides remembered from before the reload", n);
    }

private:
    static constexpr uint32_t kMax = GR_MAX_CONSTRAINTS;
    // p_cannonball01x: small and round. Holds a rope's far end in the world, invisible.
    static constexpr uint32_t kAnchorModel = 0x5A070AD3;

    struct Made {
        uint32_t id;
        uint32_t type;
        native::Entity a;
        native::Entity b;
        int rope;
        native::Entity anchor;
    };

    static const GrConstraint* Listed(const GrGuestFrame& guest, uint32_t listed, uint32_t id) noexcept {
        for (uint32_t i = 0; i < listed; ++i) {
            if (guest.constraints[i].id == id) return &guest.constraints[i];
        }
        return nullptr;
    }

    const Made* Find(uint32_t id) const noexcept {
        for (uint32_t i = 0; i < count_; ++i) {
            if (made_[i].id == id) return &made_[i];
        }
        return nullptr;
    }

    static bool Alive(const Made& m) noexcept {
        if (!native::DoesEntityExist(m.a)) return false;
        if (m.b != 0 && !native::DoesEntityExist(m.b)) return false;
        return m.rope == 0 || native::DoesRopeExist(m.rope);
    }

    bool Make(const GrConstraint& c, Made& m) noexcept {
        if (!native::DoesEntityExist(c.a) || (c.b != 0 && !native::DoesEntityExist(c.b))) return false;
        m = {c.id, c.type, c.a, c.b, 0, 0};
        switch (c.type) {
            case GR_CON_WELD:
                if (c.b != 0) {
                    // Where a is now, in b's space, and turned as it is now relative to b.
                    // The rotation difference is exact for turns about the vertical, which is
                    // what welding props and wagons mostly is.
                    const GrVec3 offset = native::WorldToEntity(c.b, native::GetEntityCoords(c.a));
                    const GrVec3 ra = native::GetEntityRotation(c.a);
                    const GrVec3 rb = native::GetEntityRotation(c.b);
                    native::AttachEntityToEntity(c.a, c.b, offset, {ra.x - rb.x, ra.y - rb.y, ra.z - rb.z},
                                                 native::IsEntityAPed(c.a));
                    GR_LOG("tools: weld %u, %d held to %d at (%.2f, %.2f, %.2f)", c.id, c.a, c.b, offset.x, offset.y,
                           offset.z);
                } else {
                    native::FreezeEntityPosition(c.a, true);
                    GR_LOG("tools: weld %u, %d held to the world", c.id, c.a);
                }
                return true;
            case GR_CON_NOCOLLIDE:
                GR_LOG("tools: no-collide %u between %d and %d", c.id, c.a, c.b);
                return c.b != 0;
            case GR_CON_ROPE: return MakeRope(c, m);
            default: return false;
        }
    }

    bool MakeRope(const GrConstraint& c, Made& m) noexcept {
        const GrVec3 pa = native::EntityToWorld(c.a, c.pos_a);
        const GrVec3 pb = c.b != 0 ? native::EntityToWorld(c.b, c.pos_b) : c.pos_b;
        native::Entity other = c.b;
        if (other == 0) {
            // Asked for every frame until it has loaded: no waiting on the script thread.
            native::RequestModel(kAnchorModel);
            if (!native::HasModelLoaded(kAnchorModel)) return false;
            other = native::CreateObject(kAnchorModel, pb);
            if (other == 0) return false;
            native::SetEntityVisible(other, false);
            native::FreezeEntityPosition(other, true);
            m.anchor = other;
        }
        const float length = c.length > 0.1f ? c.length : 0.1f;
        m.rope = native::AddRope(pa, length);
        if (m.rope == 0) {
            if (m.anchor != 0) native::DeleteEntity(m.anchor);
            GR_LOG("tools: ADD_ROPE gave no rope for rope %u", c.id);
            return false;
        }
        native::AttachEntitiesToRope(m.rope, c.a, other, c.pos_a, c.b != 0 ? c.pos_b : GrVec3{0.0f, 0.0f, 0.0f});
        native::ActivatePhysics(c.a);
        if (c.b != 0) native::ActivatePhysics(c.b);
        GR_LOG("tools: rope %u (RDR2 rope %d), %d to %d, %.2f m", c.id, m.rope, c.a, other, length);
        return true;
    }

    static void Undo(const Made& m, bool still_listed) noexcept {
        switch (m.type) {
            case GR_CON_WELD:
                if (native::DoesEntityExist(m.a)) {
                    if (m.b != 0) {
                        native::DetachEntity(m.a);
                    } else {
                        native::FreezeEntityPosition(m.a, false);
                    }
                    native::ActivatePhysics(m.a);
                }
                break;
            case GR_CON_ROPE:
                if (m.rope != 0 && native::DoesRopeExist(m.rope)) native::DeleteRope(m.rope);
                if (m.anchor != 0 && native::DoesEntityExist(m.anchor)) native::DeleteEntity(m.anchor);
                break;
            default: break;
        }
        GR_LOG("tools: %u undone (%s)", m.id, still_listed ? "an entity is gone" : "GMod no longer has it");
    }

    Made made_[kMax];
    uint32_t count_ = 0;
    static constexpr int kRestoreGraceFrames = 120;
    int restored_frames_ = 0;
    static_assert(sizeof(Made) == 24);
};

}  // namespace gr
