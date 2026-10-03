#pragma once

#include <windows.h>

namespace gr::plugin {

// From DllMain(DLL_PROCESS_ATTACH). Only remembers the module: the loader lock is held.
void OnAttach(HMODULE module) noexcept;

// From DllMain(DLL_PROCESS_DETACH). `process_terminating` is lpReserved != nullptr.
void OnDetach(bool process_terminating) noexcept;

// The script. Script Hook calls it on the game's script thread and it never returns.
// Script Hook may call it again from the top, for instance after a save is loaded.
void ScriptMain();

}  // namespace gr::plugin
