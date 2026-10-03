// Player sync, RDR2 side: while GMod owns the player, RDR2's player ped is hidden, frozen
// and put where the GMod player is every frame, and a script camera renders from the GMod
// player's eyes. RDR2's own controls are switched off so it does not also act on the keys.
//
// RDR2 takes the player back (GR_HOSTF_RDR2_OWNS_PLAYER) whenever it needs the ped
// itself: it is dead, a script has taken player control away (a scripted scene), it is on
// a mount or in a vehicle, or the user pressed the handover key. When it gives the player
// up again the anchor serial changes, so GMod anchors to wherever the ped ended up.
//
// Everything here calls natives: script thread only.

#pragma once

#include <cmath>

#include "gr_link.h"
#include "gr_log.h"
#include "gr_protocol.h"
#include "gr_units.h"
#include "input_passthrough.h"
#include "natives.h"

namespace gr {

class PlayerSync {
public:
    // Call when the script starts. Script Hook can restart the script in a process where
    // an earlier run was driving the ped (CTRL+R reloads the DLL, so nothing in memory
    // says so): the environment variable does, and the ped is given back first.
    void Reset() noexcept {
        const native::Entity ped = native::PlayerPedId();
        wchar_t marker[4];
        if (driving_ || GetEnvironmentVariableW(kDrivingMarker, marker, 4) > 0) {
            GR_LOG("player: an earlier run of the script left the ped hidden. Giving it back to RDR2");
            driving_ = true;
            Stop(ped);
        }
        ped_ = 0;
        cam_ = 0;
        model_ = 0;
        owns_ = false;
        user_owns_ = false;
        checked_ = false;
        // Never 0, and different from run to run, so a guest frame anchored to an earlier
        // run of the host can never match.
        serial_ = NowUs() | 1u;
    }

    // One frame. Applies the guest's player if GMod owns it, and fills the player part of
    // `host`. `handover` is true on the frame the handover key went down.
    void Tick(bool connected, const GrGuestFrame& guest, bool focused, bool handover, bool smooth,
              bool follow_ground, bool collide, uint32_t now_us, GrHostFrame& host, int32_t mouse_x = 0,
              int32_t mouse_y = 0) noexcept {
        const native::Entity ped = native::PlayerPedId();
        const bool paused = native::IsPauseMenuActive();

        if (handover) {
            user_owns_ = !user_owns_;
            GR_LOG("player: handover key. %s", user_owns_ ? "RDR2 has the player" : "GMod may have the player");
        }
        const char* why = user_owns_ ? "the handover key" : teleport_state_ != 0 ? "a teleport" : BusyReason(ped);
        if ((why != nullptr) != owns_) {
            owns_ = why != nullptr;
            if (owns_) {
                GR_LOG("player: RDR2 takes the player (%s)", why);
            } else {
                NextSerial();
                GR_LOG("player: RDR2 gives the player up, anchor serial %u", serial_);
            }
        }

        if (driving_ && ped != ped_) Stop(ped_);  // the game swapped the player ped
        const bool drive = connected && !owns_ && ped != 0 && (guest.flags & GR_GUESTF_PLAYER_VALID) != 0 &&
                           guest.anchor_serial == serial_;
        if (drive && !driving_) Start(ped, guest);
        if (!drive && driving_) {
            GiveHealth(ped_, guest);
            Stop(ped_);
        }
        if (!driving_) have_held_ = false;

        Teleport(ped);
        UpdateFeet(ped);
        if (!checked_ && ped != 0 && !driving_) CheckConventions(ped);

        if (driving_) {
            // Where the GMod player is now rather than where it was when the frame was
            // written: the two games' frames are not in step, and without this a moving
            // camera shakes by up to a frame's worth of travel.
            const int32_t age_us = smooth ? AgeUs(now_us, guest.time_us) : 0;
            const float age = static_cast<float>(age_us < 0 ? 0 : age_us > kMaxExtrapolateUs ? kMaxExtrapolateUs : age_us) * 1e-6f;
            GrVec3 feet{guest.pos.x + guest.vel.x * age, guest.pos.y + guest.vel.y * age,
                        guest.pos.z + guest.vel.z * age};
            GrVec3 eye{guest.eye_pos.x + guest.vel.x * age, guest.eye_pos.y + guest.vel.y * age,
                       guest.eye_pos.z + guest.vel.z * age};
            shift_ = 0.0f;
            if (follow_ground) FollowGround(guest, feet, eye);
            if (collide) Collide(ped, guest, feet, eye, host);
            held_ = feet;
            have_held_ = true;
            // The hidden ped is only put at the player when it has fallen `ped_follow_` metres
            // behind (0: every frame). See SetPedFollow.
            const GrVec3 at = native::GetEntityCoords(ped);
            const float px = feet.x - at.x, py = feet.y - at.y, pz = feet.z + feet_ - at.z;
            GrVec3 ped_at = at;
            const float off2 = px * px + py * py + pz * pz;
            if (ped_mode_ == 1 && off2 < kDriveSnap * kDriveSnap) {
                // Walked, not teleported: pushed along GMod's velocity plus what it is behind.
                // RDR2's attackers need a target that moves like a person (SetPedMode).
                native::SetEntityVelocity(ped, {guest.vel.x + px * kDriveGain, guest.vel.y + py * kDriveGain,
                                                guest.vel.z + pz * kDriveGain});
            } else if (ped_follow_ <= 0.0f || off2 > ped_follow_ * ped_follow_) {
                ped_at = {feet.x, feet.y, feet.z + feet_};
                native::SetEntityCoordsNoOffset(ped, ped_at);
            }
            // Look prediction (GrGuestFrame.look_*): the mouse counts GMod's angles do not
            // include yet are added here, so the view turns with the mouse every frame.
            GrVec3 look = guest.eye_rot;
            if (focused && !paused && (guest.look_yaw_per_count != 0.0f || guest.look_pitch_per_count != 0.0f)) {
                const int32_t dx = static_cast<int32_t>(static_cast<uint32_t>(mouse_x) - static_cast<uint32_t>(guest.look_mouse_x));
                const int32_t dy = static_cast<int32_t>(static_cast<uint32_t>(mouse_y) - static_cast<uint32_t>(guest.look_mouse_y));
                // More than this is not a frame or two of mouse: a stale or foreign total.
                if (dx > -kMaxPredictCounts && dx < kMaxPredictCounts && dy > -kMaxPredictCounts && dy < kMaxPredictCounts) {
                    look.z += static_cast<float>(dx) * guest.look_yaw_per_count;
                    look.x += static_cast<float>(dy) * guest.look_pitch_per_count;
                    look.x = look.x > 89.0f ? 89.0f : look.x < -89.0f ? -89.0f : look.x;
                    predicted_x_ = dx;
                    predicted_y_ = dy;
                }
            }
            Hide(ped);
            // Works around: alpha does not reach the ped's weapons, which are objects of their
            // own, so the holstered gun and the knife floated where the player stood (user
            // report). Every frame: drawing or holstering a weapon shows it again.
            if (ped_mode_ == 1) native::SetPedAllWeaponsVisibility(ped, false);
            Vulnerable(ped);
            Roughness(look.z);
            native::SetEntityHeading(ped, look.z);
            // The view lock (GrHostFrame.view_pos): this frame's view is published now and
            // shown `view_hold` frames from now; what is shown now is the one published that
            // many frames ago.
            const float fov = guest.fov > 0.0f ? guest.fov : kDefaultFov;
            views_[view_count_ % kViews] = {eye, look, fov < 1.0f ? 1.0f : fov > 130.0f ? 130.0f : fov};
            ++view_count_;
            host.view_pos = eye;
            host.view_rot = look;
            host.view_serial = view_count_;
            uint32_t hold = guest.view_hold < GR_MAX_VIEW_HOLD ? guest.view_hold : GR_MAX_VIEW_HOLD;
            if (hold > view_count_ - 1) hold = view_count_ - 1;
            const View& show = views_[(view_count_ - 1 - hold) % kViews];
            shown_ = show;
            if (cam_ != 0) {
                if (cam_mode_ == 2) {
                    // Attached once, and again only when the eye moves against the ped (a
                    // crouch): the camera then moves with the ped and no script sets its
                    // position.
                    const GrVec3 off{show.pos.x - ped_at.x, show.pos.y - ped_at.y, show.pos.z - ped_at.z};
                    const float ox = off.x - attached_.x, oy = off.y - attached_.y, oz = off.z - attached_.z;
                    if (!have_attached_ || ox * ox + oy * oy + oz * oz > 0.02f * 0.02f) {
                        native::AttachCamToEntity(cam_, ped, off, false);
                        attached_ = off;
                        have_attached_ = true;
                    }
                    native::SetCamRot(cam_, show.rot);
                    native::SetCamFov(cam_, show.fov);
                } else if (have_attached_) {
                    native::DetachCam(cam_);
                    have_attached_ = false;
                }
                if (cam_mode_ == 2) {
                } else if (cam_mode_ == 1) {
                    native::SetCamParams(cam_, show.pos, show.rot, show.fov);
                } else {
                    native::SetCamCoord(cam_, show.pos);
                    native::SetCamRot(cam_, show.rot);
                    native::SetCamFov(cam_, show.fov);
                }
            }
            if (!paused) {
                native::DisableAllControlActions();
                for (const PassthroughKey& key : kPassthroughKeys) {
                    if (key.control != 0) native::EnableControlAction(key.control);
                }
            }
            host.ped_pos = feet;
            host.ped_rot = {0.0f, 0.0f, look.z};
        } else if (ped != 0) {
            const GrVec3 origin = native::GetEntityCoords(ped);
            host.ped_pos = {origin.x, origin.y, origin.z - feet_};
            host.ped_rot = native::GetEntityRotation(ped);
        }

        host.ped_health = -1.0f;
        if (!driving_ && ped != 0 && !native::IsEntityDead(ped)) {
            const int max = native::GetPedMaxHealth(ped);
            const int health = native::GetEntityHealth(ped);
            if (given_health_ > 0) {
                // What GMod's health became, scaled by what the ped has lost since (see GiveHealth).
                const float f = given_frac_ * static_cast<float>(health) / static_cast<float>(given_health_);
                host.ped_health = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
            } else if (max > 0) {
                host.ped_health = static_cast<float>(health) / static_cast<float>(max);
            }
        }
        host.cam_fov = native::GetFinalRenderedCamFov();
        if (!driving_) {
            view_count_ = 0;
            host.view_serial = 0;
        }
        if (!driving_) shift_ = 0.0f;
        host.anchor_serial = serial_;
        host.ground_shift = shift_;
        host.flags = (focused ? GR_HOSTF_FOCUSED : 0) | (paused ? GR_HOSTF_PAUSED : 0) |
                     (owns_ ? GR_HOSTF_RDR2_OWNS_PLAYER : 0) | (model_ == kArthurModel ? GR_HOSTF_ARTHUR : 0);
        paused_ = paused;
    }

    // The spawn menu's teleport (GR_EVENT_TELEPORT): `to` is a place on RDR2's map.
    void RequestTeleport(const GrVec3& to) noexcept {
        teleport_to_ = to;
        teleport_state_ = 1;
        GR_LOG("player: teleport to (%.1f, %.1f, %.1f) asked for", to.x, to.y, to.z);
    }

    void SetPedFollow(float metres) noexcept { ped_follow_ = metres; }
    float ped_follow() const noexcept { return ped_follow_; }
    // How the camera is moved each frame (ini `cam_mode`): 1 SET_CAM_PARAMS (the default), 0
    // SET_CAM_COORD, SET_CAM_ROT and SET_CAM_FOV, 2 attached to the hidden ped.
    //
    // Works around the user's "when I move there is a lot of pixelation": with three separate
    // calls a frame, RDR2's DLSS (and TAA) threw its history away on every frame the camera
    // moved, and the picture fell to its internal resolution (blocky trees while walking,
    // sharp standing). Measured in one spot, strafing, full-size captures: 0 blocky twice,
    // 1 and 2 as clean as standing. Why the separate calls do it is not known.
    void SetCamMode(int mode) noexcept { cam_mode_ = mode; }
    int cam_mode() const noexcept { return cam_mode_; }
    // How the hidden ped is kept with the GMod player (ini `ped_mode`):
    //   1 (the default): see-through (alpha 0), not frozen, walked along with velocities and
    //     only teleported when it falls kDriveSnap behind.
    //   0: invisible, frozen and teleported every frame (before 2026-10-03).
    // Works around the user's "enemies don't attack me in gmod mode": with 0, wolves and
    // armed people went into combat with the player (IS_PED_IN_COMBAT) but only circled it;
    // with the ped visible to them, unfrozen and not teleported they attacked it (seen).
    void SetPedMode(int mode) noexcept { ped_mode_ = mode; }
    int ped_mode() const noexcept { return ped_mode_; }
    bool driving() const noexcept { return driving_; }
    // The camera RDR2 shows this frame while GMod drives.
    const GrVec3& shown_pos() const noexcept { return shown_.pos; }
    const GrVec3& shown_rot() const noexcept { return shown_.rot; }
    bool owns() const noexcept { return owns_; }
    bool user_owns() const noexcept { return user_owns_; }
    bool paused() const noexcept { return paused_; }
    // Metres the player is being raised above where GMod put it (FollowGround).
    float ground_shift() const noexcept { return shift_; }

private:
    struct View {
        GrVec3 pos;
        GrVec3 rot;
        float fov;
    };
    static constexpr uint32_t kViews = GR_MAX_VIEW_HOLD + 1;
    float ped_follow_ = 0.0f;
    int cam_mode_ = 1;
    int ped_mode_ = 1;
    int hidden_as_ = -1;
    GrVec3 attached_{};
    bool have_attached_ = false;
    View views_[kViews]{};
    View shown_{};
    uint32_t view_count_ = 0;

    static constexpr int32_t kMaxPredictCounts = 4000;
    static constexpr float kDriveSnap = 0.75f;  // metres behind before the ped is teleported
    static constexpr float kDriveGain = 10.0f;  // per second: how fast the error closes
    int32_t predicted_x_ = 0, predicted_y_ = 0;

    // How unsteady a turn is: over each 240 frames in which the view turned, the spread of the
    // per-frame yaw steps against their mean (0: every frame turned the same; 1: as uneven as
    // every other frame standing still). Logged, so a change to the look can be judged.
    void Roughness(float yaw) noexcept {
        float step = yaw - last_yaw_;
        if (step > 180.0f) step -= 360.0f;
        if (step < -180.0f) step += 360.0f;
        last_yaw_ = yaw;
        if (step == 0.0f) {
            if (++still_ > 30) turn_n_ = 0, turn_sum_ = turn_sq_ = 0.0;  // the turn ended: start over
            return;
        }
        still_ = 0;
        turn_sum_ += step;
        turn_sq_ += static_cast<double>(step) * step;
        if (++turn_n_ < 240) return;
        const double mean = turn_sum_ / turn_n_;
        const double var = turn_sq_ / turn_n_ - mean * mean;
        GR_LOG("look: 240 turning frames, mean step %.3f deg, roughness %.2f%s", mean,
               mean != 0.0 ? std::sqrt(var > 0.0 ? var : 0.0) / std::fabs(mean) : 0.0,
               (predicted_x_ | predicted_y_) != 0 ? " (predicting)" : "");
        turn_n_ = 0;
        turn_sum_ = turn_sq_ = 0.0;
    }
    float last_yaw_ = 0.0f;
    int still_ = 0, turn_n_ = 0;
    double turn_sum_ = 0.0, turn_sq_ = 0.0;

    static constexpr uint32_t kArthurModel = 0x0D7114C9;  // joaat("player_zero"), seen in the log
    static constexpr const wchar_t* kDrivingMarker = L"GR_PLUGIN_DRIVING_PED";
    static constexpr int32_t kMaxExtrapolateUs = 50000;
    static constexpr float kDefaultFov = 55.0f;
    static constexpr float kGroundSearchUp = 300.0f;
    static constexpr float kGroundStepUp = 1.5f;
    static constexpr float kBodyRadius = 0.3f;  // GMod's hull is 32 units wide: 0.61 m
    static constexpr float kMaxStep = 3.0f;     // metres in one frame; more is not a walk

    // Why RDR2 needs the ped itself right now, or nullptr.
    static const char* BusyReason(native::Entity ped) noexcept {
        if (ped == 0) return "no player ped";
        if (native::IsEntityDead(ped)) return "the player is dead";
        if (!native::IsPlayerControlOn(native::PlayerId())) return "a script has taken player control";
        if (native::IsPedOnMount(ped)) return "the player is on a mount";
        if (native::IsPedInAnyVehicle(ped)) return "the player is in a vehicle";
        return nullptr;
    }

    void NextSerial() noexcept {
        if (++serial_ == 0) serial_ = 1;
    }

    // Works around: until RDR2's terrain is mirrored into GMod (milestone 5), the GMod
    // player walks on a flat map while RDR2's ground rises and falls, so the camera would
    // sink into every hill. Here GMod's flat ground is draped over RDR2's: the height the
    // GMod player stands at counts as "on RDR2's ground", and only its height above that
    // (a jump, noclip) is carried over. Remove when GMod has the real collision.
    void FollowGround(const GrGuestFrame& guest, GrVec3& feet, GrVec3& eye) noexcept {
        if ((guest.flags & GR_GUESTF_ON_GROUND) != 0) flat_z_ = guest.pos.z;
        float lift = feet.z - flat_z_;
        // Walking off a ledge in GMod drops the player below what was "the ground". Only
        // noclip is allowed to take the view under RDR2's.
        if (lift < 0.0f && (guest.flags & GR_GUESTF_NOCLIP) == 0) lift = 0.0f;
        // From a little above where the ground last was, so a roof overhead is not taken
        // for the ground. No answer (nothing loaded there): keep the last one.
        float ground = 0.0f;
        if (native::GetGroundZ(feet.x, feet.y, ground_z_ + kGroundStepUp, ground)) ground_z_ = ground;
        shift_ = ground_z_ + lift - feet.z;
        feet.z += shift_;
        eye.z += shift_;
    }

    // Works around: GMod has no copy of RDR2's walls, wagons and rocks yet (milestone 5), so
    // its player walked straight through them and RDR2's camera with it. RDR2 tests the
    // move from where the player was held last frame to where GMod put it now, with rays at
    // knee, waist and head height reaching a body's radius ahead. If one hits, the move
    // slides along the surface (its part into the surface is taken out) or, if that is
    // blocked too, the player stays put. Either way block_* tells GMod, which puts its
    // player back to the same spot. Noclip goes through, as in GMod; so does a jump of more
    // than kMaxStep in one frame (a teleport or a re-anchor, not a walk).
    void Collide(native::Entity ped, const GrGuestFrame& guest, GrVec3& feet, GrVec3& eye, GrHostFrame& host) noexcept {
        if (!have_held_ || (guest.flags & GR_GUESTF_NOCLIP) != 0) return;
        const float dx = feet.x - held_.x;
        const float dy = feet.y - held_.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.0005f || length > kMaxStep) return;

        GrVec3 normal{};
        if (!Blocked(ped, held_, feet, normal)) return;
        // Slide: keep only the part of the move along the surface.
        const float into = dx * normal.x + dy * normal.y;
        GrVec3 slid{held_.x + dx - into * normal.x, held_.y + dy - into * normal.y, feet.z};
        GrVec3 unused{};
        if (into >= 0.0f || Blocked(ped, held_, slid, unused)) slid = {held_.x, held_.y, feet.z};

        eye.x += slid.x - feet.x;
        eye.y += slid.y - feet.y;
        feet.x = slid.x;
        feet.y = slid.y;
        ++block_serial_;
        host.block_serial = block_serial_;
        host.block_pos = feet;
        host.block_normal = normal;
    }

    // Whether moving the player's body from `from` to `to` (feet) hits something. `normal`
    // gets the horizontal normal of what was hit.
    bool Blocked(native::Entity ped, const GrVec3& from, const GrVec3& to, GrVec3& normal) const noexcept {
        float dx = to.x - from.x;
        float dy = to.y - from.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.0005f) return false;
        dx /= length;
        dy /= length;
        static constexpr float kHeights[] = {0.5f, 1.0f, 1.6f};
        for (const float h : kHeights) {
            native::RayHit hit{};
            const GrVec3 a{from.x, from.y, from.z + h};
            const GrVec3 b{to.x + dx * kBodyRadius, to.y + dy * kBodyRadius, to.z + h};
            // Not objects: bushes are objects too and stopped the player where RDR2 lets it
            // through. The solid ones are in GMod's own collision now (NearSync).
            if (!native::Raycast(a, b, native::kRayMap | native::kRayVehicles, ped, hit)) continue;
            float nx = hit.normal.x;
            float ny = hit.normal.y;
            const float n = std::sqrt(nx * nx + ny * ny);
            // A floor or a slope gentle enough to walk (normal mostly up) is not a wall:
            // FollowGround takes care of those.
            if (n < 0.5f) continue;
            normal = {nx / n, ny / n, 0.0f};
            return true;
        }
        return false;
    }

    // While GMod drives, the ped is kept at full health (CombatSync::Hurt) and GMod's player
    // is the one hurt. When RDR2 takes the player back (a horse, a scripted scene, F9) the
    // ped gets the GMod player's health, so the two games show the same health.
    //
    // Works around: RDR2 caps the health it takes by the health core (seen: 250 asked, 150
    // kept), so read back as a share of the maximum, GMod's 100 came back as 60 after a
    // teleport. What was given and what the ped took are kept, and the health handed back is
    // GMod's own times the share of that the ped still has.
    float given_frac_ = 0.0f;
    int given_health_ = 0;

    void GiveHealth(native::Entity ped, const GrGuestFrame& guest) noexcept {
        given_health_ = 0;
        if (ped == 0 || native::IsEntityDead(ped) || !(guest.player_health > 0.0f)) return;
        const int max = native::GetPedMaxHealth(ped);
        if (max <= 0) return;
        const float frac = guest.player_health > 1.0f ? 1.0f : guest.player_health;
        int health = static_cast<int>(std::lround(frac * static_cast<float>(max)));
        if (health < 1) health = 1;
        native::SetEntityHealth(ped, health);
        given_frac_ = frac;
        given_health_ = native::GetEntityHealth(ped);
        if (given_health_ <= 0) given_health_ = 0;
        GR_LOG("player: RDR2 takes the player with GMod's health, %.0f%% (%d of %d asked, %d kept)", frac * 100.0f,
               health, max, given_health_);
    }

    // While GMod drives, GMod's health decides (GMod has its own god mode), so the ped is kept
    // vulnerable: RDR2's damage to it is what hurts the GMod player. Seen 2026-10-03: the game
    // held the player invincible and undamageable every frame (also with RDR2 in control, no
    // mission, no trainer option on); set again after this ran, so it did not help then. The
    // log says when it finds the player invincible.
    void Vulnerable(native::Entity ped) noexcept {
        const int player = native::PlayerId();
        if (native::GetPlayerInvincible(player)) {
            native::SetPlayerInvincible(player, false);
            if (!told_invincible_) GR_LOG("player: the game has made the player invincible; switched off while GMod drives (enemies cannot hurt it otherwise)");
            told_invincible_ = true;
        }
        if (!native::GetEntityCanBeDamaged(ped)) native::SetEntityCanBeDamaged(ped, true);
    }
    bool told_invincible_ = false;

    // RDR2 has the player while this runs (BusyReason "a teleport"), so GMod has let go of it:
    // the ped is moved and held still while the place loads, then stood on the ground, and
    // the player is given back, which GMod anchors to afresh.
    static constexpr uint64_t kTeleportLoadMs = 2500;
    GrVec3 teleport_to_{};
    int teleport_state_ = 0;
    uint64_t teleport_until_ = 0;

    void Teleport(native::Entity ped) noexcept {
        if (teleport_state_ == 0 || driving_ || ped == 0) return;
        if (teleport_state_ == 1) {
            native::FreezeEntityPosition(ped, true);
            native::SetEntityCoordsNoOffset(ped, teleport_to_);
            teleport_until_ = NowMs() + kTeleportLoadMs;
            teleport_state_ = 2;
        } else if (NowMs() >= teleport_until_) {
            native::FreezeEntityPosition(ped, false);
            PutOnGround(ped);
            teleport_state_ = 0;
            const GrVec3 at = native::GetEntityCoords(ped);
            GR_LOG("player: teleported to (%.1f, %.1f, %.1f)", at.x, at.y, at.z);
        }
    }

    // Applies ped_mode_ to the ped, again whenever it changes.
    void Hide(native::Entity ped) noexcept {
        if (hidden_as_ == ped_mode_) return;
        hidden_as_ = ped_mode_;
        if (ped_mode_ == 1) {
            native::SetEntityVisible(ped, true);
            native::SetEntityAlpha(ped, 0);
            native::FreezeEntityPosition(ped, false);
        } else {
            native::ResetEntityAlpha(ped);
            native::SetEntityVisible(ped, false);
            native::FreezeEntityPosition(ped, true);
        }
        GR_LOG("player: the hidden ped is %s", ped_mode_ == 1 ? "see-through and walked" : "invisible, frozen and teleported");
    }

    void Start(native::Entity ped, const GrGuestFrame& guest) noexcept {
        // The guest anchored to the ped's feet, and the ped stands on the ground.
        flat_z_ = guest.pos.z;
        ground_z_ = guest.pos.z;
        cam_ = native::CreateScriptedCam();
        if (!native::DoesCamExist(cam_)) {
            GR_LOG("player: CREATE_CAM gave no camera. The ped will follow GMod but the view will not");
            cam_ = 0;
        } else {
            native::SetCamActive(cam_, true);
            native::RenderScriptCams(true);
        }
        hidden_as_ = -1;
        given_health_ = 0;
        Hide(ped);
        SetEnvironmentVariableW(kDrivingMarker, L"1");
        ped_ = ped;
        driving_ = true;
        GR_LOG("player: GMod drives the player (ped %d, camera %d, anchor serial %u)", ped, cam_, serial_);
    }

    void Stop(native::Entity ped) noexcept {
        native::RenderScriptCams(false);
        if (cam_ != 0) {
            if (cam_mode_ == 2) native::DetachCam(cam_);
            have_attached_ = false;
            native::SetCamActive(cam_, false);
            native::DestroyCam(cam_);
        }
        if (ped != 0) {
            PutOnGround(ped);
            native::SetEntityVisible(ped, true);
            native::ResetEntityAlpha(ped);
            native::SetPedAllWeaponsVisibility(ped, true);
            native::FreezeEntityPosition(ped, false);
        }
        SetEnvironmentVariableW(kDrivingMarker, nullptr);
        cam_ = 0;
        ped_ = 0;
        driving_ = false;
        GR_LOG("player: RDR2 drives the player again");
    }

    // GMod's ground is not RDR2's (until the terrain is mirrored into GMod), and its player
    // can fly. A ped handed back inside a hill falls through the world, and one handed
    // back in the air falls to its death, so it is stood on the ground first: the ground
    // just under its feet if it is about there already, otherwise the topmost ground at
    // that spot.
    void PutOnGround(native::Entity ped) noexcept {
        const GrVec3 origin = native::GetEntityCoords(ped);
        const float feet = origin.z - feet_;
        float ground = 0.0f;
        const bool close = native::GetGroundZ(origin.x, origin.y, feet + 1.0f, ground) && feet - ground > -0.2f &&
                           feet - ground < 0.5f;
        if (close) return;
        if (!native::GetGroundZ(origin.x, origin.y, feet + kGroundSearchUp, ground)) {
            GR_LOG("player: no ground found at (%.1f, %.1f) to stand the ped on. Left at z %.1f", origin.x, origin.y,
                   origin.z);
            return;
        }
        native::SetEntityCoordsNoOffset(ped, {origin.x, origin.y, ground + feet_});
        GR_LOG("player: the ped was %.1f m %s the ground. Stood it on the ground at z %.2f", std::fabs(feet - ground),
               feet < ground ? "under" : "above", ground);
    }

    // How far a ped's origin is above its feet: the bottom of its model's box.
    void UpdateFeet(native::Entity ped) noexcept {
        if (ped == 0) return;
        const uint32_t model = native::GetEntityModel(ped);
        if (model == model_) return;
        model_ = model;
        GrVec3 lo{};
        GrVec3 hi{};
        native::GetModelDimensions(model, lo, hi);
        // A model that reports no box at all would put the feet at the origin.
        feet_ = lo.z < -0.1f ? -lo.z : 1.0f;
        GR_LOG("player: ped model %08X, box z %.3f to %.3f, origin %.3f m above its feet (height above ground now %.3f)",
               model, lo.z, hi.z, feet_, native::GetEntityHeightAboveGround(ped));
    }

    // Logged once per run: what the game reports, next to what protocol/gr_units.h
    // assumes about RDR2's angles. If a "dot" here is not close to 1 the conversion to
    // Source angles is wrong.
    void CheckConventions(native::Entity ped) noexcept {
        checked_ = true;
        const GrVec3 rot = native::GetEntityRotation(ped);
        const GrVec3 forward = native::GetEntityForwardVector(ped);
        const GrVec3 predicted = RdrForward(rot);
        GR_LOG("convention: ped heading %.1f, rotation (pitch %.1f, roll %.1f, yaw %.1f), forward vector "
               "(%.3f, %.3f, %.3f), gr_units predicts (%.3f, %.3f, %.3f), dot %.4f",
               native::GetEntityHeading(ped), rot.x, rot.y, rot.z, forward.x, forward.y, forward.z, predicted.x,
               predicted.y, predicted.z, Dot(forward, predicted));

        // The gameplay camera looks at the player from behind, so the direction its
        // rotation is predicted to face should be close to the direction to the ped.
        const GrVec3 cam = native::GetGameplayCamCoord();
        const GrVec3 cam_rot = native::GetGameplayCamRot();
        const GrVec3 cam_forward = RdrForward(cam_rot);
        const GrVec3 at = native::GetEntityCoords(ped);
        GrVec3 to_ped{at.x - cam.x, at.y - cam.y, at.z - cam.z};
        const float length = std::sqrt(Dot(to_ped, to_ped));
        if (length > 0.01f) to_ped = {to_ped.x / length, to_ped.y / length, to_ped.z / length};
        GR_LOG("convention: gameplay camera rotation (pitch %.1f, roll %.1f, yaw %.1f), gr_units predicts it faces "
               "(%.3f, %.3f, %.3f), the ped is %.2f m away towards (%.3f, %.3f, %.3f), dot %.4f",
               cam_rot.x, cam_rot.y, cam_rot.z, cam_forward.x, cam_forward.y, cam_forward.z, length, to_ped.x,
               to_ped.y, to_ped.z, Dot(cam_forward, to_ped));
    }

    static float Dot(const GrVec3& a, const GrVec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

    native::Entity ped_ = 0;
    native::Cam cam_ = 0;
    uint32_t model_ = 0;
    uint32_t serial_ = 1;
    float feet_ = 1.0f;
    float flat_z_ = 0.0f;    // the height of GMod's flat ground, as an RDR2 z
    float ground_z_ = 0.0f;  // RDR2's ground under the player, as last found
    float shift_ = 0.0f;     // what FollowGround added to GMod's heights this frame
    GrVec3 held_{};          // feet where the player was put last frame
    bool have_held_ = false;
    uint32_t block_serial_ = 0;
    bool driving_ = false;
    bool owns_ = false;
    bool user_owns_ = false;
    bool paused_ = false;
    bool checked_ = false;
};

}  // namespace gr
