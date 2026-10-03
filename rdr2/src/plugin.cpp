// The RDR2 end of the bridge. Every frame: shake hands with GMod, capture input for it,
// put the player ped and the camera where the GMod player is, publish RDR2's frame, and
// say what is going on in the log and in one line of on-screen text.

#include "plugin.h"

#include <cstdio>
#include <cwchar>
#include <cstring>

#include "gr_link.h"
#include "gr_log.h"
#include "combat_sync.h"
#include "constraint_sync.h"
#include "possess_sync.h"
#include "weapon_view.h"
#include "veil_sync.h"
#include "gr_protocol.h"
#include "input_capture.h"
#include "light_sync.h"
#include "near_sync.h"
#include "probe_sync.h"
#include "natives.h"
#include "persist.h"
#include "player_sync.h"
#include "spawn_sync.h"
#include "terrain_sync.h"
#include "world_sync.h"

namespace gr::plugin {
namespace {

HMODULE g_module = nullptr;
Link g_link;
LinkState g_last_state = LinkState::Closed;
bool g_multiplayer = false;
bool g_status_text = true;
bool g_smooth = true;
bool g_follow_ground = true;
bool g_collide = true;
bool g_hide_hud = true;
constexpr int kPedFollowCm = 0;
constexpr int kCamMode = 1;  // PlayerSync::SetCamMode
// Off unless the ini asks: every way of changing RDR2's window broke something (see
// KeepComposited). The supported way is the driver's present method, README "Play".
bool g_keep_composited = false;
bool g_hud_hidden = false;
bool g_handover_down = false;
uint64_t g_next_window_search_ms = 0;
PlayerSync g_player;
WorldSync g_world;
LightSync g_light;
TerrainSync g_terrain;
CombatSync g_combat;
ConstraintSync g_constraints;
PossessSync g_possess;
WeaponView g_weapon_view;
VeilSync g_veils;
SpawnSync g_spawn;
NearSync g_near;
ProbeSync g_probes;
// persist.h: the spawn list, then the constraints. Bump the version on any change to either.
constexpr uint32_t kPersistVersion = 1;
constexpr uint32_t kPersistBytes = SpawnSync::kSaveBytes + ConstraintSync::kSaveBytes;
uint8_t* g_persist = nullptr;
uint32_t g_frame = 0;
uint32_t g_guest_frames_read = 0;
char g_status[160] = "";

// Both frames are static: 20 KB is too much to put on a script fiber's stack every
// frame, and the per-frame path must not allocate.
GrHostFrame g_host{};
GrGuestFrame g_guest{};

// Directory of the .asi, with a trailing backslash.
bool ModuleDir(wchar_t* out, size_t chars) noexcept {
    const DWORD n = GetModuleFileNameW(g_module, out, static_cast<DWORD>(chars));
    if (n == 0 || n >= chars) return false;
    wchar_t* slash = std::wcsrchr(out, L'\\');
    if (!slash) return false;
    slash[1] = L'\0';
    return true;
}

void ApplyHud(bool hide) noexcept;

wchar_t g_temp_ini[MAX_PATH + 32] = L"";
// Test switches only (ini `live_settings=1`): the camera switches are read again every two
// seconds, so they can be compared without a script reload moving the player. It reads a
// file on the script thread, so it is never on by default.
bool g_live_settings = false;
uint32_t g_live_read_ms = 0;

void ReadCamSettings() noexcept {
    const auto ini = [](const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(L"GarrysRedemption", key, fallback, g_temp_ini));
    };
    g_player.SetCamMode(ini(L"cam_mode", g_player.cam_mode()));
    g_player.SetPedMode(ini(L"ped_mode", g_player.ped_mode()));
    g_player.SetPedFollow(static_cast<float>(ini(L"ped_follow_cm", static_cast<int>(g_player.ped_follow() * 100.0f))) / 100.0f);
    g_smooth = ini(L"smooth", g_smooth ? 1 : 0) != 0;
}

// Everything here may touch the disk: it runs once, before the first frame.
void Init() noexcept {
    wchar_t dir[MAX_PATH] = L"";
    wchar_t path[MAX_PATH + 32] = L"";
    const bool have_dir = ModuleDir(dir, MAX_PATH);

    bool logging = false;
    if (have_dir) {
        std::swprintf(path, MAX_PATH + 32, L"%lsGarrysRedemption.log", dir);
        logging = Log::Get().Start(path);
    }
    if (!logging) {
        // The game folder is not writable (a protected install location, usually).
        wchar_t temp[MAX_PATH] = L"";
        if (GetTempPathW(MAX_PATH, temp)) {
            std::swprintf(path, MAX_PATH + 32, L"%lsGarrysRedemption.log", temp);
            Log::Get().Start(path);
        }
    }

    if (have_dir) {
        std::swprintf(path, MAX_PATH + 32, L"%lsGarrysRedemption.ini", dir);
        // %TEMP%\GarrysRedemption.ini overrides the ini beside the plugin: it can be written
        // without administrator rights when the game is under Program Files (and is read again
        // on a script reload).
        wchar_t temp_ini[MAX_PATH + 32] = L"";
        const DWORD temp_len = GetTempPathW(MAX_PATH, temp_ini);
        if (temp_len > 0 && temp_len < MAX_PATH) std::wcscat(temp_ini, L"GarrysRedemption.ini");
        const auto ini = [&](const wchar_t* key, int fallback) {
            return static_cast<int>(GetPrivateProfileIntW(L"GarrysRedemption", key,
                                                          GetPrivateProfileIntW(L"GarrysRedemption", key, fallback, path),
                                                          temp_ini));
        };
        Log::Get().SetVerbose(ini(L"verbose", 0) != 0);
        g_status_text = ini(L"status_text", 1) != 0;
        g_smooth = ini(L"smooth", 1) != 0;
        g_follow_ground = ini(L"follow_ground", 1) != 0;
        g_collide = ini(L"collide", 1) != 0;
        g_hide_hud = ini(L"hide_hud", 1) != 0;
        g_world.SetRigidHold(ini(L"hold_rigid", 0) != 0);
        g_player.SetPedFollow(static_cast<float>(ini(L"ped_follow_cm", kPedFollowCm)) / 100.0f);
        g_player.SetCamMode(ini(L"cam_mode", kCamMode));
        g_player.SetPedMode(ini(L"ped_mode", 1));
        std::wcscpy(g_temp_ini, temp_ini);
        g_live_settings = ini(L"live_settings", 0) != 0;
        g_keep_composited = ini(L"keep_composited", 0) != 0;
        g_terrain.SetBudgetUs(ini(L"terrain_budget_us", 1000));
        GR_LOG("settings: smooth %d, collide %d, ped_follow_cm %d, cam_mode %d", g_smooth ? 1 : 0, g_collide ? 1 : 0,
               ini(L"ped_follow_cm", kPedFollowCm), ini(L"cam_mode", kCamMode));
    }

    // Test hook: tools/tests point each run at its own shared memory so they cannot
    // collide with a real session. Never set in normal use.
    wchar_t name[96] = L"";
    const DWORD n = GetEnvironmentVariableW(L"GR_SHM_NAME_OVERRIDE", name, 96);
    const wchar_t* shm_name = (n > 0 && n < 96) ? name : GR_SHM_NAME;

    GR_LOG("Garry's Redemption RDR2 plugin: protocol %u, shared memory %u bytes, verbose %d",
           GR_PROTOCOL_VERSION, GR_SHM_SIZE, Log::Get().verbose() ? 1 : 0);

    g_frame = 0;
    g_guest_frames_read = 0;
    g_last_state = LinkState::Closed;
    g_handover_down = false;
    g_next_window_search_ms = 0;
    g_host = GrHostFrame{};
    g_guest = GrGuestFrame{};
    g_player.Reset();
    g_world.Reset();
    g_light.Reset();
    g_terrain.Reset();
    g_combat.Reset();
    g_constraints.Reset();
    g_possess.Reset();
    g_weapon_view.Reset();
    g_veils.Reset();
    g_spawn.Reset();
    g_near.Reset();
    g_probes.Reset();
    // What an earlier run of the plugin in this game made (persist.h).
    bool fresh = true;
    g_persist = Persist::Block(kPersistVersion, kPersistBytes, fresh);
    if (g_persist && !fresh) {
        g_spawn.Restore(g_persist);
        g_constraints.Restore(g_persist + SpawnSync::kSaveBytes);
    }
    // An earlier run of the plugin may have been unloaded with them hidden (CTRL+R while
    // GMod had the player): nothing can call natives while a DLL unloads.
    g_hud_hidden = true;
    ApplyHud(false);
    if (g_link.Open(Role::Host, NowMs(), shm_name)) {
        GR_LOG("link: shared memory %s", g_link.created() ? "created" : "already existed, opened");
    } else {
        GR_LOG("link: could not create the shared memory (error %lu), will keep retrying",
               g_link.last_error());
    }
}

void OnStateChange(LinkState from, LinkState to) noexcept {
    const GrPeer& peer = g_link.peer();
    GR_LOG("link: %s -> %s", LinkStateName(from), LinkStateName(to));
    switch (to) {
        case LinkState::Connected:
            GR_LOG("link: GMod module connected (pid %u, protocol %u)", peer.pid, peer.protocol_version);
            break;
        case LinkState::VersionMismatch:
            GR_LOG("link: version mismatch. This plugin: protocol %u, %u bytes. GMod module: "
                   "protocol %u, %u bytes. Update both from the same release.",
                   GR_PROTOCOL_VERSION, GR_SHM_SIZE, peer.protocol_version, peer.shm_size);
            break;
        case LinkState::PeerRefused:
            GR_LOG("link: GMod module refuses to run (%s)", ReasonName(peer.reason));
            break;
        default:
            break;
    }
}

// GMod's HUD replaces RDR2's minimap and cores (health, stamina, dead eye; the horse's)
// while GMod has the player; every other part of RDR2's HUD (prompts, banners, the wanted
// notice, help text) stays. The natives are switches, so hiding is repeated every frame (in
// case one of RDR2's own scripts turns them back on) and undone once when RDR2 has the
// player again.
void ApplyHud(bool hide) noexcept {
    if (hide) {
        native::DisplayRadar(false);
        native::ShowPlayerCores(false);
        native::ShowHorseCores(false);
    } else if (g_hud_hidden) {
        native::DisplayRadar(true);
        native::ShowPlayerCores(true);
        native::ShowHorseCores(true);
        GR_LOG("hud: RDR2's minimap and cores shown again");
    }
    if (hide && !g_hud_hidden) GR_LOG("hud: RDR2's minimap and cores hidden while GMod has the player");
    g_hud_hidden = hide;
}

// Works around: when RDR2's borderless window covers its monitor exactly, the graphics driver
// sends RDR2's frames straight to the screen, past Windows' compositor, and GMod's overlay (a
// window of its own on top) is never drawn: the user saw RDR2 with no HUD and no physgun at
// 3840x2160 on a 3840x2160 monitor, while at 1920x1080 on the same monitor all was well. A
// window that is not exactly the monitor's rectangle is not taken for a full-screen one, so
// the window is given a one pixel border that lies just outside the screen: its client area,
// RDR2's picture, is still exactly the monitor, and its size is what RDR2 set.
// Two earlier versions, both seen to fail:
//   - one pixel taller: RDR2 took 3840x2161 for its resolution, wrote it to its settings and
//     fell back to 1024x768 when the user next opened them (user report);
//   - moved up one pixel: the bottom row of the screen was free and Windows put the taskbar
//     over the game.
// `keep_composited=0` in the ini turns it off.
void KeepComposited(HWND window) noexcept {
    if (!g_keep_composited) return;
    RECT rect{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetWindowRect(window, &rect) ||
        !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) ||
        !EqualRect(&rect, &monitor.rcMonitor)) {
        return;
    }
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if ((style & (WS_BORDER | WS_THICKFRAME)) != 0) return;  // RDR2's own bordered window: not ours to change
    SetWindowLongPtrW(window, GWL_STYLE, style | WS_BORDER);
    // Asynchronously: the window belongs to another thread, and the script thread must not
    // wait for it.
    SetWindowPos(window, nullptr, rect.left - 1, rect.top - 1, rect.right - rect.left + 2, rect.bottom - rect.top + 2,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
    GR_LOG("window: RDR2 covers its whole monitor (%ldx%ld). Given a border just outside the screen so Windows "
           "keeps drawing GMod's overlay over it",
           rect.right - rect.left, rect.bottom - rect.top);
}

// The game window does not exist yet when the script first runs in some start-up orders,
// so keep looking, but not every frame.
void FindGameWindow(uint64_t now_ms) noexcept {
    InputCapture& input = InputCapture::Get();
    // Works around: RDR2 makes a new window when its display mode is changed (windowed to
    // borderless, another resolution; seen: the handle changed). The plugin kept the dead
    // one, so RDR2 was never "in front" again: no input reached GMod and RDR2's own menus
    // got stuck half open (user report after switching to 4K). A dead window is let go of
    // and looked for again. Checked on the same once-a-second beat as the search.
    if (now_ms < g_next_window_search_ms) return;
    g_next_window_search_ms = now_ms + 1000;
    if (input.installed()) {
        if (IsWindow(input.window())) {
            KeepComposited(input.window());
            return;
        }
        GR_LOG("input: the game window %p is gone (a display mode change). Looking for the new one",
               static_cast<void*>(input.window()));
        input.Remove();
    }
    if (!input.Install()) return;

    wchar_t name[64] = L"";
    GetClassNameW(input.window(), name, 64);
    const char* source = "nothing in the process has registered the mouse and the plugin could not either: no mouse look";
    switch (input.mouse_source()) {
        case InputCapture::MouseSource::GameRegistration:
            source = input.raw_target() == input.window() ? "the game's own raw input registration, on this window"
                     : input.raw_target() == nullptr     ? "the game's own raw input registration, following focus"
                                                         : "the game's own raw input registration, on ANOTHER window";
            break;
        case InputCapture::MouseSource::OwnRegistration:
            source = "raw input registered by the plugin: the game had not registered the mouse";
            break;
        default:
            break;
    }
    GR_LOG("input: game window %p, class \"%ls\". Mouse from %s", static_cast<void*>(input.window()), name, source);
}

void UpdateStatus(LinkState state) noexcept {
    switch (state) {
        case LinkState::Connected:
            std::snprintf(g_status, sizeof(g_status), "Garry's Redemption: connected to GMod, %s",
                          g_player.driving()     ? "which has the player (F9 hands it to RDR2)"
                          : g_player.user_owns() ? "RDR2 has the player (F9 hands it to GMod)"
                          : g_player.owns()      ? "RDR2 has the player until it is back on foot and in control"
                                                 : "waiting for its player");
            break;
        case LinkState::VersionMismatch:
            std::snprintf(g_status, sizeof(g_status),
                          "Garry's Redemption: version mismatch (plugin protocol %u, GMod module protocol %u)",
                          GR_PROTOCOL_VERSION, g_link.peer().protocol_version);
            break;
        case LinkState::PeerRefused:
            std::snprintf(g_status, sizeof(g_status), "Garry's Redemption: GMod module refuses to run");
            break;
        case LinkState::Closed:
            std::snprintf(g_status, sizeof(g_status), "Garry's Redemption: shared memory unavailable");
            break;
        default:
            std::snprintf(g_status, sizeof(g_status), "Garry's Redemption: waiting for GMod");
            break;
    }
}

// RDR2's own frame pacing, logged every 600 frames: the mean frame, the longest, and how many
// took more than half as long again as the mean. What GMod costs RDR2 shows here (its work
// shares the GPU), and so does a look that is not fluid.
uint32_t g_pace_last_us = 0, g_pace_n = 0, g_pace_max_us = 0;
uint64_t g_pace_sum_us = 0;
uint32_t g_pace_frames[600];
void Pace(uint32_t now_us) noexcept {
    const uint32_t dt = now_us - g_pace_last_us;
    g_pace_last_us = now_us;
    if (dt == 0 || dt > 1000000) return;
    g_pace_frames[g_pace_n] = dt;
    g_pace_sum_us += dt;
    if (dt > g_pace_max_us) g_pace_max_us = dt;
    if (++g_pace_n < 600) return;
    const uint32_t mean = static_cast<uint32_t>(g_pace_sum_us / 600);
    uint32_t slow = 0;
    for (const uint32_t f : g_pace_frames) slow += f > mean + mean / 2 ? 1u : 0u;
    GR_LOG("pace: 600 frames, mean %.2f ms (%.0f fps), longest %.2f ms, %u slow, %s has the player", mean / 1000.0,
           1e6 / mean, g_pace_max_us / 1000.0, slow, g_player.driving() ? "GMod" : "RDR2");
    g_pace_n = 0;
    g_pace_sum_us = 0;
    g_pace_max_us = 0;
}

// One frame. No waits, no file I/O, no allocation.
void Tick() noexcept {
    ++g_frame;
    Log::Get().SetFrame(g_frame);

    // Story mode only. The moment any network session shows up, refuse for the rest of
    // the process's life and stop calling natives altogether.
    if (native::IsMultiplayer()) {
        g_multiplayer = true;
        g_link.Refuse(GR_REASON_MULTIPLAYER);
        g_link.Tick(NowMs());
        GR_LOG("multiplayer session detected: the bridge is off until the game is restarted. "
               "Garry's Redemption is story mode only.");
        return;
    }

    const LinkState state = g_link.Tick(NowMs());
    if (state != g_last_state) {
        OnStateChange(g_last_state, state);
        g_last_state = state;
    }

    // Not connected: whatever the guest last said is void, its player above all.
    if (state != LinkState::Connected) g_guest = GrGuestFrame{};
    if (g_link.Read(g_guest)) ++g_guest_frames_read;

    FindGameWindow(NowMs());
    InputCapture& input = InputCapture::Get();
    const bool focused = input.focused();
    // The handover key is the plugin's own, so it is read here and never sent to GMod.
    const bool handover_down = focused && (GetAsyncKeyState(kHandoverVk) & 0x8000) != 0;
    const bool handover = handover_down && !g_handover_down && state == LinkState::Connected;
    g_handover_down = handover_down;

    const uint32_t now_us = NowUs();
    Pace(now_us);
    if (g_live_settings && now_us / 1000 - g_live_read_ms > 2000) {
        g_live_read_ms = now_us / 1000;
        ReadCamSettings();
    }
    // GMod standing its player on the terrain chunks replaces draping its flat ground over
    // RDR2's.
    const bool terrain = state == LinkState::Connected && (g_guest.flags & GR_GUESTF_TERRAIN) != 0;
    g_player.Tick(state == LinkState::Connected, g_guest, focused, handover, g_smooth, g_follow_ground && !terrain,
                  g_collide, now_us, g_host, input.mouse_total_x(), input.mouse_total_y());

    // Peds GMod is simulating follow their proxies; then everything near the player is
    // listed for GMod, driven ones included. Only while GMod has the player: its proxies
    // live around the GMod player.
    const native::Entity player_ped = native::PlayerPedId();
    g_world.Drive(state == LinkState::Connected && g_player.driving(), g_guest, player_ped,
                  g_player.ground_shift());
    if (g_player.driving()) {
        g_world.Scan(player_ped, g_host.ped_pos, g_spawn, g_host);
        g_light.Tick(player_ped, {g_host.ped_pos.x, g_host.ped_pos.y, g_host.ped_pos.z + 1.6f}, native::GetFrameTime(),
                     g_host);
        g_terrain.Tick(g_link, terrain, player_ped, g_host.ped_pos, g_guest.vel);
        g_near.Tick(player_ped, g_host.ped_pos, (g_guest.flags & GR_GUESTF_NOCLIP) != 0, g_spawn, g_world, g_host);
    } else {
        g_host.entity_count = 0;
        g_host.nearby_count = 0;
        g_host.light_flags = 0;
    }
    ApplyHud(g_hide_hud && g_player.driving());

    // GMod's hits and explosions, and RDR2's damage to the player's ped.
    // The spawn menu's events come in the same ring.
    g_combat.Tick(state == LinkState::Connected && g_player.driving(), g_link.peer_generation(), g_guest, player_ped,
                  g_spawn, g_host);
    g_spawn.Tick(NowMs(), g_host);
    GrVec3 teleport_to{};
    if (g_combat.TakeTeleport(teleport_to)) g_player.RequestTeleport(teleport_to);
    // RDR2's own model of the gun the GMod player holds, in front of the camera.
    g_weapon_view.Tick(state == LinkState::Connected && g_player.driving(), g_guest, g_player.shown_pos(),
                       g_player.shown_rot(), g_combat.shot_ms());
    // Welds, ropes, no-collides and thrusters from GMod's tools.
    g_constraints.Tick(state == LinkState::Connected && g_player.driving(), g_guest);
    // The possess tool: one ped walks where GMod's player steers it.
    g_possess.Tick(state == LinkState::Connected && g_player.driving(), g_guest, player_ped);
    // RDR2's trees and bushes in front of GMod's own props.
    g_veils.Tick(state == LinkState::Connected && g_player.driving(), g_guest, player_ped, g_host);
    // RDR2's trees, walls and rocks in the way of GMod's grenades, balls, rockets and thrown things.
    g_probes.Tick(state == LinkState::Connected && g_player.driving(), g_guest, player_ped, g_spawn, g_world, g_host);
    if (g_persist) {
        g_spawn.Save(g_persist);
        g_constraints.Save(g_persist + SpawnSync::kSaveBytes);
    }

    // Input is GMod's only while GMod has the player and RDR2 is the window being typed
    // into with no RDR2 menu over it.
    input.Fill(g_host.input, g_player.driving() && focused && !g_player.paused());

    g_host.frame = g_frame;
    g_host.time_us = now_us;
    g_host.game_time_ms = static_cast<uint32_t>(native::GetGameTimer());
    g_host.dt = native::GetFrameTime();
    g_link.Publish(g_host);

    if ((g_frame % 600) == 0) {
        GR_VERBOSE("guest frame %u is %d us old and %u host frames behind; %u read, %u torn; ped (%.2f, %.2f, %.2f); "
                   "%u raw input messages, %u mouse packets, mouse total (%d, %d)",
                   g_guest.frame, AgeUs(now_us, g_guest.time_us), g_frame - 1 - g_guest.host_frame_seen,
                   g_guest_frames_read, g_link.torn_reads(), g_host.ped_pos.x, g_host.ped_pos.y, g_host.ped_pos.z,
                   input.raw_messages(), input.mouse_packets(), g_host.input.mouse_dx, g_host.input.mouse_dy);
    }

    if (g_status_text) {
        UpdateStatus(state);
        native::DrawText(g_status, 0.02f, 0.02f, 0.35f);
    }
}

}  // namespace

void OnAttach(HMODULE module) noexcept { g_module = module; }

void OnDetach(bool process_terminating) noexcept {
    g_link.Close();
    // No natives here (this is not the script thread): if GMod had the player, the ped
    // stays hidden until the script starts again and PlayerSync::Reset gives it back.
    // A dying process takes its hook with it; touching another thread now is not safe.
    if (!process_terminating && !InputCapture::Get().Remove()) {
        GR_LOG("input: the game's window thread is stuck in the plugin's hook, so the plugin stays loaded. A "
               "script reload will not start it again: restart the game");
    }
    GR_LOG("plugin unloading (%s)", process_terminating ? "game exiting" : "script reload");
    Log::Get().StopFromDllMain(process_terminating);
}

void ScriptMain() {
    Init();
    for (;;) {
        if (g_multiplayer) {
            // Refused. Keep the heartbeat going so GMod can show why, and touch nothing
            // in the game.
            g_link.Tick(NowMs());
        } else {
            Tick();
        }
        scriptWait(0);
    }
}

}  // namespace gr::plugin
