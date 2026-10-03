// The GPU path for overlay frames: GMod's back buffer is copied on the GPU into textures the
// overlay's Direct3D 11 device owns, so a frame never comes back to the CPU.
//
// Works around: reading the back buffer back to system memory made the driver wait inside
// GMod's Present for the transfer (measured 8 to 26 ms a frame, GMod down from 150 to
// 35-100 fps). Direct3D 11 makes the textures with a shared handle; GMod's Direct3D 9
// device opens them by passing that handle to CreateTexture (how OBS captures D3D9 games),
// and StretchRect copies into them. If the D3D9 device refuses the handle, `failed` is set
// and the CPU readback (FrameMailbox) is used instead.
//
// Who writes what: the render thread writes `request`, `failed` and `ready`; the overlay
// thread writes `handles`, `width`, `height` and then `generation`.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>

namespace gr {

struct GpuShare {
    static constexpr uint32_t kTextures = 3;

    // Render thread: the size it needs, (width << 16) | height. 0 = nothing yet.
    std::atomic<uint32_t> request{0};
    // Render thread: opening the shared textures failed; use the CPU path for good.
    std::atomic<bool> failed{false};
    // Render thread: the newest finished copy, (serial << 2) | texture index. 0 = none yet.
    std::atomic<uint32_t> ready{0};
    std::atomic<uint32_t> ready_time_us{0};  // when that frame was presented
    // Made by the overlay thread's Start before the thread runs: the render thread sets it
    // when `ready` changes, so the overlay draws at once instead of on its next poll.
    HANDLE wake = nullptr;

    // Overlay thread: textures of width x height. `generation` changes after the rest is
    // written, and is 0 until the first set exists.
    HANDLE handles[kTextures] = {};
    uint32_t width = 0;
    uint32_t height = 0;
    std::atomic<uint32_t> generation{0};
};

}  // namespace gr
