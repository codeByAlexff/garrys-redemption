// Control surface of the stand-in ScriptHookRDR2.dll, used by asi_runner. The six
// functions the plugin itself imports are declared in rdr2/src/scripthook.h.

#pragma once

#ifndef GR_FAKE_API
#define GR_FAKE_API __declspec(dllimport)
#endif

enum GrFakeCount {
    GR_FAKE_NATIVE_CALLS = 0,
    // Calls of a hash the stand-in does not implement: the plugin has started using a
    // native that needs adding to fake_scripthook.cpp.
    GR_FAKE_UNKNOWN_NATIVES = 1,
    // Calls made in frames after the one in which a network check first answered true.
    // The plugin must make none.
    GR_FAKE_NATIVES_AFTER_MULTIPLAYER = 2,
    GR_FAKE_UNREGISTERED = 3,
};

// The stand-in's small world: one player ped standing on flat ground and at most one
// script camera. Everything the plugin does to them through natives lands here.
struct GrFakeWorld {
    float ped_origin[3];  // the ped's origin, 1 m above its feet, as GET_ENTITY_COORDS reports it
    float ped_heading;
    int ped_visible;
    int ped_frozen;
    int cam_exists;
    int cam_active;
    int rendering_script_cams;
    float cam_pos[3];
    float cam_rot[3];  // pitch, roll, yaw
    float cam_fov;
    unsigned cams_created;
    unsigned cams_destroyed;
    unsigned frames_controls_disabled;  // frames in which DISABLE_ALL_CONTROL_ACTIONS was called
    unsigned controls_enabled;          // ENABLE_CONTROL_ACTION calls in the last such frame
    int player_control;                 // what IS_PLAYER_CONTROL_ON answers
};

extern "C" {
// One game frame: resumes the registered script until its next scriptWait. Returns 0 if
// no script is registered.
GR_FAKE_API int GrFake_RunFrame(void);
GR_FAKE_API void GrFake_SetMultiplayer(int on);
// What the script drew with _BG_DISPLAY_TEXT during the last frame, "" if nothing.
GR_FAKE_API const char* GrFake_DrawnText(void);
GR_FAKE_API unsigned GrFake_Count(int which);
GR_FAKE_API unsigned long long GrFake_LastUnknownNative(void);
GR_FAKE_API void GrFake_GetWorld(GrFakeWorld* out);
// 0 makes IS_PLAYER_CONTROL_ON answer false, as during a scripted scene. With `move` set
// the "scene" also puts the ped 5 m further east and turns it, so the plugin has somewhere
// new to anchor GMod to afterwards.
GR_FAKE_API void GrFake_SetPlayerControl(int on, int move);
}
