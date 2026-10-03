// Measures how bright and what colour RDR2's picture is, so GMod's props and viewmodel can
// be lit to match it (gr/overlay.lua).
//
// The light the plugin sends is worked out from the clock (rdr2/src/light_sync.h) and knows
// nothing of weather, fog, RDR2's exposure or lamps: props came out too bright or too dark
// for the scene around them (user report). There is no hook in RDR2 to read its frame, so a
// thread here copies a band of RDR2's window off the screen, shrunk to 16 by 8 pixels, a few
// times a second, and keeps the average as a linear colour. The band is the middle of the
// picture left of the viewmodel: mostly ground and things at the player's level.
//
// Off the game's threads, and a small StretchBlt: nothing in either game waits for it.

#pragma once

#include <atomic>
#include <cmath>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace gr {

class SceneProbe {
public:
    static SceneProbe& Get() noexcept {
        static SceneProbe instance;
        return instance;
    }

    void Start() noexcept {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { Run(); });
    }

    void Stop() noexcept {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    // The last measurement, linear 0 to 1 per channel. false if there is none newer than 2 s
    // (RDR2 is not in front, or the probe is off).
    bool Color(float& r, float& g, float& b) const noexcept {
        const ULONGLONG at = at_ms_.load(std::memory_order_acquire);
        if (at == 0 || GetTickCount64() - at > 2000) return false;
        r = r_.load(std::memory_order_relaxed);
        g = g_.load(std::memory_order_relaxed);
        b = b_.load(std::memory_order_relaxed);
        return true;
    }

private:
    static constexpr int kW = 16;
    static constexpr int kH = 8;
    static constexpr DWORD kEveryMs = 350;

    static float Linear(unsigned char c) noexcept { return std::pow(static_cast<float>(c) / 255.0f, 2.2f); }

    void Run() noexcept {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = kW;
        info.bmiHeader.biHeight = -kH;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        unsigned char* pixels = nullptr;
        const HDC mem = CreateCompatibleDC(nullptr);
        const HBITMAP bitmap = CreateDIBSection(mem, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (!mem || !bitmap || !pixels) {
            if (bitmap) DeleteObject(bitmap);
            if (mem) DeleteDC(mem);
            running_.store(false);
            return;
        }
        const HGDIOBJ old = SelectObject(mem, bitmap);
        SetStretchBltMode(mem, HALFTONE);  // averages the pixels it shrinks, instead of picking some
        SetBrushOrgEx(mem, 0, 0, nullptr);

        while (running_.load(std::memory_order_acquire)) {
            Sleep(kEveryMs);
            const HWND game = FindWindowW(L"sgaWindow", nullptr);
            if (!game || GetForegroundWindow() != game || IsIconic(game)) continue;
            RECT client{};
            POINT origin{0, 0};
            if (!GetClientRect(game, &client) || !ClientToScreen(game, &origin)) continue;
            const int w = client.right - client.left;
            const int h = client.bottom - client.top;
            if (w < 64 || h < 64) continue;
            const HDC screen = GetDC(nullptr);
            if (!screen) continue;
            const BOOL ok = StretchBlt(mem, 0, 0, kW, kH, screen, origin.x + w * 5 / 100, origin.y + h * 42 / 100,
                                       w * 55 / 100, h * 28 / 100, SRCCOPY);
            ReleaseDC(nullptr, screen);
            if (!ok) continue;
            GdiFlush();
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            for (int i = 0; i < kW * kH; ++i) {
                b += Linear(pixels[i * 4 + 0]);
                g += Linear(pixels[i * 4 + 1]);
                r += Linear(pixels[i * 4 + 2]);
            }
            const float n = static_cast<float>(kW * kH);
            r_.store(r / n, std::memory_order_relaxed);
            g_.store(g / n, std::memory_order_relaxed);
            b_.store(b / n, std::memory_order_relaxed);
            at_ms_.store(GetTickCount64(), std::memory_order_release);
        }
        SelectObject(mem, old);
        DeleteObject(bitmap);
        DeleteDC(mem);
    }

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<float> r_{0.0f};
    std::atomic<float> g_{0.0f};
    std::atomic<float> b_{0.0f};
    std::atomic<ULONGLONG> at_ms_{0};
};

}  // namespace gr
