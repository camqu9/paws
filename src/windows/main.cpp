// paws - Universal FSR 1.0 Spatial Upscaler & GTA V Smart Launcher (Windows)
// Zero third-party wrapper libraries: Win32 + DirectX 11 + DXGI Desktop Duplication + HLSL FSR 1.0 (EASU + RCAS).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace fs = std::filesystem;

namespace {

// Lightweight zero-dependency COM smart pointer
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* Get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T** ReleaseAndGetAddressOf() {
        Reset();
        return &ptr_;
    }
    void Reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

fs::path GetSelfDirectory() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return fs::current_path();
    return fs::path(buf).parent_path();
}

std::wstring GetProcessImagePath(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(buf));
    std::wstring result;
    if (QueryFullProcessImageNameW(hProc, 0, buf, &size)) {
        result.assign(buf, size);
    }
    CloseHandle(hProc);
    return result;
}

bool IsGta5ExeName(const std::wstring& pathOrName) {
    fs::path p(pathOrName);
    std::wstring fname = ToLower(p.filename().wstring());
    return fname == L"gta5.exe";
}

// Find a running GTA5.exe process if present
bool FindRunningGta5(DWORD& outPid, fs::path& outExePath) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (ToLower(pe.szExeFile) == L"gta5.exe") {
                outPid = pe.th32ProcessID;
                std::wstring full = GetProcessImagePath(outPid);
                if (!full.empty()) outExePath = fs::path(full);
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

struct WindowSearchCtx {
    DWORD targetPid = 0;
    std::wstring titleQueryLower;
    HWND foundHwnd = nullptr;
};

BOOL CALLBACK EnumWindowsByPidProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WindowSearchCtx*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->targetPid) {
        RECT rc{};
        if (GetClientRect(hwnd, &rc) && (rc.right - rc.left) >= 160 && (rc.bottom - rc.top) >= 120) {
            ctx->foundHwnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

BOOL CALLBACK EnumWindowsByTitleProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WindowSearchCtx*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512] = {};
    int len = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (len <= 0) return TRUE;
    std::wstring titleLower = ToLower(std::wstring(title, len));
    if (titleLower.find(ctx->titleQueryLower) != std::wstring::npos) {
        RECT rc{};
        if (GetClientRect(hwnd, &rc) && (rc.right - rc.left) >= 160 && (rc.bottom - rc.top) >= 120) {
            ctx->foundHwnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

HWND FindWindowByPid(DWORD pid) {
    WindowSearchCtx ctx{};
    ctx.targetPid = pid;
    EnumWindows(EnumWindowsByPidProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.foundHwnd;
}

HWND FindWindowByTitle(const std::wstring& query) {
    WindowSearchCtx ctx{};
    ctx.titleQueryLower = ToLower(query);
    EnumWindows(EnumWindowsByTitleProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.foundHwnd;
}

// ============================================================================
// Smart Launcher: GTA V Legacy Auto-Deployment & Direct Launch
// ============================================================================
int RunGtaVSmartLauncher(const fs::path& gtaExePath, bool alreadyRunning, DWORD runningPid) {
    std::cout << "[paws] GTA V Legacy detected (" << WideToUtf8(gtaExePath.wstring()) << ")\n";
    std::cout << "[paws] Routing to internal DX11 FSR 2/3 ASI plugin (paws_gtav.asi) instead of FSR 1.0 overlay.\n";

    fs::path gameDir = gtaExePath.parent_path();
    fs::path selfDir = GetSelfDirectory();
    fs::path srcAsi = selfDir / L"paws_gtav.asi";
    fs::path dstAsi = gameDir / L"paws_gtav.asi";

    // 1. Auto-deploy paws_gtav.asi into GTA5.exe directory if not already present
    if (fs::exists(dstAsi)) {
        std::cout << "[paws] paws_gtav.asi is already present in: " << WideToUtf8(gameDir.wstring()) << "\n";
    } else if (fs::exists(srcAsi)) {
        std::error_code ec;
        if (fs::copy_file(srcAsi, dstAsi, fs::copy_options::overwrite_existing, ec)) {
            std::cout << "[paws] Deployed paws_gtav.asi -> " << WideToUtf8(dstAsi.wstring()) << "\n";
        } else {
            std::cerr << "[paws] ERROR: Failed to copy paws_gtav.asi (" << ec.message() << "). "
                      << "Try running as Administrator if the game is in Program Files.\n";
        }
    } else {
        std::cerr << "[paws] WARNING: Could not find paws_gtav.asi next to paws.exe ("
                  << WideToUtf8(srcAsi.wstring()) << ").\n"
                  << "       Build both targets with CMake so paws_gtav.asi sits beside paws.exe.\n";
    }

    // 2. Check for ASI loader (dinput8.dll)
    fs::path dinputPath = gameDir / L"dinput8.dll";
    if (!fs::exists(dinputPath)) {
        std::cout << "\n======================================================================\n"
                  << "[paws] WARNING: ASI loader (dinput8.dll) NOT found in GTA V directory!\n"
                  << "       GTA5.exe will NOT load paws_gtav.asi without an ASI loader.\n"
                  << "       Please install Ultimate ASI Loader or Script Hook V (dinput8.dll)\n"
                  << "       into: " << WideToUtf8(gameDir.wstring()) << "\n"
                  << "       REMINDER: Use .asi plugins in GTA V Story Mode ONLY (never GTA Online).\n"
                  << "======================================================================\n\n";
    } else {
        std::cout << "[paws] Verified ASI loader (dinput8.dll) in game directory.\n";
    }

    // 3. Launch GTA5.exe directly (do not spawn FSR 1.0 overlay)
    if (alreadyRunning) {
        std::cout << "[paws] GTA5.exe is already running (PID " << runningPid << ").\n"
                  << "[paws] If paws_gtav.asi was just copied, restart GTA V for it to load.\n"
                  << "[paws] In-game hotkeys: F8 = Cycle Normal/Depth/MotionVectors, F9 = Toggle FSR.\n";
        return 0;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = L"\"" + gtaExePath.wstring() + L"\"";
    std::wstring workDir = gameDir.wstring();

    std::cout << "[paws] Launching GTA5.exe...\n";
    std::cout << "[paws] In-game hotkeys: F8 = Cycle Debug View, F9 = Toggle FSR 2/3.\n";
    std::cout << "[paws] Logs will be written to: " << WideToUtf8((gameDir / L"paws_gtav.log").wstring()) << "\n";

    if (!CreateProcessW(gtaExePath.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                        0, nullptr, workDir.c_str(), &si, &pi)) {
        std::cerr << "[paws] ERROR: CreateProcessW failed for GTA5.exe (error " << GetLastError() << ").\n";
        return 1;
    }

    auto startTime = std::chrono::steady_clock::now();
    std::cout << "[paws] Waiting for GTA5.exe (PID " << pi.dwProcessId << ") to exit...\n";
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startTime).count();
    std::cout << "[paws] GTA5.exe exited with code " << exitCode
              << " after " << elapsed << "s.\n";
    return static_cast<int>(exitCode);
}

// ============================================================================
// HLSL Shaders: Fullscreen Triangle VS + FSR 1.0 EASU PS + FSR 1.0 RCAS PS
// ============================================================================
static const char* kFullscreenVS = R"HLSL(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 pos = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(pos * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = pos;
    return o;
}
)HLSL";

// Pass 1: FSR 1.0 EASU (Edge-Adaptive Spatial Upsampling)
static const char* kFsrEasuPS = R"HLSL(
Texture2D<float4> g_SrcTex : register(t0);
SamplerState g_LinearClamp : register(s0);

cbuffer EasuCB : register(b0) {
    float2 g_SrcSize;
    float2 g_InvSrcSize;
    float2 g_DstSize;
    float2 g_InvDstSize;
};

float Luma(float3 c) {
    return dot(c, float3(0.299, 0.587, 0.114));
}

// Fast Lanczos2-like approximation used in FSR 1.0 EASU
float EasuWeight(float d2, float lob, float clp) {
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    return wB * wA;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 pp = uv * g_SrcSize - 0.5;
    float2 fp = floor(pp);
    float2 f  = pp - fp;
    float2 baseUv = (fp + 0.5) * g_InvSrcSize;

    // 12-tap cross-plus footprint (b,c, e,f,g,h, i,j,k,l, n,o)
    float3 b = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 0, -1) * g_InvSrcSize, 0).rgb;
    float3 c = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 1, -1) * g_InvSrcSize, 0).rgb;
    float3 e = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2(-1,  0) * g_InvSrcSize, 0).rgb;
    float3 f0= g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 0,  0) * g_InvSrcSize, 0).rgb;
    float3 g = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 1,  0) * g_InvSrcSize, 0).rgb;
    float3 h = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 2,  0) * g_InvSrcSize, 0).rgb;
    float3 i = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2(-1,  1) * g_InvSrcSize, 0).rgb;
    float3 j = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 0,  1) * g_InvSrcSize, 0).rgb;
    float3 k = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 1,  1) * g_InvSrcSize, 0).rgb;
    float3 l = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 2,  1) * g_InvSrcSize, 0).rgb;
    float3 n = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 0,  2) * g_InvSrcSize, 0).rgb;
    float3 o = g_SrcTex.SampleLevel(g_LinearClamp, baseUv + float2( 1,  2) * g_InvSrcSize, 0).rgb;

    // Directional gradient estimation on central 2x2 neighborhood
    float lb = Luma(b), lc = Luma(c), le = Luma(e), lf = Luma(f0);
    float lg = Luma(g), lh = Luma(h), li = Luma(i), lj = Luma(j);
    float lk = Luma(k), ll = Luma(l), ln = Luma(n), lo = Luma(o);

    float2 dir = float2(0.0, 0.0);
    float len = 0.0;

    // Accumulate gradient from the 4 bilinearly-weighted sub-quads
    float w0 = (1.0 - f.x) * (1.0 - f.y);
    float w1 = f.x * (1.0 - f.y);
    float w2 = (1.0 - f.x) * f.y;
    float w3 = f.x * f.y;

    float2 g0 = float2(lg - le, lj - lb);
    float2 g1 = float2(lh - lf, lk - lc);
    float2 g2 = float2(lk - li, ln - lf);
    float2 g3 = float2(ll - lj, lo - lg);

    dir = g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3;
    float dirLen2 = dot(dir, dir);
    float invDirLen = (dirLen2 > 1e-6) ? rsqrt(dirLen2) : 0.0;
    dir = (dirLen2 > 1e-6) ? (dir * invDirLen) : float2(1.0, 0.0);
    len = saturate(sqrt(dirLen2) * 2.0);

    // Anisotropic stretch along edge direction
    float stretch = 1.0 / max(abs(dir.x), abs(dir.y));
    float2 aniso = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    float3 taps[12] = { b, c, e, f0, g, h, i, j, k, l, n, o };
    float2 offs[12] = {
        float2( 0,-1), float2( 1,-1),
        float2(-1, 0), float2( 0, 0), float2( 1, 0), float2( 2, 0),
        float2(-1, 1), float2( 0, 1), float2( 1, 1), float2( 2, 1),
        float2( 0, 2), float2( 1, 2)
    };

    float3 acc = float3(0.0, 0.0, 0.0);
    float wSum = 0.0;
    [unroll]
    for (int t = 0; t < 12; ++t) {
        float2 d = offs[t] - f;
        // Rotate into edge-aligned basis
        float2 rd = float2(d.x * dir.x + d.y * dir.y, -d.x * dir.y + d.y * dir.x);
        rd *= aniso;
        float w = EasuWeight(dot(rd, rd), lob, clp);
        acc += taps[t] * w;
        wSum += w;
    }

    float3 color = acc / max(wSum, 1e-5);

    // Anti-ringing clamp to inner 2x2 min/max
    float3 min4 = min(min(f0, g), min(j, k));
    float3 max4 = max(max(f0, g), max(j, k));
    color = clamp(color, min4, max4);

    return float4(color, 1.0);
}
)HLSL";

// Pass 2: FSR 1.0 RCAS (Robust Contrast-Adaptive Sharpening)
static const char* kFsrRcasPS = R"HLSL(
Texture2D<float4> g_EasuTex : register(t0);

cbuffer RcasCB : register(b0) {
    float g_Sharpness; // 0.0 = sharpest, 1.0 = mildest (stops)
    int   g_EnableRcas;
    float2 g_Pad;
};

float Luma(float3 c) {
    return 0.5 * c.g + 0.25 * (c.r + c.b);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    int2 p = int2(pos.xy);
    float3 e = g_EasuTex.Load(int3(p, 0)).rgb;
    if (g_EnableRcas == 0) {
        return float4(e, 1.0);
    }

    float3 b = g_EasuTex.Load(int3(p + int2( 0, -1), 0)).rgb;
    float3 d = g_EasuTex.Load(int3(p + int2(-1,  0), 0)).rgb;
    float3 f = g_EasuTex.Load(int3(p + int2( 1,  0), 0)).rgb;
    float3 h = g_EasuTex.Load(int3(p + int2( 0,  1), 0)).rgb;

    float bL = Luma(b);
    float dL = Luma(d);
    float eL = Luma(e);
    float fL = Luma(f);
    float hL = Luma(h);

    // Noise detection (FSR RCAS standard)
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float maxL = max(max(max(bL, dL), max(fL, hL)), eL);
    float minL = min(min(min(bL, dL), min(fL, hL)), eL);
    nz = saturate(abs(nz) / max(maxL - minL, 1e-5));
    nz = -0.5 * nz + 1.0;

    // Min and max of ring
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));

    // Limiters to avoid clipping [0, 1]
    float3 hitMin = mn4 / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0));

    // Apply user sharpness (in stops: exp2(-sharpness)) and noise attenuation
    float sharpScale = exp2(-clamp(g_Sharpness, 0.0, 2.0));
    lobe *= sharpScale * nz;

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    float3 outColor = (lobe * (b + d + f + h) + e) * rcpL;
    return float4(saturate(outColor), 1.0);
}
)HLSL";

struct EasuCB {
    float srcSize[2];
    float invSrcSize[2];
    float dstSize[2];
    float invDstSize[2];
};

struct RcasCB {
    float sharpness;
    int enableRcas;
    float pad[2];
};

bool CompileShader(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& outBlob) {
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            outBlob.ReleaseAndGetAddressOf(), err.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        if (err) {
            std::cerr << "[paws] HLSL compile error (" << target << "):\n"
                      << static_cast<const char*>(err->GetBufferPointer()) << "\n";
        }
        return false;
    }
    return true;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Universal FSR 1.0 Overlay Loop (DX11 + DXGI Desktop Duplication)
// ============================================================================
int RunFsr1Overlay(HWND targetHwnd, float sharpness) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HMONITOR hMon = MonitorFromWindow(targetHwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW monInfo{};
    monInfo.cbSize = sizeof(monInfo);
    GetMonitorInfoW(hMon, &monInfo);
    int monX = monInfo.rcMonitor.left;
    int monY = monInfo.rcMonitor.top;
    int monW = monInfo.rcMonitor.right - monInfo.rcMonitor.left;
    int monH = monInfo.rcMonitor.bottom - monInfo.rcMonitor.top;

    // Create borderless click-through fullscreen overlay window
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PawsFsr1OverlayClass";
    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    HWND overlayHwnd = CreateWindowExW(
        exStyle, wc.lpszClassName, L"paws FSR 1.0 Overlay",
        WS_POPUP | WS_VISIBLE,
        monX, monY, monW, monH,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!overlayHwnd) {
        std::cerr << "[paws] ERROR: Failed to create overlay window.\n";
        return 1;
    }

    SetLayeredWindowAttributes(overlayHwnd, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(overlayHwnd, WDA_EXCLUDEFROMCAPTURE)) {
        std::cerr << "[paws] WARNING: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed (err "
                  << GetLastError() << "). Requires Windows 10 2004+.\n";
    }

    // Create D3D11 device & swapchain
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = static_cast<UINT>(monW);
    scd.BufferDesc.Height = static_cast<UINT>(monH);
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = overlayHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swapChain;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &fl, 1, D3D11_SDK_VERSION,
        &scd, swapChain.ReleaseAndGetAddressOf(),
        device.ReleaseAndGetAddressOf(), nullptr, ctx.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[paws] ERROR: D3D11CreateDeviceAndSwapChain failed (0x" << std::hex << hr << std::dec << ").\n";
        DestroyWindow(overlayHwnd);
        return 1;
    }

    ComPtr<ID3D11RenderTargetView> backbufferRTV;
    {
        ComPtr<ID3D11Texture2D> bb;
        swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(bb.ReleaseAndGetAddressOf()));
        device->CreateRenderTargetView(bb.Get(), nullptr, backbufferRTV.ReleaseAndGetAddressOf());
    }

    // Setup DXGI Desktop Duplication on matching monitor output
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> dxgiAdapter;
    ComPtr<IDXGIOutput> dxgiOutput;
    ComPtr<IDXGIOutput1> dxgiOutput1;
    ComPtr<IDXGIOutputDuplication> duplication;

    device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDev.ReleaseAndGetAddressOf()));
    dxgiDev->GetAdapter(dxgiAdapter.ReleaseAndGetAddressOf());

    auto initDuplication = [&]() -> bool {
        duplication.Reset();
        dxgiOutput.Reset();
        dxgiOutput1.Reset();
        for (UINT i = 0; dxgiAdapter->EnumOutputs(i, dxgiOutput.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC od{};
            dxgiOutput->GetDesc(&od);
            if (od.Monitor == hMon) break;
            dxgiOutput.Reset();
        }
        if (!dxgiOutput) {
            dxgiAdapter->EnumOutputs(0, dxgiOutput.ReleaseAndGetAddressOf());
        }
        if (!dxgiOutput) return false;
        if (FAILED(dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1),
                                              reinterpret_cast<void**>(dxgiOutput1.ReleaseAndGetAddressOf())))) {
            return false;
        }
        return SUCCEEDED(dxgiOutput1->DuplicateOutput(device.Get(), duplication.ReleaseAndGetAddressOf()));
    };

    if (!initDuplication()) {
        std::cerr << "[paws] ERROR: Failed to initialize DXGI Desktop Duplication.\n";
        DestroyWindow(overlayHwnd);
        return 1;
    }

    // Compile shaders
    ComPtr<ID3DBlob> vsBlob, easuBlob, rcasBlob;
    if (!CompileShader(kFullscreenVS, "main", "vs_5_0", vsBlob) ||
        !CompileShader(kFsrEasuPS, "main", "ps_5_0", easuBlob) ||
        !CompileShader(kFsrRcasPS, "main", "ps_5_0", rcasBlob)) {
        DestroyWindow(overlayHwnd);
        return 1;
    }

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> easuPS, rcasPS;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vs.ReleaseAndGetAddressOf());
    device->CreatePixelShader(easuBlob->GetBufferPointer(), easuBlob->GetBufferSize(), nullptr, easuPS.ReleaseAndGetAddressOf());
    device->CreatePixelShader(rcasBlob->GetBufferPointer(), rcasBlob->GetBufferSize(), nullptr, rcasPS.ReleaseAndGetAddressOf());

    // Intermediate EASU render target (fullscreen)
    ComPtr<ID3D11Texture2D> easuTex;
    ComPtr<ID3D11RenderTargetView> easuRTV;
    ComPtr<ID3D11ShaderResourceView> easuSRV;
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(monW);
        td.Height = static_cast<UINT>(monH);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&td, nullptr, easuTex.ReleaseAndGetAddressOf());
        device->CreateRenderTargetView(easuTex.Get(), nullptr, easuRTV.ReleaseAndGetAddressOf());
        device->CreateShaderResourceView(easuTex.Get(), nullptr, easuSRV.ReleaseAndGetAddressOf());
    }

    // Linear clamp sampler
    ComPtr<ID3D11SamplerState> sampler;
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&sd, sampler.ReleaseAndGetAddressOf());
    }

    // Constant buffers
    ComPtr<ID3D11Buffer> easuCBuf, rcasCBuf;
    {
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.ByteWidth = sizeof(EasuCB);
        device->CreateBuffer(&bd, nullptr, easuCBuf.ReleaseAndGetAddressOf());
        bd.ByteWidth = sizeof(RcasCB);
        device->CreateBuffer(&bd, nullptr, rcasCBuf.ReleaseAndGetAddressOf());
    }

    // Dynamic source texture (resized to match target window client rect)
    ComPtr<ID3D11Texture2D> srcTex;
    ComPtr<ID3D11ShaderResourceView> srcSRV;
    UINT srcW = 0, srcH = 0;

    auto ensureSrcTex = [&](UINT w, UINT h) {
        if (srcTex && srcW == w && srcH == h) return;
        srcTex.Reset();
        srcSRV.Reset();
        srcW = w;
        srcH = h;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&td, nullptr, srcTex.ReleaseAndGetAddressOf());
        device->CreateShaderResourceView(srcTex.Get(), nullptr, srcSRV.ReleaseAndGetAddressOf());
    };

    bool enableRcas = true;
    bool prevF8 = false;
    bool running = true;
    int frameCount = 0;
    auto fpsTimer = std::chrono::steady_clock::now();

    std::cout << "[paws] FSR 1.0 overlay active (" << monW << "x" << monH << ").\n";
    std::cout << "[paws] Hotkeys: F8 = Toggle RCAS Sharpening | END = Quit\n";

    while (running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!IsWindow(targetHwnd)) {
            std::cout << "\n[paws] Target window closed. Exiting.\n";
            break;
        }

        // Hotkeys
        bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8Down && !prevF8) {
            enableRcas = !enableRcas;
            std::cout << "\n[paws] RCAS Sharpening: " << (enableRcas ? "ON" : "OFF") << "\n";
        }
        prevF8 = f8Down;

        if (GetAsyncKeyState(VK_END) & 0x8000) {
            std::cout << "\n[paws] END pressed. Exiting.\n";
            break;
        }

        // Capture desktop frame
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> desktopRes;
        hr = duplication->AcquireNextFrame(16, &frameInfo, desktopRes.ReleaseAndGetAddressOf());
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            initDuplication();
            continue;
        }
        if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Texture2D> desktopTex;
            desktopRes->QueryInterface(__uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(desktopTex.ReleaseAndGetAddressOf()));

            RECT clientRc{};
            GetClientRect(targetHwnd, &clientRc);
            POINT tl{clientRc.left, clientRc.top};
            POINT br{clientRc.right, clientRc.bottom};
            ClientToScreen(targetHwnd, &tl);
            ClientToScreen(targetHwnd, &br);

            int x0 = std::clamp(static_cast<int>(tl.x) - monX, 0, monW - 1);
            int y0 = std::clamp(static_cast<int>(tl.y) - monY, 0, monH - 1);
            int x1 = std::clamp(static_cast<int>(br.x) - monX, x0 + 1, monW);
            int y1 = std::clamp(static_cast<int>(br.y) - monY, y0 + 1, monH);

            UINT cw = static_cast<UINT>(x1 - x0);
            UINT ch = static_cast<UINT>(y1 - y0);
            if (cw >= 16 && ch >= 16) {
                ensureSrcTex(cw, ch);
                D3D11_BOX box{};
                box.left = static_cast<UINT>(x0);
                box.top = static_cast<UINT>(y0);
                box.right = static_cast<UINT>(x1);
                box.bottom = static_cast<UINT>(y1);
                box.front = 0;
                box.back = 1;
                ctx->CopySubresourceRegion(srcTex.Get(), 0, 0, 0, 0, desktopTex.Get(), 0, &box);
            }
            duplication->ReleaseFrame();
        }

        if (!srcSRV) continue;

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(monW);
        vp.Height = static_cast<float>(monH);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);

        // Pass 1: EASU
        EasuCB easuData{};
        easuData.srcSize[0] = static_cast<float>(srcW);
        easuData.srcSize[1] = static_cast<float>(srcH);
        easuData.invSrcSize[0] = 1.0f / easuData.srcSize[0];
        easuData.invSrcSize[1] = 1.0f / easuData.srcSize[1];
        easuData.dstSize[0] = static_cast<float>(monW);
        easuData.dstSize[1] = static_cast<float>(monH);
        easuData.invDstSize[0] = 1.0f / easuData.dstSize[0];
        easuData.invDstSize[1] = 1.0f / easuData.dstSize[1];
        ctx->UpdateSubresource(easuCBuf.Get(), 0, nullptr, &easuData, 0, 0);

        ID3D11RenderTargetView* rtv1 = easuRTV.Get();
        ID3D11ShaderResourceView* srv1 = srcSRV.Get();
        ID3D11SamplerState* smp = sampler.Get();
        ID3D11Buffer* cb1 = easuCBuf.Get();
        ctx->OMSetRenderTargets(1, &rtv1, nullptr);
        ctx->PSSetShader(easuPS.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb1);
        ctx->PSSetShaderResources(0, 1, &srv1);
        ctx->PSSetSamplers(0, 1, &smp);
        ctx->Draw(3, 0);

        // Unbind SRV before Pass 2
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);

        // Pass 2: RCAS -> Backbuffer
        RcasCB rcasData{};
        rcasData.sharpness = sharpness;
        rcasData.enableRcas = enableRcas ? 1 : 0;
        ctx->UpdateSubresource(rcasCBuf.Get(), 0, nullptr, &rcasData, 0, 0);

        ID3D11RenderTargetView* rtv2 = backbufferRTV.Get();
        ID3D11ShaderResourceView* srv2 = easuSRV.Get();
        ID3D11Buffer* cb2 = rcasCBuf.Get();
        ctx->OMSetRenderTargets(1, &rtv2, nullptr);
        ctx->PSSetShader(rcasPS.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb2);
        ctx->PSSetShaderResources(0, 1, &srv2);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &nullSrv);

        swapChain->Present(1, 0);

        // Live console FPS output
        ++frameCount;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - fpsTimer).count();
        if (dt >= 0.5) {
            double fps = static_cast<double>(frameCount) / dt;
            std::printf("\r[paws] FPS: %6.1f | %ux%u -> %dx%d | RCAS: %-3s   ",
                        fps, srcW, srcH, monW, monH, enableRcas ? "ON" : "OFF");
            std::fflush(stdout);
            frameCount = 0;
            fpsTimer = now;
        }
    }

    DestroyWindow(overlayHwnd);
    return 0;
}

void PrintUsage() {
    std::cout << "paws - Universal FSR 1.0 Upscaler & GTA V Legacy FSR 2/3 Smart Launcher\n"
              << "Usage:\n"
              << "  paws.exe <path-to-exe-or-window-title> [--sharpness <0.0-2.0>]\n\n"
              << "Examples:\n"
              << "  paws.exe \"C:\\Games\\GTAV\\GTA5.exe\"   (auto-deploys paws_gtav.asi & launches directly)\n"
              << "  paws.exe \"Some Window Title\"          (attaches universal FSR 1.0 overlay)\n"
              << "  paws.exe \"C:\\Apps\\Game.exe\"          (launches & upscales with FSR 1.0)\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    std::string targetArg;
    float sharpness = 0.2f;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (a == "--sharpness" && i + 1 < argc) {
            sharpness = std::stof(argv[++i]);
        } else if (targetArg.empty()) {
            targetArg = a;
        }
    }

    if (targetArg.empty()) {
        PrintUsage();
        return 1;
    }

    std::wstring wTarget = Utf8ToWide(targetArg);
    fs::path targetPath(wTarget);

    // ------------------------------------------------------------------------
    // Auto-Detection & Routing: Check for GTA5.exe (path or running process)
    // ------------------------------------------------------------------------
    if (IsGta5ExeName(wTarget)) {
        DWORD runningPid = 0;
        fs::path runningPath;
        bool isRunning = FindRunningGta5(runningPid, runningPath);
        if (fs::exists(targetPath)) {
            return RunGtaVSmartLauncher(targetPath, isRunning, runningPid);
        } else if (isRunning && !runningPath.empty()) {
            return RunGtaVSmartLauncher(runningPath, true, runningPid);
        } else {
            std::cerr << "[paws] ERROR: GTA5.exe was specified but file does not exist and process is not running.\n";
            return 1;
        }
    }

    // Also check if user passed a window title belonging to a running GTA5.exe
    if (HWND existing = FindWindowByTitle(wTarget)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(existing, &pid);
        std::wstring imgPath = GetProcessImagePath(pid);
        if (!imgPath.empty() && IsGta5ExeName(imgPath)) {
            return RunGtaVSmartLauncher(fs::path(imgPath), true, pid);
        }
        std::cout << "[paws] Attached to existing window matching: \"" << targetArg << "\"\n";
        return RunFsr1Overlay(existing, sharpness);
    }

    // If targetArg is an existing executable path, launch it and wait for its window
    if (fs::exists(targetPath) && fs::is_regular_file(targetPath)) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmdLine = L"\"" + targetPath.wstring() + L"\"";
        std::wstring workDir = targetPath.parent_path().wstring();

        std::cout << "[paws] Launching: " << targetArg << "\n";
        if (!CreateProcessW(targetPath.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                            0, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
            std::cerr << "[paws] ERROR: Failed to launch executable (error " << GetLastError() << ").\n";
            return 1;
        }
        CloseHandle(pi.hThread);

        std::cout << "[paws] Waiting for application window...\n";
        HWND hwnd = nullptr;
        for (int attempt = 0; attempt < 150; ++attempt) {
            hwnd = FindWindowByPid(pi.dwProcessId);
            if (hwnd) break;
            if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
                std::cerr << "[paws] Target process exited before creating a window.\n";
                CloseHandle(pi.hProcess);
                return 1;
            }
        }
        CloseHandle(pi.hProcess);

        if (!hwnd) {
            std::cerr << "[paws] ERROR: Timed out waiting for target window.\n";
            return 1;
        }
        return RunFsr1Overlay(hwnd, sharpness);
    }

    std::cerr << "[paws] ERROR: Could not find window or executable matching: \"" << targetArg << "\"\n";
    return 1;
}
