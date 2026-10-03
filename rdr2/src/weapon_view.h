// RDR2's own gun in the GMod player's hand.
//
// When the GMod player holds one of RDR2's guns (gr/sweps.lua, picked from GMod's weapon
// HUD), GMod draws no viewmodel and names the gun in GrGuestFrame.weapon. Here RDR2's real
// model of that gun is made as a weapon object and held in front of the camera, lower right,
// every frame: the gun on screen is RDR2's, drawn and lit by RDR2. It is fired with RDR2's
// own bullets and sound (combat_sync.h). There is no hand and no reload animation: the
// object is placed, not held by a ped. A shot kicks it up for a moment.
//
// Everything here calls natives: script thread only. No allocation.

#pragma once

#include <cmath>

#include "gr_link.h"
#include "gr_log.h"
#include "gr_protocol.h"
#include "natives.h"

namespace gr {

class WeaponView {
public:
    void Reset() noexcept {
        // An object made by an earlier run of the plugin cannot be found again; it stays where
        // it was left until the game removes it.
        object_ = 0;
        weapon_ = 0;
    }

    // `cam_pos`, `cam_rot`: the camera RDR2 shows this frame. `shot_ms`: when the last GMod
    // shot was fired (NowMs), for the kick.
    void Tick(bool driving, const GrGuestFrame& guest, const GrVec3& cam_pos, const GrVec3& cam_rot,
              uint64_t shot_ms) noexcept {
        uint32_t want = driving ? guest.weapon : 0;
        if (want != 0 && want != weapon_ && !native::IsWeaponValid(want)) want = 0;
        if (want != weapon_) {
            Drop();
            weapon_ = want;
        }
        if (weapon_ == 0) return;
        if (object_ != 0 && !native::DoesEntityExist(object_)) object_ = 0;
        if (object_ == 0) {
            // Asked for every frame until it has loaded: no waiting on the script thread.
            native::RequestWeaponAsset(weapon_);
            if (!native::HasWeaponAssetLoaded(weapon_)) return;
            object_ = native::CreateWeaponObject(weapon_, cam_pos);
            if (object_ == 0) {
                GR_LOG("weapon: RDR2 made no object for weapon %08X", weapon_);
                weapon_ = 0;
                return;
            }
            native::SetEntityCollision(object_, false);
            native::FreezeEntityPosition(object_, true);
            GR_LOG("weapon: RDR2's model of weapon %08X is in the player's view (object %d)", weapon_, object_);
        }

        // Third person: in the hand of GMod's player model, pointing where it aims.
        if ((guest.weapon_flags & GR_WEAPONF_AT_HAND) != 0) {
            native::SetEntityCoordsNoOffset(object_, guest.weapon_pos);
            native::SetEntityRotation(object_, {0.0f, -guest.weapon_rot.x, guest.weapon_rot.z + 90.0f});
            return;
        }
        // The camera's axes from its rotation (gr_units' convention: yaw 0 faces +Y).
        const float pitch = cam_rot.x * kRad, yaw = cam_rot.z * kRad;
        const GrVec3 f{-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
        const GrVec3 r{std::cos(yaw), std::sin(yaw), 0.0f};
        const GrVec3 u{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
        // A shot throws the muzzle up and the gun back, easing out.
        const uint64_t since = NowMs() - shot_ms;
        const float kick = since < kKickMs ? 1.0f - static_cast<float>(since) / static_cast<float>(kKickMs) : 0.0f;
        const float fwd = kForward - kick * 0.05f;
        const GrVec3 at{cam_pos.x + f.x * fwd + r.x * kRight - u.x * kDown,
                        cam_pos.y + f.y * fwd + r.y * kRight - u.y * kDown,
                        cam_pos.z + f.z * fwd + r.z * kRight - u.z * kDown};
        native::SetEntityCoordsNoOffset(object_, at);
        // RDR2's gun models point along their own +X (seen: with the camera's yaw the barrel
        // pointed right), so the gun is turned a quarter left and the camera's pitch becomes
        // a turn about the gun's own Y.
        native::SetEntityRotation(object_, {0.0f, -(cam_rot.x + kick * 9.0f), cam_rot.z + 90.0f});
    }

private:
    static constexpr float kRad = 0.0174532925f;
    static constexpr float kForward = 0.42f;  // metres in front of the camera
    static constexpr float kRight = 0.16f;
    static constexpr float kDown = 0.17f;
    static constexpr uint64_t kKickMs = 160;

    void Drop() noexcept {
        if (object_ != 0 && native::DoesEntityExist(object_)) native::DeleteEntity(object_);
        if (weapon_ != 0) native::RemoveWeaponAsset(weapon_);
        object_ = 0;
    }

    native::Entity object_ = 0;
    uint32_t weapon_ = 0;
};

}  // namespace gr
