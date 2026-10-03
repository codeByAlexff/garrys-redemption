#include <windows.h>

#include "plugin.h"
#include "scripthook.h"

// Same shape as the SDK samples: register on attach, unregister on detach. The
// unregister call is what makes Script Hook's CTRL+R script reload work.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            gr::plugin::OnAttach(module);
            scriptRegister(module, gr::plugin::ScriptMain);
            break;
        case DLL_PROCESS_DETACH:
            scriptUnregister(module);
            gr::plugin::OnDetach(reserved != nullptr);
            break;
    }
    return TRUE;
}
