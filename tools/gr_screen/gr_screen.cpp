// gr_screen OUT.bmp [full]: saves what the primary monitor is really showing, at half size
// (or full size with `full`: needed to judge fine detail such as upscaler shimmer).
//
// Works around: tools/rdr2_grab.py reads the screen through GDI, which only sees what
// Windows' compositor last drew. When RDR2 covers the whole monitor its frames can go to the
// screen past the compositor, and GDI then returns a stale picture of RDR2 (seen at
// 3840x2160: the same frame for minutes). Desktop duplication reads the image that is
// actually scanned out, the overlay included or not, as the user sees it.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: gr_screen OUT.bmp\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return 1;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* candidate = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &candidate) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            candidate->GetDesc(&desc);
            // The primary monitor is the one whose desktop rectangle starts at (0, 0).
            if (desc.AttachedToDesktop && desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0) {
                output = candidate;
                break;
            }
            candidate->Release();
        }
        if (!output) adapter->Release();
    }
    if (!output) {
        std::printf("no primary output\n");
        return 1;
    }
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("no device\n");
        return 1;
    }
    IDXGIOutput1* output1 = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    const HRESULT made = output1 ? output1->DuplicateOutput(device, &dup) : E_FAIL;
    if (FAILED(made)) {
        std::printf("DuplicateOutput failed 0x%08lx\n", static_cast<unsigned long>(made));
        return 1;
    }
    ID3D11Texture2D* frame = nullptr;
    for (int tries = 0; tries < 60 && !frame; ++tries) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        const HRESULT hr = dup->AcquireNextFrame(100, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {
            std::printf("AcquireNextFrame failed 0x%08lx\n", static_cast<unsigned long>(hr));
            return 1;
        }
        // The first frame is always handed out; one with a present time is a real picture.
        if (info.LastPresentTime.QuadPart != 0 || tries > 20) {
            resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&frame));
        }
        resource->Release();
        if (!frame) dup->ReleaseFrame();
    }
    if (!frame) {
        std::printf("no frame\n");
        return 1;
    }
    D3D11_TEXTURE2D_DESC desc{};
    frame->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.MiscFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) return 1;
    context->CopyResource(staging, frame);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return 1;

    const bool wide = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;  // an HDR desktop: not handled
    const uint32_t step = argc > 2 && std::strcmp(argv[2], "full") == 0 ? 1 : 2;
    const uint32_t w = desc.Width / step, h = desc.Height / step;
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
    for (uint32_t y = 0; y < h && !wide; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * step * mapped.RowPitch;
        uint8_t* out = &pixels[static_cast<size_t>(h - 1 - y) * w * 3];  // a BMP's rows go bottom up
        for (uint32_t x = 0; x < w; ++x) {
            out[x * 3 + 0] = row[x * 4 * step + 0];
            out[x * 3 + 1] = row[x * 4 * step + 1];
            out[x * 3 + 2] = row[x * 4 * step + 2];
        }
    }
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(w);
    info.biHeight = static_cast<LONG>(h);
    info.biPlanes = 1;
    info.biBitCount = 24;
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::fwrite(&file, sizeof(file), 1, f);
    std::fwrite(&info, sizeof(info), 1, f);
    std::fwrite(pixels.data(), 1, pixels.size(), f);
    std::fclose(f);
    std::printf("%ux%u format %u%s -> %s\n", desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
                wide ? " (HDR: not converted)" : "", argv[1]);
    return 0;
}
