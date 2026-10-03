// Spawn sync, RDR2 side (milestone 7): GMod's spawn menu asks for an RDR2 ped, animal,
// vehicle or object by model hash, and this makes it, keeps track of it for undo and
// cleanup, and deletes what GMod's remover, undo and cleanup take away.
//
// A model has to be streamed in before anything can be made of it, which takes a few
// frames: a spawn waits in `pending_` meanwhile, asking again each frame, never waiting.
// What became of each spawn goes back to GMod in GrHostFrame.spawn_results.
//
// The entities stay until GMod removes them: made by this script, they are its mission
// entities and the game does not clean them up.
//
// Everything here calls natives: script thread only. No allocation: fixed arrays.

#pragma once

#include <cmath>
#include <cstring>

#include "gr_log.h"
#include "gr_protocol.h"
#include "natives.h"

namespace gr {

class SpawnSync {
public:
    static constexpr uint32_t kMaxSpawned = 128;

    void Reset() noexcept {
        pending_count_ = 0;
        spawned_count_ = 0;
        result_count_ = 0;
        prune_next_ = 0;
    }

    // GMod restarted: its ids start again from 1, so the old ones must not match new ones.
    // What it spawned stays spawned (cleanup still finds it).
    void ForgetIds() noexcept {
        for (uint32_t i = 0; i < spawned_count_; ++i) spawned_[i].id = 0;
        pending_count_ = 0;
    }

    // One GR_EVENT_SPAWN, GR_EVENT_REMOVE or GR_EVENT_CLEAR_WANTED.
    void Apply(const GrEvent& e, uint64_t now_ms) noexcept {
        switch (e.type) {
            case GR_EVENT_SPAWN: Request(e, now_ms); break;
            case GR_EVENT_REMOVE: Remove(e); break;
            case GR_EVENT_CLEAR_WANTED: ClearWanted(); break;
            default: break;
        }
    }

    // Once a frame: makes what has finished loading, forgets what no longer exists, and
    // publishes the results.
    void Tick(uint64_t now_ms, GrHostFrame& host) noexcept {
        for (uint32_t i = 0; i < pending_count_;) {
            Pending& p = pending_[i];
            native::RequestModel(p.model);
            if (native::HasModelLoaded(p.model)) {
                Create(p);
            } else if (now_ms < p.deadline_ms) {
                ++i;
                continue;
            } else {
                GR_LOG("spawn: model %08X did not load in time", p.model);
                Result(p.id, 0, p.model, GR_SPAWN_TIMEOUT);
            }
            pending_[i] = pending_[--pending_count_];
        }

        // A few a frame: a ped can die and be cleaned up, a wagon can be wrecked.
        for (uint32_t n = 0; n < 8 && spawned_count_ > 0; ++n) {
            if (prune_next_ >= spawned_count_) prune_next_ = 0;
            if (!native::DoesEntityExist(spawned_[prune_next_].handle)) {
                spawned_[prune_next_] = spawned_[--spawned_count_];
            } else {
                ++prune_next_;
            }
        }

        host.spawn_result_count = result_count_;
        for (uint32_t i = 0; i < GR_MAX_SPAWN_RESULTS; ++i) host.spawn_results[i] = results_[i];
    }

    bool IsSpawned(native::Entity e) const noexcept { return Find(e) != nullptr; }

    // The spawned objects (not peds, not vehicles: the world scan finds those itself).
    uint32_t Objects(native::Entity* out, uint32_t max) const noexcept {
        uint32_t n = 0;
        for (uint32_t i = 0; i < spawned_count_ && n < max; ++i) {
            if (spawned_[i].kind == GR_ENT_OBJECT) out[n++] = spawned_[i].handle;
        }
        return n;
    }

    uint32_t spawned_count() const noexcept { return spawned_count_; }

    // persist.h: what was spawned, kept across a plugin reload. Pending spawns are not kept
    // (their models are asked for again by nobody; GMod's undo of one finds nothing).
    static constexpr uint32_t kSaveBytes = 4 + 16 * kMaxSpawned;  // 16: sizeof(Spawned), asserted below
    void Save(uint8_t* to) const noexcept {
        std::memcpy(to, &spawned_count_, 4);
        std::memcpy(to + 4, spawned_, sizeof(Spawned) * spawned_count_);
    }
    void Restore(const uint8_t* from) noexcept {
        uint32_t n = 0;
        std::memcpy(&n, from, 4);
        if (n > kMaxSpawned) return;
        std::memcpy(spawned_, from + 4, sizeof(Spawned) * n);
        spawned_count_ = n;
        if (n) GR_LOG("spawn: %u spawned entities remembered from before the reload", n);
    }

private:
    static constexpr uint32_t kMaxPending = 16;
    static constexpr uint64_t kLoadTimeoutMs = 10000;
    static constexpr int kMaxHarnesses = 4;

    struct Pending {
        uint32_t id;
        uint32_t model;
        GrVec3 pos;
        float heading;
        uint64_t deadline_ms;
    };
    struct Spawned {
        uint32_t id;
        native::Entity handle;
        uint32_t model;
        uint32_t kind;  // GR_ENT_PED (any ped), GR_ENT_VEHICLE or GR_ENT_OBJECT
    };

    void Request(const GrEvent& e, uint64_t now_ms) noexcept {
        if (!native::IsModelInCdimage(e.model)) {
            GR_LOG("spawn: %u: no model %08X in the game", e.id, e.model);
            Result(e.id, 0, e.model, GR_SPAWN_UNKNOWN_MODEL);
            return;
        }
        if (pending_count_ == kMaxPending || spawned_count_ + pending_count_ >= kMaxSpawned) {
            Result(e.id, 0, e.model, GR_SPAWN_FULL);
            return;
        }
        // RDR2's heading 0 faces +Y and turns anticlockwise: forward is (-sin h, cos h).
        const float heading = std::atan2(-e.dir.x, e.dir.y) * 57.29578f;
        pending_[pending_count_++] = {e.id, e.model, e.pos, heading, now_ms + kLoadTimeoutMs};
        native::RequestModel(e.model);
    }

    void Create(const Pending& p) noexcept {
        GrVec3 min{}, max{};
        native::GetModelDimensions(p.model, min, max);
        // `pos` is the ground under GMod's crosshair: stand the model's bottom on it.
        const GrVec3 at{p.pos.x, p.pos.y, p.pos.z - min.z + 0.05f};
        native::Entity e = 0;
        uint32_t kind = GR_ENT_OBJECT;
        if (native::IsModelAPed(p.model)) {
            kind = GR_ENT_PED;
            e = native::CreatePed(p.model, at, p.heading);
            if (e != 0) native::SetRandomOutfitVariation(e);
        } else if (native::IsModelAVehicle(p.model)) {
            kind = GR_ENT_VEHICLE;
            // Side on to the player: facing it, a wagon's team walks into the player.
            e = native::CreateVehicle(p.model, at, p.heading + 90.0f);
            if (e != 0) native::SetVehicleOnGroundProperly(e);
        } else {
            e = native::CreateObject(p.model, at);
            if (e != 0) native::SetEntityHeading(e, p.heading);
        }
        native::SetModelAsNoLongerNeeded(p.model);
        if (e == 0) {
            GR_LOG("spawn: %u: the game made nothing of model %08X", p.id, p.model);
            Result(p.id, 0, p.model, GR_SPAWN_FAILED);
            return;
        }
        spawned_[spawned_count_++] = {p.id, e, p.model, kind};
        GR_LOG("spawn: %u: model %08X is %s %d at (%.1f, %.1f, %.1f)", p.id, p.model,
               kind == GR_ENT_PED ? "ped" : kind == GR_ENT_VEHICLE ? "vehicle" : "object", e, at.x, at.y, at.z);
        Result(p.id, e, p.model, GR_SPAWN_OK);
    }

    void Remove(const GrEvent& e) noexcept {
        if (e.flags & GR_EVENTF_ALL_SPAWNED) {
            GR_LOG("spawn: cleanup removes %u spawned entities", spawned_count_);
            while (spawned_count_ > 0) Delete(spawned_[spawned_count_ - 1].handle);
            pending_count_ = 0;
            return;
        }
        native::Entity handle = e.handle;
        if (handle == 0 && e.id != 0) {
            for (uint32_t i = 0; i < spawned_count_ && handle == 0; ++i) {
                if (spawned_[i].id == e.id) handle = spawned_[i].handle;
            }
            // Undone before it was made.
            for (uint32_t i = 0; i < pending_count_; ++i) {
                if (pending_[i].id == e.id) pending_[i--] = pending_[--pending_count_];
            }
        }
        if (handle == 0 || handle == native::PlayerPedId()) return;
        GR_LOG("spawn: removing %d%s", handle, IsSpawned(handle) ? " (spawned)" : "");
        Delete(handle);
    }

    // Deletes an entity and forgets it; a draft vehicle's horses go with it when they are
    // ours (made with the wagon).
    void Delete(native::Entity handle) noexcept {
        Spawned* s = Find(handle);
        const bool ours = s != nullptr;
        if (s) *s = spawned_[--spawned_count_];
        if (!native::DoesEntityExist(handle)) return;
        if (ours && native::IsEntityAVehicle(handle)) {
            for (int h = 0; h < kMaxHarnesses; ++h) {
                const native::Entity horse = native::GetPedInDraftHarness(handle, h);
                if (horse != 0 && native::DoesEntityExist(horse)) native::DeleteEntity(horse);
            }
        }
        native::DeleteEntity(handle);
    }

    void ClearWanted() noexcept {
        const int player = native::PlayerId();
        const int bounty = native::GetBounty(player);
        native::ClearWanted(player);
        GR_LOG("law: wanted and bounty cleared (the bounty was %d, now %d)", bounty, native::GetBounty(player));
    }

    void Result(uint32_t id, native::Entity handle, uint32_t model, uint32_t status) noexcept {
        const uint32_t seq = ++result_count_;
        results_[(seq - 1) % GR_MAX_SPAWN_RESULTS] = {seq, id, handle, model, status};
    }

    Spawned* Find(native::Entity handle) noexcept {
        for (uint32_t i = 0; i < spawned_count_; ++i) {
            if (spawned_[i].handle == handle) return &spawned_[i];
        }
        return nullptr;
    }
    const Spawned* Find(native::Entity handle) const noexcept { return const_cast<SpawnSync*>(this)->Find(handle); }

    Pending pending_[kMaxPending] = {};
    uint32_t pending_count_ = 0;
    Spawned spawned_[kMaxSpawned] = {};
    uint32_t spawned_count_ = 0;
    uint32_t prune_next_ = 0;
    GrSpawnResult results_[GR_MAX_SPAWN_RESULTS] = {};
    uint32_t result_count_ = 0;
    static_assert(sizeof(Spawned) == 16);
};

}  // namespace gr
