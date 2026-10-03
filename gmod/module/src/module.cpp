// The GMod end of the bridge as a Lua module: the Lua binding around gr::Guest (guest.h),
// which maps the shared memory as the guest. It sets GR.Native; the Lua addon
// (gmod/addon/lua/gr/bridge.lua) calls Tick() first and Publish() last in every frame.
//
// Lua speaks Source units and angles throughout. Numbers go in and come out as plain
// arguments and results, never tables: these run every frame and must not make garbage.

#include <GarrysMod/Lua/Interface.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "d3d9_capture.h"
#include "gr_link.h"
#include "gr_log.h"
#include "gr_protocol.h"
#include "guest.h"
#include "keep_active.h"
#include "overlay_window.h"
#include "window_fit.h"
#include "scene_probe.h"

namespace {

using GarrysMod::Lua::ILuaBase;
namespace LuaType = GarrysMod::Lua::Type;

gr::Guest g_guest;
gr::LinkState g_last_state = gr::LinkState::Closed;
// More than one Lua state can require the module (client and menu). The link belongs to
// the process, so only the first open and the last close touch it.
int g_open_count = 0;
// gr_keep_active, as last set from Lua. The engine is only kept active while this is set
// and RDR2 is connected: on its own, GMod should behave like GMod.
bool g_keep_active_wanted = true;

// The log goes next to the DLL (garrysmod/lua/bin), or to %TEMP% if that is not writable.
void StartLog() noexcept {
    wchar_t path[MAX_PATH + 32] = L"";
    wchar_t dir[MAX_PATH] = L"";
    HMODULE self = nullptr;
    bool logging = false;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&StartLog), &self)) {
        const DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
        wchar_t* slash = (n > 0 && n < MAX_PATH) ? std::wcsrchr(dir, L'\\') : nullptr;
        if (slash) {
            slash[1] = L'\0';
            std::swprintf(path, MAX_PATH + 32, L"%lsGarrysRedemption_gmod.log", dir);
            logging = gr::Log::Get().Start(path);
        }
    }
    if (!logging && GetTempPathW(MAX_PATH, dir)) {
        std::swprintf(path, MAX_PATH + 32, L"%lsGarrysRedemption_gmod.log", dir);
        gr::Log::Get().Start(path);
    }
}

void ApplyKeepActive() noexcept {
    gr::KeepActive& keep = gr::KeepActive::Get();
    const bool want = g_keep_active_wanted && g_guest.link().state() == gr::LinkState::Connected;
    if (want && !keep.installed() && !keep.Install()) {
        GR_LOG("keep-active: no Valve001 window on this thread. GMod will drop to 20 fps when it is not in front");
        return;
    }
    if (keep.keeping() != want) {
        keep.Keep(want);
        GR_LOG("keep-active: %s (%u deactivations swallowed so far)", want ? "on" : "off", keep.swallowed());
    }
}

void OnStateChange(gr::LinkState from, gr::LinkState to) noexcept {
    const GrPeer& peer = g_guest.link().peer();
    GR_LOG("link: %s -> %s", gr::LinkStateName(from), gr::LinkStateName(to));
    switch (to) {
        case gr::LinkState::Connected:
            GR_LOG("link: RDR2 plugin connected (pid %u, protocol %u)", peer.pid, peer.protocol_version);
            break;
        case gr::LinkState::VersionMismatch:
            GR_LOG("link: version mismatch. This module: protocol %u, %u bytes. RDR2 plugin: "
                   "protocol %u, %u bytes. Update both from the same release.",
                   GR_PROTOCOL_VERSION, GR_SHM_SIZE, peer.protocol_version, peer.shm_size);
            break;
        case gr::LinkState::PeerRefused:
            GR_LOG("link: RDR2 plugin refuses to run (%s)", gr::ReasonName(peer.reason));
            break;
        default:
            break;
    }
    ApplyKeepActive();
}

void SetNumber(ILuaBase* LUA, const char* key, double value) {
    LUA->PushNumber(value);
    LUA->SetField(-2, key);
}

void SetBool(ILuaBase* LUA, const char* key, bool value) {
    LUA->PushBool(value);
    LUA->SetField(-2, key);
}

void SetString(ILuaBase* LUA, const char* key, const char* value) {
    LUA->PushString(value);
    LUA->SetField(-2, key);
}

void SetFunction(ILuaBase* LUA, const char* key, GarrysMod::Lua::CFunc fn) {
    LUA->PushCFunction(fn);
    LUA->SetField(-2, key);
}

float ArgFloat(ILuaBase* LUA, int index) { return static_cast<float>(LUA->CheckNumber(index)); }

gr::SrcVec ArgVec(ILuaBase* LUA, int first) {
    return {ArgFloat(LUA, first), ArgFloat(LUA, first + 1), ArgFloat(LUA, first + 2)};
}

// GR.Native.Tick() -> state name. The first call of a frame: heartbeat, and RDR2's frame.
LUA_FUNCTION(Tick) {
    const gr::LinkState state = g_guest.Tick(gr::NowMs());
    gr::Log::Get().SetFrame(g_guest.frame());
    if (state != g_last_state) {
        OnStateChange(g_last_state, state);
        g_last_state = state;
    }
    if ((g_guest.frame() % 600) == 0 && state == gr::LinkState::Connected) {
        const GrHostFrame& host = g_guest.host();
        GR_VERBOSE("host frame %u is %d us old (game time %u ms, dt %.4f), %u frames read, %u torn", host.frame,
                   gr::AgeUs(gr::NowUs(), host.time_us), host.game_time_ms, host.dt, g_guest.host_frames_read(),
                   g_guest.link().torn_reads());
    }
    // GMod as a background task at RDR2's size (window_fit.h). The window is found by
    // keep-active once the link is up; before that there is nothing to fit to.
    {
        gr::KeepActive& keep = gr::KeepActive::Get();
        if (!keep.installed() && state == gr::LinkState::Connected) keep.Install();
        const GrHostFrame& host = g_guest.host();
        const bool fresh = state == gr::LinkState::Connected && host.frame != 0;
        const gr::D3D9Capture::Stats back = gr::D3D9Capture::Get().stats();
        gr::WindowFit::Get().Tick(keep.window(), state == gr::LinkState::Connected, fresh ? host.input.screen_w : 0,
                                  fresh ? host.input.screen_h : 0, back.width, back.height, gr::NowMs());
    }
    LUA->PushString(gr::LinkStateName(state));
    return 1;
}

// GR.Native.SetWeapon(hash): the RDR2 gun the player holds in first person, 0 for none.
// With six more numbers (x, y, z, pitch, yaw, roll) it is in the hand of the player's model.
LUA_FUNCTION(SetWeapon) {
    const uint32_t hash = static_cast<uint32_t>(static_cast<int64_t>(LUA->CheckNumber(1)));
    if (LUA->IsType(7, LuaType::Number)) {
        g_guest.SetWeaponAtHand(hash, ArgVec(LUA, 2), {ArgFloat(LUA, 5), ArgFloat(LUA, 6), ArgFloat(LUA, 7)});
    } else {
        g_guest.SetWeapon(hash);
    }
    return 0;
}

// GR.Native.SetMouseLook(yaw_per_count, pitch_per_count): how far one mouse count turns the
// view, in Source's degrees; 0, 0 while the mouse is not turning it (GrGuestFrame.look_*).
LUA_FUNCTION(SetMouseLook) {
    g_guest.SetMouseLook(ArgFloat(LUA, 1), ArgFloat(LUA, 2));
    return 0;
}

// GR.Native.SetWindowFit(hide, fit): hide GMod's window while RDR2 runs; keep GMod's
// resolution at RDR2's (window_fit.h).
LUA_FUNCTION(SetWindowFit) {
    gr::WindowFit::Get().SetWanted(LUA->GetBool(1), LUA->GetBool(2), !LUA->IsType(3, LuaType::Bool) || LUA->GetBool(3));
    return 0;
}

// GR.Native.Publish(). The last call of a frame: sends what SetPlayer was given.
LUA_FUNCTION(Publish) {
    (void)LUA;
    g_guest.Publish(gr::NowUs());
    return 0;
}

// GR.Native.State() -> state name as of the last Tick().
LUA_FUNCTION(State) {
    LUA->PushString(gr::LinkStateName(g_guest.link().state()));
    return 1;
}

// GR.Native.HostState() -> captured, rdr2_owns_player, paused, focused, arthur
LUA_FUNCTION(HostState) {
    const uint32_t flags = g_guest.host().flags;
    LUA->PushBool(g_guest.captured());
    LUA->PushBool((flags & GR_HOSTF_RDR2_OWNS_PLAYER) != 0);
    LUA->PushBool((flags & GR_HOSTF_PAUSED) != 0);
    LUA->PushBool((flags & GR_HOSTF_FOCUSED) != 0);
    LUA->PushBool((flags & GR_HOSTF_ARTHUR) != 0);
    return 5;
}

// GR.Native.Anchored() -> bool
LUA_FUNCTION(Anchored) {
    LUA->PushBool(g_guest.anchored());
    return 1;
}

// GR.Native.Anchor(x, y, z) -> ok, pitch, yaw, roll. Declares that the GMod player,
// standing at (x, y, z), is where RDR2's player ped is. The angles are the ped's, as
// Source angles. ok is false while RDR2 has not said where its ped is or owns the player.
LUA_FUNCTION(Anchor) {
    gr::SrcAngle angles{};
    const gr::SrcVec at = ArgVec(LUA, 1);
    const bool ok = g_guest.Anchor(at, angles);
    if (ok) {
        const gr::Origin& o = g_guest.origin();
        GR_LOG("anchor: GMod (%.1f, %.1f, %.1f) is RDR2 (%.2f, %.2f, %.2f), origin (%.3f, %.3f, %.3f), serial %u",
               at.x, at.y, at.z, g_guest.host().ped_pos.x, g_guest.host().ped_pos.y, g_guest.host().ped_pos.z, o.x,
               o.y, o.z, g_guest.host().anchor_serial);
    }
    LUA->PushBool(ok);
    LUA->PushNumber(angles.pitch);
    LUA->PushNumber(angles.yaw);
    LUA->PushNumber(angles.roll);
    return 4;
}

// GR.Native.SetPlayer(px, py, pz, vx, vy, vz, ex, ey, ez, pitch, yaw, roll, fov,
//                     on_ground, crouching, noclip, menu_open)
// Feet, velocity, eye position, eye angles and Player:GetFOV(), all as GMod has them.
LUA_FUNCTION(SetPlayer) {
    const gr::SrcAngle angles{ArgFloat(LUA, 10), ArgFloat(LUA, 11), ArgFloat(LUA, 12)};
    uint32_t flags = 0;
    if (LUA->GetBool(14)) flags |= GR_GUESTF_ON_GROUND;
    if (LUA->GetBool(15)) flags |= GR_GUESTF_CROUCHING;
    if (LUA->GetBool(16)) flags |= GR_GUESTF_NOCLIP;
    if (LUA->GetBool(17)) flags |= GR_GUESTF_MENU_OPEN;
    g_guest.SetPlayer(ArgVec(LUA, 1), ArgVec(LUA, 4), ArgVec(LUA, 7), angles, ArgFloat(LUA, 13), flags);
    return 0;
}

// GR.Native.NextKey() -> vk, down, or nothing when Lua has been told of every change.
// vk is a Windows virtual-key code (mouse buttons are 1, 2, 4, 5 and 6).
LUA_FUNCTION(NextKey) {
    int vk = 0;
    bool down = false;
    if (!g_guest.NextKey(vk, down)) return 0;
    LUA->PushNumber(vk);
    LUA->PushBool(down);
    return 2;
}

// GR.Native.Mouse() -> dx, dy, wheel since the last call: raw mouse counts (right and
// down are positive) and wheel movement in units of 120 per notch (up is positive).
LUA_FUNCTION(Mouse) {
    int dx = 0, dy = 0, wheel = 0;
    g_guest.TakeMouse(dx, dy, wheel);
    LUA->PushNumber(dx);
    LUA->PushNumber(dy);
    LUA->PushNumber(wheel);
    return 3;
}

// GR.Native.Cursor() -> x, y, width, height: RDR2's cursor in its window, and the window.
LUA_FUNCTION(Cursor) {
    const GrInput& input = g_guest.host().input;
    LUA->PushNumber(input.cursor_x);
    LUA->PushNumber(input.cursor_y);
    LUA->PushNumber(input.screen_w);
    LUA->PushNumber(input.screen_h);
    return 4;
}

// GR.Native.Stats() -> table. The fields are what gr_status prints.
LUA_FUNCTION(Stats) {
    const GrPeer& peer = g_guest.link().peer();
    const GrHostFrame& host = g_guest.host();
    const GrGuestFrame& guest = g_guest.guest();
    const gr::SrcVec ped = g_guest.ped_pos();
    LUA->CreateTable();
    SetString(LUA, "state", gr::LinkStateName(g_guest.link().state()));
    SetNumber(LUA, "protocol", GR_PROTOCOL_VERSION);
    SetNumber(LUA, "shm_size", GR_SHM_SIZE);
    SetNumber(LUA, "guest_frame", g_guest.frame());
    SetNumber(LUA, "host_frame", host.frame);
    SetNumber(LUA, "host_frames_read", g_guest.host_frames_read());
    SetNumber(LUA, "host_frame_age_ms", host.frame ? gr::AgeUs(gr::NowUs(), host.time_us) / 1000.0 : 0.0);
    SetNumber(LUA, "torn_reads", g_guest.link().torn_reads());
    SetNumber(LUA, "peer_pid", peer.pid);
    SetNumber(LUA, "peer_protocol", peer.protocol_version);
    SetNumber(LUA, "peer_shm_size", peer.shm_size);
    SetString(LUA, "peer_reason", gr::ReasonName(peer.state == GR_PEER_REFUSED ? peer.reason : GR_REASON_NONE));
    SetBool(LUA, "keep_active", gr::KeepActive::Get().keeping());
    SetBool(LUA, "engine_active", gr::KeepActive::Get().engine_active());
    SetNumber(LUA, "deactivations_swallowed", gr::KeepActive::Get().swallowed());
    SetBool(LUA, "anchored", g_guest.anchored());
    SetBool(LUA, "captured", g_guest.captured());
    SetBool(LUA, "rdr2_owns_player", (host.flags & GR_HOSTF_RDR2_OWNS_PLAYER) != 0);
    SetBool(LUA, "rdr2_paused", (host.flags & GR_HOSTF_PAUSED) != 0);
    SetBool(LUA, "rdr2_focused", (host.flags & GR_HOSTF_FOCUSED) != 0);
    SetNumber(LUA, "rdr2_x", guest.pos.x);
    SetNumber(LUA, "rdr2_y", guest.pos.y);
    SetNumber(LUA, "rdr2_z", guest.pos.z);
    SetNumber(LUA, "rdr2_pitch", guest.eye_rot.x);
    SetNumber(LUA, "rdr2_yaw", guest.eye_rot.z);
    SetNumber(LUA, "ped_x", ped.x);
    SetNumber(LUA, "ped_y", ped.y);
    SetNumber(LUA, "ped_z", ped.z);
    SetNumber(LUA, "mouse_x", host.input.mouse_dx);
    SetNumber(LUA, "mouse_y", host.input.mouse_dy);
    return 1;
}

// GR.Native.EntityCount() -> n: RDR2 peds and horses near the player, nearest first.
// 0 while GMod does not have the player.
LUA_FUNCTION(EntityCount) {
    LUA->PushNumber(g_guest.entity_count());
    return 1;
}

// GR.Native.Entity(i) -> handle, type, flags, model, x, y, z, pitch, yaw, roll, vx, vy, vz,
//     minx, miny, minz, maxx, maxy, maxz, health
// i from 1. Source units and angles; the box is in the entity's own space. type is
// GR_ENT_* (1 ped, 2 animal, 3 horse), flags GR_ENTF_* (1 dead, 2 ragdoll, 4 driven,
// 8 frozen). Nothing for an i out of range.
LUA_FUNCTION(Entity) {
    gr::Guest::SrcEntity e{};
    if (!g_guest.Entity(static_cast<uint32_t>(LUA->CheckNumber(1)) - 1, e)) return 0;
    LUA->PushNumber(e.handle);
    LUA->PushNumber(e.type);
    LUA->PushNumber(e.flags);
    LUA->PushNumber(e.model);
    LUA->PushNumber(e.pos.x);
    LUA->PushNumber(e.pos.y);
    LUA->PushNumber(e.pos.z);
    LUA->PushNumber(e.angles.pitch);
    LUA->PushNumber(e.angles.yaw);
    LUA->PushNumber(e.angles.roll);
    LUA->PushNumber(e.vel.x);
    LUA->PushNumber(e.vel.y);
    LUA->PushNumber(e.vel.z);
    LUA->PushNumber(e.mins.x);
    LUA->PushNumber(e.mins.y);
    LUA->PushNumber(e.mins.z);
    LUA->PushNumber(e.maxs.x);
    LUA->PushNumber(e.maxs.y);
    LUA->PushNumber(e.maxs.z);
    LUA->PushNumber(e.health);
    return 20;
}

// GR.Native.Block() -> serial, x, y, nx, ny: where RDR2's world last stopped the player
// (Source units) and the horizontal normal of what stopped it. serial changes each time
// it happens. Nothing before the first time, or while not anchored.
LUA_FUNCTION(Block) {
    uint32_t serial = 0;
    gr::SrcVec pos{};
    gr::SrcVec normal{};
    if (!g_guest.Block(serial, pos, normal)) return 0;
    LUA->PushNumber(serial);
    LUA->PushNumber(pos.x);
    LUA->PushNumber(pos.y);
    LUA->PushNumber(normal.x);
    LUA->PushNumber(normal.y);
    return 5;
}

// GR.Native.Light() -> dx, dy, dz, r, g, b, ar, ag, ab, flags: the direction towards
// RDR2's sun or moon (Source axes, not normalised), its linear colour at the player (0 in
// shade), the sky's ambient colour, and GR_LIGHTF_* flags. Nothing while not available.
LUA_FUNCTION(Light) {
    gr::SrcVec dir{};
    GrVec3 color{};
    GrVec3 ambient{};
    uint32_t flags = 0;
    if (!g_guest.Light(dir, color, ambient, flags)) return 0;
    LUA->PushNumber(dir.x);
    LUA->PushNumber(dir.y);
    LUA->PushNumber(dir.z);
    LUA->PushNumber(color.x);
    LUA->PushNumber(color.y);
    LUA->PushNumber(color.z);
    LUA->PushNumber(ambient.x);
    LUA->PushNumber(ambient.y);
    LUA->PushNumber(ambient.z);
    LUA->PushNumber(flags);
    return 10;
}

// GR.Native.Event(type, flags, handle, damage, radius, fx, fy, fz, px, py, pz, dx, dy, dz, force)
// -> ok. Queues a GR_EVENT_* for RDR2 (see GrEvent): type 1 bullet, 2 hit, 3 explosion.
// Positions in Source units, damage in RDR2 health points, radius in units, force in units
// per second along d. false while not anchored.
LUA_FUNCTION(Event) {
    const bool ok = g_guest.AddEvent(static_cast<uint32_t>(LUA->CheckNumber(1)), static_cast<uint32_t>(LUA->CheckNumber(2)),
                                     static_cast<int32_t>(LUA->CheckNumber(3)), ArgFloat(LUA, 4), ArgFloat(LUA, 5),
                                     ArgVec(LUA, 6), ArgVec(LUA, 9), ArgVec(LUA, 12), ArgFloat(LUA, 15),
                                     LUA->IsType(16, LuaType::Number)
                                         ? static_cast<uint32_t>(static_cast<int64_t>(LUA->GetNumber(16)))
                                         : 0u);
    LUA->PushBool(ok);
    return 1;
}

// GR.Native.Possess(handle, mx, my, jump): the RDR2 ped the GMod player drives (0: none) and
// the way it should go, a Source direction whose length is the pace (gr/possess.lua).
LUA_FUNCTION(Possess) {
    g_guest.SetPossess(static_cast<int32_t>(LUA->CheckNumber(1)), {ArgFloat(LUA, 2), ArgFloat(LUA, 3), 0.0f},
                       LUA->GetBool(4));
    return 0;
}

// GR.Native.Joaat(name) -> hash: the model hash RDR2 knows `name` by.
LUA_FUNCTION(Joaat) {
    LUA->PushNumber(gr::Joaat(LUA->CheckString(1)));
    return 1;
}

// GR.Native.Spawn(id, model, px, py, pz, fx, fy, fz) -> ok. Asks RDR2 for one entity of
// `model` (a hash) standing on p, facing along f (Source units and direction). id, never 0,
// names it for Remove and in SpawnResult.
LUA_FUNCTION(Spawn) {
    const double model = LUA->CheckNumber(2);
    const bool ok = g_guest.AddSpawn(static_cast<uint32_t>(LUA->CheckNumber(1)),
                                     static_cast<uint32_t>(static_cast<int64_t>(model)), ArgVec(LUA, 3), ArgVec(LUA, 6));
    LUA->PushBool(ok);
    return 1;
}

// GR.Native.Remove(handle, id, all) -> ok. Deletes RDR2 entity `handle`, or if 0 the one
// spawned as `id`, or with `all` true everything spawned.
LUA_FUNCTION(Remove) {
    const bool ok = g_guest.AddRemove(static_cast<int32_t>(LUA->CheckNumber(1)), static_cast<uint32_t>(LUA->CheckNumber(2)),
                                      LUA->GetBool(3) ? GR_EVENTF_ALL_SPAWNED : 0u);
    LUA->PushBool(ok);
    return 1;
}

// GR.Native.ClearWanted() -> ok. The law forgets the player's crimes and bounty.
// GR.Native.Teleport(x, y, z) -> ok: RDR2 moves the player to that place on its own map
// (RDR2 world metres). The anchor drops and is made again there.
LUA_FUNCTION(Teleport) {
    LUA->PushBool(g_guest.AddTeleport(ArgFloat(LUA, 1), ArgFloat(LUA, 2), ArgFloat(LUA, 3)));
    return 1;
}

LUA_FUNCTION(ClearWanted) {
    LUA->PushBool(g_guest.AddClearWanted());
    return 1;
}

// GR.Native.SpawnResultCount() -> seq of the newest spawn result (0: none yet).
LUA_FUNCTION(SpawnResultCount) {
    LUA->PushNumber(g_guest.spawn_result_count());
    return 1;
}

// GR.Native.SpawnResult(seq) -> id, handle, model, status, or nothing once it has left the
// ring. status: 1 ok, 2 unknown model, 3 timed out loading, 4 too many, 5 failed.
LUA_FUNCTION(SpawnResult) {
    GrSpawnResult r{};
    if (!g_guest.SpawnResult(static_cast<uint32_t>(LUA->CheckNumber(1)), r)) return 0;
    LUA->PushNumber(r.id);
    LUA->PushNumber(r.handle);
    LUA->PushNumber(r.model);
    LUA->PushNumber(r.status);
    return 4;
}

// GR.Native.TakePlayerHurt() -> n: GMod health points RDR2 took from the player's ped since
// the last call.
LUA_FUNCTION(TakePlayerHurt) {
    LUA->PushNumber(g_guest.TakePlayerHurt());
    return 1;
}

// GR.Native.TakeAnchorHealth() -> fraction: the health (0 to 1 of the maximum) the player should
// have since the last anchor, once (full at the start of a session, otherwise what RDR2's ped
// had); -1 when there is nothing new.
LUA_FUNCTION(TakeAnchorHealth) {
    LUA->PushNumber(g_guest.TakeAnchorHealth());
    return 1;
}

// GR.Native.SetPlayerHealth(fraction): the player's health over its maximum, for RDR2's ped
// when RDR2 takes the player. -1 = unknown.
LUA_FUNCTION(SetPlayerHealth) {
    g_guest.SetPlayerHealth(ArgFloat(LUA, 1));
    return 0;
}

// GR.Native.ClearConstraints(), then GR.Native.Constraint(id, type, a, b, ax, ay, az, bx, by,
// bz, length) -> ok for each constraint between RDR2 entities that exists (see GrConstraint;
// Source units and each entity's own axes, b 0 = the world and a world point). The list
// stands until cleared again and goes to RDR2 with every Publish.
LUA_FUNCTION(ClearConstraints) {
    (void)LUA;
    g_guest.ClearConstraints();
    return 0;
}

LUA_FUNCTION(Constraint) {
    const bool ok = g_guest.AddConstraint(
        static_cast<uint32_t>(LUA->CheckNumber(1)), static_cast<uint32_t>(LUA->CheckNumber(2)),
        static_cast<int32_t>(LUA->CheckNumber(3)), static_cast<int32_t>(LUA->CheckNumber(4)), ArgVec(LUA, 5),
        ArgVec(LUA, 8), ArgFloat(LUA, 11));
    LUA->PushBool(ok);
    return 1;
}

// GR.Native.HostView() -> x, y, z, pitch, yaw, roll, serial: the camera RDR2 is about to show
// (GrHostFrame.view_pos), or nothing. GR.Native.SetViewHold(frames): how many of its frames
// RDR2 waits before showing a view it published.
LUA_FUNCTION(HostView) {
    gr::SrcVec pos{};
    gr::SrcAngle angles{};
    uint32_t serial = 0;
    if (!g_guest.HostView(pos, angles, serial)) return 0;
    LUA->PushNumber(pos.x);
    LUA->PushNumber(pos.y);
    LUA->PushNumber(pos.z);
    LUA->PushNumber(angles.pitch);
    LUA->PushNumber(angles.yaw);
    LUA->PushNumber(angles.roll);
    LUA->PushNumber(serial);
    return 7;
}

LUA_FUNCTION(SetViewHold) {
    const double frames = LUA->CheckNumber(1);
    g_guest.SetViewHold(frames < 0 ? 0u : static_cast<uint32_t>(frames));
    return 0;
}

// GR.Native.ClearVeils(), GR.Native.Veil(id, x, y, z, radius) -> ok: lists one of GMod's own
// things for RDR2 to look for foliage in front of (GrVeilRequest). Source units.
LUA_FUNCTION(ClearVeils) {
    (void)LUA;
    g_guest.ClearVeils();
    return 0;
}

LUA_FUNCTION(Veil) {
    LUA->PushBool(g_guest.AddVeil(static_cast<uint32_t>(LUA->CheckNumber(1)), ArgVec(LUA, 2), ArgFloat(LUA, 5)));
    return 1;
}

// GR.Native.VeilMask(id) -> mask, or nothing if RDR2 has not looked at it yet.
LUA_FUNCTION(VeilMask) {
    uint32_t mask = 0;
    if (!g_guest.VeilMask(static_cast<uint32_t>(LUA->CheckNumber(1)), mask)) return 0;
    LUA->PushNumber(mask);
    return 1;
}

// GR.Native.ClearProbes(), GR.Native.Probe(id, fx, fy, fz, tx, ty, tz) -> ok: lists a stretch
// one of GMod's fast things is about to cover, for RDR2 to test against its world
// (GrProbeRequest). Source units.
LUA_FUNCTION(ClearProbes) {
    (void)LUA;
    g_guest.ClearProbes();
    return 0;
}

LUA_FUNCTION(Probe) {
    LUA->PushBool(g_guest.AddProbe(static_cast<uint32_t>(LUA->CheckNumber(1)), ArgVec(LUA, 2), ArgVec(LUA, 5)));
    return 1;
}

// GR.Native.ProbeHit(id) -> flags, x, y, z, nx, ny, nz: the first of RDR2's surfaces on that
// stretch (GR_PROBEF_* flags, Source units, unit normal), or nothing.
LUA_FUNCTION(ProbeHit) {
    uint32_t flags = 0;
    gr::SrcVec pos{};
    GrVec3 normal{};
    if (!g_guest.ProbeHit(static_cast<uint32_t>(LUA->CheckNumber(1)), flags, pos, normal)) return 0;
    LUA->PushNumber(flags);
    LUA->PushNumber(pos.x);
    LUA->PushNumber(pos.y);
    LUA->PushNumber(pos.z);
    LUA->PushNumber(normal.x);
    LUA->PushNumber(normal.y);
    LUA->PushNumber(normal.z);
    return 7;
}

// GR.Native.GroundZ(x, y) -> z of RDR2's ground there in Source units, or nothing.
LUA_FUNCTION(GroundZ) {
    float z = 0.0f;
    const gr::SrcVec p{static_cast<float>(LUA->CheckNumber(1)), static_cast<float>(LUA->CheckNumber(2)), 0.0f};
    if (!g_guest.GroundZ(p, z)) return 0;
    LUA->PushNumber(z);
    return 1;
}

// GR.Native.SceneProbe(on): starts or stops measuring RDR2's picture (scene_probe.h).
// GR.Native.SceneColor() -> r, g, b linear, or nothing without a fresh measurement.
LUA_FUNCTION(SceneProbe) {
    LUA->CheckType(1, LuaType::Bool);
    if (LUA->GetBool(1)) {
        gr::SceneProbe::Get().Start();
    } else {
        gr::SceneProbe::Get().Stop();
    }
    return 0;
}

LUA_FUNCTION(SceneColor) {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (!gr::SceneProbe::Get().Color(r, g, b)) return 0;
    LUA->PushNumber(r);
    LUA->PushNumber(g);
    LUA->PushNumber(b);
    return 3;
}

// GR.Native.CanAnchor() -> bool: whether Anchor would succeed now.
LUA_FUNCTION(CanAnchor) {
    LUA->PushBool(!g_guest.anchored() && g_guest.can_anchor());
    return 1;
}

// GR.Native.Unanchor(): lets go of the anchor. RDR2 shows its ped until the next Anchor.
LUA_FUNCTION(Unanchor) {
    (void)LUA;
    if (g_guest.anchored()) GR_LOG("anchor: dropped by Lua");
    g_guest.Unanchor();
    return 0;
}

// GR.Native.Rebase(dx, dy, dz, px, py, pz): floating origin. What was at (dx, dy, dz) is
// at (0,0,0) from now on; the caller moves the player (which was at p) and everything GMod
// simulates by -d in the same tick.
LUA_FUNCTION(Rebase) {
    const gr::SrcVec d = ArgVec(LUA, 1);
    g_guest.Rebase(d, ArgVec(LUA, 4));
    const gr::Origin& o = g_guest.origin();
    GR_LOG("origin: rebased by (%.0f, %.0f, %.0f) units, origin now (%.3f, %.3f, %.3f)", d.x, d.y, d.z, o.x, o.y, o.z);
    return 0;
}

// GR.Native.OriginSerial() -> n: changes whenever Source (0,0,0) is moved to another RDR2
// position (anchor, rebase). Anything placed from RDR2 positions must be placed again.
LUA_FUNCTION(OriginSerial) {
    LUA->PushNumber(g_guest.origin_serial());
    return 1;
}

// GR.Native.SetTerrain(bool): whether GMod stands its player on the terrain chunks. Tells
// RDR2 to sample them and to stop draping GMod's flat ground over its own.
LUA_FUNCTION(SetTerrain) {
    LUA->CheckType(1, LuaType::Bool);
    const bool on = LUA->GetBool(1);
    if (on != g_guest.terrain()) GR_LOG("terrain: %s", on ? "on" : "off");
    g_guest.SetTerrain(on);
    return 0;
}

// GR.Native.Terrain() -> bool: what SetTerrain was last given (the server realm sets it,
// the client reads it: one module image serves both).
LUA_FUNCTION(Terrain) {
    LUA->PushBool(g_guest.terrain());
    return 1;
}

// GR.Native.ChunkSerial(slot) -> serial, slot from 1. 0 for an empty slot or while not
// connected. A change means the slot has new content.
LUA_FUNCTION(ChunkSerial) {
    const double slot = LUA->CheckNumber(1);
    LUA->PushNumber(slot >= 1 && slot <= GR_MAX_CHUNKS ? g_guest.ChunkSerial(static_cast<uint32_t>(slot) - 1) : 0);
    return 1;
}

// GR.Native.ChunkMesh(slot) -> serial, cx, cy, base_z, holes, walls, x, y, z, mins, maxs, vertices
// The slot's chunk as a triangle soup for Entity:PhysicsFromMesh: a table of Vectors, three
// per triangle, relative to the chunk's corner, which is at (x, y, z) in Source units under
// the current origin. base_z is the corner's RDR2 height (for ChunkPos); mins and maxs
// (Vectors) bound the vertices. Cells with a corner where no ground was found lose the
// triangles touching it. Each wall is two more triangles standing up from the ground.
// Nothing if the slot is empty or the copy was torn.
LUA_FUNCTION(ChunkMesh) {
    const double slot = LUA->CheckNumber(1);
    if (slot < 1 || slot > GR_MAX_CHUNKS || !g_guest.ReadChunk(static_cast<uint32_t>(slot) - 1)) return 0;
    const GrChunk& c = g_guest.chunk();
    const gr::SrcVec at = g_guest.ChunkPos(c.cx, c.cy, c.base_z);
    const float cell = static_cast<float>(GR_CHUNK_CELL_CM / 100.0 * gr::kUnitsPerMetre);
    LUA->PushNumber(c.serial);
    LUA->PushNumber(c.cx);
    LUA->PushNumber(c.cy);
    LUA->PushNumber(c.base_z);
    LUA->PushNumber(c.holes);
    const uint32_t walls = std::min(c.wall_count, GR_MAX_CHUNK_WALLS);
    LUA->PushNumber(walls);
    LUA->PushNumber(at.x);
    LUA->PushNumber(at.y);
    LUA->PushNumber(at.z);

    Vector lo;
    Vector hi;
    lo.x = lo.y = lo.z = 1e30f;
    hi.x = hi.y = hi.z = -1e30f;
    const auto grow = [&](const Vector& v) {
        lo.x = std::min(lo.x, v.x);
        lo.y = std::min(lo.y, v.y);
        lo.z = std::min(lo.z, v.z);
        hi.x = std::max(hi.x, v.x);
        hi.y = std::max(hi.y, v.y);
        hi.z = std::max(hi.z, v.z);
    };
    const auto valid = [&c](uint32_t i, uint32_t j) { return c.heights[j * GR_CHUNK_VERTS + i] > GR_CHUNK_NO_GROUND_BELOW; };
    const auto vertex = [&](uint32_t i, uint32_t j) {
        Vector v;
        v.x = static_cast<float>(i) * cell;
        v.y = static_cast<float>(j) * cell;
        v.z = static_cast<float>((c.heights[j * GR_CHUNK_VERTS + i] - c.base_z) * gr::kUnitsPerMetre);
        return v;
    };
    // RDR2 world metres to the chunk's own Source space.
    const double corner_x = c.cx * (GR_CHUNK_CELLS * GR_CHUNK_CELL_CM / 100.0);
    const double corner_y = c.cy * (GR_CHUNK_CELLS * GR_CHUNK_CELL_CM / 100.0);
    const auto local = [&](float x, float y, float z) {
        Vector v;
        v.x = static_cast<float>((x - corner_x) * gr::kUnitsPerMetre);
        v.y = static_cast<float>((y - corner_y) * gr::kUnitsPerMetre);
        v.z = static_cast<float>((z - c.base_z) * gr::kUnitsPerMetre);
        return v;
    };
    // Built on the stack below the table, then pushed in: three vertices per call.
    int n = 0;
    LUA->CreateTable();
    const auto push = [&](const Vector& a, const Vector& b, const Vector& d) {
        for (const Vector* v : {&a, &b, &d}) {
            grow(*v);
            LUA->PushNumber(++n);
            LUA->PushVector(*v);
            LUA->SetTable(-3);
        }
    };
    const auto tri = [&](uint32_t ai, uint32_t aj, uint32_t bi, uint32_t bj, uint32_t ci, uint32_t cj) {
        push(vertex(ai, aj), vertex(bi, bj), vertex(ci, cj));
    };
    for (uint32_t j = 0; j < GR_CHUNK_CELLS; ++j) {
        for (uint32_t i = 0; i < GR_CHUNK_CELLS; ++i) {
            const bool a = valid(i, j), b = valid(i + 1, j), cc = valid(i, j + 1), d = valid(i + 1, j + 1);
            // Split along the a-d diagonal; with one corner missing, the triangle of the
            // other three.
            if (a && b && d) tri(i, j, i + 1, j, i + 1, j + 1);
            if (a && d && cc) tri(i, j, i + 1, j + 1, i, j + 1);
            if (!a && b && cc && d) tri(i + 1, j, i + 1, j + 1, i, j + 1);
            if (!d && a && b && cc) tri(i, j, i + 1, j, i, j + 1);
        }
    }
    for (uint32_t k = 0; k < walls; ++k) {
        const GrWall& w = c.walls[k];
        // Along the wall: (-ny, nx) is "left".
        const float lx = w.x - w.ny * w.left;
        const float ly = w.y + w.nx * w.left;
        const float rx = w.x + w.ny * w.right;
        const float ry = w.y - w.nx * w.right;
        const Vector a = local(lx, ly, w.z0);
        const Vector b = local(rx, ry, w.z0);
        const Vector t = local(rx, ry, w.z1);
        const Vector u = local(lx, ly, w.z1);
        push(a, b, t);
        push(a, t, u);
    }
    if (n == 0) lo = hi = Vector();
    LUA->PushVector(lo);
    LUA->Insert(-2);  // the bounds before the table
    LUA->PushVector(hi);
    LUA->Insert(-2);
    return 12;
}

// GR.Native.Near() -> n: how many of RDR2's near panels (GrHostFrame.nearby) there are; 0 while
// not anchored.
LUA_FUNCTION(Near) {
    LUA->PushNumber(g_guest.anchored() ? std::min(g_guest.host().nearby_count, GR_MAX_NEAR) : 0);
    return 1;
}

// GR.Native.NearPanel(i) -> x, y, z, nx, ny, half_width, half_height: panel i (1-based) as a
// face at (x, y, z) (its middle, Source units under the current origin) turned to (nx, ny),
// half_width along it and half_height up and down, in units. Nothing if there is no such panel.
LUA_FUNCTION(NearPanel) {
    const double i = LUA->CheckNumber(1);
    const GrHostFrame& h = g_guest.host();
    if (!g_guest.anchored() || i < 1 || i > std::min(h.nearby_count, GR_MAX_NEAR)) return 0;
    const GrWall& w = h.nearby[static_cast<uint32_t>(i) - 1];
    const gr::SrcVec at = gr::RdrPosToSource({w.x, w.y, (w.z0 + w.z1) * 0.5f}, g_guest.origin());
    LUA->PushNumber(at.x);
    LUA->PushNumber(at.y);
    LUA->PushNumber(at.z);
    LUA->PushNumber(w.nx);
    LUA->PushNumber(w.ny);
    LUA->PushNumber((w.left + w.right) * 0.5 * gr::kUnitsPerMetre);
    LUA->PushNumber((w.z1 - w.z0) * 0.5 * gr::kUnitsPerMetre);
    return 7;
}

// GR.Native.ChunkPos(cx, cy, base_z) -> x, y, z: where that chunk's corner is now, in
// Source units (after the origin moved).
LUA_FUNCTION(ChunkPos) {
    const gr::SrcVec at = g_guest.ChunkPos(static_cast<int32_t>(LUA->CheckNumber(1)),
                                           static_cast<int32_t>(LUA->CheckNumber(2)), ArgFloat(LUA, 3));
    LUA->PushNumber(at.x);
    LUA->PushNumber(at.y);
    LUA->PushNumber(at.z);
    return 3;
}

// GR.Native.ChunkOf(x, y) -> cx, cy: the chunk a Source position is in.
LUA_FUNCTION(ChunkOf) {
    int32_t cx = 0, cy = 0;
    g_guest.ChunkOf({ArgFloat(LUA, 1), ArgFloat(LUA, 2), 0.0f}, cx, cy);
    LUA->PushNumber(cx);
    LUA->PushNumber(cy);
    return 2;
}

// GR.Native.ClearDriven(), then GR.Native.Drive(handle, flags, x, y, z, pitch, yaw, roll,
// vx, vy, vz) -> ok for each proxy GMod is simulating. flags: 1 held, 2 frozen. The list
// stands until cleared again, and goes to RDR2 with every Publish.
LUA_FUNCTION(ClearDriven) {
    (void)LUA;
    g_guest.ClearDriven();
    return 0;
}

LUA_FUNCTION(Drive) {
    const bool ok = g_guest.AddDriven(static_cast<int32_t>(LUA->CheckNumber(1)), static_cast<uint32_t>(LUA->CheckNumber(2)),
                                      ArgVec(LUA, 3), {ArgFloat(LUA, 6), ArgFloat(LUA, 7), ArgFloat(LUA, 8)},
                                      ArgVec(LUA, 9));
    LUA->PushBool(ok);
    return 1;
}

// GR.Native.Overlay(capture, shown, alpha): capture = read GMod's frame back at every
// Present, shown = put it over RDR2 (only while RDR2 is in front), alpha = how the frame's
// alpha is read (gr::OverlayAlpha: 0 raw, 1 squared, 2 black key, 3 opaque, 4 coverage).
void InstallOverlay() noexcept;
uint64_t g_next_overlay_try_ms = 0;

LUA_FUNCTION(Overlay) {
    if (LUA->GetBool(1) && !gr::D3D9Capture::Get().installed() && gr::NowMs() >= g_next_overlay_try_ms) {
        g_next_overlay_try_ms = gr::NowMs() + 2000;
        InstallOverlay();
    }
    gr::D3D9Capture::Get().SetEnabled(LUA->GetBool(1));
    // Works around: RDR2's pause menu and map stop its script thread, and until the link timed
    // out two seconds later GMod's HUD and gun lay over RDR2's menu (user report). A host
    // frame older than a few frames means RDR2 is showing something of its own.
    const bool fresh = gr::AgeUs(gr::NowUs(), g_guest.host().time_us) < 150000;
    gr::OverlayWindow::Get().SetWanted(LUA->GetBool(2) && fresh);
    if (LUA->IsType(3, LuaType::Number)) {
        gr::OverlayWindow::Get().SetAlpha(static_cast<gr::OverlayAlpha>(std::min(static_cast<uint32_t>(LUA->GetNumber(3)), 4u)));
    }
    return 0;
}

// GR.Native.OverlayStats() -> table, for gr_overlay.
LUA_FUNCTION(OverlayStats) {
    const gr::D3D9Capture::Stats c = gr::D3D9Capture::Get().stats();
    const gr::OverlayWindow::Stats w = gr::OverlayWindow::Get().stats();
    LUA->CreateTable();
    SetBool(LUA, "hooked", gr::D3D9Capture::Get().installed());
    SetBool(LUA, "capturing", gr::D3D9Capture::Get().enabled());
    SetNumber(LUA, "presents", c.presents);
    SetNumber(LUA, "captures", c.captures);
    SetNumber(LUA, "failures", c.failures);
    SetNumber(LUA, "width", c.width);
    SetNumber(LUA, "height", c.height);
    SetNumber(LUA, "format", c.format);
    SetNumber(LUA, "multisample", c.multisample);
    SetNumber(LUA, "capture_us", c.last_capture_us);
    SetNumber(LUA, "skipped", c.skipped);
    SetNumber(LUA, "present_us", c.present_us);
    SetBool(LUA, "window", w.running);
    SetBool(LUA, "shown", w.shown);
    SetNumber(LUA, "frames", w.frames);
    SetNumber(LUA, "upload_us", w.last_upload_us);
    SetNumber(LUA, "age_us", w.last_age_us);
    SetNumber(LUA, "avg_age_us", w.avg_age_us);
    SetNumber(LUA, "x", w.x);
    SetNumber(LUA, "y", w.y);
    SetNumber(LUA, "w", w.w);
    SetNumber(LUA, "h", w.h);
    SetNumber(LUA, "alpha", w.alpha);
    SetBool(LUA, "window_hidden", gr::WindowFit::Get().hidden());
    return 1;
}

// GR.Native.GpuPriority(level) -> NTSTATUS: GMod's GPU scheduling class, 2 normal, 3 above
// normal, 4 high (D3DKMT_SCHEDULINGPRIORITYCLASS). Works around: GMod's tiny overlay frame
// waited behind RDR2's heavy one on the shared GPU before its copy was done, which was most
// of the overlay's delay. Raising GMod's class lets its little work go first.
LUA_FUNCTION(GpuPriority) {
    using SetClassFn = LONG(APIENTRY*)(HANDLE, int);
    static const auto set_class = reinterpret_cast<SetClassFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass")));
    const int level = std::clamp(static_cast<int>(LUA->CheckNumber(1)), 0, 4);
    const LONG status = set_class ? set_class(GetCurrentProcess(), level) : -1;
    GR_LOG("overlay: GPU scheduling class %d (status 0x%08lx)", level, static_cast<unsigned long>(status));
    LUA->PushNumber(static_cast<double>(status));
    return 1;
}

// GR.Native.EngineSetting(name, value) -> ok. Sets one of a few engine convars that Lua may
// not set (RunConsoleCommand: "Command is blocked"), by handing GMod's own window the
// command as WM_COPYDATA, the way Hammer and tools/gmod_console.py send console commands.
// Only the names below and plain numbers, so no other Lua can use it to run anything else.
// Works around: GMod's sound cut off (snd_mute_losefocus, as GMod is never really in front)
// and lagged (snd_mixahead 0.1) behind RDR2 (gr/sounds.lua).
LUA_FUNCTION(EngineSetting) {
    // fps_max: the view lock (gr/player.lua) wants GMod's frames short. Up to 400.
    static const char* const kAllowed[] = {"snd_mute_losefocus", "snd_mixahead", "fps_max"};
    const bool fps = std::strcmp(LUA->CheckString(1), "fps_max") == 0;
    const char* name = LUA->CheckString(1);
    const double value = LUA->CheckNumber(2);
    bool allowed = false;
    for (const char* a : kAllowed) allowed = allowed || std::strcmp(a, name) == 0;
    const HWND window = gr::KeepActive::Get().window();
    if (!allowed || !window || value < 0.0 || value > (fps ? 400.0 : 1.0) || (fps && value < 30.0)) {
        LUA->PushBool(false);
        return 1;
    }
    char command[96];
    std::snprintf(command, sizeof(command), "%s %.3f", name, value);
    COPYDATASTRUCT data{0, static_cast<DWORD>(std::strlen(command) + 1), command};
    SendMessageA(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
    LUA->PushBool(true);
    return 1;
}

// GR.Native.PostButton(vk, down) -> ok. Hands GMod's own window a key or mouse button as
// the window message Windows would have sent it, so the engine's own key path sees it.
// Works around: the weapon selection HUD takes its confirming click from the engine's key
// events, not from the user command the addon builds, so a weapon highlighted with the
// wheel or a number key was never picked (gr/input.lua).
LUA_FUNCTION(PostButton) {
    const UINT vk = static_cast<UINT>(LUA->CheckNumber(1));
    LUA->CheckType(2, LuaType::Bool);
    const bool down = LUA->GetBool(2);
    const HWND window = gr::KeepActive::Get().window();
    bool ok = false;
    if (window && vk > 0 && vk < 256) {
        switch (vk) {
            case VK_LBUTTON: ok = PostMessageW(window, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0, 0); break;
            case VK_RBUTTON: ok = PostMessageW(window, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? MK_RBUTTON : 0, 0); break;
            case VK_MBUTTON: ok = PostMessageW(window, down ? WM_MBUTTONDOWN : WM_MBUTTONUP, down ? MK_MBUTTON : 0, 0); break;
            default: {
                // Source reads the scan code out of lParam, not the virtual key.
                const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
                ok = PostMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, vk,
                                  down ? (scan | 1) : (scan | 1 | (LPARAM{3} << 30)));
            }
        }
    }
    LUA->PushBool(ok);
    return 1;
}

// GR.Native.OverlayDump(path): the next captured frame also goes to `path`, raw: four
// uint32 (width, height, D3DFORMAT, 0), then B G R A rows. tools/overlay_dump.py reads it.
LUA_FUNCTION(OverlayDump) {
    gr::D3D9Capture::Get().RequestDump(LUA->CheckString(1));
    return 0;
}

// GR.Native.SetVerbose(bool)
LUA_FUNCTION(SetVerbose) {
    LUA->CheckType(1, LuaType::Bool);
    gr::Log::Get().SetVerbose(LUA->GetBool(1));
    return 0;
}

// GR.Native.SetKeepActive(bool): whether GMod is kept at full speed while RDR2 is
// connected and in front.
LUA_FUNCTION(SetKeepActive) {
    LUA->CheckType(1, LuaType::Bool);
    g_keep_active_wanted = LUA->GetBool(1);
    ApplyKeepActive();
    return 0;
}

// GMod finds a server-realm module by the name gmsv_gr_win64.dll and a client one by
// gmcl_gr_win64.dll, and loads two files as two separate images. The proxies live in the
// server realm (the physgun acts there) and the link in the client, so the server copy
// (the same build under the other name) does nothing itself: it loads the client file and
// hands the server's Lua state to it. One image, one link, one origin, serving both
// realms. In single player both realms run on the game's main thread (logged at open),
// which is what makes sharing g_guest without a lock sound.
HMODULE g_forward_to = nullptr;

HMODULE ClientImageIfServerCopy() noexcept {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH] = L"";
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ClientImageIfServerCopy), &self)) {
        return nullptr;
    }
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* name = (n > 0 && n < MAX_PATH) ? std::wcsrchr(path, L'\\') : nullptr;
    if (!name || _wcsnicmp(name + 1, L"gmsv_", 5) != 0) return nullptr;
    std::wmemcpy(name + 1, L"gmcl_", 5);
    return LoadLibraryW(path);
}

// Tried at every open until it works: the server realm opens before the game's window and
// device are fully up.
void InstallOverlay() noexcept {
    if (gr::D3D9Capture::Get().installed()) return;
    if (gr::D3D9Capture::Get().Install()) {
        GR_LOG("overlay: Direct3D 9 Present hooked");
        gr::OverlayWindow::Get().SetDumpSource(
            [](char* path, size_t chars) { return gr::D3D9Capture::Get().TakeDumpRequest(path, chars); });
        gr::OverlayWindow::Get().Start(&gr::D3D9Capture::Get().mailbox(), &gr::D3D9Capture::Get().share());
    } else {
        GR_LOG("overlay: could not hook Direct3D 9 Present. No HUD over RDR2");
    }
}

int Open(ILuaBase* LUA) {
    if (g_open_count++ == 0) {
        StartLog();
        GR_LOG("Garry's Redemption GMod module: protocol %u, shared memory %u bytes", GR_PROTOCOL_VERSION,
               GR_SHM_SIZE);
        g_last_state = gr::LinkState::Closed;
        if (g_guest.Open(gr::NowMs())) {
            GR_LOG("link: shared memory %s", g_guest.link().created() ? "created" : "already existed, opened");
        } else {
            GR_LOG("link: could not create the shared memory (error %lu), will keep retrying",
                   g_guest.link().last_error());
        }
    }
    InstallOverlay();

    // _G.GR.Native = { ... }. The addon makes GR before it requires the module, but do not
    // depend on that.
    LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    LUA->GetField(-1, "GR");
    if (!LUA->IsType(-1, LuaType::Table)) {
        LUA->Pop();
        LUA->CreateTable();
        LUA->Push(-1);
        LUA->SetField(-3, "GR");
    }
    LUA->CreateTable();
    SetFunction(LUA, "Tick", Tick);
    SetFunction(LUA, "Publish", Publish);
    SetFunction(LUA, "State", State);
    SetFunction(LUA, "ClearProbes", ClearProbes);
    SetFunction(LUA, "Probe", Probe);
    SetFunction(LUA, "ProbeHit", ProbeHit);
    SetFunction(LUA, "HostState", HostState);
    SetFunction(LUA, "Anchored", Anchored);
    SetFunction(LUA, "Anchor", Anchor);
    SetFunction(LUA, "SetPlayer", SetPlayer);
    SetFunction(LUA, "NextKey", NextKey);
    SetFunction(LUA, "Mouse", Mouse);
    SetFunction(LUA, "Cursor", Cursor);
    SetFunction(LUA, "Stats", Stats);
    SetFunction(LUA, "SetVerbose", SetVerbose);
    SetFunction(LUA, "SetKeepActive", SetKeepActive);
    SetFunction(LUA, "EntityCount", EntityCount);
    SetFunction(LUA, "Entity", Entity);
    SetFunction(LUA, "ClearDriven", ClearDriven);
    SetFunction(LUA, "Drive", Drive);
    SetFunction(LUA, "Block", Block);
    SetFunction(LUA, "Light", Light);
    SetFunction(LUA, "Event", Event);
    SetFunction(LUA, "TakePlayerHurt", TakePlayerHurt);
    SetFunction(LUA, "TakeAnchorHealth", TakeAnchorHealth);
    SetFunction(LUA, "Teleport", Teleport);
    SetFunction(LUA, "Near", Near);
    SetFunction(LUA, "NearPanel", NearPanel);
    SetFunction(LUA, "SetPlayerHealth", SetPlayerHealth);
    SetFunction(LUA, "Joaat", Joaat);
    SetFunction(LUA, "Spawn", Spawn);
    SetFunction(LUA, "Remove", Remove);
    SetFunction(LUA, "ClearWanted", ClearWanted);
    SetFunction(LUA, "SpawnResultCount", SpawnResultCount);
    SetFunction(LUA, "SpawnResult", SpawnResult);
    SetFunction(LUA, "ClearConstraints", ClearConstraints);
    SetFunction(LUA, "Constraint", Constraint);
    SetFunction(LUA, "CanAnchor", CanAnchor);
    SetFunction(LUA, "Unanchor", Unanchor);
    SetFunction(LUA, "Rebase", Rebase);
    SetFunction(LUA, "OriginSerial", OriginSerial);
    SetFunction(LUA, "SetTerrain", SetTerrain);
    SetFunction(LUA, "Terrain", Terrain);
    SetFunction(LUA, "ChunkSerial", ChunkSerial);
    SetFunction(LUA, "ChunkMesh", ChunkMesh);
    SetFunction(LUA, "ChunkPos", ChunkPos);
    SetFunction(LUA, "ChunkOf", ChunkOf);
    SetFunction(LUA, "Overlay", Overlay);
    SetFunction(LUA, "OverlayStats", OverlayStats);
    SetFunction(LUA, "OverlayDump", OverlayDump);
    SetFunction(LUA, "GpuPriority", GpuPriority);
    SetFunction(LUA, "EngineSetting", EngineSetting);
    SetFunction(LUA, "PostButton", PostButton);
    SetFunction(LUA, "HostView", HostView);
    SetFunction(LUA, "SetViewHold", SetViewHold);
    SetFunction(LUA, "Possess", Possess);
    SetFunction(LUA, "SetWeapon", SetWeapon);
    SetFunction(LUA, "SetMouseLook", SetMouseLook);
    SetFunction(LUA, "SetWindowFit", SetWindowFit);
    SetFunction(LUA, "ClearVeils", ClearVeils);
    SetFunction(LUA, "Veil", Veil);
    SetFunction(LUA, "VeilMask", VeilMask);
    SetFunction(LUA, "GroundZ", GroundZ);
    SetFunction(LUA, "SceneProbe", SceneProbe);
    SetFunction(LUA, "SceneColor", SceneColor);
    LUA->SetField(-2, "Native");
    LUA->Pop(2);

    LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    LUA->GetField(-1, "SERVER");
    const bool server = LUA->GetBool(-1);
    LUA->Pop(2);
    GR_LOG("opened in the %s realm on thread %lu (%d Lua states)", server ? "server" : "client/menu",
           GetCurrentThreadId(), g_open_count);
    return 0;
}

int Close(ILuaBase* LUA) {
    (void)LUA;
    if (--g_open_count == 0) {
        // Marks this side GONE, so the RDR2 plugin drops to "waiting" at once instead of
        // after the heartbeat timeout.
        g_guest.Close();
        gr::SceneProbe::Get().Stop();
        if (!gr::KeepActive::Get().Remove()) {
            GR_LOG("keep-active: another mod subclassed the game window after this one. Left in place as a "
                   "pass-through, and this DLL stays loaded until GMod exits");
        }
        GR_LOG("module closing");
        gr::Log::Get().Stop();
    }
    return 0;
}

}  // namespace

// Written out instead of GMOD_MODULE_OPEN/CLOSE so the server copy can pass the raw
// lua_State on to the client image's entry points.
GMOD_DLL_EXPORT int gmod13_open(lua_State* L) {
    if (!g_forward_to) g_forward_to = ClientImageIfServerCopy();
    if (g_forward_to) {
        using Entry = int (*)(lua_State*);
        const auto open = reinterpret_cast<Entry>(GetProcAddress(g_forward_to, "gmod13_open"));
        if (open && reinterpret_cast<void*>(open) != reinterpret_cast<void*>(&gmod13_open)) return open(L);
    }
    return Open(L->luabase);
}

GMOD_DLL_EXPORT int gmod13_close(lua_State* L) {
    if (g_forward_to) {
        using Entry = int (*)(lua_State*);
        const auto close = reinterpret_cast<Entry>(GetProcAddress(g_forward_to, "gmod13_close"));
        const int result = close ? close(L) : 0;
        FreeLibrary(g_forward_to);
        g_forward_to = nullptr;
        return result;
    }
    return Close(L->luabase);
}
