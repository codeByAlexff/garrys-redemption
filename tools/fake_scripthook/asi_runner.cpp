// Runs GarrysRedemption.asi without RDR2: loads it against the stand-in ScriptHookRDR2.dll
// next to this exe and plays the game's part, one script frame at a time.
//
//   asi_runner --asi <path> [--seconds N] [--fps N] [--multiplayer-after S]
//              [--scene-from S --scene-to S]
//
// --scene-from/--scene-to play a scripted scene: for that stretch the game reports player
// control as off, and when it ends the ped is 5 m further east.
//
// Prints each new line of text the plugin draws and each change to the player ped and the
// script camera, then two lines for the tests:
//   world ped=1500.00,-2192.00,61.00 heading=0.0 visible=1 frozen=0 cam_exists=0 ...
//   summary registered=1 frames=360 native_calls=2520 unknown_natives=0 natives_after_multiplayer=0 unloaded=1
//
// The plugin opens the same shared memory as in the game (or GR_SHM_NAME_OVERRIDE), so a
// real GMod with the module installed connects to it exactly as it would to RDR2.

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fake_scripthook.h"

namespace {

// "connected to GMod (guest frame 123)" changes every frame. Compare with the numbers
// blanked so each kind of line prints once.
std::string WithoutNumbers(const char* text) {
    std::string out;
    for (const char* c = text; *c; ++c) {
        if (std::isdigit(static_cast<unsigned char>(*c))) {
            if (out.empty() || out.back() != '#') out.push_back('#');
        } else {
            out.push_back(*c);
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const char* asi = nullptr;
    double seconds = 5.0;
    double fps = 60.0;
    double multiplayer_after = -1.0;
    double scene_from = -1.0;
    double scene_to = -1.0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--asi")) asi = argv[i + 1];
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--fps")) fps = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--multiplayer-after")) multiplayer_after = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--scene-from")) scene_from = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--scene-to")) scene_to = std::atof(argv[i + 1]);
    }
    if (!asi || fps <= 0) {
        std::printf("usage: asi_runner --asi <path> [--seconds N] [--fps N] [--multiplayer-after S] "
                    "[--scene-from S --scene-to S]\n");
        return 2;
    }

    // DllMain registers the script. The plugin's import of ScriptHookRDR2.dll resolves to
    // the stand-in already loaded into this process.
    const HMODULE plugin = LoadLibraryA(asi);
    if (!plugin) {
        std::printf("could not load %s (error %lu)\n", asi, GetLastError());
        return 2;
    }
    std::printf("loaded %s\n", asi);

    const ULONGLONG start = GetTickCount64();
    const DWORD frame_ms = static_cast<DWORD>(1000.0 / fps);
    std::string last_drawn;
    unsigned long long last_unknown = 0;
    unsigned frames = 0;
    bool multiplayer = false;
    int scene = 0;  // 0 not started, 1 playing, 2 over
    int registered = 0;
    GrFakeWorld world{};
    GrFake_GetWorld(&world);
    const GrFakeWorld start_world = world;
    int last_hidden = 0, last_frozen = 0, last_rendering = 0;
    unsigned driven_frames = 0;
    float max_north = 0.0f;
    float eye_height = 0.0f;
    float cam_yaw = 0.0f;
    for (;;) {
        const double elapsed = static_cast<double>(GetTickCount64() - start) / 1000.0;
        if (elapsed >= seconds) break;
        if (!multiplayer && multiplayer_after >= 0 && elapsed >= multiplayer_after) {
            multiplayer = true;
            GrFake_SetMultiplayer(1);
            std::printf("multiplayer session started\n");
        }
        if (scene == 0 && scene_from >= 0 && elapsed >= scene_from) {
            scene = 1;
            GrFake_SetPlayerControl(0, 0);
            std::printf("scene started: player control off\n");
        } else if (scene == 1 && elapsed >= scene_to) {
            scene = 2;
            GrFake_SetPlayerControl(1, 1);
            std::printf("scene over: player control on, ped moved 5 m east\n");
        }
        registered |= GrFake_RunFrame();
        ++frames;

        GrFake_GetWorld(&world);
        const int hidden = !world.ped_visible;
        if (hidden != last_hidden || world.ped_frozen != last_frozen || world.rendering_script_cams != last_rendering) {
            std::printf("world: ped %s and %s, %s camera rendering\n", hidden ? "hidden" : "visible",
                        world.ped_frozen ? "frozen" : "free", world.rendering_script_cams ? "script" : "gameplay");
            last_hidden = hidden;
            last_frozen = world.ped_frozen;
            last_rendering = world.rendering_script_cams;
        }
        if (world.rendering_script_cams && world.cam_active) {
            ++driven_frames;
            // The camera above the ped's feet, which are 1 m below its origin.
            eye_height = world.cam_pos[2] - (world.ped_origin[2] - 1.0f);
            cam_yaw = world.cam_rot[2];
        }
        if (world.ped_origin[1] - start_world.ped_origin[1] > max_north) {
            max_north = world.ped_origin[1] - start_world.ped_origin[1];
        }

        const char* drawn = GrFake_DrawnText();
        const std::string kind = WithoutNumbers(drawn);
        if (*drawn && kind != last_drawn) {
            std::printf("draw: %s\n", drawn);
            last_drawn = kind;
        }
        if (GrFake_LastUnknownNative() != last_unknown) {
            last_unknown = GrFake_LastUnknownNative();
            std::printf("unknown native 0x%016llX: add it to tools/fake_scripthook/fake_scripthook.cpp\n", last_unknown);
        }
        std::fflush(stdout);
        Sleep(frame_ms);
    }

    // DllMain again: the plugin unregisters its script, closes the link and its log.
    FreeLibrary(plugin);
    std::printf("world ped=%.2f,%.2f,%.2f heading=%.1f visible=%d frozen=%d cam_exists=%d rendering=%d "
                "cams_created=%u cams_destroyed=%u driven_frames=%u max_north=%.2f eye_height=%.2f cam_yaw=%.1f "
                "cam_fov=%.1f controls_disabled_frames=%u controls_enabled=%u\n",
                world.ped_origin[0], world.ped_origin[1], world.ped_origin[2], world.ped_heading, world.ped_visible,
                world.ped_frozen, world.cam_exists, world.rendering_script_cams, world.cams_created,
                world.cams_destroyed, driven_frames, max_north, eye_height, cam_yaw, world.cam_fov,
                world.frames_controls_disabled, world.controls_enabled);
    std::printf("summary registered=%d frames=%u native_calls=%u unknown_natives=%u natives_after_multiplayer=%u unloaded=%u\n",
                registered, frames, GrFake_Count(GR_FAKE_NATIVE_CALLS), GrFake_Count(GR_FAKE_UNKNOWN_NATIVES),
                GrFake_Count(GR_FAKE_NATIVES_AFTER_MULTIPLAYER), GrFake_Count(GR_FAKE_UNREGISTERED));
    return registered ? 0 : 1;
}
