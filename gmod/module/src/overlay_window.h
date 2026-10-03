// Shows GMod's captured frame on top of RDR2: a click-through, never-activated, topmost
// window laid exactly over RDR2's client area, whose content is a premultiplied-alpha
// DirectComposition swap chain that Windows' compositor blends over whatever is below.
//
// Why a window of its own and not a hook on RDR2's present: RDR2 can run on Vulkan or
// DX12, and on this machine its folder also holds OptiScaler (as dxgi.dll) and ReShade,
// which hook the same calls. The compositor does not care which API drew the game below,
// so this works with all of them and touches nothing in RDR2's process. It needs RDR2 in
// a window or borderless (its own setting); exclusive fullscreen would hide it. The cost
// is that the overlay is not tied to RDR2's frames: it updates when GMod renders.
//
// Everything runs on a thread of its own (created here, with its own message loop), so
// neither GMod's render thread nor RDR2 ever waits for it.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "frame_mailbox.h"
#include "gpu_share.h"
#include "gr_link.h"
#include "gr_log.h"

namespace gr {

// How the captured frame's alpha is turned into the overlay's.
enum class OverlayAlpha : uint32_t {
    Raw = 0,      // the back buffer's alpha as it is, colour taken as premultiplied. Right
                  //   when overlay.lua makes the HUD and VGUI write alpha (the default)
    Squared = 1,  // the back buffer holds alpha squared (translucent drawing over a cleared
                  //   alpha of 0 blends its alpha channel the same way as its colour): sqrt
    BlackKey = 2, // no usable alpha: brightness is coverage, as if drawn additively on black
    Opaque = 3,   // the whole GMod frame, for checking alignment
    Coverage = 4, // the alpha channel or the brightest colour channel, whichever is more.
                  //   What GMod's frame needs: 3D drawing writes alpha (the viewmodel, with
                  //   depth-to-alpha off), but VGUI and the HUD write colour only, so over a
                  //   black clear their brightness is their coverage
};

class OverlayWindow {
public:
    static OverlayWindow& Get() noexcept {
        static OverlayWindow instance;
        return instance;
    }

    // Main thread, once. The thread runs for the life of the process (the DLL is pinned by
    // D3D9Capture), so there is no Stop.
    // Asked from the GPU path's draw: whether a frame dump is wanted, and where to.
    using DumpRequestFn = bool (*)(char* path, size_t chars);
    void SetDumpSource(DumpRequestFn fn) noexcept { dump_request_ = fn; }

    bool Start(FrameMailbox* mailbox, GpuShare* share) noexcept {
        if (thread_) return true;
        mailbox_ = mailbox;
        share_ = share;
        share_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread_ = CreateThread(nullptr, 0, &OverlayWindow::ThreadMain, this, 0, nullptr);
        return thread_ != nullptr;
    }

    // Lua: whether the overlay should be on screen at all (GMod has RDR2's player). It is
    // still only shown while RDR2 is the window in front.
    void SetWanted(bool on) noexcept { wanted_.store(on, std::memory_order_release); }
    void SetAlpha(OverlayAlpha mode) noexcept { alpha_.store(static_cast<uint32_t>(mode), std::memory_order_release); }

    struct Stats {
        bool running;
        bool shown;
        uint32_t frames;
        uint32_t last_upload_us;
        uint32_t last_age_us;  // capture to present
        uint32_t avg_age_us;   // the same, averaged
        int32_t x, y, w, h;    // where the window is, physical pixels
        uint32_t alpha;
    };
    Stats stats() const noexcept {
        return {running_.load(), shown_.load(), frames_.load(), last_upload_us_.load(), last_age_us_.load(),
                avg_age_out_.load(), x_, y_, w_, h_, alpha_.load()};
    }

private:
    OverlayWindow() = default;

    static DWORD WINAPI ThreadMain(LPVOID param) {
        static_cast<OverlayWindow*>(param)->Run();
        return 0;
    }

    void Run() noexcept {
        // GMod is not DPI aware, so on a scaled display its windows and every rectangle it
        // asks about are in scaled pixels. This thread works in the physical pixels RDR2
        // renders in, or the overlay would be stretched and misplaced.
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!CreateOverlay()) {
            GR_LOG("overlay: could not create the overlay window or its swap chain. No HUD over RDR2");
            return;
        }
        running_.store(true);
        GR_LOG("overlay: window and DirectComposition swap chain ready");

        for (;;) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Place();
            ServeShareRequest();
            if (share_->failed.load(std::memory_order_acquire)) {
                const OverlayFrame* frame = mailbox_->Take();
                if (frame && shown_.load()) Show(*frame);
            } else {
                ShowShared();
            }
            // The render thread sets the event as each copy is finished; the timeout keeps
            // window placement and the CPU path going without it.
            if (share_->wake) {
                WaitForSingleObject(share_->wake, 2);
            } else {
                Sleep(1);
            }
        }
    }

    bool CreateOverlay() noexcept {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &OverlayWindow::Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GarrysRedemptionOverlay";
        RegisterClassExW(&wc);
        // LAYERED + TRANSPARENT: mouse input goes through to RDR2 below. NOACTIVATE: never
        // takes the foreground from RDR2 (the bridge only captures input while RDR2 is in
        // front). NOREDIRECTIONBITMAP: the content is the composition swap chain alone.
        window_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                      WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                  wc.lpszClassName, L"Garry's Redemption overlay", WS_POPUP, 0, 0, 64, 64, nullptr,
                                  nullptr, wc.hInstance, nullptr);
        if (!window_) return false;
        SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &context_))) {
            return false;
        }
        IDXGIDevice* dxgi_device = nullptr;
        if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device)))) {
            return false;
        }
        // The overlay's one draw a frame should not queue behind RDR2's frame on the GPU.
        dxgi_device->SetGPUThreadPriority(7);
        IDXGIAdapter* adapter = nullptr;
        dxgi_device->GetAdapter(&adapter);
        if (adapter) {
            adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory_));
            adapter->Release();
        }
        const bool ok =
            factory_ && SUCCEEDED(DCompositionCreateDevice(dxgi_device, __uuidof(IDCompositionDevice),
                                                           reinterpret_cast<void**>(&dcomp_))) &&
            SUCCEEDED(dcomp_->CreateTargetForHwnd(window_, TRUE, &target_)) && SUCCEEDED(dcomp_->CreateVisual(&visual_)) &&
            SUCCEEDED(target_->SetRoot(visual_));
        dxgi_device->Release();
        return ok;
    }

    // (Re)creates the swap chain at the captured frame's size.
    bool EnsureSwapChain(uint32_t w, uint32_t h) noexcept {
        if (swap_ && swap_w_ == w && swap_h_ == h) return true;
        if (target_view_) {
            target_view_->Release();
            target_view_ = nullptr;
        }
        if (swap_) {
            visual_->SetContent(nullptr);
            swap_->Release();
            swap_ = nullptr;
        }
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = w;
        desc.Height = h;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(factory_->CreateSwapChainForComposition(device_, &desc, nullptr, &swap_))) {
            GR_LOG("overlay: CreateSwapChainForComposition %ux%u failed", w, h);
            return false;
        }
        visual_->SetContent(swap_);
        swap_w_ = w;
        swap_h_ = h;
        scaled_w_ = scaled_h_ = 0;  // the transform depends on the size: redo it
        const size_t bytes = static_cast<size_t>(w) * h * 4;
        if (bytes > scratch_bytes_) {
            uint8_t* grown = static_cast<uint8_t*>(std::realloc(scratch_, bytes));
            if (!grown) return false;
            scratch_ = grown;
            scratch_bytes_ = bytes;
        }
        GR_LOG("overlay: swap chain %ux%u", w, h);
        return true;
    }

    // Keeps the window over RDR2's client area, and on screen only while RDR2 is in front.
    void Place() noexcept {
        const uint64_t now = GetTickCount64();
        if ((!game_ || !IsWindow(game_)) && now >= next_search_ms_) {
            game_ = FindWindowW(L"sgaWindow", nullptr);
            next_search_ms_ = now + 1000;
        }
        bool show = wanted_.load(std::memory_order_acquire) && game_ && GetForegroundWindow() == game_ &&
                    !IsIconic(game_);
        RECT client{};
        POINT origin{0, 0};
        if (show && (!GetClientRect(game_, &client) || !ClientToScreen(game_, &origin))) show = false;
        if (show) {
            // All of RDR2's client area, also the row the plugin keeps off the screen
            // (KeepComposited): GMod's picture is RDR2's size and lies exactly on it.
            const RECT area{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
            const int w = area.right - area.left;
            const int h = area.bottom - area.top;
            if (origin.x != x_ || origin.y != y_ || w != w_ || h != h_ || !shown_.load()) {
                x_ = origin.x;
                y_ = origin.y;
                w_ = w;
                h_ = h;
                SetWindowPos(window_, HWND_TOPMOST, x_, y_, w_, h_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                scaled_w_ = scaled_h_ = 0;
            }
            if (!shown_.load()) {
                shown_.store(true);
                GR_LOG("overlay: shown at (%d, %d), %dx%d", x_, y_, w_, h_);
            }
        } else if (shown_.load()) {
            ShowWindow(window_, SW_HIDE);
            shown_.store(false);
            GR_LOG("overlay: hidden");
        }
    }

    // ---- GPU path (gpu_share.h)

    // Makes the shared textures at the size the render thread asks for.
    void ServeShareRequest() noexcept {
        const uint32_t request = share_->request.load(std::memory_order_acquire);
        if (request == 0 || request == served_request_ || share_->failed.load()) return;
        served_request_ = request;
        const uint32_t w = request >> 16;
        const uint32_t h = request & 0xFFFF;
        // The old set stays alive: D3D9 may still have it open until it sees the new one.
        for (uint32_t i = 0; i < GpuShare::kTextures; ++i) {
            if (old_srv_[i]) old_srv_[i]->Release();
            if (old_texture_[i]) old_texture_[i]->Release();
            old_srv_[i] = srv_[i];
            old_texture_[i] = shared_[i];
            srv_[i] = nullptr;
            shared_[i] = nullptr;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // D3DFMT_A8R8G8B8 on the D3D9 side
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        for (uint32_t i = 0; i < GpuShare::kTextures; ++i) {
            IDXGIResource* resource = nullptr;
            HANDLE handle = nullptr;
            const bool ok = SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &shared_[i])) &&
                            SUCCEEDED(device_->CreateShaderResourceView(shared_[i], nullptr, &srv_[i])) &&
                            SUCCEEDED(shared_[i]->QueryInterface(__uuidof(IDXGIResource),
                                                                 reinterpret_cast<void**>(&resource))) &&
                            SUCCEEDED(resource->GetSharedHandle(&handle));
            if (resource) resource->Release();
            if (!ok) {
                GR_LOG("overlay: could not make shared textures %ux%u, using the CPU path", w, h);
                share_->failed.store(true, std::memory_order_release);
                return;
            }
            share_->handles[i] = handle;
        }
        share_->width = w;
        share_->height = h;
        share_->generation.store(share_->generation.load() + 1, std::memory_order_release);
        GR_LOG("overlay: shared textures %ux%u made", w, h);
    }

    bool EnsurePipeline() noexcept {
        if (pixel_shader_) return true;
        // A triangle that covers the screen, and the frame's pixels turned into
        // premultiplied colour with OverlayAlpha's rules. Compiled at start-up by
        // d3dcompiler_47.dll, which every Windows 10 and 11 has.
        static const char kSource[] = R"(
cbuffer Params : register(b0) { uint mode; uint3 pad; };
Texture2D<float4> frame : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float4 c = frame.Load(int3(pos.xy, 0));
    float a = 1;
    if (mode == 0) a = c.a;
    else if (mode == 1) a = sqrt(c.a);
    else if (mode == 2) a = max(c.r, max(c.g, c.b));
    else if (mode == 4) a = max(c.a, max(c.r, max(c.g, c.b)));
    // Raw: colour stays above alpha where GMod drew additively (the physgun's glow), which
    // the compositor then adds, as GMod would have. The guessed rules clamp it.
    if (mode == 0) return c;
    return float4(min(c.rgb, a), a);
}
)";
        ID3DBlob* vs = nullptr;
        ID3DBlob* ps = nullptr;
        ID3DBlob* errors = nullptr;
        bool ok = SUCCEEDED(D3DCompile(kSource, sizeof(kSource) - 1, "overlay", nullptr, nullptr, "vs", "vs_4_0", 0, 0,
                                       &vs, &errors)) &&
                  SUCCEEDED(D3DCompile(kSource, sizeof(kSource) - 1, "overlay", nullptr, nullptr, "ps", "ps_4_0", 0, 0,
                                       &ps, &errors));
        if (!ok && errors) GR_LOG("overlay: shader: %s", static_cast<const char*>(errors->GetBufferPointer()));
        ok = ok &&
             SUCCEEDED(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_shader_)) &&
             SUCCEEDED(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_shader_));
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (errors) errors->Release();
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 16;
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = ok && SUCCEEDED(device_->CreateBuffer(&cb, nullptr, &params_));
        if (!ok) {
            GR_LOG("overlay: could not build the overlay shader, using the CPU path");
            share_->failed.store(true, std::memory_order_release);
        }
        return ok;
    }

    // Draws the newest finished copy, if it is newer than the last one shown.
    void ShowShared() noexcept {
        const uint32_t ready = share_->ready.load(std::memory_order_acquire);
        if (ready == 0 || ready == shown_ready_ || !shown_.load()) return;
        const uint32_t index = ready & 3;
        if (index >= GpuShare::kTextures || !srv_[index]) return;
        const uint32_t w = share_->width;
        const uint32_t h = share_->height;
        if (!EnsurePipeline() || !EnsureSwapChain(w, h) || !EnsureTarget()) return;
        shown_ready_ = ready;
        const uint32_t start = NowUs();
        ApplyScale();

        const uint32_t mode[4] = {alpha_.load(std::memory_order_acquire), 0, 0, 0};
        context_->UpdateSubresource(params_, 0, nullptr, mode, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        context_->OMSetRenderTargets(1, &target_view_, nullptr);
        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(vertex_shader_, nullptr, 0);
        context_->PSSetShader(pixel_shader_, nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, &params_);
        context_->PSSetShaderResources(0, 1, &srv_[index]);
        context_->Draw(3, 0);
        char dump_path[MAX_PATH];
        if (dump_request_ && dump_request_(dump_path, MAX_PATH)) DumpShared(shared_[index], w, h, dump_path);
        ID3D11ShaderResourceView* none = nullptr;
        context_->PSSetShaderResources(0, 1, &none);
        swap_->Present(0, 0);
        frames_.fetch_add(1, std::memory_order_relaxed);
        last_upload_us_.store(static_cast<uint32_t>(AgeUs(NowUs(), start)), std::memory_order_relaxed);
        const uint32_t age = static_cast<uint32_t>(AgeUs(NowUs(), share_->ready_time_us.load()));
        last_age_us_.store(age, std::memory_order_relaxed);
        // About the last 64 frames, so one reading of gr_overlay_status means something.
        avg_age_us_ = avg_age_us_ == 0 ? age : avg_age_us_ + (static_cast<int32_t>(age) - static_cast<int32_t>(avg_age_us_)) / 64;
        avg_age_out_.store(avg_age_us_, std::memory_order_relaxed);
    }

    // Debugging aid (gr_overlay_dump): the frame as GMod drew it, before any alpha rule, in
    // the same raw format as the CPU path's dump.
    void DumpShared(ID3D11Texture2D* texture, uint32_t w, uint32_t h, const char* path) noexcept {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) return;
        context_->CopyResource(staging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            FILE* f = std::fopen(path, "wb");
            if (f) {
                const uint32_t header[4] = {w, h, 21, 0};
                std::fwrite(header, sizeof(header), 1, f);
                for (uint32_t y = 0; y < h; ++y) {
                    std::fwrite(static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, 1,
                                static_cast<size_t>(w) * 4, f);
                }
                std::fclose(f);
                GR_LOG("overlay: dumped a %ux%u frame to %s", w, h, path);
            }
            context_->Unmap(staging, 0);
        }
        staging->Release();
    }

    bool EnsureTarget() noexcept {
        if (target_view_) return true;
        ID3D11Texture2D* buffer = nullptr;
        if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) return false;
        const bool ok = SUCCEEDED(device_->CreateRenderTargetView(buffer, nullptr, &target_view_));
        buffer->Release();
        return ok;
    }

    void ApplyScale() noexcept {
        if (scaled_w_ == w_ && scaled_h_ == h_) return;
        // GMod renders at its own resolution; stretch to RDR2's if they differ.
        D2D_MATRIX_3X2_F scale{};
        scale._11 = static_cast<float>(w_) / static_cast<float>(swap_w_);
        scale._22 = static_cast<float>(h_) / static_cast<float>(swap_h_);
        visual_->SetTransform(scale);
        dcomp_->Commit();
        scaled_w_ = w_;
        scaled_h_ = h_;
    }

    void Show(const OverlayFrame& frame) noexcept {
        const uint32_t start = NowUs();
        if (!EnsureSwapChain(frame.width, frame.height)) return;
        ApplyScale();

        const uint8_t* pixels = Convert(frame);
        ID3D11Texture2D* buffer = nullptr;
        if (SUCCEEDED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) {
            context_->UpdateSubresource(buffer, 0, nullptr, pixels, frame.width * 4, 0);
            buffer->Release();
            swap_->Present(0, 0);
            frames_.fetch_add(1, std::memory_order_relaxed);
        }
        last_upload_us_.store(static_cast<uint32_t>(AgeUs(NowUs(), start)), std::memory_order_relaxed);
        last_age_us_.store(static_cast<uint32_t>(AgeUs(NowUs(), frame.time_us)), std::memory_order_relaxed);
    }

    const uint8_t* Convert(const OverlayFrame& frame) noexcept {
        const OverlayAlpha mode = static_cast<OverlayAlpha>(alpha_.load(std::memory_order_acquire));
        if (mode == OverlayAlpha::Raw) return frame.pixels;
        if (!sqrt_ready_) {
            for (int i = 0; i < 256; ++i) sqrt_lut_[i] = static_cast<uint8_t>(std::lround(std::sqrt(i / 255.0) * 255.0));
            sqrt_ready_ = true;
        }
        const size_t count = static_cast<size_t>(frame.width) * frame.height;
        const uint8_t* in = frame.pixels;
        uint8_t* out = scratch_;
        for (size_t i = 0; i < count; ++i, in += 4, out += 4) {
            const uint8_t b = in[0], g = in[1], r = in[2];
            uint8_t a = 255;
            if (mode == OverlayAlpha::Squared) {
                a = sqrt_lut_[in[3]];
            } else if (mode == OverlayAlpha::BlackKey || mode == OverlayAlpha::Coverage) {
                a = b > g ? (b > r ? b : r) : (g > r ? g : r);
                if (mode == OverlayAlpha::Coverage && in[3] > a) a = in[3];
            }
            // Premultiplied colour can never exceed its alpha.
            out[0] = b < a ? b : a;
            out[1] = g < a ? g : a;
            out[2] = r < a ? r : a;
            out[3] = a;
        }
        return scratch_;
    }

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        switch (message) {
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            default:
                return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    HANDLE thread_ = nullptr;
    FrameMailbox* mailbox_ = nullptr;
    GpuShare* share_ = nullptr;
    DumpRequestFn dump_request_ = nullptr;
    std::atomic<bool> wanted_{false};
    std::atomic<uint32_t> alpha_{static_cast<uint32_t>(OverlayAlpha::Raw)};
    std::atomic<bool> running_{false};
    std::atomic<bool> shown_{false};
    std::atomic<uint32_t> frames_{0};
    std::atomic<uint32_t> last_upload_us_{0};
    std::atomic<uint32_t> last_age_us_{0};
    std::atomic<uint32_t> avg_age_out_{0};

    // Overlay thread only.
    HWND window_ = nullptr;
    HWND game_ = nullptr;
    uint64_t next_search_ms_ = 0;
    int32_t x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIFactory2* factory_ = nullptr;
    IDCompositionDevice* dcomp_ = nullptr;
    IDCompositionTarget* target_ = nullptr;
    IDCompositionVisual* visual_ = nullptr;
    IDXGISwapChain1* swap_ = nullptr;
    uint32_t swap_w_ = 0, swap_h_ = 0;
    int32_t scaled_w_ = 0, scaled_h_ = 0;
    uint8_t* scratch_ = nullptr;
    size_t scratch_bytes_ = 0;
    uint8_t sqrt_lut_[256] = {};
    ID3D11RenderTargetView* target_view_ = nullptr;
    ID3D11VertexShader* vertex_shader_ = nullptr;
    ID3D11PixelShader* pixel_shader_ = nullptr;
    ID3D11Buffer* params_ = nullptr;
    ID3D11Texture2D* shared_[GpuShare::kTextures] = {};
    ID3D11ShaderResourceView* srv_[GpuShare::kTextures] = {};
    ID3D11Texture2D* old_texture_[GpuShare::kTextures] = {};
    ID3D11ShaderResourceView* old_srv_[GpuShare::kTextures] = {};
    uint32_t served_request_ = 0;
    uint32_t shown_ready_ = 0;
    uint32_t avg_age_us_ = 0;
    bool sqrt_ready_ = false;
};

}  // namespace gr
