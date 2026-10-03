// keep_active.h against a stand-in for the engine's window: a hidden top-level window
// whose procedure records what WM_ACTIVATE told it, the way the engine does.

#include <thread>

#include "gr_test.h"
#include "keep_active.h"

namespace {

struct FakeEngine {
    bool active = false;
    int activations = 0;
    int deactivations = 0;
};

FakeEngine g_engine;
int g_above_calls = 0;
WNDPROC g_below = nullptr;

void Record(WPARAM wparam) {
    g_engine.active = LOWORD(wparam) != WA_INACTIVE;
    ++(g_engine.active ? g_engine.activations : g_engine.deactivations);
}

LRESULT CALLBACK EngineProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ACTIVATE) {
        Record(wparam);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK EngineProcA(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ACTIVATE) {
        Record(wparam);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

// Another mod subclassing the same window after us.
LRESULT CALLBACK AboveProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    ++g_above_calls;
    return CallWindowProcW(g_below, window, message, wparam, lparam);
}

constexpr wchar_t kClassW[] = L"GrTestEngineW";
constexpr char kClassA[] = "GrTestEngineA";

// Never shown, so it is never the foreground window and the tests cannot steal focus.
HWND MakeEngineWindow(bool unicode) {
    g_engine = FakeEngine{};
    if (unicode) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = EngineProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassW;
        RegisterClassW(&wc);  // fails harmlessly from the second test on
        return CreateWindowExW(0, kClassW, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    }
    WNDCLASSA wc{};
    wc.lpfnWndProc = EngineProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kClassA;
    RegisterClassA(&wc);
    return CreateWindowExA(0, kClassA, "", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

void Activate(HWND window, bool active) {
    SendMessageW(window, WM_ACTIVATE, active ? WA_ACTIVE : WA_INACTIVE, 0);
}

void SwallowsOnlyWhileKeeping(bool unicode) {
    gr::KeepActive& keep = gr::KeepActive::Get();
    const HWND window = MakeEngineWindow(unicode);
    CHECK(window != nullptr);
    const LONG_PTR proc_before = GetWindowLongPtrW(window, GWLP_WNDPROC);
    CHECK(keep.Install(unicode ? kClassW : L"GrTestEngineA"));
    CHECK(keep.installed());
    const uint32_t swallowed_before = keep.swallowed();

    // Installed but not keeping: the engine hears everything.
    Activate(window, true);
    CHECK(g_engine.active);
    Activate(window, false);
    CHECK(!g_engine.active);
    CHECK(!keep.engine_active());

    // Keeping: the engine is woken at once, and losing the foreground does not reach it.
    keep.Keep(true);
    CHECK(g_engine.active);
    CHECK(g_engine.activations == 2);
    Activate(window, false);
    Activate(window, false);
    CHECK(g_engine.active);
    CHECK(g_engine.deactivations == 1);
    CHECK(keep.swallowed() == swallowed_before + 2);
    Activate(window, true);
    CHECK(g_engine.activations == 3);

    // Released: the window is not really in front, and the engine is told so.
    keep.Keep(false);
    CHECK(!g_engine.active);
    CHECK(g_engine.deactivations == 2);

    CHECK(keep.Remove());
    CHECK(!keep.installed());
    CHECK(GetWindowLongPtrW(window, GWLP_WNDPROC) == proc_before);
    Activate(window, true);
    CHECK(g_engine.active);
    DestroyWindow(window);
}

}  // namespace

GR_TEST(keep_active_swallows_deactivation_only_while_keeping) { SwallowsOnlyWhileKeeping(true); }

GR_TEST(keep_active_works_on_an_ansi_window) { SwallowsOnlyWhileKeeping(false); }

GR_TEST(keep_active_does_not_wake_an_engine_that_is_awake) {
    gr::KeepActive& keep = gr::KeepActive::Get();
    const HWND window = MakeEngineWindow(true);
    CHECK(keep.InstallOn(window));
    Activate(window, true);
    keep.Keep(true);
    keep.Keep(true);
    CHECK(g_engine.activations == 1);
    CHECK(keep.Remove());
    CHECK(!g_engine.active);
    DestroyWindow(window);
}

GR_TEST(keep_active_removes_itself_when_keeping) {
    gr::KeepActive& keep = gr::KeepActive::Get();
    const HWND window = MakeEngineWindow(true);
    CHECK(keep.InstallOn(window));
    keep.Keep(true);
    CHECK(g_engine.active);
    // Remove() alone must not leave the engine believing it is in front for ever.
    CHECK(keep.Remove());
    CHECK(!g_engine.active);
    CHECK(!keep.keeping());
    DestroyWindow(window);
}

GR_TEST(keep_active_stays_as_a_pass_through_when_subclassed_on_top) {
    gr::KeepActive& keep = gr::KeepActive::Get();
    const HWND window = MakeEngineWindow(true);
    CHECK(keep.InstallOn(window));
    g_above_calls = 0;
    g_below = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AboveProc)));

    keep.Keep(true);
    CHECK(g_engine.active);
    CHECK(g_above_calls > 0);  // the wake-up went through the window, not around the other mod
    Activate(window, false);
    CHECK(g_engine.active);

    // Our procedure cannot be unhooked from under the other one. It must stop swallowing
    // and keep forwarding.
    CHECK(!keep.Remove());
    CHECK(keep.installed());
    CHECK(!keep.keeping());
    CHECK(!g_engine.active);
    Activate(window, true);
    CHECK(g_engine.active);
    Activate(window, false);
    CHECK(!g_engine.active);

    DestroyWindow(window);
    CHECK(!keep.installed());
}

GR_TEST(keep_active_forgets_a_destroyed_window) {
    gr::KeepActive& keep = gr::KeepActive::Get();
    HWND window = MakeEngineWindow(true);
    CHECK(keep.InstallOn(window));
    keep.Keep(true);
    DestroyWindow(window);
    CHECK(!keep.installed());
    CHECK(!keep.keeping());
    CHECK(keep.Remove());

    // And can be installed again, as happens when GMod reloads its Lua state.
    window = MakeEngineWindow(true);
    CHECK(keep.InstallOn(window));
    CHECK(keep.Remove());
    DestroyWindow(window);
}

GR_TEST(keep_active_refuses_a_missing_window_and_a_foreign_thread) {
    gr::KeepActive& keep = gr::KeepActive::Get();
    CHECK(!keep.Install(L"GrTestNoSuchWindowClass"));
    CHECK(!keep.installed());

    const HWND window = MakeEngineWindow(true);
    bool installed_from_thread = true;
    std::thread([&] { installed_from_thread = keep.InstallOn(window); }).join();
    CHECK(!installed_from_thread);
    CHECK(!keep.installed());
    DestroyWindow(window);
}
