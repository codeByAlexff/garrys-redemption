// Captures GMod's finished frame for the overlay: a hook on Direct3D 9's Present reads the
// back buffer back into system memory and posts it to a FrameMailbox.
//
// GMod draws with plain Direct3D 9 (shaderapidx9.dll imports Direct3DCreate9, checked with
// dumpbin). The game's device is found in shaderapidx9.dll's data (FindGameDevice), and
// the entries of its vtable are patched. Both IDirect3DDevice9::Present
// and IDirect3DSwapChain9::Present are patched; Reset is patched to let go of the
// default-pool surface first, without which Reset fails (a resolution change would lose
// the device).
//
// The hooks stay for the life of the process (the DLL is pinned once they are in): a
// Present already running on the render thread when the module unloaded would otherwise
// return into freed code. While capture is off they only forward the call.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "frame_mailbox.h"
#include "gpu_share.h"
#include "gr_link.h"
#include "gr_log.h"

namespace gr {

class D3D9Capture {
public:
    static D3D9Capture& Get() noexcept {
        static D3D9Capture instance;
        return instance;
    }

    // Main thread. Returns false if the hooks could not be put in yet (the game's device
    // does not exist before its window does); call again later.
    bool Install() noexcept {
        if (installed_) return true;
        IDirect3DDevice9* device = FindGameDevice();
        if (!device) return false;
        void** device_vtable = *reinterpret_cast<void***>(device);
        void** swapchain_vtable = nullptr;
        IDirect3DSwapChain9* chain = nullptr;
        if (SUCCEEDED(device->GetSwapChain(0, &chain))) {
            swapchain_vtable = *reinterpret_cast<void***>(chain);
            chain->Release();
        }
        device->Release();

        orig_reset_ = reinterpret_cast<ResetFn>(Patch(device_vtable, kDeviceReset, &HookReset));
        orig_present_ = reinterpret_cast<PresentFn>(Patch(device_vtable, kDevicePresent, &HookPresent));
        if (swapchain_vtable) {
            orig_sc_present_ =
                reinterpret_cast<SwapChainPresentFn>(Patch(swapchain_vtable, kSwapChainPresent, &HookSwapChainPresent));
        }
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&HookPresent), &self);
        installed_ = orig_present_ != nullptr && orig_reset_ != nullptr;
        GR_LOG("overlay: game device %p, vtable %p, Present %p, Reset %p, swap chain vtable %p",
               static_cast<void*>(device), static_cast<void*>(device_vtable), reinterpret_cast<void*>(orig_present_),
               reinterpret_cast<void*>(orig_reset_), static_cast<void*>(swapchain_vtable));
        return installed_;
    }

    void SetEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_release); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    bool installed() const noexcept { return installed_; }

    FrameMailbox& mailbox() noexcept { return mailbox_; }
    GpuShare& share() noexcept { return share_; }

    // Asks the next capture to also write the raw frame to `path` (a debugging aid: the
    // render thread writes the file, which GMod can afford once and RDR2 never sees).
    void RequestDump(const char* path) noexcept {
        std::snprintf(dump_path_, sizeof(dump_path_), "%s", path);
        dump_requested_.store(true, std::memory_order_release);
    }

    // The overlay thread takes the request on the GPU path, where frames never reach the CPU
    // here. Returns false if none is waiting.
    bool TakeDumpRequest(char* path, size_t chars) noexcept {
        if (!dump_requested_.exchange(false, std::memory_order_acq_rel)) return false;
        std::snprintf(path, chars, "%s", dump_path_);
        return true;
    }

    struct Stats {
        uint32_t presents;
        uint32_t captures;
        uint32_t failures;
        uint32_t width;
        uint32_t height;
        uint32_t format;          // D3DFORMAT of the back buffer
        uint32_t multisample;
        uint32_t last_capture_us; // how long the last capture took
        uint32_t skipped;         // frames whose readback was not done when its slot came round
        uint32_t present_us;      // how long the game's own Present took, last frame
    };
    Stats stats() const noexcept {
        return {presents_.load(), captures_.load(), failures_.load(), width_, height_, format_, multisample_,
                last_capture_us_.load(), skipped_.load(), present_us_.load()};
    }

private:
    struct Slot {
        IDirect3DSurface9* target = nullptr;  // default pool, the GPU copy of the back buffer
        IDirect3DSurface9* system = nullptr;  // system memory, where it is read from
        bool pending = false;
        uint32_t time_us = 0;                 // when the frame was presented
    };
    static constexpr uint32_t kRing = 3;

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                           const RGNDATA*, DWORD);
    using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    // Slots in the COM vtables, fixed by d3d9.h's interface order.
    static constexpr int kDeviceReset = 16;
    static constexpr int kDevicePresent = 17;
    static constexpr int kSwapChainPresent = 3;

    D3D9Capture() = default;

    // Works around: a device made here does not share the game's vtable (seen: the hooks
    // went into the probe device's vtable and caught no Present at all; d3d9.dll has more
    // than one device class). So the game's own device is found instead: shaderapidx9.dll
    // keeps it in a global, which is a pointer in its writable data to an object whose
    // vtable lies in d3d9.dll and which answers QueryInterface for IDirect3DDevice9.
    // Returns it with a reference held, or nullptr.
    static IDirect3DDevice9* FindGameDevice() noexcept {
        const HMODULE api = GetModuleHandleW(L"shaderapidx9.dll");
        const HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
        if (!api || !d3d9) {
            if (log_scan_) GR_LOG("overlay: shaderapidx9 %p, d3d9 %p", static_cast<void*>(api), static_cast<void*>(d3d9));
            return nullptr;
        }
        uintptr_t d3d9_lo = 0, d3d9_hi = 0;
        ImageRange(d3d9, d3d9_lo, d3d9_hi);

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(api);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(api) + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0) continue;
            const auto* begin = reinterpret_cast<const uintptr_t*>(reinterpret_cast<const uint8_t*>(api) +
                                                                   section->VirtualAddress);
            const size_t count = section->Misc.VirtualSize / sizeof(uintptr_t);
            for (size_t k = 0; k < count; ++k) {
                const uintptr_t candidate = begin[k];
                if (candidate < 0x10000 || (candidate & 7) != 0 || !Readable(candidate, sizeof(void*))) continue;
                const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(candidate);
                if (vtable < d3d9_lo || vtable >= d3d9_hi) continue;
                IDirect3DDevice9* device = DeviceOf(reinterpret_cast<IUnknown*>(candidate));
                if (log_scan_) {
                    GR_LOG("overlay: shaderapidx9+%llx -> %p, vtable d3d9+%llx, device %p",
                           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&begin[k]) -
                                                           reinterpret_cast<uintptr_t>(api)),
                           reinterpret_cast<void*>(candidate), static_cast<unsigned long long>(vtable - d3d9_lo),
                           static_cast<void*>(device));
                }
                if (device) return device;
            }
        }
        log_scan_ = false;  // once is enough to read; the retries would fill the log
        return nullptr;
    }

    static inline bool log_scan_ = true;

    // The device itself, or the device that made a resource, query, shader, declaration,
    // state block or swap chain (each has GetDevice). With a reference held, or nullptr.
    static IDirect3DDevice9* DeviceOf(IUnknown* object) noexcept {
        IDirect3DDevice9* device = nullptr;
        if (SUCCEEDED(object->QueryInterface(__uuidof(IDirect3DDevice9), reinterpret_cast<void**>(&device)))) {
            return device;
        }
        if ((device = Via<IDirect3DResource9>(object)) != nullptr) return device;
        if ((device = Via<IDirect3DQuery9>(object)) != nullptr) return device;
        if ((device = Via<IDirect3DVertexShader9>(object)) != nullptr) return device;
        if ((device = Via<IDirect3DPixelShader9>(object)) != nullptr) return device;
        if ((device = Via<IDirect3DVertexDeclaration9>(object)) != nullptr) return device;
        if ((device = Via<IDirect3DStateBlock9>(object)) != nullptr) return device;
        return Via<IDirect3DSwapChain9>(object);
    }

    template <class T>
    static IDirect3DDevice9* Via(IUnknown* object) noexcept {
        T* typed = nullptr;
        if (FAILED(object->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&typed))) || !typed) return nullptr;
        IDirect3DDevice9* device = nullptr;
        if (FAILED(typed->GetDevice(&device))) device = nullptr;
        typed->Release();
        return device;
    }

    static void ImageRange(HMODULE module, uintptr_t& lo, uintptr_t& hi) noexcept {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        lo = reinterpret_cast<uintptr_t>(module);
        hi = lo + nt->OptionalHeader.SizeOfImage;
    }

    static bool Readable(uintptr_t address, size_t bytes) noexcept {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        return address + bytes <= end;
    }

    static void* Patch(void** vtable, int slot, void* hook) noexcept {
        DWORD old_protect = 0;
        if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old_protect)) return nullptr;
        void* original = vtable[slot];
        if (original == hook) {
            // Already ours (a second Install): the real function was stored the first time.
            VirtualProtect(&vtable[slot], sizeof(void*), old_protect, &old_protect);
            return nullptr;
        }
        vtable[slot] = hook;
        VirtualProtect(&vtable[slot], sizeof(void*), old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), &vtable[slot], sizeof(void*));
        return original;
    }

    static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* src, const RECT* dst,
                                                 HWND window, const RGNDATA* dirty) {
        Get().OnPresent(device);
        const uint32_t start = NowUs();
        const HRESULT hr = Get().orig_present_(device, src, dst, window, dirty);
        Get().present_us_.store(static_cast<uint32_t>(AgeUs(NowUs(), start)), std::memory_order_relaxed);
        Get().AfterPresent();
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE HookSwapChainPresent(IDirect3DSwapChain9* chain, const RECT* src,
                                                          const RECT* dst, HWND window, const RGNDATA* dirty,
                                                          DWORD flags) {
        IDirect3DDevice9* device = nullptr;
        if (SUCCEEDED(chain->GetDevice(&device))) {
            Get().OnPresent(device);
            device->Release();
        }
        const HRESULT hr = Get().orig_sc_present_(chain, src, dst, window, dirty, flags);
        Get().AfterPresent();
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
        Get().ReleaseSurfaces();
        Get().ReleaseShared();
        return Get().orig_reset_(device, params);
    }

    void ReleaseSurfaces() noexcept {
        for (Slot& slot : ring_) {
            if (slot.target) slot.target->Release();
            if (slot.system) slot.system->Release();
            slot = Slot{};
        }
        width_ = height_ = 0;
    }

    bool EnsureSurfaces(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc) noexcept {
        if (ring_[0].system && width_ == desc.Width && height_ == desc.Height &&
            format_ == static_cast<uint32_t>(desc.Format) && multisample_ == static_cast<uint32_t>(desc.MultiSampleType)) {
            return true;
        }
        ReleaseSurfaces();
        for (Slot& slot : ring_) {
            if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                  &slot.target, nullptr)) ||
                FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                           &slot.system, nullptr))) {
                ReleaseSurfaces();
                return false;
            }
        }
        width_ = desc.Width;
        height_ = desc.Height;
        format_ = static_cast<uint32_t>(desc.Format);
        multisample_ = static_cast<uint32_t>(desc.MultiSampleType);
        GR_LOG("overlay: capturing %ux%u, back buffer format %u, multisample %u", width_, height_, format_,
               multisample_);
        return true;
    }

    void OnPresent(IDirect3DDevice9* device) noexcept {
        presents_.fetch_add(1, std::memory_order_relaxed);
        if (!enabled()) return;
        const uint32_t start = NowUs();
        if (Capture(device)) {
            captures_.fetch_add(1, std::memory_order_relaxed);
        } else {
            failures_.fetch_add(1, std::memory_order_relaxed);
        }
        last_capture_us_.store(static_cast<uint32_t>(AgeUs(NowUs(), start)), std::memory_order_relaxed);
    }

    // Works around: reading the back buffer straight back stalls until the GPU has finished
    // the frame (measured 10 to 46 ms a frame at 1920x1080). Instead each frame is copied on
    // the GPU into one slot of a ring and its transfer to system memory is queued; the slot
    // is only locked when the ring comes round to it again, kRing - 1 frames later, by when
    // the transfer is long done. A slot still busy then is skipped, never waited for.
    bool Capture(IDirect3DDevice9* device) noexcept {
        IDirect3DSurface9* back = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return false;
        D3DSURFACE_DESC desc{};
        // Only the 32-bit formats whose bytes are B G R A (or B G R X) in memory.
        const bool usable = SUCCEEDED(back->GetDesc(&desc)) &&
                            (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8) &&
                            EnsureSurfaces(device, desc);
        if (!usable) {
            back->Release();
            return false;
        }

        if (!share_.failed.load(std::memory_order_acquire)) {
            const bool ok = CaptureShared(device, back, desc);
            back->Release();
            return ok;
        }

        Slot& slot = ring_[next_ % kRing];
        if (slot.pending) {
            slot.pending = false;
            Collect(slot);
        }
        const bool ok = SUCCEEDED(device->StretchRect(back, nullptr, slot.target, nullptr, D3DTEXF_NONE)) &&
                        SUCCEEDED(device->GetRenderTargetData(slot.target, slot.system));
        back->Release();
        if (!ok) return false;
        slot.pending = true;
        slot.time_us = NowUs();
        ++next_;
        return true;
    }

    void Collect(Slot& slot) noexcept {
        D3DLOCKED_RECT locked{};
        if (FAILED(slot.system->LockRect(&locked, nullptr, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT))) {
            skipped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        OverlayFrame& frame = mailbox_.WriteBuffer();
        if (frame.Reserve(width_, height_)) {
            const size_t row = static_cast<size_t>(width_) * 4;
            const uint8_t* from = static_cast<const uint8_t*>(locked.pBits);
            for (uint32_t y = 0; y < height_; ++y) {
                std::memcpy(frame.pixels + y * row, from + static_cast<size_t>(y) * locked.Pitch, row);
            }
            frame.serial = ++serial_;
            frame.time_us = slot.time_us;
            Dump(frame);
            mailbox_.Post();
        }
        slot.system->UnlockRect();
    }

    // ---- GPU path (gpu_share.h)

    void ReleaseShared() noexcept {
        for (uint32_t i = 0; i < GpuShare::kTextures; ++i) {
            if (shared_surface_[i]) shared_surface_[i]->Release();
            if (shared_texture_[i]) shared_texture_[i]->Release();
            if (shared_query_[i]) shared_query_[i]->Release();
            shared_surface_[i] = nullptr;
            shared_texture_[i] = nullptr;
            shared_query_[i] = nullptr;
            shared_pending_[i] = false;
        }
        opened_generation_ = 0;
    }

    // Opens the overlay's textures in this device. false: they are not there yet.
    bool OpenShared(IDirect3DDevice9* device, uint32_t w, uint32_t h) noexcept {
        const uint32_t generation = share_.generation.load(std::memory_order_acquire);
        if (generation == opened_generation_ && opened_generation_ != 0) return true;
        if (generation == 0 || share_.width != w || share_.height != h) return false;
        ReleaseShared();
        for (uint32_t i = 0; i < GpuShare::kTextures; ++i) {
            HANDLE handle = share_.handles[i];
            const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                     &shared_texture_[i], &handle);
            if (FAILED(hr) || FAILED(shared_texture_[i]->GetSurfaceLevel(0, &shared_surface_[i])) ||
                FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &shared_query_[i]))) {
                GR_LOG("overlay: Direct3D 9 could not open the shared texture (0x%08lx). Falling back to reading "
                       "frames back through the CPU, which costs GMod frame rate",
                       static_cast<unsigned long>(hr));
                ReleaseShared();
                share_.failed.store(true, std::memory_order_release);
                return false;
            }
        }
        opened_generation_ = generation;
        GR_LOG("overlay: GPU path open, %ux%u, %u shared textures", w, h, GpuShare::kTextures);
        return true;
    }

    // Copies the back buffer into the next shared texture and tells the overlay about the
    // copies the GPU has finished. A copy is only announced once its event query reports
    // it done, so the overlay never reads a texture D3D9 is still writing.
    bool CaptureShared(IDirect3DDevice9* device, IDirect3DSurface9* back, const D3DSURFACE_DESC& desc) noexcept {
        width_ = desc.Width;
        height_ = desc.Height;
        format_ = static_cast<uint32_t>(desc.Format);
        multisample_ = static_cast<uint32_t>(desc.MultiSampleType);
        const uint32_t want = (desc.Width << 16) | (desc.Height & 0xFFFF);
        if (share_.request.load(std::memory_order_relaxed) != want) {
            share_.request.store(want, std::memory_order_release);
        }
        if (!OpenShared(device, desc.Width, desc.Height)) return !share_.failed.load();

        AnnounceFinished(0);

        const uint32_t i = next_ % GpuShare::kTextures;
        if (FAILED(device->StretchRect(back, nullptr, shared_surface_[i], nullptr, D3DTEXF_NONE))) return false;
        shared_query_[i]->Issue(D3DISSUE_END);
        shared_pending_[i] = true;
        shared_serial_[i] = ++serial_ & 0x3FFFFFFF;
        shared_time_us_[i] = NowUs();
        ++next_;
        return true;
    }

    // Tells the overlay about the newest copy the GPU has finished. flags: 0, or
    // D3DGETDATA_FLUSH to also submit what is queued.
    void AnnounceFinished(DWORD flags) noexcept {
        uint32_t newest = 0;
        uint32_t newest_index = 0;
        for (uint32_t i = 0; i < GpuShare::kTextures; ++i) {
            if (shared_pending_[i] && shared_query_[i]->GetData(nullptr, 0, flags) == S_OK) {
                shared_pending_[i] = false;
                if (shared_serial_[i] > newest) {
                    newest = shared_serial_[i];
                    newest_index = i;
                }
            }
        }
        if (newest == 0) return;
        share_.ready_time_us.store(shared_time_us_[newest_index], std::memory_order_relaxed);
        share_.ready.store((newest << 2) | newest_index, std::memory_order_release);
        if (share_.wake) SetEvent(share_.wake);
    }

    // Works around: a copy was only announced at the next Present, a whole GMod frame after
    // the GPU had finished it (measured 6 to 8.5 ms from capture to screen at 310 fps). Right
    // after Present the frame has been submitted, and the copy is often done by the time
    // the GMod frame's own work is; checking here costs one query poll.
    void AfterPresent() noexcept {
        if (!enabled() || share_.failed.load(std::memory_order_acquire) || opened_generation_ == 0) return;
        AnnounceFinished(D3DGETDATA_FLUSH);
    }

    void Dump(const OverlayFrame& frame) noexcept {
        if (!dump_requested_.exchange(false, std::memory_order_acq_rel)) return;
        FILE* f = std::fopen(dump_path_, "wb");
        if (!f) return;
        const uint32_t header[4] = {frame.width, frame.height, format_, 0};
        std::fwrite(header, sizeof(header), 1, f);
        std::fwrite(frame.pixels, 1, static_cast<size_t>(frame.width) * frame.height * 4, f);
        std::fclose(f);
        GR_LOG("overlay: dumped a %ux%u frame to %s", frame.width, frame.height, dump_path_);
    }

    bool installed_ = false;
    PresentFn orig_present_ = nullptr;
    SwapChainPresentFn orig_sc_present_ = nullptr;
    ResetFn orig_reset_ = nullptr;
    std::atomic<bool> enabled_{false};

    // Render thread only.
    Slot ring_[kRing];
    uint32_t next_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t format_ = 0;
    uint32_t multisample_ = 0;
    uint32_t serial_ = 0;
    FrameMailbox mailbox_;
    GpuShare share_;
    IDirect3DTexture9* shared_texture_[GpuShare::kTextures] = {};
    IDirect3DSurface9* shared_surface_[GpuShare::kTextures] = {};
    IDirect3DQuery9* shared_query_[GpuShare::kTextures] = {};
    bool shared_pending_[GpuShare::kTextures] = {};
    uint32_t shared_serial_[GpuShare::kTextures] = {};
    uint32_t shared_time_us_[GpuShare::kTextures] = {};
    uint32_t opened_generation_ = 0;

    std::atomic<uint32_t> presents_{0};
    std::atomic<uint32_t> captures_{0};
    std::atomic<uint32_t> failures_{0};
    std::atomic<uint32_t> skipped_{0};
    std::atomic<uint32_t> present_us_{0};
    std::atomic<uint32_t> last_capture_us_{0};
    std::atomic<bool> dump_requested_{false};
    char dump_path_[MAX_PATH] = "";
};

}  // namespace gr
