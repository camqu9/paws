#pragma once

#include "paws_common.h"

#include <d3d11_1.h>
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace paws {

// Zero-dependency NVIDIA NGX ABI definitions matching NVIDIA/DLSS nvsdk_ngx_params.h
enum NvxResult : uint32_t {
    NVX_SUCCESS = 0x1,
    NVX_FAIL = 0xBAD00000,
};

enum NvxFeature : int {
    NVX_FEATURE_FRAME_GENERATION = 11,
};

enum NvxVersion : int {
    NVX_VERSION_API = 0x0000015,
};

struct NvxHandle {
    unsigned int Id;
};

struct NvxParameter {
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;

    virtual NvxResult Get(const char* name, unsigned long long* out) const = 0;
    virtual NvxResult Get(const char* name, float* out) const = 0;
    virtual NvxResult Get(const char* name, double* out) const = 0;
    virtual NvxResult Get(const char* name, unsigned int* out) const = 0;
    virtual NvxResult Get(const char* name, int* out) const = 0;
    virtual NvxResult Get(const char* name, ID3D11Resource** out) const = 0;
    virtual NvxResult Get(const char* name, ID3D12Resource** out) const = 0;
    virtual NvxResult Get(const char* name, void** out) const = 0;

    virtual void Reset() = 0;
};

class NvxParameterMap final : public NvxParameter {
public:
    void Set(const char* n, unsigned long long v) override { values_[n] = Number(static_cast<double>(v)); }
    void Set(const char* n, float v) override { values_[n] = Number(v); }
    void Set(const char* n, double v) override { values_[n] = Number(v); }
    void Set(const char* n, unsigned int v) override { values_[n] = Number(v); }
    void Set(const char* n, int v) override { values_[n] = Number(v); }
    void Set(const char* n, ID3D11Resource* v) override { values_[n] = Pointer(v, Kind::D3D11); }
    void Set(const char* n, ID3D12Resource* v) override { values_[n] = Pointer(v, Kind::D3D12); }
    void Set(const char* n, void* v) override { values_[n] = Pointer(v, Kind::ANY); }

    NvxResult Get(const char* n, unsigned long long* v) const override { return Read(n, v); }
    NvxResult Get(const char* n, float* v) const override { return Read(n, v); }
    NvxResult Get(const char* n, double* v) const override { return Read(n, v); }
    NvxResult Get(const char* n, unsigned int* v) const override { return Read(n, v); }
    NvxResult Get(const char* n, int* v) const override { return Read(n, v); }
    NvxResult Get(const char* n, ID3D11Resource** v) const override { return ReadPtr(n, v, Kind::D3D11); }
    NvxResult Get(const char* n, ID3D12Resource** v) const override { return ReadPtr(n, v, Kind::D3D12); }
    NvxResult Get(const char* n, void** v) const override { return ReadPtr(n, v, Kind::ANY); }

    void Reset() override { values_.clear(); }

private:
    enum class Kind { NUMBER, D3D11, D3D12, ANY };
    struct Value {
        Kind kind = Kind::NUMBER;
        double number = 0.0;
        void* ptr = nullptr;
    };
    static Value Number(double v) { return {Kind::NUMBER, v, nullptr}; }
    static Value Pointer(void* p, Kind k) { return {k, 0.0, p}; }

    template <typename T>
    NvxResult Read(const char* n, T* out) const {
        auto it = values_.find(n);
        if (!out || it == values_.end() || it->second.kind != Kind::NUMBER) return NVX_FAIL;
        *out = static_cast<T>(it->second.number);
        return NVX_SUCCESS;
    }

    template <typename T>
    NvxResult ReadPtr(const char* n, T** out, Kind k) const {
        auto it = values_.find(n);
        if (!out || it == values_.end() || it->second.kind == Kind::NUMBER ||
            (k != Kind::ANY && it->second.kind != Kind::ANY && it->second.kind != k)) {
            return NVX_FAIL;
        }
        *out = static_cast<T*>(it->second.ptr);
        return NVX_SUCCESS;
    }

    std::unordered_map<std::string, Value> values_;
};

// Apex Neural Rendering Frame Generation (ANR FG) DX11 <-> D3D12 bridge
class AnrFrameGen {
public:
    AnrFrameGen() = default;
    ~AnrFrameGen() { Shutdown(); }

    void SetModuleDirectory(const std::wstring& dir) { moduleDir_ = dir; }

    // Ensures D3D12 device, interop textures, and ANR FG feature are ready for (width, height, fmt)
    bool EnsureReady(ID3D11Device* d3d11Dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt);

    // Feeds current real DX11 frame into ANR FG (D3D12) and writes AI-generated frame into outInterpTex.
    // Returns true if an interpolated frame was generated (false on frame 0 reset or failure).
    bool GenerateFrame(ID3D11DeviceContext* d3d11Ctx,
                       ID3D11Texture2D* realBackbuffer,
                       ID3D11Texture2D* outInterpTex);

    void OnResize();
    void Shutdown();

    bool IsReady() const { return ready_; }
    bool HasFailedPermanently() const { return failedPermanently_; }

private:
    bool InitD3D12AndDll();
    bool CreateResources(ID3D11Device* d3d11Dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt);
    void ReleaseResources();
    bool SubmitAndWait();
    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

    using PFN_InitExt = NvxResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, NvxVersion, const void*);
    using PFN_Populate = NvxResult(__cdecl*)(NvxParameter*);
    using PFN_CreateFeature = NvxResult(__cdecl*)(ID3D12GraphicsCommandList*, NvxFeature, NvxParameter*, NvxHandle**);
    using PFN_EvaluateFeature = NvxResult(__cdecl*)(ID3D12GraphicsCommandList*, const NvxHandle*, NvxParameter*, void*);
    using PFN_ReleaseFeature = NvxResult(__cdecl*)(NvxHandle*);
    using PFN_Shutdown1 = NvxResult(__cdecl*)(ID3D12Device*);

    std::wstring moduleDir_;
    bool d3d12Initialized_ = false;
    bool ready_ = false;
    bool failedPermanently_ = false;
    bool useSharedHandles_ = false;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT dx11Format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT d3d12Format_ = DXGI_FORMAT_UNKNOWN;
    bool swapRB_ = false;
    uint64_t frameCount_ = 0;

    // DLLs & NGX entry points
    HMODULE d3d12Mod_ = nullptr;
    HMODULE anrMod_ = nullptr;
    PFN_InitExt ngxInitExt_ = nullptr;
    PFN_Populate ngxPopulate_ = nullptr;
    PFN_CreateFeature ngxCreate_ = nullptr;
    PFN_EvaluateFeature ngxEvaluate_ = nullptr;
    PFN_ReleaseFeature ngxRelease_ = nullptr;
    PFN_Shutdown1 ngxShutdown_ = nullptr;
    NvxParameterMap paramMap_;
    NvxHandle* fgHandle_ = nullptr;

    // D3D12 core objects
    ID3D12Device* d3d12Dev_ = nullptr;
    ID3D12CommandQueue* cmdQueue_ = nullptr;
    ID3D12CommandAllocator* cmdAlloc_ = nullptr;
    ID3D12GraphicsCommandList* cmdList_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fenceEvent_ = nullptr;
    uint64_t fenceValue_ = 0;

    // D3D12 textures for ANR FG
    ID3D12Resource* d3d12Backbuffer_ = nullptr;
    ID3D12Resource* d3d12Depth_ = nullptr;
    ID3D12Resource* d3d12MVecs_ = nullptr;
    ID3D12Resource* d3d12OutInterp_ = nullptr;
    ID3D12Resource* d3d12OutReal_ = nullptr;

    // Shared-handle zero-copy bridge (preferred)
    HANDLE sharedInHandle_ = nullptr;
    HANDLE sharedOutHandle_ = nullptr;
    ID3D11Texture2D* dx11SharedIn_ = nullptr;
    ID3D11Texture2D* dx11SharedOut_ = nullptr;

    // Staging fallback bridge (works on all drivers if shared handles are unsupported)
    ID3D11Texture2D* dx11StagingRead_ = nullptr;
    ID3D11Texture2D* dx11StagingWrite_ = nullptr;
    ID3D12Resource* d3d12UploadBuf_ = nullptr;
    ID3D12Resource* d3d12ReadbackBuf_ = nullptr;
    uint8_t* uploadPtr_ = nullptr;
    uint8_t* readbackPtr_ = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT inFootprint_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT outFootprint_{};
};

} // namespace paws
