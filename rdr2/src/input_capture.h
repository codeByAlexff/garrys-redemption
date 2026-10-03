// Captures the keyboard and mouse for the GMod player and fills GrInput.
//
// Keys and buttons are read with GetAsyncKeyState, which reports the physical state no
// matter which window consumes the key messages.
//
// Mouse movement has to be raw counts: the cursor is hidden and pinned while the game is
// in front, so cursor positions say nothing. A process can only have one raw-input
// registration for the mouse, and the game may hold it, so the plugin does not register a
// window of its own. It watches the WM_INPUT messages the game's window thread takes off
// its queue (a WH_GETMESSAGE hook on that one thread) and reads each one before the game
// does; reading does not consume it. Only if nothing in the process has registered the
// mouse does the plugin register it, for the game's own window.
//
// A hook rather than a window subclass because the plugin can be unloaded in mid-game
// (Script Hook's CTRL+R): a hook procedure returns before the message is dispatched, so
// it is never left on the stack underneath the code that unloads the DLL.
//
// Has no dependency on Script Hook, so protocol/tests can run it against a window of its
// own. Fill() is called on the script thread; the hook runs on the window's thread; the
// totals they share are atomics.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "gr_protocol.h"
#include "input_passthrough.h"

namespace gr {

class InputCapture {
public:
    enum class MouseSource { None, GameRegistration, OwnRegistration };

    using KeyStateFn = SHORT(WINAPI*)(int);

    static InputCapture& Get() noexcept {
        static InputCapture instance;
        return instance;
    }

    // Finds this process's main window and starts watching its mouse input. False if the
    // process has no visible top-level window yet: call again later.
    bool Install() noexcept {
        if (window_) return true;
        Search search{};
        EnumWindows(&InputCapture::MatchWindow, reinterpret_cast<LPARAM>(&search));
        return search.best && InstallOn(search.best);
    }

    bool InstallOn(HWND window) noexcept {
        if (window_) return window_ == window;
        const DWORD thread = GetWindowThreadProcessId(window, nullptr);
        if (!thread) return false;
        hook_ = SetWindowsHookExW(WH_GETMESSAGE, &InputCapture::HookProc, nullptr, thread);
        if (!hook_) return false;
        window_ = window;
        thread_ = thread;

        HWND target = nullptr;
        if (MouseRegistered(target)) {
            mouse_source_ = MouseSource::GameRegistration;
            raw_target_ = target;
        } else {
            // Works around: a game that reads the mouse some other way registers nothing,
            // and then no WM_INPUT would ever arrive. Flags 0: only while in front.
            RAWINPUTDEVICE device{kUsagePageGeneric, kUsageMouse, 0, window};
            if (RegisterRawInputDevices(&device, 1, sizeof(device))) {
                mouse_source_ = MouseSource::OwnRegistration;
                raw_target_ = window;
            }
        }
        return true;
    }

    // Returns false if the window's thread is stuck inside the hook procedure. The DLL
    // must then not be unloaded, and has been pinned.
    bool Remove() noexcept {
        if (!window_) return true;
        if (mouse_source_ == MouseSource::OwnRegistration) {
            RAWINPUTDEVICE device{kUsagePageGeneric, kUsageMouse, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&device, 1, sizeof(device));
        }
        UnhookWindowsHookEx(hook_);
        // No call of HookProc starts after the unhook, but the window's thread may be in
        // the middle of one. Give it a moment to leave. (Asking that thread to answer a
        // message does not work here: during a script reload it is blocked, seen in-game.)
        bool clear = GetCurrentThreadId() == thread_;
        for (int waited_ms = 0; !clear && waited_ms < 100; ++waited_ms) {
            Sleep(1);
            clear = in_hook_.load() == 0;
        }
        if (!clear) {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&InputCapture::HookProc), &self);
        }
        hook_ = nullptr;
        window_ = nullptr;
        raw_target_ = nullptr;
        thread_ = 0;
        mouse_source_ = MouseSource::None;
        return clear;
    }

    bool installed() const noexcept { return window_ != nullptr; }
    HWND window() const noexcept { return window_; }
    HWND raw_target() const noexcept { return raw_target_; }
    MouseSource mouse_source() const noexcept { return mouse_source_; }
    // True while the game's window is the one the user is typing into.
    bool focused() const noexcept { return window_ && GetForegroundWindow() == window_; }
    // WM_INPUT messages seen, and how many of them were mouse packets that could be read.
    // The running mouse totals, as Fill writes them (player_sync.h predicts the look with them).
    int32_t mouse_total_x() const noexcept { return mouse_x_.load(std::memory_order_relaxed); }
    int32_t mouse_total_y() const noexcept { return mouse_y_.load(std::memory_order_relaxed); }
    uint32_t raw_messages() const noexcept { return raw_messages_.load(std::memory_order_relaxed); }
    uint32_t mouse_packets() const noexcept { return mouse_packets_.load(std::memory_order_relaxed); }

    // Writes the whole block. With `capture` false every key reads as up and the flag is
    // clear; the mouse totals keep their values, so the guest sees no jump when capture
    // comes back.
    void Fill(GrInput& out, bool capture, KeyStateFn key_state = &GetAsyncKeyState) const noexcept {
        std::memset(out.keys, 0, sizeof(out.keys));
        if (capture) {
            // 0 is not a key and 255 is reserved.
            for (int vk = 1; vk < 255; ++vk) {
                if (IsPassthroughKey(vk)) continue;
                if (key_state(vk) & 0x8000) out.keys[vk >> 3] |= static_cast<uint8_t>(1u << (vk & 7));
            }
        }
        out.mouse_dx = mouse_x_.load(std::memory_order_relaxed);
        out.mouse_dy = mouse_y_.load(std::memory_order_relaxed);
        out.wheel = wheel_.load(std::memory_order_relaxed);
        out.cursor_x = 0;
        out.cursor_y = 0;
        out.screen_w = 0;
        out.screen_h = 0;
        if (window_) {
            POINT cursor{};
            RECT client{};
            if (GetCursorPos(&cursor) && ScreenToClient(window_, &cursor)) {
                out.cursor_x = cursor.x;
                out.cursor_y = cursor.y;
            }
            if (GetClientRect(window_, &client)) {
                out.screen_w = static_cast<uint32_t>(client.right - client.left);
                out.screen_h = static_cast<uint32_t>(client.bottom - client.top);
            }
        }
        out.flags = capture ? GR_INPUT_CAPTURED : 0;
    }

    // One raw mouse packet. Public for the tests; in the game only the hook calls it.
    void OnRawMouse(const RAWMOUSE& mouse) noexcept {
        if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
            // Tablets, remote desktop and injected absolute moves report a position, as a
            // fraction (0..65535) of the screen or of the whole desktop. Turn it into
            // pixels so that it moves the view about as far as a mouse count would.
            const bool desktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const LONG x = MulDiv(mouse.lLastX, GetSystemMetrics(desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), 65535);
            const LONG y = MulDiv(mouse.lLastY, GetSystemMetrics(desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN), 65535);
            if (have_absolute_) {
                Add(mouse_x_, x - absolute_x_);
                Add(mouse_y_, y - absolute_y_);
            }
            absolute_x_ = x;
            absolute_y_ = y;
            have_absolute_ = true;
        } else {
            Add(mouse_x_, mouse.lLastX);
            Add(mouse_y_, mouse.lLastY);
        }
        if (mouse.usButtonFlags & RI_MOUSE_WHEEL) Add(wheel_, static_cast<SHORT>(mouse.usButtonData));
        mouse_packets_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    static constexpr USHORT kUsagePageGeneric = 0x01;
    static constexpr USHORT kUsageMouse = 0x02;

    struct Search {
        HWND best = nullptr;
        long area = 0;
    };

    InputCapture() = default;

    // The totals wrap by design (GrInput): add as unsigned so the wrap is defined.
    static void Add(std::atomic<int32_t>& total, LONG delta) noexcept {
        total.store(static_cast<int32_t>(static_cast<uint32_t>(total.load(std::memory_order_relaxed)) +
                                         static_cast<uint32_t>(delta)),
                    std::memory_order_relaxed);
    }

    // The biggest visible, unowned top-level window of this process: the game window, not
    // a splash screen, a tool window or a hidden helper.
    static BOOL CALLBACK MatchWindow(HWND window, LPARAM param) noexcept {
        Search* search = reinterpret_cast<Search*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
        RECT client{};
        if (!GetClientRect(window, &client)) return TRUE;
        const long area = client.right * client.bottom;
        if (area > search->area) {
            search->best = window;
            search->area = area;
        }
        return TRUE;
    }

    static bool MouseRegistered(HWND& target) noexcept {
        RAWINPUTDEVICE devices[16];
        UINT count = 16;
        const UINT n = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
        if (n == static_cast<UINT>(-1)) return true;  // more than 16: assume, and leave alone
        for (UINT i = 0; i < n; ++i) {
            if (devices[i].usUsagePage == kUsagePageGeneric && devices[i].usUsage == kUsageMouse) {
                target = devices[i].hwndTarget;
                return true;
            }
        }
        return false;
    }

    static LRESULT CALLBACK HookProc(int code, WPARAM wparam, LPARAM lparam) noexcept {
        InputCapture& self = Get();
        self.in_hook_.fetch_add(1);
        // PM_REMOVE only: a message that is merely peeked at would be counted twice.
        if (code == HC_ACTION && wparam == PM_REMOVE) {
            const MSG* message = reinterpret_cast<const MSG*>(lparam);
            if (message->message == WM_INPUT) self.OnRawInput(reinterpret_cast<HRAWINPUT>(message->lParam));
        }
        const LRESULT result = CallNextHookEx(nullptr, code, wparam, lparam);
        self.in_hook_.fetch_sub(1);
        return result;
    }

    void OnRawInput(HRAWINPUT handle) noexcept {
        raw_messages_.fetch_add(1, std::memory_order_relaxed);
        // A mouse packet always fits. Anything bigger (some HID device) fails the call and
        // is not ours to read anyway.
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
        if (raw.header.dwType == RIM_TYPEMOUSE) OnRawMouse(raw.data.mouse);
    }

    HWND window_ = nullptr;
    HWND raw_target_ = nullptr;
    HHOOK hook_ = nullptr;
    DWORD thread_ = 0;
    MouseSource mouse_source_ = MouseSource::None;
    std::atomic<int32_t> mouse_x_{0};
    std::atomic<int32_t> mouse_y_{0};
    std::atomic<int32_t> wheel_{0};
    std::atomic<uint32_t> raw_messages_{0};
    std::atomic<uint32_t> mouse_packets_{0};
    std::atomic<int> in_hook_{0};  // calls of HookProc in progress
    // Only touched on the window's thread.
    LONG absolute_x_ = 0;
    LONG absolute_y_ = 0;
    bool have_absolute_ = false;
};

}  // namespace gr
