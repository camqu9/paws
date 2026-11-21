// Standalone smoke test for AnrFrameGen (DX11 <-> D3D12 <-> ANR nvngx_dlssg.dll)
#include "anr_fg.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

int main() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

    paws::Logger::Instance().Init((exeDir / L"anr_smoke_test.log").c_str());
    std::printf("[smoke] exeDir = %ls\n", exeDir.c_str());

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr) || !dev) {
        std::printf("[smoke] FAIL: D3D11CreateDevice failed (0x%08X)\n", static_cast<unsigned>(hr));
        return 1;
    }

    constexpr UINT W = 1280, H = 720;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* inTex = nullptr;
    ID3D11Texture2D* outTex = nullptr;
    ID3D11Texture2D* readTex = nullptr;
    dev->CreateTexture2D(&td, nullptr, &inTex);
    dev->CreateTexture2D(&td, nullptr, &outTex);
    D3D11_TEXTURE2D_DESC rtd = td;
    rtd.Usage = D3D11_USAGE_STAGING;
    rtd.BindFlags = 0;
    rtd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&rtd, nullptr, &readTex);

    paws::AnrFrameGen fg;
    fg.SetModuleDirectory(exeDir.wstring());
    if (!fg.EnsureReady(dev, W, H, DXGI_FORMAT_B8G8R8A8_UNORM)) {
        std::printf("[smoke] FAIL: AnrFrameGen::EnsureReady failed (check anr_smoke_test.log)\n");
        return 2;
    }

    std::vector<uint32_t> pixels(W * H, 0xFF336699u);
    for (int f = 0; f < 5; ++f) {
        // Shift bright vertical stripe each frame so ANR FG has real motion to interpolate
        UINT stripeStart = 100 + f * 40;
        UINT stripeEnd = 200 + f * 40;
        std::fill(pixels.begin(), pixels.end(), 0xFF203040u);
        for (UINT y = 0; y < H; ++y) {
            for (UINT x = stripeStart; x < stripeEnd; ++x) {
                pixels[y * W + x] = 0xFFE0A040u;
            }
        }
        ctx->UpdateSubresource(inTex, 0, nullptr, pixels.data(), W * 4, 0);

        auto t0 = std::chrono::steady_clock::now();
        bool produced = fg.GenerateFrame(ctx, inTex, outTex);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Read back middle row of outTex to verify where ANR placed the interpolated stripe!
        ctx->CopyResource(readTex, outTex);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        int minX = -1, maxX = -1;
        uint32_t midPix = 0;
        if (SUCCEEDED(ctx->Map(readTex, 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(mapped.pData) + (H / 2) * mapped.RowPitch);
            midPix = row[0];
            for (UINT x = 0; x < W; ++x) {
                uint32_t r = (row[x] >> 16) & 0xFF;
                if (r > 120) {
                    if (minX < 0) minX = static_cast<int>(x);
                    maxX = static_cast<int>(x);
                }
            }
            ctx->Unmap(readTex, 0);
        }
        std::printf("[smoke] frame %d: in=[%u..%u] out=[%d..%d] bg=0x%08X produced=%d time=%.2f ms\n",
                    f, stripeStart, stripeEnd, minX, maxX, midPix, produced ? 1 : 0, ms);
    }

    fg.Shutdown();
    readTex->Release();
    inTex->Release();
    outTex->Release();
    ctx->Release();
    dev->Release();
    paws::Logger::Instance().Shutdown();
    std::printf("[smoke] PASS!\n");
    return 0;
}
