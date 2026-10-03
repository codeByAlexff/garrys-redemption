// Typed wrappers over the natives in native_hashes.h. Callable only from the script
// thread (inside ScriptMain), like every native.

#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "gr_protocol.h"
#include "native_hashes.h"
#include "scripthook.h"

namespace gr::native {

// Every argument travels as one 64-bit slot. Smaller types sit in the low bytes with
// the rest zeroed, floats included (their bits, not a conversion).
template <class T>
inline void Push(T value) noexcept {
    static_assert(sizeof(T) <= sizeof(UINT64) && std::is_trivially_copyable_v<T>);
    UINT64 slot = 0;
    std::memcpy(&slot, &value, sizeof(T));
    nativePush64(slot);
}

template <class R, class... Args>
inline R Invoke(uint64_t hash, Args... args) noexcept {
    nativeInit(hash);
    (Push(args), ...);
    const PUINT64 result = nativeCall();
    if constexpr (!std::is_void_v<R>) {
        static_assert(sizeof(R) <= sizeof(UINT64) && std::is_trivially_copyable_v<R>);
        R out;
        std::memcpy(&out, result, sizeof(R));
        return out;
    }
}

// A native that returns a Vector3 fills three result slots, one float in the low half of
// each.
template <class... Args>
inline GrVec3 InvokeVec3(uint64_t hash, Args... args) noexcept {
    nativeInit(hash);
    (Push(args), ...);
    const PUINT64 result = nativeCall();
    GrVec3 out;
    std::memcpy(&out.x, &result[0], sizeof(float));
    std::memcpy(&out.y, &result[1], sizeof(float));
    std::memcpy(&out.z, &result[2], sizeof(float));
    return out;
}

// The same shape for a Vector3 the game writes through a pointer argument.
struct ScrVec3 {
    float x;
    uint32_t pad_x;
    float y;
    uint32_t pad_y;
    float z;
    uint32_t pad_z;
};
static_assert(sizeof(ScrVec3) == 24);

// Handles are plain ints to the game.
using Entity = int;
using Cam = int;

// True in any kind of network session. Three checks because none of them is documented
// well enough to trust alone, and a false positive only costs a refused bridge.
inline bool IsMultiplayer() noexcept {
    return Invoke<int>(native_hash::NETWORK_IS_IN_SESSION) != 0 ||
           Invoke<int>(native_hash::NETWORK_IS_SESSION_STARTED) != 0 ||
           Invoke<int>(native_hash::NETWORK_IS_GAME_IN_PROGRESS) != 0;
}

inline int GetGameTimer() noexcept { return Invoke<int>(native_hash::GET_GAME_TIMER); }
inline float GetFrameTime() noexcept { return Invoke<float>(native_hash::GET_FRAME_TIME); }

// Draws one line of debug text this frame. x and y are 0..1 screen fractions.
//
// _BG_DISPLAY_TEXT only accepts a string made by VAR_STRING. The (10, "LITERAL_STRING",
// text) form is the one community scripts use for raw text; it agrees with the
// database's notes on VAR_STRING (first flag bit clear, a text label as the extra
// argument) but the flag value itself is not documented there. Verified in-game on
// 1.0.1491.50 (2026-10-01): the status line shows.
inline void DrawText(const char* text, float x, float y, float scale) noexcept {
    const char* var = Invoke<const char*>(native_hash::VAR_STRING, 10, "LITERAL_STRING", text);
    Invoke<void>(native_hash::BG_SET_TEXT_SCALE, scale, scale);
    Invoke<void>(native_hash::BG_SET_TEXT_COLOR, 255, 255, 255, 255);
    Invoke<void>(native_hash::BG_DISPLAY_TEXT, var, x, y);
}

inline Entity PlayerPedId() noexcept { return Invoke<int>(native_hash::PLAYER_PED_ID); }
inline int PlayerId() noexcept { return Invoke<int>(native_hash::PLAYER_ID); }
inline bool IsPlayerControlOn(int player) noexcept {
    return Invoke<int>(native_hash::IS_PLAYER_CONTROL_ON, player) != 0;
}
inline bool IsPauseMenuActive() noexcept { return Invoke<int>(native_hash::IS_PAUSE_MENU_ACTIVE) != 0; }
// Switches, not per-frame: whatever is turned off stays off until turned on again.
inline void DisplayRadar(bool on) noexcept { Invoke<void>(native_hash::DISPLAY_RADAR, on ? 1 : 0); }
inline void ShowPlayerCores(bool on) noexcept { Invoke<void>(native_hash::SHOW_PLAYER_CORES, on ? 1 : 0); }
inline void ShowHorseCores(bool on) noexcept { Invoke<void>(native_hash::SHOW_HORSE_CORES, on ? 1 : 0); }

// The entity's origin. For a ped that is its middle, not its feet.
inline GrVec3 GetEntityCoords(Entity e) noexcept {
    return InvokeVec3(native_hash::GET_ENTITY_COORDS, e, 0 /*alive: unused*/, 1 /*realCoords*/);
}
// x = pitch, y = roll, z = yaw, rotation order 2: the wire format.
inline GrVec3 GetEntityRotation(Entity e) noexcept {
    return InvokeVec3(native_hash::GET_ENTITY_ROTATION, e, 2);
}
inline float GetEntityHeading(Entity e) noexcept { return Invoke<float>(native_hash::GET_ENTITY_HEADING, e); }
inline GrVec3 GetEntityForwardVector(Entity e) noexcept {
    return InvokeVec3(native_hash::GET_ENTITY_FORWARD_VECTOR, e);
}
inline uint32_t GetEntityModel(Entity e) noexcept { return Invoke<uint32_t>(native_hash::GET_ENTITY_MODEL, e); }
inline float GetEntityHeightAboveGround(Entity e) noexcept {
    return Invoke<float>(native_hash::GET_ENTITY_HEIGHT_ABOVE_GROUND, e);
}
inline bool IsEntityDead(Entity e) noexcept { return Invoke<int>(native_hash::IS_ENTITY_DEAD, e) != 0; }
inline bool IsEntityVisible(Entity e) noexcept { return Invoke<int>(native_hash::IS_ENTITY_VISIBLE, e) != 0; }
inline bool GetEntityCollisionDisabled(Entity e) noexcept {
    return Invoke<int>(native_hash::GET_ENTITY_COLLISION_DISABLED, e) != 0;
}
inline void SetEntityVisible(Entity e, bool on) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_VISIBLE, e, on ? 1 : 0);
}
inline void FreezeEntityPosition(Entity e, bool on) noexcept {
    Invoke<void>(native_hash::FREEZE_ENTITY_POSITION, e, on ? 1 : 0);
}
inline void SetPedGravity(Entity ped, bool on) noexcept {
    Invoke<void>(native_hash::SET_PED_GRAVITY, ped, on ? 1 : 0);
}
inline void SetPedCanRagdoll(Entity ped, bool on) noexcept {
    Invoke<void>(native_hash::SET_PED_CAN_RAGDOLL, ped, on ? 1 : 0);
}
// p1 false, resetCrouch true: what community scripts pass. Ends a ragdoll too (seen).
inline void ClearPedTasksImmediately(Entity ped) noexcept {
    Invoke<void>(native_hash::CLEAR_PED_TASKS_IMMEDIATELY, ped, 0, 1);
}
inline bool IsPedUsingAnyScenario(Entity ped) noexcept {
    return Invoke<int>(native_hash::IS_PED_USING_ANY_SCENARIO, ped) != 0;
}
// ---- entities near a point (world_sync.h)
// p0 true as community scripts pass. TODO(verify): what it means.
inline int CreateItemset() noexcept { return Invoke<int>(native_hash::CREATE_ITEMSET, 1); }
inline bool IsItemsetValid(int set) noexcept { return Invoke<int>(native_hash::IS_ITEMSET_VALID, set) != 0; }
inline void ClearItemset(int set) noexcept { Invoke<void>(native_hash::CLEAR_ITEMSET, set); }
inline int GetItemsetSize(int set) noexcept { return Invoke<int>(native_hash::GET_ITEMSET_SIZE, set); }
inline Entity GetIndexedItemInItemset(int index, int set) noexcept {
    return Invoke<Entity>(native_hash::GET_INDEXED_ITEM_IN_ITEMSET, index, set);
}
// Fills `set` with the entities within `radius` of `at`; returns how many. `type`: see world_sync.h.
inline int GetEntitiesNearPoint(const GrVec3& at, float radius, int set, int type) noexcept {
    return Invoke<int>(native_hash::GET_ENTITIES_NEAR_POINT, at.x, at.y, at.z, radius, set, type);
}
inline bool IsEntityAnObject(Entity e) noexcept { return Invoke<int>(native_hash::IS_ENTITY_AN_OBJECT, e) != 0; }
// ---- the possess tool
inline void SetBlockingOfNonTemporaryEvents(Entity ped, bool on) noexcept {
    Invoke<void>(native_hash::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS, ped, on ? 1 : 0);
}
// p1 and p2 true, as community scripts pass. TODO(verify): what they mean.
inline void ClearPedTasks(Entity ped) noexcept { Invoke<void>(native_hash::CLEAR_PED_TASKS, ped, 1, 1); }
inline void TaskStandStill(Entity ped, int ms) noexcept { Invoke<void>(native_hash::TASK_STAND_STILL, ped, ms); }
inline void TaskJump(Entity ped) noexcept { Invoke<void>(native_hash::TASK_JUMP, ped, 1); }
// `pace` is the move blend speed: 1 walk, 2 run, 3 sprint. Never teleports; p8 0.
inline void TaskGoStraightToCoord(Entity ped, const GrVec3& to, float pace, float heading) noexcept {
    Invoke<void>(native_hash::TASK_GO_STRAIGHT_TO_COORD, ped, to.x, to.y, to.z, pace, -1, heading, 0.5f, 0);
}
// p1 0 and p2 true: TODO(verify) what they mean (GTA V's REQUEST_WEAPON_ASSET takes flags 31, 0).
inline void RequestWeaponAsset(uint32_t weapon) noexcept {
    Invoke<void>(native_hash::REQUEST_WEAPON_ASSET, weapon, 0, 1);
}
inline bool HasWeaponAssetLoaded(uint32_t weapon) noexcept {
    return Invoke<int>(native_hash::HAS_WEAPON_ASSET_LOADED, weapon) != 0;
}
inline void RemoveWeaponAsset(uint32_t weapon) noexcept { Invoke<void>(native_hash::REMOVE_WEAPON_ASSET, weapon); }
inline Entity CreateWeaponObject(uint32_t weapon, const GrVec3& at) noexcept {
    return Invoke<int>(native_hash::CREATE_WEAPON_OBJECT, weapon, 0, at.x, at.y, at.z, 1, 1.0f);
}
inline void SetEntityCollision(Entity e, bool on) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_COLLISION, e, on ? 1 : 0, 0);
}
inline bool IsWeaponValid(uint32_t weapon) noexcept { return Invoke<int>(native_hash::IS_WEAPON_VALID, weapon) != 0; }
inline void SetEntityHeading(Entity e, float heading) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_HEADING, e, heading);
}
// Puts the entity's origin exactly at `p` (SET_ENTITY_COORDS would add a ground offset).
inline void SetEntityCoordsNoOffset(Entity e, const GrVec3& p) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_COORDS_NO_OFFSET, e, p.x, p.y, p.z, 0, 0, 0);
}
inline void GetModelDimensions(uint32_t model, GrVec3& min, GrVec3& max) noexcept {
    ScrVec3 lo{};
    ScrVec3 hi{};
    Invoke<void>(native_hash::GET_MODEL_DIMENSIONS, model, &lo, &hi);
    min = {lo.x, lo.y, lo.z};
    max = {hi.x, hi.y, hi.z};
}

// The highest ground at or below (x, y, z). False if there is none, or none loaded.
inline bool GetGroundZ(float x, float y, float z, float& ground) noexcept {
    // The game writes a float through the pointer; give it a full slot to write into.
    UINT64 slot = 0;
    const bool found = Invoke<int>(native_hash::GET_GROUND_Z_FOR_3D_COORD, x, y, z, &slot, 0) != 0;
    std::memcpy(&ground, &slot, sizeof(float));
    return found;
}

struct RayHit {
    bool hit;
    GrVec3 pos;
    GrVec3 normal;
    Entity entity;
};

// Shape test flags. The native database does not list them; they are GTA V's, and were
// checked in-game with a ray at a tent 3.4 m away: 1 hit the map (the tent wall), 4 a ped
// behind it, 8 its ragdoll bounds, 16 an object, 2 nothing (docs/NATIVES.md).
inline constexpr int kRayMap = 1;
inline constexpr int kRayVehicles = 2;
inline constexpr int kRayObjects = 16;
inline constexpr int kRayFoliage = 256;  // trees and bushes: seen in-game, a ray with only this
                                         // flag was stopped by a pine's branches (veil_sync.h)

// A ray from `from` to `to` that ignores `ignore`. Synchronous: the game answers at once.
inline bool Raycast(const GrVec3& from, const GrVec3& to, int flags, Entity ignore, RayHit& out) noexcept {
    const int handle = Invoke<int>(native_hash::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE, from.x, from.y,
                                   from.z, to.x, to.y, to.z, flags, ignore, 4);
    UINT64 hit = 0;
    ScrVec3 end{};
    ScrVec3 normal{};
    UINT64 entity = 0;
    const int status = Invoke<int>(native_hash::GET_SHAPE_TEST_RESULT, handle, &hit, &end, &normal, &entity);
    out.hit = status == 2 && (hit & 0xFFFFFFFF) != 0;
    out.pos = {end.x, end.y, end.z};
    out.normal = {normal.x, normal.y, normal.z};
    out.entity = static_cast<Entity>(entity & 0xFFFFFFFF);
    return out.hit;
}

inline int GetClockHours() noexcept { return Invoke<int>(native_hash::GET_CLOCK_HOURS); }
inline int GetClockMinutes() noexcept { return Invoke<int>(native_hash::GET_CLOCK_MINUTES); }
inline int GetClockSeconds() noexcept { return Invoke<int>(native_hash::GET_CLOCK_SECONDS); }
inline int GetInteriorFromEntity(Entity entity) noexcept {
    return Invoke<int>(native_hash::GET_INTERIOR_FROM_ENTITY, entity);
}

inline bool IsPedOnMount(Entity ped) noexcept { return Invoke<int>(native_hash::IS_PED_ON_MOUNT, ped) != 0; }
inline bool IsPedInAnyVehicle(Entity ped) noexcept {
    return Invoke<int>(native_hash::IS_PED_IN_ANY_VEHICLE, ped, 0) != 0;
}

inline bool DoesEntityExist(Entity e) noexcept { return Invoke<int>(native_hash::DOES_ENTITY_EXIST, e) != 0; }
// The database names the second argument p1 and says nothing about it. 0 is what
// community scripts pass.
inline GrVec3 GetEntityVelocity(Entity e) noexcept { return InvokeVec3(native_hash::GET_ENTITY_VELOCITY, e, 0); }
inline void SetEntityVelocity(Entity e, const GrVec3& v) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_VELOCITY, e, v.x, v.y, v.z);
}
inline int GetEntityHealth(Entity e) noexcept { return Invoke<int>(native_hash::GET_ENTITY_HEALTH, e); }
inline void SetEntityHealth(Entity e, int health) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_HEALTH, e, health, 0);
}
inline int GetPedMaxHealth(Entity ped) noexcept { return Invoke<int>(native_hash::GET_PED_MAX_HEALTH, ped); }
inline void ApplyDamageToPed(Entity ped, int damage, int bone, Entity killer) noexcept {
    Invoke<void>(native_hash::APPLY_DAMAGE_TO_PED, ped, damage, 0, bone, killer);
}
inline GrVec3 GetPedBoneCoords(Entity ped, int bone) noexcept {
    return InvokeVec3(native_hash::GET_PED_BONE_COORDS, ped, bone, 0.0f, 0.0f, 0.0f);
}
// A real bullet from `from` to `to`, fired by `owner`'s hand as far as the game can tell
// (law and witnesses react to the owner). p7 true and speed -1 (the weapon's own) are what
// community scripts pass; p13 is undocumented and passed false. TODO(verify) in-game: p7.
inline void ShootBullet(const GrVec3& from, const GrVec3& to, int damage, uint32_t weapon, Entity owner,
                        bool audible) noexcept {
    Invoke<void>(native_hash::SHOOT_SINGLE_BULLET_BETWEEN_COORDS, from.x, from.y, from.z, to.x, to.y, to.z, damage, 1,
                 weapon, owner, audible ? 1 : 0, 0, -1.0f, 0);
}
// ---- attachments and ropes (milestone 8)

// Holds `child` to `parent` at `offset` (parent's space) turned by `rot` (degrees, relative).
// p9, p15, p16 false, vertex 0 and fixed rotation are what community scripts pass; bone 0 is
// the root. TODO(verify): the meaning of p9, p15 and p16.
inline void AttachEntityToEntity(Entity child, Entity parent, const GrVec3& offset, const GrVec3& rot,
                                 bool child_is_ped) noexcept {
    Invoke<void>(native_hash::ATTACH_ENTITY_TO_ENTITY, child, parent, 0, offset.x, offset.y, offset.z, rot.x, rot.y,
                 rot.z, 0, 0, 1, child_is_ped ? 1 : 0, 0, 1, 0, 0);
}
inline void DetachEntity(Entity e) noexcept { Invoke<void>(native_hash::DETACH_ENTITY, e, 1, 1); }
inline bool IsEntityAttached(Entity e) noexcept { return Invoke<int>(native_hash::IS_ENTITY_ATTACHED, e) != 0; }
inline GrVec3 WorldToEntity(Entity e, const GrVec3& p) noexcept {
    return InvokeVec3(native_hash::GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS, e, p.x, p.y, p.z);
}
inline GrVec3 EntityToWorld(Entity e, const GrVec3& p) noexcept {
    return InvokeVec3(native_hash::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS, e, p.x, p.y, p.z);
}
inline void SetEntityNoCollisionEntity(Entity a, Entity b, bool this_frame_only) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_NO_COLLISION_ENTITY, a, b, this_frame_only ? 1 : 0);
}
// A rope at `at`. Type 6, p9 31 and p10 -1 are what a working RedM rope script passes
// (github.com/Big-Yoda/redm-hanging); not networked in story mode. The plain ADD_ROPE with
// GTA V's arguments made a rope that did nothing (seen 2026-10-02).
inline int AddRope(const GrVec3& at, float length) noexcept {
    return Invoke<int>(native_hash::ADD_ROPE_2, at.x, at.y, at.z, 0.0f, 0.0f, 0.0f, length, 6, 0, 31, -1.0f);
}
// Each end at a point in its entity's own space; no bones.
inline void AttachEntitiesToRope(int rope, Entity a, Entity b, const GrVec3& local_a, const GrVec3& local_b) noexcept {
    Invoke<void>(native_hash::ATTACH_ENTITIES_TO_ROPE_2, rope, a, b, local_a.x, local_a.y, local_a.z, local_b.x,
                 local_b.y, local_b.z, static_cast<const char*>(nullptr), static_cast<const char*>(nullptr));
}
inline void DeleteRope(int rope) noexcept {
    UINT64 slot = static_cast<uint32_t>(rope);
    Invoke<void>(native_hash::DELETE_ROPE, &slot);
}
inline bool DoesRopeExist(int rope) noexcept { return Invoke<int>(native_hash::DOES_ROPE_EXIST, rope) != 0; }

inline void AddOwnedExplosion(Entity owner, const GrVec3& p, int tag, float damage_scale, float camera_shake) noexcept {
    Invoke<void>(native_hash::ADD_OWNED_EXPLOSION, owner, p.x, p.y, p.z, tag, damage_scale, 1, 0, camera_shake);
}
// ---- creating and deleting (milestone 7)

inline void RequestModel(uint32_t model) noexcept { Invoke<void>(native_hash::REQUEST_MODEL, model, 0); }
inline bool HasModelLoaded(uint32_t model) noexcept { return Invoke<int>(native_hash::HAS_MODEL_LOADED, model) != 0; }
inline void SetModelAsNoLongerNeeded(uint32_t model) noexcept {
    Invoke<void>(native_hash::SET_MODEL_AS_NO_LONGER_NEEDED, model);
}
inline bool IsModelInCdimage(uint32_t model) noexcept {
    return Invoke<int>(native_hash::IS_MODEL_IN_CDIMAGE, model) != 0;
}
inline bool IsModelAPed(uint32_t model) noexcept { return Invoke<int>(native_hash::IS_MODEL_A_PED, model) != 0; }
inline bool IsModelAVehicle(uint32_t model) noexcept {
    return Invoke<int>(native_hash::IS_MODEL_A_VEHICLE, model) != 0;
}
// Local to this game (not networked), owned by no script host.
inline Entity CreatePed(uint32_t model, const GrVec3& p, float heading) noexcept {
    return Invoke<int>(native_hash::CREATE_PED, model, p.x, p.y, p.z, heading, 0, 0, 0, 0);
}
inline void SetRandomOutfitVariation(Entity ped) noexcept {
    Invoke<void>(native_hash::SET_RANDOM_OUTFIT_VARIATION, ped, 1);
}
// Draft vehicles (wagons, coaches) come with their horses.
inline Entity CreateVehicle(uint32_t model, const GrVec3& p, float heading) noexcept {
    return Invoke<int>(native_hash::CREATE_VEHICLE, model, p.x, p.y, p.z, heading, 0, 0, 0, 0);
}
inline void SetVehicleOnGroundProperly(Entity vehicle) noexcept {
    Invoke<int>(native_hash::SET_VEHICLE_ON_GROUND_PROPERLY, vehicle, 0);
}
inline Entity GetPedInDraftHarness(Entity vehicle, int harness) noexcept {
    return Invoke<int>(native_hash::GET_PED_IN_DRAFT_HARNESS, vehicle, harness);
}
// Dynamic: it has physics and falls, rolls and can be knocked about.
inline Entity CreateObject(uint32_t model, const GrVec3& p) noexcept {
    return Invoke<int>(native_hash::CREATE_OBJECT, model, p.x, p.y, p.z, 0, 0, 1, 0, 0);
}
inline bool IsEntityAPed(Entity e) noexcept { return Invoke<int>(native_hash::IS_ENTITY_A_PED, e) != 0; }
inline bool IsEntityAVehicle(Entity e) noexcept { return Invoke<int>(native_hash::IS_ENTITY_A_VEHICLE, e) != 0; }
// `rot` as on the wire: x = pitch, y = roll, z = yaw, rotation order 2.
inline void SetEntityRotation(Entity e, const GrVec3& rot) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_ROTATION, e, rot.x, rot.y, rot.z, 2, 1);
}
// Wakes an object's physics: a new or resting object ignores SET_ENTITY_VELOCITY (seen:
// a spawned crate GMod threw only turned in RDR2).
inline void ActivatePhysics(Entity e) noexcept { Invoke<void>(native_hash::ACTIVATE_PHYSICS, e); }
inline void ResurrectPed(Entity ped) noexcept { Invoke<void>(native_hash::RESURRECT_PED, ped); }
inline bool IsEntityInWater(Entity e) noexcept { return Invoke<int>(native_hash::IS_ENTITY_IN_WATER, e) != 0; }
inline bool IsEntityFrozenNow(Entity e) noexcept { return Invoke<int>(native_hash::IS_ENTITY_FROZEN, e) != 0; }
// Whatever kept a world entity from falling is undone: a moored boat's anchor, a placed
// object's static state, gravity switched off. Calling the vehicle and object ones on the
// other kind is left out.
inline void MakeFallable(Entity e) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_DYNAMIC, e, 1);
    Invoke<void>(native_hash::SET_ENTITY_HAS_GRAVITY, e, 1);
    if (IsEntityAVehicle(e)) {
        Invoke<void>(native_hash::SET_BOAT_ANCHOR, e, 0);
    } else {
        Invoke<void>(native_hash::SET_ACTIVATE_OBJECT_PHYSICS_AS_SOON_AS_IT_IS_UNFROZEN, e, 1);
    }
}
// DELETE_ENTITY only deletes what the calling script owns: take it over first, from
// whichever script had it.
inline void DeleteEntity(Entity e) noexcept {
    Invoke<void>(native_hash::SET_ENTITY_AS_MISSION_ENTITY, e, 1, 1);
    UINT64 slot = static_cast<uint32_t>(e);
    Invoke<void>(native_hash::DELETE_ENTITY, &slot);
}

// ---- law

inline int GetBounty(int player) noexcept { return Invoke<int>(native_hash::GET_BOUNTY, player); }
inline void ClearWanted(int player) noexcept {
    Invoke<void>(native_hash::CLEAR_WANTED_SCORE, player);
    Invoke<void>(native_hash::CLEAR_BOUNTY, player);
    Invoke<void>(native_hash::SET_BOUNTY_HUNTER_PURSUIT_CLEARED);
}

inline bool IsPedHuman(Entity ped) noexcept { return Invoke<int>(native_hash::IS_PED_HUMAN, ped) != 0; }
inline bool IsModelAHorse(uint32_t model) noexcept {
    return Invoke<int>(native_hash::IS_THIS_MODEL_A_HORSE, model) != 0;
}
inline bool IsPedRagdoll(Entity ped) noexcept { return Invoke<int>(native_hash::IS_PED_RAGDOLL, ped) != 0; }
// A ragdoll for between min and max ms, no NM task message. Ragdoll type 0 for people; 1 for
// animals. Works around: with type 0 an animal held in the physgun hung stiff in an animated
// pose, dropped it landed on its feet and knelt down slowly, and its ragdoll ended after
// about 2 s whatever the time given (user: "animal entities fall really stiff and slow").
// With type 1 it hangs and falls limp like a person (seen with a bull). Type 2 is refused
// for animals (returns false). The types are GTA V's numbering: TODO(verify) their RDR2
// meaning beyond what was seen.
inline bool SetPedToRagdoll(Entity ped, int min_ms, int max_ms) noexcept {
    const int type = IsPedHuman(ped) ? 0 : 1;
    return Invoke<int>(native_hash::SET_PED_TO_RAGDOLL, ped, min_ms, max_ms, type, 0, 0,
                       static_cast<const char*>(nullptr)) != 0;
}
inline void ResetPedRagdollTimer(Entity ped) noexcept { Invoke<void>(native_hash::RESET_PED_RAGDOLL_TIMER, ped); }

// "DEFAULT_SCRIPTED_CAMERA" is the name the game's own scripts and community scripts
// create free cameras with. The database documents the native but not the names it
// accepts. Verified in-game 2026-10-01: it returns a camera and RDR2 renders from it.
inline Cam CreateScriptedCam() noexcept {
    return Invoke<int>(native_hash::CREATE_CAM, "DEFAULT_SCRIPTED_CAMERA", 0);
}
inline bool DoesCamExist(Cam cam) noexcept { return Invoke<int>(native_hash::DOES_CAM_EXIST, cam) != 0; }
inline void DestroyCam(Cam cam) noexcept { Invoke<void>(native_hash::DESTROY_CAM, cam, 0); }
inline void SetCamActive(Cam cam, bool on) noexcept { Invoke<void>(native_hash::SET_CAM_ACTIVE, cam, on ? 1 : 0); }
inline void SetCamCoord(Cam cam, const GrVec3& p) noexcept {
    Invoke<void>(native_hash::SET_CAM_COORD, cam, p.x, p.y, p.z);
}
// `rot` is x = pitch, y = roll, z = yaw, as on the wire.
inline void SetCamRot(Cam cam, const GrVec3& rot) noexcept {
    Invoke<void>(native_hash::SET_CAM_ROT, cam, rot.x, rot.y, rot.z, 2);
}
inline void SetCamFov(Cam cam, float fov) noexcept { Invoke<void>(native_hash::SET_CAM_FOV, cam, fov); }
// Position, rotation (order 2, as SetCamRot) and FOV in one call, no blend (p8 0).
inline void SetCamParams(Cam cam, const GrVec3& p, const GrVec3& rot, float fov) noexcept {
    Invoke<void>(native_hash::SET_CAM_PARAMS, cam, p.x, p.y, p.z, rot.x, rot.y, rot.z, fov, 0, 1, 1, 2, 0, 0);
}
// `offset` in world axes when `relative` is false (the camera does not turn with the entity).
inline void AttachCamToEntity(Cam cam, Entity e, const GrVec3& offset, bool relative) noexcept {
    Invoke<void>(native_hash::ATTACH_CAM_TO_ENTITY, cam, e, offset.x, offset.y, offset.z, relative ? 1 : 0);
}
inline void DetachCam(Cam cam) noexcept { Invoke<void>(native_hash::DETACH_CAM, cam); }
inline void SetEntityAlpha(Entity e, int alpha) noexcept { Invoke<void>(native_hash::SET_ENTITY_ALPHA, e, alpha, 0); }
inline void ResetEntityAlpha(Entity e) noexcept { Invoke<void>(native_hash::RESET_ENTITY_ALPHA, e); }
inline void SetPedAllWeaponsVisibility(Entity ped, bool visible) noexcept {
    Invoke<void>(native_hash::SET_PED_ALL_WEAPONS_VISIBILITY, ped, visible ? 1 : 0);
}
inline bool GetPlayerInvincible(int player) noexcept { return Invoke<int>(native_hash::GET_PLAYER_INVINCIBLE, player) != 0; }
inline void SetPlayerInvincible(int player, bool on) noexcept { Invoke<void>(native_hash::SET_PLAYER_INVINCIBLE, player, on ? 1 : 0); }
inline bool GetEntityCanBeDamaged(Entity e) noexcept { return Invoke<int>(native_hash::GET_ENTITY_CAN_BE_DAMAGED, e) != 0; }
inline void SetEntityCanBeDamaged(Entity e, bool on) noexcept { Invoke<void>(native_hash::SET_ENTITY_CAN_BE_DAMAGED, e, on ? 1 : 0); }
// Switches the picture between the script cameras and the game's own, with no blend.
inline void RenderScriptCams(bool on) noexcept {
    Invoke<void>(native_hash::RENDER_SCRIPT_CAMS, on ? 1 : 0, 0, 0, 1, 1, 0);
}
inline GrVec3 GetGameplayCamCoord() noexcept { return InvokeVec3(native_hash::GET_GAMEPLAY_CAM_COORD); }
inline GrVec3 GetGameplayCamRot() noexcept { return InvokeVec3(native_hash::GET_GAMEPLAY_CAM_ROT, 2); }
inline float GetFinalRenderedCamFov() noexcept { return Invoke<float>(native_hash::GET_FINAL_RENDERED_CAM_FOV); }

// Both last for one frame: call every frame for as long as RDR2 must not act on input.
inline void DisableAllControlActions() noexcept { Invoke<void>(native_hash::DISABLE_ALL_CONTROL_ACTIONS, 0); }
inline void EnableControlAction(uint32_t action) noexcept {
    Invoke<void>(native_hash::ENABLE_CONTROL_ACTION, 0, action, 1);
}

}  // namespace gr::native
