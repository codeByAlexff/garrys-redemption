// A stand-in for ScriptHookRDR2.dll, for running GarrysRedemption.asi without the game.
//
// It is a test double written from the plugin's side of the interface: it exports the six
// functions the plugin imports (same declarations, rdr2/src/scripthook.h) and answers the
// natives the plugin calls with canned values. It contains nothing of Script Hook RDR2 or
// of the game, hooks nothing, and is never packaged: it only exists in rdr2/build/harness.
//
// Like the real one, it runs the script as a fiber: scriptWait switches back to the
// "game" (asi_runner's loop), which resumes the script on a later frame.

#define GR_SCRIPTHOOK_API __declspec(dllexport)
#define GR_FAKE_API __declspec(dllexport)

#include "fake_scripthook.h"

#include <cmath>
#include <cstring>

#include "native_hashes.h"
#include "scripthook.h"

namespace {

namespace hash = gr::native_hash;

void (*g_script_main)() = nullptr;
void* g_main_fiber = nullptr;
void* g_script_fiber = nullptr;
ULONGLONG g_wake_at = 0;
ULONGLONG g_started = 0;

UINT64 g_native = 0;
UINT64 g_args[32];
unsigned g_arg_count = 0;
UINT64 g_result[4];

bool g_multiplayer = false;
unsigned g_frame = 0;
unsigned g_refused_frame = 0;  // 0 = no network check has answered true yet
unsigned g_counts[4];
UINT64 g_last_unknown = 0;
char g_var_string[256];
char g_drawn[256];

// Made by rdr2/CMakeLists.txt from native_hashes.h.
constexpr UINT64 kRecorded[] = {
#include "known_native_hashes.inc"
};

bool Recorded(UINT64 native) {
    for (UINT64 known : kRecorded) {
        if (known == native) return true;
    }
    return false;
}

void CALLBACK ScriptFiber(void*) {
    g_script_main();
    // A fiber function that returns ends the thread. A real script never returns either.
    for (;;) SwitchToFiber(g_main_fiber);
}

void CopyText(char* to, UINT64 from) {
    const char* text = reinterpret_cast<const char*>(from);
    std::strncpy(to, text ? text : "", 255);
    to[255] = '\0';
}

template <class T>
void Return(T value) {
    g_result[0] = 0;
    std::memcpy(g_result, &value, sizeof(T));
}

// A Vector3 result: one float in the low half of each of three slots.
void ReturnVec3(float x, float y, float z) {
    g_result[0] = g_result[1] = g_result[2] = 0;
    std::memcpy(&g_result[0], &x, sizeof(float));
    std::memcpy(&g_result[1], &y, sizeof(float));
    std::memcpy(&g_result[2], &z, sizeof(float));
}

float ArgFloat(unsigned i) {
    float value = 0.0f;
    if (i < g_arg_count) std::memcpy(&value, &g_args[i], sizeof(float));
    return value;
}

int ArgInt(unsigned i) { return i < g_arg_count ? static_cast<int>(g_args[i]) : 0; }

// A Vector3 the native writes through a pointer argument: floats 8 bytes apart.
void WriteVec3(unsigned i, float x, float y, float z) {
    if (i >= g_arg_count || !g_args[i]) return;
    float* out = reinterpret_cast<float*>(g_args[i]);
    out[0] = x;
    out[2] = y;
    out[4] = z;
}

// The world. The same spot as the scene in tools/fake_rdr2.py: kilometres from the world
// origin, so anything that forgets the floating origin shows up.
constexpr int kPed = 77;
constexpr int kCam = 501;
constexpr float kGroundZ = 60.0f;
constexpr float kPedOriginHeight = 1.0f;    // GET_MODEL_DIMENSIONS answers a box from -1 to +0.8
constexpr unsigned kPedModel = 0x0D7114C9;  // what the real game reported for the player ped
constexpr float kPi = 3.14159265f;

GrFakeWorld g_world = {{1500.0f, -2200.0f, kGroundZ + kPedOriginHeight}, 30.0f, 1, 0, 0, 0, 0,
                       {0, 0, 0}, {0, 0, 0}, 50.0f, 0, 0, 0, 0, 1};
unsigned g_controls_frame = 0;  // the frame DISABLE_ALL_CONTROL_ACTIONS was last called in

float HeadingRad() { return g_world.ped_heading * kPi / 180.0f; }

// Rotation order 2 reports yaw in (-180, 180]; headings run 0 to 360.
float HeadingAsYaw() {
    const float yaw = std::fmod(g_world.ped_heading, 360.0f);
    return yaw > 180.0f ? yaw - 360.0f : yaw;
}

}  // namespace

void scriptRegister(HMODULE, void (*LP_SCRIPT_MAIN)()) { g_script_main = LP_SCRIPT_MAIN; }

void scriptUnregister(HMODULE) {
    // Called from the plugin's DllMain, on the main fiber: the script is parked in
    // scriptWait and can simply be thrown away.
    if (g_script_fiber) DeleteFiber(g_script_fiber);
    g_script_fiber = nullptr;
    g_script_main = nullptr;
    ++g_counts[GR_FAKE_UNREGISTERED];
}

void scriptWait(DWORD time) {
    g_wake_at = GetTickCount64() + time;
    SwitchToFiber(g_main_fiber);
}

void nativeInit(UINT64 native) {
    g_native = native;
    g_arg_count = 0;
}

void nativePush64(UINT64 value) {
    if (g_arg_count < 32) g_args[g_arg_count++] = value;
}

// The fake world has no peds but the player.
int worldGetAllPeds(int*, int) { return 0; }
int worldGetAllVehicles(int*, int) { return 0; }
int worldGetAllObjects(int*, int) { return 0; }

PUINT64 nativeCall() {
    ++g_counts[GR_FAKE_NATIVE_CALLS];
    if (g_refused_frame && g_frame > g_refused_frame) ++g_counts[GR_FAKE_NATIVES_AFTER_MULTIPLAYER];

    switch (g_native) {
        case hash::NETWORK_IS_IN_SESSION:
        case hash::NETWORK_IS_SESSION_STARTED:
        case hash::NETWORK_IS_GAME_IN_PROGRESS:
            if (g_multiplayer && !g_refused_frame) g_refused_frame = g_frame;
            Return<int>(g_multiplayer ? 1 : 0);
            break;
        case hash::GET_GAME_TIMER:
            Return<int>(static_cast<int>(GetTickCount64() - g_started));
            break;
        case hash::GET_FRAME_TIME:
            Return<float>(1.0f / 60.0f);
            break;
        case hash::VAR_STRING:
            // (flags, "LITERAL_STRING", text)
            CopyText(g_var_string, g_arg_count > 2 ? g_args[2] : 0);
            Return<const char*>(g_var_string);
            break;
        case hash::BG_DISPLAY_TEXT:
            CopyText(g_drawn, g_arg_count > 0 ? g_args[0] : 0);
            Return<int>(0);
            break;
        case hash::BG_SET_TEXT_SCALE:
        case hash::BG_SET_TEXT_COLOR:
            Return<int>(0);
            break;

        case hash::PLAYER_PED_ID:
            Return<int>(kPed);
            break;
        case hash::PLAYER_ID:
            Return<int>(0);
            break;
        case hash::IS_PLAYER_CONTROL_ON:
            Return<int>(g_world.player_control);
            break;
        case hash::IS_PAUSE_MENU_ACTIVE:
        case hash::IS_ENTITY_DEAD:
        case hash::IS_PED_ON_MOUNT:
        case hash::IS_PED_IN_ANY_VEHICLE:
            Return<int>(0);
            break;

        case hash::GET_ENTITY_COORDS:
            ReturnVec3(g_world.ped_origin[0], g_world.ped_origin[1], g_world.ped_origin[2]);
            break;
        case hash::GET_ENTITY_ROTATION:
            ReturnVec3(0.0f, 0.0f, HeadingAsYaw());
            break;
        case hash::GET_ENTITY_HEADING:
            Return<float>(g_world.ped_heading);
            break;
        case hash::GET_ENTITY_FORWARD_VECTOR:
            // Heading 0 faces +Y and grows anticlockwise.
            ReturnVec3(-std::sin(HeadingRad()), std::cos(HeadingRad()), 0.0f);
            break;
        case hash::GET_ENTITY_MODEL:
            Return<unsigned>(kPedModel);
            break;
        case hash::GET_ENTITY_HEIGHT_ABOVE_GROUND:
            Return<float>(g_world.ped_origin[2] - kGroundZ);
            break;
        case hash::GET_MODEL_DIMENSIONS:
            WriteVec3(1, -0.3f, -0.25f, -kPedOriginHeight);
            WriteVec3(2, 0.3f, 0.25f, 0.8f);
            Return<int>(0);
            break;
        case hash::GET_GROUND_Z_FOR_3D_COORD:
            // Flat ground everywhere. The game writes one float through the pointer.
            if (g_arg_count > 3 && g_args[3] && ArgFloat(2) >= kGroundZ) {
                const float ground = kGroundZ;
                std::memcpy(reinterpret_cast<void*>(g_args[3]), &ground, sizeof(float));
                Return<int>(1);
            } else {
                Return<int>(0);
            }
            break;
        case hash::IS_ENTITY_VISIBLE:
            Return<int>(g_world.ped_visible);
            break;
        case hash::SET_ENTITY_VISIBLE:
            if (ArgInt(0) == kPed) g_world.ped_visible = ArgInt(1) != 0;
            Return<int>(0);
            break;
        case hash::FREEZE_ENTITY_POSITION:
            if (ArgInt(0) == kPed) g_world.ped_frozen = ArgInt(1) != 0;
            Return<int>(0);
            break;
        case hash::SET_ENTITY_HEADING:
            if (ArgInt(0) == kPed) g_world.ped_heading = std::fmod(ArgFloat(1) + 360.0f, 360.0f);
            Return<int>(0);
            break;
        case hash::SET_ENTITY_COORDS_NO_OFFSET:
            if (ArgInt(0) == kPed) {
                for (unsigned i = 0; i < 3; ++i) g_world.ped_origin[i] = ArgFloat(1 + i);
            }
            Return<int>(0);
            break;

        case hash::CREATE_CAM: {
            const char* name = g_arg_count > 0 ? reinterpret_cast<const char*>(g_args[0]) : nullptr;
            const bool ok = name && !std::strcmp(name, "DEFAULT_SCRIPTED_CAMERA") && !g_world.cam_exists;
            if (ok) {
                g_world.cam_exists = 1;
                ++g_world.cams_created;
            }
            Return<int>(ok ? kCam : 0);
            break;
        }
        case hash::DOES_CAM_EXIST:
            Return<int>(ArgInt(0) == kCam && g_world.cam_exists);
            break;
        case hash::DESTROY_CAM:
            if (ArgInt(0) == kCam && g_world.cam_exists) {
                g_world.cam_exists = 0;
                g_world.cam_active = 0;
                ++g_world.cams_destroyed;
            }
            Return<int>(0);
            break;
        case hash::SET_CAM_ACTIVE:
            if (ArgInt(0) == kCam && g_world.cam_exists) g_world.cam_active = ArgInt(1) != 0;
            Return<int>(0);
            break;
        case hash::SET_CAM_COORD:
            if (ArgInt(0) == kCam && g_world.cam_exists) {
                for (unsigned i = 0; i < 3; ++i) g_world.cam_pos[i] = ArgFloat(1 + i);
            }
            Return<int>(0);
            break;
        case hash::SET_CAM_ROT:
            if (ArgInt(0) == kCam && g_world.cam_exists) {
                for (unsigned i = 0; i < 3; ++i) g_world.cam_rot[i] = ArgFloat(1 + i);
            }
            Return<int>(0);
            break;
        case hash::SET_CAM_PARAMS:
            if (ArgInt(0) == kCam && g_world.cam_exists) {
                for (unsigned i = 0; i < 3; ++i) g_world.cam_pos[i] = ArgFloat(1 + i);
                for (unsigned i = 0; i < 3; ++i) g_world.cam_rot[i] = ArgFloat(4 + i);
                g_world.cam_fov = ArgFloat(7);
            }
            Return<int>(0);
            break;
        case hash::ATTACH_CAM_TO_ENTITY:
        case hash::DETACH_CAM:
            Return<int>(0);
            break;
        case hash::SET_CAM_FOV:
            if (ArgInt(0) == kCam && g_world.cam_exists) g_world.cam_fov = ArgFloat(1);
            Return<int>(0);
            break;
        case hash::RENDER_SCRIPT_CAMS:
            g_world.rendering_script_cams = ArgInt(0) != 0;
            Return<int>(0);
            break;
        case hash::GET_GAMEPLAY_CAM_COORD:
            // Three metres behind the ped and level with it.
            ReturnVec3(g_world.ped_origin[0] + 3.0f * std::sin(HeadingRad()),
                       g_world.ped_origin[1] - 3.0f * std::cos(HeadingRad()), g_world.ped_origin[2]);
            break;
        case hash::GET_GAMEPLAY_CAM_ROT:
            ReturnVec3(0.0f, 0.0f, HeadingAsYaw());
            break;
        case hash::GET_FINAL_RENDERED_CAM_FOV:
            Return<float>(g_world.rendering_script_cams && g_world.cam_active ? g_world.cam_fov : 50.0f);
            break;

        case hash::DISABLE_ALL_CONTROL_ACTIONS:
            if (g_controls_frame != g_frame) {
                g_controls_frame = g_frame;
                ++g_world.frames_controls_disabled;
                g_world.controls_enabled = 0;
            }
            Return<int>(0);
            break;
        case hash::ENABLE_CONTROL_ACTION:
            ++g_world.controls_enabled;
            Return<int>(0);
            break;
        default:
            // The fake world is empty past the player: every other native the plugin has
            // recorded in native_hashes.h answers zero. One it has not recorded is the
            // mistake this counts (a hash used without going through the list).
            if (!Recorded(g_native)) {
                ++g_counts[GR_FAKE_UNKNOWN_NATIVES];
                g_last_unknown = g_native;
            }
            g_result[0] = g_result[1] = g_result[2] = g_result[3] = 0;
            break;
    }
    return g_result;
}

extern "C" {

int GrFake_RunFrame(void) {
    if (!g_script_main) return 0;
    if (!g_main_fiber) {
        g_main_fiber = ConvertThreadToFiber(nullptr);
        g_started = GetTickCount64();
    }
    if (!g_script_fiber) g_script_fiber = CreateFiber(0, &ScriptFiber, nullptr);
    ++g_frame;
    g_drawn[0] = '\0';
    if (GetTickCount64() >= g_wake_at) SwitchToFiber(g_script_fiber);
    return 1;
}

void GrFake_SetMultiplayer(int on) { g_multiplayer = on != 0; }
const char* GrFake_DrawnText(void) { return g_drawn; }
unsigned GrFake_Count(int which) { return (which >= 0 && which < 4) ? g_counts[which] : 0; }
unsigned long long GrFake_LastUnknownNative(void) { return g_last_unknown; }
void GrFake_GetWorld(GrFakeWorld* out) { *out = g_world; }

void GrFake_SetPlayerControl(int on, int move) {
    g_world.player_control = on != 0;
    if (move) {
        g_world.ped_origin[0] += 5.0f;
        g_world.ped_heading = 120.0f;
    }
}

}  // extern "C"
