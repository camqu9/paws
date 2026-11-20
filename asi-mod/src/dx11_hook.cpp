#include "dx11_hook.h"
#include "camera.h"
#include "fsr_pipeline.h"
#include "paws_common.h"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace paws {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);

constexpr size_t kVTableIdxPresent = 8;
constexpr size_t kVTableIdxResizeBuffers = 13;
constexpr size_t kVTableIdxOMSetRenderTargets = 33;

void** g_SwapChainVTable = nullptr;
void** g_ContextVTable = nullptr;

PresentFn g_OrigPresent = nullptr;
ResizeBuffersFn g_OrigResizeBuffers = nullptr;
OMSetRenderTargetsFn g_OrigOMSetRenderTargets = nullptr;

std::mutex g_StateMutex;
bool g_HookInstalled = false;
bool g_InInternalCall = false;

ID3D11Device* g_Device = nullptr;
ID3D11DeviceContext* g_Context = nullptr;

FsrPipeline g_Pipeline;
CameraTracker g_Camera;

// Tracked 3D scene buffers before 2D HUD draws
ID3D11Texture2D* g_PendingColorTex = nullptr;
ID3D11Texture2D* g_PendingDepthTex = nullptr;
UINT g_LastLoggedW = 0;
UINT g_LastLoggedH = 0;
DXGI_FORMAT g_LastLoggedColorFmt = DXGI_FORMAT_UNKNOWN;
DXGI_FORMAT g_LastLoggedDepthFmt = DXGI_FORMAT_UNKNOWN;

// Hotkey & FPS state
bool g_FsrEnabled = true;
DebugViewMode g_DebugMode = DebugViewMode::Normal;
bool g_PrevF8 = false;
bool g_PrevF9 = false;

auto g_LastFrameTime = std::chrono::steady_clock::now();
auto g_FpsLogTimer = std::chrono::steady_clock::now();
int g_FpsFrameCount = 0;

bool PatchVTableEntry(void** vtable, size_t index, void* hookFn, void** outOrigFn) {
    if (!vtable) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    if (outOrigFn) {
        *outOrigFn = vtable[index];
    }
    vtable[index] = hookFn;
    DWORD dummy = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), &vtable[index], sizeof(void*));
    return true;
}

bool IsTargetDepthFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            return true;
        default:
            return false;
    }
}

void ReleasePendingBuffers() {
    if (g_PendingColorTex) {
        g_PendingColorTex->Release();
        g_PendingColorTex = nullptr;
    }
    if (g_PendingDepthTex) {
        g_PendingDepthTex->Release();
        g_PendingDepthTex = nullptr;
    }
}

// ============================================================================
// Hooked ID3D11DeviceContext::OMSetRenderTargets
// Captures the main 3D color buffer and matching Depth-Stencil texture
// (D32_FLOAT / R32_TYPELESS) right before the 2D HUD draws.
// ============================================================================
void STDMETHODCALLTYPE Hooked_OMSetRenderTargets(
    ID3D11DeviceContext* pContext,
    UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView) {

    if (!g_InInternalCall && pContext == g_Context) {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        g_InInternalCall = true;

        // If we previously saw the main 3D scene pass and the game is now switching
        // to a pass without a depth buffer (e.g., 2D HUD / UI), snapshot the 3D buffers now.
        if (g_PendingColorTex && g_PendingDepthTex && pDepthStencilView == nullptr && !g_Pipeline.HasCapturedFrame()) {
            g_Pipeline.CaptureSceneBuffers(pContext, g_PendingColorTex, g_PendingDepthTex);
            ReleasePendingBuffers();
        }

        // Inspect newly bound RTV + DSV to detect the main 3D scene pass
        if (pDepthStencilView != nullptr && NumViews >= 1 &&
            ppRenderTargetViews != nullptr && ppRenderTargetViews[0] != nullptr) {
            ID3D11Resource* colorRes = nullptr;
            ID3D11Resource* depthRes = nullptr;
            ppRenderTargetViews[0]->GetResource(&colorRes);
            pDepthStencilView->GetResource(&depthRes);

            if (colorRes && depthRes) {
                ID3D11Texture2D* colorTex = nullptr;
                ID3D11Texture2D* depthTex = nullptr;
                colorRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&colorTex));
                depthRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTex));

                if (colorTex && depthTex) {
                    D3D11_TEXTURE2D_DESC cDesc{}, dDesc{};
                    colorTex->GetDesc(&cDesc);
                    depthTex->GetDesc(&dDesc);

                    if (cDesc.Width >= 640 && cDesc.Height >= 360 &&
                        cDesc.Width == dDesc.Width && cDesc.Height == dDesc.Height &&
                        IsTargetDepthFormat(dDesc.Format)) {
                        if (cDesc.Width != g_LastLoggedW || cDesc.Height != g_LastLoggedH ||
                            cDesc.Format != g_LastLoggedColorFmt || dDesc.Format != g_LastLoggedDepthFmt) {
                            g_LastLoggedW = cDesc.Width;
                            g_LastLoggedH = cDesc.Height;
                            g_LastLoggedColorFmt = cDesc.Format;
                            g_LastLoggedDepthFmt = dDesc.Format;
                            PAWS_LOG("Detected main 3D buffers: %ux%u | ColorFmt=%d | DepthFmt=%d",
                                     cDesc.Width, cDesc.Height,
                                     static_cast<int>(cDesc.Format),
                                     static_cast<int>(dDesc.Format));
                        }

                        ReleasePendingBuffers();
                        g_PendingColorTex = colorTex;
                        g_PendingDepthTex = depthTex;
                        colorTex = nullptr; // Ownership transferred
                        depthTex = nullptr;
                    }
                }

                if (colorTex) colorTex->Release();
                if (depthTex) depthTex->Release();
            }
            if (colorRes) colorRes->Release();
            if (depthRes) depthRes->Release();
        }

        g_InInternalCall = false;
    }

    g_OrigOMSetRenderTargets(pContext, NumViews, ppRenderTargetViews, pDepthStencilView);
}

// ============================================================================
// Hooked IDXGISwapChain::ResizeBuffers
// ============================================================================
HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers(
    IDXGISwapChain* pSwapChain,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags) {

    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        ReleasePendingBuffers();
        g_Pipeline.OnResize();
        PAWS_LOG("ResizeBuffers: %ux%u (fmt=%d)", Width, Height, static_cast<int>(NewFormat));
    }

    return g_OrigResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// ============================================================================
// Hooked IDXGISwapChain::Present
// ============================================================================
HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        g_InInternalCall = true;

        // Lazy initialization of real game device & context
        if (!g_Device) {
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_Device))) && g_Device) {
                g_Device->GetImmediateContext(&g_Context);
                g_Pipeline.Init(g_Device, g_Context);
                PAWS_LOG("Attached to game ID3D11Device=%p, ImmediateContext=%p", g_Device, g_Context);
            }
        }

        // Poll hotkeys (edge-triggered)
        bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8Down && !g_PrevF8) {
            int nextMode = (static_cast<int>(g_DebugMode) + 1) % static_cast<int>(DebugViewMode::Count);
            g_DebugMode = static_cast<DebugViewMode>(nextMode);
            PAWS_LOG("Hotkey F8 -> Debug View: %s", DebugViewName(g_DebugMode));
        }
        g_PrevF8 = f8Down;

        bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9Down && !g_PrevF9) {
            g_FsrEnabled = !g_FsrEnabled;
            PAWS_LOG("Hotkey F9 -> FSR 2/3: %s", g_FsrEnabled ? "ENABLED" : "DISABLED");
        }
        g_PrevF9 = f9Down;

        // Frame timing & Halton jitter advance
        auto now = std::chrono::steady_clock::now();
        float dtMs = std::chrono::duration<float, std::milli>(now - g_LastFrameTime).count();
        g_LastFrameTime = now;

        ID3D11Texture2D* backbuffer = nullptr;
        if (g_Device && g_Context &&
            SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))) &&
            backbuffer) {

            D3D11_TEXTURE2D_DESC bbDesc{};
            backbuffer->GetDesc(&bbDesc);
            g_Camera.BeginFrame(bbDesc.Width, bbDesc.Height, dtMs);

            // Fallback snapshot if depth was still bound at Present
            if (!g_Pipeline.HasCapturedFrame() && g_PendingColorTex && g_PendingDepthTex) {
                g_Pipeline.CaptureSceneBuffers(g_Context, g_PendingColorTex, g_PendingDepthTex);
            }
            ReleasePendingBuffers();

            // Run Motion Vector CS -> FSR 2/3 -> Debug Overlay
            g_Pipeline.Execute(g_Context, backbuffer, g_Camera, g_FsrEnabled, g_DebugMode);
            g_Pipeline.ResetFrameCaptureFlag();

            backbuffer->Release();
        }

        // Periodic FPS logging to paws_gtav.log (every 5 seconds)
        ++g_FpsFrameCount;
        double logDt = std::chrono::duration<double>(now - g_FpsLogTimer).count();
        if (logDt >= 5.0) {
            double fps = static_cast<double>(g_FpsFrameCount) / logDt;
            PAWS_LOG("Status | FPS: %.1f | FSR: %s | DebugView: %s | Jitter: (%.3f, %.3f)",
                     fps,
                     g_FsrEnabled ? "ON" : "OFF",
                     DebugViewName(g_DebugMode),
                     g_Camera.GetJitterPixelsX(),
                     g_Camera.GetJitterPixelsY());
            g_FpsFrameCount = 0;
            g_FpsLogTimer = now;
        }

        g_InInternalCall = false;
    }

    return g_OrigPresent(pSwapChain, SyncInterval, Flags);
}

} // namespace

bool Dx11Hook::Install() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    if (g_HookInstalled) return true;

    // Create a temporary hidden window + dummy DX11 device/swapchain to locate the DX11 vtables
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PawsDummyDx11Class";
    RegisterClassExW(&wc);

    HWND dummyHwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                     0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummyHwnd) {
        PAWS_LOG("ERROR: Failed to create dummy HWND for vtable discovery.");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* dummySwapChain = nullptr;
    ID3D11Device* dummyDevice = nullptr;
    ID3D11DeviceContext* dummyContext = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &fl, 1, D3D11_SDK_VERSION,
        &scd, &dummySwapChain, &dummyDevice, nullptr, &dummyContext);

    if (FAILED(hr) || !dummySwapChain || !dummyContext) {
        PAWS_LOG("ERROR: Dummy D3D11CreateDeviceAndSwapChain failed (0x%08X).", hr);
        DestroyWindow(dummyHwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    g_SwapChainVTable = *reinterpret_cast<void***>(dummySwapChain);
    g_ContextVTable = *reinterpret_cast<void***>(dummyContext);

    bool ok1 = PatchVTableEntry(g_SwapChainVTable, kVTableIdxPresent,
                                reinterpret_cast<void*>(&Hooked_Present),
                                reinterpret_cast<void**>(&g_OrigPresent));
    bool ok2 = PatchVTableEntry(g_SwapChainVTable, kVTableIdxResizeBuffers,
                                reinterpret_cast<void*>(&Hooked_ResizeBuffers),
                                reinterpret_cast<void**>(&g_OrigResizeBuffers));
    bool ok3 = PatchVTableEntry(g_ContextVTable, kVTableIdxOMSetRenderTargets,
                                reinterpret_cast<void*>(&Hooked_OMSetRenderTargets),
                                reinterpret_cast<void**>(&g_OrigOMSetRenderTargets));

    dummyContext->Release();
    dummyDevice->Release();
    dummySwapChain->Release();
    DestroyWindow(dummyHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (!ok1 || !ok2 || !ok3) {
        PAWS_LOG("ERROR: Failed to patch DX11 vtables (Present=%d Resize=%d OMSetRT=%d).", ok1, ok2, ok3);
        return false;
    }

    g_HookInstalled = true;
    PAWS_LOG("DX11 VTable hooks installed (Present=%p, ResizeBuffers=%p, OMSetRenderTargets=%p).",
             reinterpret_cast<void*>(g_OrigPresent),
             reinterpret_cast<void*>(g_OrigResizeBuffers),
             reinterpret_cast<void*>(g_OrigOMSetRenderTargets));
    return true;
}

void Dx11Hook::Uninstall() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    if (!g_HookInstalled) return;

    if (g_SwapChainVTable && g_OrigPresent) {
        PatchVTableEntry(g_SwapChainVTable, kVTableIdxPresent,
                         reinterpret_cast<void*>(g_OrigPresent), nullptr);
    }
    if (g_SwapChainVTable && g_OrigResizeBuffers) {
        PatchVTableEntry(g_SwapChainVTable, kVTableIdxResizeBuffers,
                         reinterpret_cast<void*>(g_OrigResizeBuffers), nullptr);
    }
    if (g_ContextVTable && g_OrigOMSetRenderTargets) {
        PatchVTableEntry(g_ContextVTable, kVTableIdxOMSetRenderTargets,
                         reinterpret_cast<void*>(g_OrigOMSetRenderTargets), nullptr);
    }

    ReleasePendingBuffers();
    g_Pipeline.Shutdown();
    if (g_Context) {
        g_Context->Release();
        g_Context = nullptr;
    }
    if (g_Device) {
        g_Device->Release();
        g_Device = nullptr;
    }
    g_HookInstalled = false;
    PAWS_LOG("DX11 VTable hooks removed cleanly.");
}

} // namespace paws
