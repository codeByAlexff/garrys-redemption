// The Script Hook RDR2 functions this plugin imports from ScriptHookRDR2.dll.
//
// These are declarations of that DLL's interface, with the signatures of inc/main.h in
// Script Hook RDR2 SDK v1.0.1207.73 (http://www.dev-c.com/rdr2/scripthookrdr2/). They are
// C++ functions, so the signature is part of the exported name: change a parameter type
// here and the plugin will fail to load in the game. Only what is used is declared, and
// each one is also listed in ../scripthook_imports.def.

#pragma once

#include <windows.h>

// tools/fake_scripthook defines this as dllexport to build its stand-in from these same
// declarations, so the two cannot drift apart.
#ifndef GR_SCRIPTHOOK_API
#define GR_SCRIPTHOOK_API __declspec(dllimport)
#endif

// Yields to the game until `time` ms have passed. 0 = until the next frame. Scripts run
// as fibers on the game's script thread, so this is the only way a frame ever ends.
GR_SCRIPTHOOK_API void scriptWait(DWORD time);
GR_SCRIPTHOOK_API void scriptRegister(HMODULE module, void (*LP_SCRIPT_MAIN)());
GR_SCRIPTHOOK_API void scriptUnregister(HMODULE module);

// Native call sequence: init with the hash, push each argument as 64 bits, call. The
// returned pointer is to the result slots and is only valid until the next call.
GR_SCRIPTHOOK_API void nativeInit(UINT64 hash);
GR_SCRIPTHOOK_API void nativePush64(UINT64 val);
GR_SCRIPTHOOK_API PUINT64 nativeCall();

// Fills `arr` with the handles of up to `arrSize` peds in the world, returns how many.
GR_SCRIPTHOOK_API int worldGetAllPeds(int* arr, int arrSize);
// The same for vehicles (wagons, coaches, boats, trains).
GR_SCRIPTHOOK_API int worldGetAllVehicles(int* arr, int arrSize);
// The same for objects (props: crates, barrels, fences, doors, carried things).
GR_SCRIPTHOOK_API int worldGetAllObjects(int* arr, int arrSize);
