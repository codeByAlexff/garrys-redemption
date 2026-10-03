// Makes GMod a background task that always draws at RDR2's size.
//
// Size: the overlay shows GMod's frame over RDR2's client area, so GMod has to render exactly
// as many pixels as RDR2 shows. It used to be started at a fixed size (-w 1920 -h 1080), and
// when RDR2's resolution was changed GMod's HUD was stretched and no longer where RDR2's
// picture was (user report at 3840x2160). Now, whenever RDR2's client size (GrInput.screen_w
// and screen_h, which the plugin measures) differs from GMod's back buffer, GMod is told to
// change its video mode, the way a user would from the console: `mat_setvideomode W H 1`,
// handed to the game's own window as WM_COPYDATA (Lua may not run that command). The capture
// follows by itself: Reset lets go of the old surfaces and new shared textures are made.
//
// Hidden: GMod's window is of no use while RDR2 shows its picture, and it sat on the desktop
// and in the taskbar. It is hidden for as long as RDR2's window exists once the two have
// been linked, and shown again when RDR2 is gone, so GMod can still be used and closed.
// The engine renders and simulates the same hidden as shown (measured, docs/DESIGN.md).
//
// Main thread only (the thread that owns the window and runs Lua).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gr_log.h"

namespace gr {

class WindowFit {
public:
    static WindowFit& Get() noexcept {
        static WindowFit instance;
        return instance;
    }

    void SetWanted(bool hide, bool fit, bool quit_with_host) noexcept {
        want_hide_ = hide;
        want_fit_ = fit;
        quit_with_host_ = quit_with_host;
    }

    // Once a frame. `window`: GMod's game window (null: not found yet). `host_w`, `host_h`:
    // RDR2's client size, 0 if not known. `back_w`, `back_h`: GMod's back buffer as last
    // captured, 0 if not known.
    void Tick(HWND window, bool connected, uint32_t host_w, uint32_t host_h, uint32_t back_w, uint32_t back_h,
              uint64_t now_ms) noexcept {
        if (!window) return;
        if (connected) linked_once_ = true;
        if (now_ms >= next_look_ms_) {
            next_look_ms_ = now_ms + 500;
            host_window_ = FindWindowW(L"sgaWindow", nullptr) != nullptr;
        }
        Hide(window, want_hide_ && linked_once_ && host_window_);
        // The two games belong together: RDR2 gone for good after they were linked, and GMod
        // quits rather than be left behind as a window nobody asked for. Five seconds, because
        // RDR2 makes a new window when its display mode changes.
        if (linked_once_ && !host_window_ && quit_with_host_) {
            if (gone_since_ms_ == 0) gone_since_ms_ = now_ms;
            if (now_ms - gone_since_ms_ > 5000 && !quit_sent_) {
                quit_sent_ = true;
                GR_LOG("window: RDR2 has closed. Closing GMod too (gr_quit_with_rdr2 0 keeps it)");
                char command[] = "quit";
                COPYDATASTRUCT data{0, sizeof(command), command};
                SendMessageA(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
            }
        } else {
            gone_since_ms_ = 0;
        }
        if (want_fit_ && connected) Fit(window, host_w, host_h, back_w, back_h, now_ms);
    }

    bool hidden() const noexcept { return hidden_; }
    uint32_t asked_w() const noexcept { return asked_w_; }
    uint32_t asked_h() const noexcept { return asked_h_; }

private:
    WindowFit() = default;

    void Hide(HWND window, bool hide) noexcept {
        if (hide) {
            // Also when it is already ours to hide: a video mode change shows the window again.
            if (IsWindowVisible(window)) {
                // Works around: a minimised GMod presents no frames at all (seen: 0 presents,
                // no HUD over RDR2), and hiding a minimised window leaves it minimised.
                if (IsIconic(window)) ShowWindow(window, SW_SHOWNOACTIVATE);
                ShowWindow(window, SW_HIDE);
                if (!hidden_) GR_LOG("window: GMod's window hidden while RDR2 runs (gr_hide_window 0 keeps it)");
            }
            hidden_ = true;
            if (IsIconic(window)) ShowWindow(window, SW_SHOWNOACTIVATE);
        } else if (hidden_) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            hidden_ = false;
            GR_LOG("window: GMod's window shown again");
        }
    }

    void Fit(HWND window, uint32_t host_w, uint32_t host_h, uint32_t back_w, uint32_t back_h, uint64_t now_ms) noexcept {
        // Source's smallest mode is 640x480; and nothing to compare with before the first capture.
        if (host_w < 640 || host_h < 480 || back_w == 0 || back_h == 0) return;
        if (host_w == back_w && host_h == back_h) {
            pending_w_ = pending_h_ = 0;
            asked_w_ = asked_h_ = 0;
            return;
        }
        // The same size seen for half a second (a window being dragged larger passes through
        // many), and a mode change given time to happen before it is asked for again.
        if (host_w != pending_w_ || host_h != pending_h_) {
            pending_w_ = host_w;
            pending_h_ = host_h;
            pending_since_ms_ = now_ms;
            return;
        }
        if (now_ms - pending_since_ms_ < 500 || now_ms < next_ask_ms_) return;
        // A size GMod did not take (it has its own limits) is not asked for over and over:
        // every try is a hitch. The overlay stretches GMod's frame to RDR2's instead.
        if (host_w == asked_w_ && host_h == asked_h_) return;
        next_ask_ms_ = now_ms + 5000;
        asked_w_ = host_w;
        asked_h_ = host_h;
        char command[64];
        std::snprintf(command, sizeof(command), "mat_setvideomode %u %u 1", host_w, host_h);
        COPYDATASTRUCT data{0, static_cast<DWORD>(std::strlen(command) + 1), command};
        SendMessageA(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
        GR_LOG("window: RDR2 shows %ux%u and GMod draws %ux%u: asked GMod for %ux%u", host_w, host_h, back_w, back_h,
               host_w, host_h);
    }

    bool quit_with_host_ = true;
    bool quit_sent_ = false;
    uint64_t gone_since_ms_ = 0;
    bool want_hide_ = true;
    bool want_fit_ = true;
    bool linked_once_ = false;
    bool host_window_ = false;
    bool hidden_ = false;
    uint64_t next_look_ms_ = 0;
    uint64_t next_ask_ms_ = 0;
    uint64_t pending_since_ms_ = 0;
    uint32_t pending_w_ = 0, pending_h_ = 0;
    uint32_t asked_w_ = 0, asked_h_ = 0;
};

}  // namespace gr
