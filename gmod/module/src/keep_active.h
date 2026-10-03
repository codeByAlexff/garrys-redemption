// Keeps GMod running at full speed while its window is not in front.
//
// Works around: the engine sleeps 50 ms per frame (20 fps) for as long as it believes it
// is inactive, and GMod has no convar to turn that off. Its only source for that belief
// is WM_ACTIVATE (measured, see docs/DESIGN.md). So the game window is subclassed, and
// while Keep(true) is in force WM_ACTIVATE(WA_INACTIVE) never reaches the engine.
//
// Has no GMod dependency so that protocol/tests can run it against a window of its own.
// Everything here must be called on the thread that owns the window: in GMod that is the
// main thread, which is also the one that runs Lua.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cwchar>

namespace gr {

class KeepActive {
public:
    static KeepActive& Get() noexcept {
        static KeepActive instance;
        return instance;
    }

    // Subclasses this process's top-level window of the given class. "Valve001" is the
    // Source engine's game window.
    bool Install(const wchar_t* window_class = L"Valve001") noexcept {
        if (window_) return true;
        Search search{window_class, nullptr};
        EnumWindows(&KeepActive::MatchWindow, reinterpret_cast<LPARAM>(&search));
        return search.found && InstallOn(search.found);
    }

    bool InstallOn(HWND window) noexcept {
        if (window_) return window_ == window;
        // From another thread a message could arrive between swapping the procedure and
        // storing the one it replaced. Refusing is better than that race.
        if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return false;
        window_ = window;
        unicode_ = IsWindowUnicode(window) != FALSE;
        engine_active_ = GetForegroundWindow() == window;
        keeping_ = false;
        original_ = SwapProc(&KeepActive::Proc);
        if (!original_) {
            window_ = nullptr;
            return false;
        }
        return true;
    }

    // true: the engine is told it is active now and is not told when it stops being so.
    // false: the engine is told the truth again.
    void Keep(bool on) noexcept {
        if (!window_ || keeping_ == on) return;
        keeping_ = on;
        if (on) {
            if (!engine_active_) Send(WA_ACTIVE);
        } else if (engine_active_ && GetForegroundWindow() != window_) {
            Send(WA_INACTIVE);
        }
    }

    // Returns false if the window procedure could not be put back because someone else
    // subclassed the window after us and now holds a pointer to Proc. Proc then stays in
    // the chain as a pass-through, and this module is pinned so that pointer stays valid
    // after GMod unloads the module.
    bool Remove() noexcept {
        if (!window_) return true;
        Keep(false);
        if (CurrentProc() != &KeepActive::Proc) {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&KeepActive::Proc), &self);
            return false;
        }
        SwapProc(original_);
        window_ = nullptr;
        original_ = nullptr;
        return true;
    }

    bool installed() const noexcept { return window_ != nullptr; }
    HWND window() const noexcept { return window_; }
    bool keeping() const noexcept { return keeping_; }
    // What the engine was last told, which is what decides its frame rate.
    bool engine_active() const noexcept { return engine_active_; }
    uint32_t swallowed() const noexcept { return swallowed_; }

private:
    struct Search {
        const wchar_t* window_class;
        HWND found;
    };

    KeepActive() = default;

    static BOOL CALLBACK MatchWindow(HWND window, LPARAM param) noexcept {
        Search* search = reinterpret_cast<Search*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        wchar_t name[64] = L"";
        GetClassNameW(window, name, 64);
        if (std::wcscmp(name, search->window_class) != 0) return TRUE;
        search->found = window;
        return FALSE;
    }

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        KeepActive& self = Get();
        const WNDPROC original = self.original_;
        const bool unicode = self.unicode_;
        if (message == WM_ACTIVATE) {
            if (LOWORD(wparam) == WA_INACTIVE && self.keeping_) {
                ++self.swallowed_;
                return 0;
            }
            self.engine_active_ = LOWORD(wparam) != WA_INACTIVE;
        } else if (message == WM_NCDESTROY) {
            // The last message a window gets. Nothing is left to restore.
            self.window_ = nullptr;
            self.original_ = nullptr;
            self.keeping_ = false;
        }
        return unicode ? CallWindowProcW(original, window, message, wparam, lparam)
                       : CallWindowProcA(original, window, message, wparam, lparam);
    }

    // The A and W calls are not interchangeable: asking for the procedure of a window of
    // the other kind returns a thunk handle instead of the pointer.
    WNDPROC SwapProc(WNDPROC to) const noexcept {
        const LONG_PTR value = reinterpret_cast<LONG_PTR>(to);
        return reinterpret_cast<WNDPROC>(unicode_ ? SetWindowLongPtrW(window_, GWLP_WNDPROC, value)
                                                  : SetWindowLongPtrA(window_, GWLP_WNDPROC, value));
    }

    WNDPROC CurrentProc() const noexcept {
        return reinterpret_cast<WNDPROC>(unicode_ ? GetWindowLongPtrW(window_, GWLP_WNDPROC)
                                                  : GetWindowLongPtrA(window_, GWLP_WNDPROC));
    }

    // Through the window, not straight to original_, so a subclass above this one sees it.
    void Send(WPARAM activation) const noexcept {
        if (unicode_) {
            SendMessageW(window_, WM_ACTIVATE, activation, 0);
        } else {
            SendMessageA(window_, WM_ACTIVATE, activation, 0);
        }
    }

    HWND window_ = nullptr;
    WNDPROC original_ = nullptr;
    bool unicode_ = true;
    bool keeping_ = false;
    bool engine_active_ = false;
    uint32_t swallowed_ = 0;
};

}  // namespace gr
