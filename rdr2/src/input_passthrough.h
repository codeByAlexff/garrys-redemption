// The keys that stay with RDR2 while GMod drives the player.
//
// Everything else on the keyboard and mouse goes to GMod over the bridge, and RDR2's own
// controls are switched off every frame so it does not also act on it. The keys listed
// here are the exception both ways: they are never sent to GMod, and the RDR2 control
// behind each one is switched back on.
//
// The pairing assumes RDR2's default key bindings. A user who has rebound one of these
// in RDR2 gets the RDR2 action on their own key (the control is what is enabled), but the
// default key is still the one withheld from GMod.
//
// The same list is in README.md: change both.

#pragma once

#include <cstdint>

#include "native_hashes.h"

namespace gr {

struct PassthroughKey {
    int vk;            // Windows virtual-key code withheld from GMod
    uint32_t control;  // RDR2 control action left enabled, 0 for none
    const char* what;
};

inline constexpr PassthroughKey kPassthroughKeys[] = {
    {0x1B /* VK_ESCAPE */, control_hash::INPUT_FRONTEND_PAUSE_ALTERNATE, "RDR2 pause menu"},
    {'P', control_hash::INPUT_FRONTEND_PAUSE, "RDR2 pause menu"},
    {'M', control_hash::INPUT_MAP, "RDR2 map"},
    // Handled by the plugin itself: hands the player to RDR2 and back.
    {0x78 /* VK_F9 */, 0, "hand the player to RDR2 or back to GMod"},
};

inline constexpr int kHandoverVk = 0x78;  // VK_F9

inline constexpr bool IsPassthroughKey(int vk) noexcept {
    for (const PassthroughKey& key : kPassthroughKeys) {
        if (key.vk == vk) return true;
    }
    return false;
}

}  // namespace gr
