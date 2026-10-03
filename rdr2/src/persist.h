// State that must outlive the plugin's DLL: CTRL+R unloads and reloads the script, the
// game keeps running, and what the plugin made in it (spawned entities, ropes, welds) stays.
// Before this the reloaded plugin forgot all of it: spawned entities were no longer flagged
// (cleanup and undo missed them), spawned objects lost their proxies, and GMod's welds and
// ropes were made a second time over the old ones.
//
// The state lives in a block on the process heap, which a DLL unload does not free; its
// address is kept in an environment variable of the process (as player_sync.h keeps its
// marker). A header with a magic, a version and the size guards against a block left by a
// different build: it is then ignored and a new one made.
//
// Script thread only. The block is allocated once per process.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace gr {

class Persist {
public:
    // The block of `bytes` for layout `version`, zeroed if new. `fresh` tells whether it was
    // just made (nothing to restore). nullptr if the heap refused.
    static uint8_t* Block(uint32_t version, uint32_t bytes, bool& fresh) noexcept {
        fresh = false;
        wchar_t text[32];
        const DWORD n = GetEnvironmentVariableW(kVariable, text, 32);
        if (n > 0 && n < 32) {
            auto* header = reinterpret_cast<Header*>(static_cast<uintptr_t>(std::wcstoull(text, nullptr, 16)));
            if (header && header->magic == kMagic && header->version == version && header->bytes == bytes) {
                return reinterpret_cast<uint8_t*>(header + 1);
            }
        }
        // A block of another build is left where it is: freeing memory a different layout
        // describes is not worth the risk for a few kilobytes.
        auto* header = static_cast<Header*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Header) + bytes));
        if (!header) return nullptr;
        header->magic = kMagic;
        header->version = version;
        header->bytes = bytes;
        std::swprintf(text, 32, L"%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(header)));
        SetEnvironmentVariableW(kVariable, text);
        fresh = true;
        return reinterpret_cast<uint8_t*>(header + 1);
    }

private:
    static constexpr const wchar_t* kVariable = L"GR_PLUGIN_STATE";
    static constexpr uint32_t kMagic = 0x53524747;  // "GGRS"
    struct Header {
        uint32_t magic;
        uint32_t version;
        uint32_t bytes;
        uint32_t pad;
    };
};

}  // namespace gr
