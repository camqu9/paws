#include "anr_fg.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace paws {

namespace {

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

void SafeCloseHandle(HANDLE& h) {
    if (h) {
        CloseHandle(h);
        h = nullptr;
    }
}

bool NvxSucceeded(NvxResult r) {
    return (static_cast<uint32_t>(r) & 0xFFF00000u) != 0xBAD00000u;
}

DXGI_FORMAT NormalizeBackbufferFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        default: return fmt;
    }
}

} // namespace

void AnrFrameGen::Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (!res || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList_->ResourceBarrier(1, &b);
}

bool AnrFrameGen::SubmitAndWait() {
    if (FAILED(cmdList_->Close())) return false;
    ID3D12CommandList* lists[] = { cmdList_ };
    cmdQueue_->ExecuteCommandLists(1, lists);
    ++fenceValue_;
    if (FAILED(cmdQueue_->Signal(fence_, fenceValue_))) return false;
    if (fence_->GetCompletedValue() < fenceValue_) {
        fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, 5000);
    }
    if (FAILED(cmdAlloc_->Reset())) return false;
    if (FAILED(cmdList_->Reset(cmdAlloc_, nullptr))) return false;
    return d3d12Dev_->GetDeviceRemovedReason() == S_OK;
}

bool AnrFrameGen::InitD3D12AndDll() {
    if (d3d12Initialized_) return true;
    if (failedPermanently_) return false;

    // 1. Locate ANR_FG\nvngx_dlssg.dll next to paws_gtav.asi
    std::filesystem::path dllPath = std::filesystem::path(moduleDir_) / L"ANR_FG" / L"nvngx_dlssg.dll";
    if (!std::filesystem::exists(dllPath)) {
        PAWS_LOG("ANR FG: %ls not found — ANR Frame Generation disabled (using RCAS fallback).", dllPath.c_str());
        failedPermanently_ = true;
        return false;
    }

    // 2. Load d3d12.dll dynamically and create D3D12 device
    d3d12Mod_ = LoadLibraryW(L"d3d12.dll");
    if (!d3d12Mod_) {
        PAWS_LOG("ANR FG: Failed to load d3d12.dll (err=%lu).", GetLastError());
        failedPermanently_ = true;
        return false;
    }

    auto pfnCreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(d3d12Mod_, "D3D12CreateDevice"));
    if (!pfnCreateDevice) {
        PAWS_LOG("ANR FG: D3D12CreateDevice export missing.");
        failedPermanently_ = true;
        return false;
    }

    HRESULT hr = pfnCreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Dev_));
    if (FAILED(hr) || !d3d12Dev_) {
        PAWS_LOG("ANR FG: D3D12CreateDevice failed (0x%08X).", static_cast<unsigned>(hr));
        failedPermanently_ = true;
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d3d12Dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&cmdQueue_))) ||
        FAILED(d3d12Dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc_))) ||
        FAILED(d3d12Dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc_, nullptr, IID_PPV_ARGS(&cmdList_))) ||
        FAILED(d3d12Dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        PAWS_LOG("ANR FG: Failed to create D3D12 queue/allocator/list/fence.");
        failedPermanently_ = true;
        return false;
    }
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 3. Load ANR's nvngx_dlssg.dll directly (no OptiScaler or _nvngx.dll needed!)
    anrMod_ = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!anrMod_) {
        PAWS_LOG("ANR FG: LoadLibraryExW failed for %ls (err=%lu).", dllPath.c_str(), GetLastError());
        failedPermanently_ = true;
        return false;
    }

    ngxInitExt_ = reinterpret_cast<PFN_InitExt>(GetProcAddress(anrMod_, "NVSDK_NGX_D3D12_Init_Ext"));
    ngxPopulate_ = reinterpret_cast<PFN_Populate>(GetProcAddress(anrMod_, "NVSDK_NGX_D3D12_PopulateParameters_Impl"));
    ngxCreate_ = reinterpret_cast<PFN_CreateFeature>(GetProcAddress(anrMod_, "NVSDK_NGX_D3D12_CreateFeature"));
    ngxEvaluate_ = reinterpret_cast<PFN_EvaluateFeature>(GetProcAddress(anrMod_, "NVSDK_NGX_D3D12_EvaluateFeature"));
    ngxRelease_ = reinterpret_cast<PFN_ReleaseFeature>(GetProcAddress(anrMod_, "NVSDK_NGX_D3D12_ReleaseFeature"));
    ngxShutdown_ = reinterpret_cast<PFN_Shutdown1>(GetProcAddress(anrMod_, "NVSDK_NGX_D3D12_Shutdown1"));

    if (!ngxInitExt_ || !ngxCreate_ || !ngxEvaluate_ || !ngxRelease_) {
        PAWS_LOG("ANR FG: nvngx_dlssg.dll missing required NGX D3D12 exports.");
        failedPermanently_ = true;
        return false;
    }

    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);

    NvxResult r = ngxInitExt_(1ULL, tempDir, d3d12Dev_, NVX_VERSION_API, nullptr);
    if (!NvxSucceeded(r)) {
        PAWS_LOG("ANR FG: NVSDK_NGX_D3D12_Init_Ext failed (0x%08X).", static_cast<unsigned>(r));
        failedPermanently_ = true;
        return false;
    }

    if (ngxPopulate_) {
        ngxPopulate_(&paramMap_);
    }

    unsigned int maxCount = 0;
    paramMap_.Get("DLSSG.MultiFrameCountMax", &maxCount);
    PAWS_LOG("ANR FG: Initialized nvngx_dlssg.dll on D3D12 (MultiFrameCountMax=%u).", maxCount);

    d3d12Initialized_ = true;
    return true;
}

void AnrFrameGen::ReleaseResources() {
    if (fgHandle_ && ngxRelease_) {
        ngxRelease_(fgHandle_);
        fgHandle_ = nullptr;
    }
    if (d3d12UploadBuf_ && uploadPtr_) {
        d3d12UploadBuf_->Unmap(0, nullptr);
        uploadPtr_ = nullptr;
    }
    if (d3d12ReadbackBuf_ && readbackPtr_) {
        d3d12ReadbackBuf_->Unmap(0, nullptr);
        readbackPtr_ = nullptr;
    }
    SafeRelease(dx11SharedIn_);
    SafeRelease(dx11SharedOut_);
    SafeCloseHandle(sharedInHandle_);
    SafeCloseHandle(sharedOutHandle_);
    SafeRelease(dx11StagingRead_);
    SafeRelease(dx11StagingWrite_);
    SafeRelease(d3d12UploadBuf_);
    SafeRelease(d3d12ReadbackBuf_);
    SafeRelease(d3d12Backbuffer_);
    SafeRelease(d3d12Depth_);
    SafeRelease(d3d12MVecs_);
    SafeRelease(d3d12OutInterp_);
    SafeRelease(d3d12OutReal_);

    ready_ = false;
    useSharedHandles_ = false;
    width_ = 0;
    height_ = 0;
    frameCount_ = 0;
}

bool AnrFrameGen::CreateResources(ID3D11Device* d3d11Dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt) {
    ReleaseResources();

    DXGI_FORMAT normFmt = NormalizeBackbufferFormat(fmt);
    dx11Format_ = normFmt;
    d3d12Format_ = normFmt;
    swapRB_ = false;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto makeTex = [&](DXGI_FORMAT texFmt, D3D12_RESOURCE_FLAGS flags,
                       D3D12_HEAP_FLAGS heapFlags, D3D12_RESOURCE_STATES initState,
                       ID3D12Resource** outRes) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = width;
        td.Height = height;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = texFmt;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = flags;
        return SUCCEEDED(d3d12Dev_->CreateCommittedResource(
            &defaultHeap, heapFlags, &td, initState, nullptr, IID_PPV_ARGS(outRes)));
    };

    // 1. Try zero-copy DX11 <-> D3D12 Shared NT Handles first (fastest!)
    ID3D11Device1* d3d11Dev1 = nullptr;
    if (d3d11Dev && SUCCEEDED(d3d11Dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d3d11Dev1))) && d3d11Dev1) {
        bool okIn = makeTex(d3d12Format_,
                            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
                            D3D12_HEAP_FLAG_SHARED,
                            D3D12_RESOURCE_STATE_COMMON,
                            &d3d12Backbuffer_);
        bool okOut = okIn && makeTex(d3d12Format_,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
                                     D3D12_HEAP_FLAG_SHARED,
                                     D3D12_RESOURCE_STATE_COMMON,
                                     &d3d12OutInterp_);
        bool okReal = okOut && makeTex(d3d12Format_,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       D3D12_HEAP_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       &d3d12OutReal_);

        if (okReal &&
            SUCCEEDED(d3d12Dev_->CreateSharedHandle(d3d12Backbuffer_, nullptr, GENERIC_ALL, nullptr, &sharedInHandle_)) &&
            SUCCEEDED(d3d12Dev_->CreateSharedHandle(d3d12OutInterp_, nullptr, GENERIC_ALL, nullptr, &sharedOutHandle_)) &&
            SUCCEEDED(d3d11Dev1->OpenSharedResource1(sharedInHandle_, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dx11SharedIn_))) &&
            SUCCEEDED(d3d11Dev1->OpenSharedResource1(sharedOutHandle_, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dx11SharedOut_)))) {
            useSharedHandles_ = true;
            PAWS_LOG("ANR FG: Using ZERO-COPY DX11<->D3D12 shared NT handle bridge (%ux%u, fmt=%d).",
                     width, height, static_cast<int>(d3d12Format_));
        } else {
            ReleaseResources();
        }
        d3d11Dev1->Release();
    }

    // 2. Fallback: Staging bridge (guaranteed to work on all DXVK/VKD3D/Windows configs)
    if (!useSharedHandles_) {
        bool okOut = makeTex(d3d12Format_, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &d3d12OutInterp_);
        if (!okOut && d3d12Format_ == DXGI_FORMAT_B8G8R8A8_UNORM) {
            d3d12Format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapRB_ = true;
            okOut = makeTex(d3d12Format_, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &d3d12OutInterp_);
        }
        bool okReal = okOut && makeTex(d3d12Format_, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &d3d12OutReal_);
        bool okIn = okReal && makeTex(d3d12Format_, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &d3d12Backbuffer_);
        if (!okIn) {
            PAWS_LOG("ANR FG: Failed to create D3D12 textures (%ux%u, fmt=%d).", width, height, static_cast<int>(d3d12Format_));
            ReleaseResources();
            return false;
        }

        // Upload + Readback buffers
        D3D12_RESOURCE_DESC inDesc = d3d12Backbuffer_->GetDesc();
        D3D12_RESOURCE_DESC outDesc = d3d12OutInterp_->GetDesc();
        UINT64 inTotal = 0, outTotal = 0;
        d3d12Dev_->GetCopyableFootprints(&inDesc, 0, 1, 0, &inFootprint_, nullptr, nullptr, &inTotal);
        d3d12Dev_->GetCopyableFootprints(&outDesc, 0, 1, 0, &outFootprint_, nullptr, nullptr, &outTotal);

        auto makeBuf = [&](UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, ID3D12Resource** outBuf) -> bool {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = type;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = bytes;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return SUCCEEDED(d3d12Dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(outBuf)));
        };

        if (!makeBuf(inTotal, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &d3d12UploadBuf_) ||
            !makeBuf(outTotal, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, &d3d12ReadbackBuf_) ||
            FAILED(d3d12UploadBuf_->Map(0, nullptr, reinterpret_cast<void**>(&uploadPtr_))) ||
            FAILED(d3d12ReadbackBuf_->Map(0, nullptr, reinterpret_cast<void**>(&readbackPtr_)))) {
            PAWS_LOG("ANR FG: Failed to create/map D3D12 upload/readback buffers.");
            ReleaseResources();
            return false;
        }

        if (d3d11Dev) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = width;
            sd.Height = height;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = dx11Format_;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(d3d11Dev->CreateTexture2D(&sd, nullptr, &dx11StagingRead_))) {
                ReleaseResources();
                return false;
            }
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(d3d11Dev->CreateTexture2D(&sd, nullptr, &dx11StagingWrite_))) {
                ReleaseResources();
                return false;
            }
        }
        PAWS_LOG("ANR FG: Using staging DX11<->D3D12 bridge (%ux%u, fmt=%d, swapRB=%d).",
                 width, height, static_cast<int>(d3d12Format_), swapRB_ ? 1 : 0);
    }

    // 2.5 Create Depth (R32_FLOAT) and Motion Vectors (R16G16_FLOAT) textures required by ANR FG
    if (!makeTex(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                 D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &d3d12Depth_) ||
        !makeTex(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                 D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &d3d12MVecs_)) {
        PAWS_LOG("ANR FG: Failed to create D3D12 depth/motion textures (%ux%u).", width, height);
        ReleaseResources();
        return false;
    }

    // 3. Create ANR FG feature on D3D12 command list
    paramMap_.Set("CreationNodeMask", 1u);
    paramMap_.Set("VisibilityNodeMask", 1u);
    paramMap_.Set("Width", width);
    paramMap_.Set("Height", height);
    paramMap_.Set("DLSSG.Width", width);
    paramMap_.Set("DLSSG.Height", height);
    paramMap_.Set("DLSSG.BackbufferFormat", static_cast<unsigned int>(d3d12Format_));
    paramMap_.Set("DLSSG.InternalWidth", width);
    paramMap_.Set("DLSSG.InternalHeight", height);
    paramMap_.Set("DLSSG.DynamicResolution", 0u);

    NvxResult r = ngxCreate_(cmdList_, NVX_FEATURE_FRAME_GENERATION, &paramMap_, &fgHandle_);
    if (!NvxSucceeded(r) || !fgHandle_) {
        PAWS_LOG("ANR FG: NVSDK_NGX_D3D12_CreateFeature failed (0x%08X).", static_cast<unsigned>(r));
        ReleaseResources();
        failedPermanently_ = true;
        return false;
    }

    if (!SubmitAndWait()) {
        PAWS_LOG("ANR FG: D3D12 device lost during CreateFeature.");
        ReleaseResources();
        failedPermanently_ = true;
        return false;
    }

    width_ = width;
    height_ = height;
    frameCount_ = 0;
    ready_ = true;
    PAWS_LOG("ANR FG: Feature created and READY (%ux%u, 2X AI Frame Generation active!).", width, height);
    return true;
}

bool AnrFrameGen::EnsureReady(ID3D11Device* d3d11Dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt) {
    if (failedPermanently_ || width < 320 || height < 240) return false;
    if (!InitD3D12AndDll()) return false;
    DXGI_FORMAT normFmt = NormalizeBackbufferFormat(fmt);
    if (ready_ && width_ == width && height_ == height && dx11Format_ == normFmt) {
        return true;
    }
    return CreateResources(d3d11Dev, width, height, normFmt);
}

bool AnrFrameGen::GenerateFrame(ID3D11DeviceContext* d3d11Ctx,
                                ID3D11Texture2D* realBackbuffer,
                                ID3D11Texture2D* outInterpTex) {
    if (!ready_ || !fgHandle_) return false;

    // 1. Copy DX11 realBackbuffer -> D3D12 d3d12Backbuffer_
    if (useSharedHandles_) {
        if (d3d11Ctx && realBackbuffer && dx11SharedIn_) {
            d3d11Ctx->CopyResource(dx11SharedIn_, realBackbuffer);
            d3d11Ctx->Flush();
        }
        Transition(d3d12Backbuffer_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(d3d12OutInterp_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } else {
        if (d3d11Ctx && realBackbuffer && dx11StagingRead_) {
            d3d11Ctx->CopyResource(dx11StagingRead_, realBackbuffer);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(d3d11Ctx->Map(dx11StagingRead_, 0, D3D11_MAP_READ, 0, &mapped))) return false;
            const size_t rowBytes = static_cast<size_t>(width_) * 4;
            for (uint32_t y = 0; y < height_; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                uint8_t* dst = uploadPtr_ + inFootprint_.Offset + y * inFootprint_.Footprint.RowPitch;
                std::memcpy(dst, src, rowBytes);
                if (swapRB_) {
                    for (size_t x = 0; x < rowBytes; x += 4) std::swap(dst[x], dst[x + 2]);
                }
            }
            d3d11Ctx->Unmap(dx11StagingRead_, 0);
        }

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = d3d12Backbuffer_;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = d3d12UploadBuf_;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = inFootprint_;
        cmdList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        Transition(d3d12Backbuffer_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // 2. Evaluate ANR FG (1 interpolated frame between previous real frame and current real frame)
    const bool isReset = (frameCount_ == 0);
    paramMap_.Set("Width", width_);
    paramMap_.Set("Height", height_);
    paramMap_.Set("DLSSG.Width", width_);
    paramMap_.Set("DLSSG.Height", height_);
    paramMap_.Set("DLSSG.BackbufferFormat", static_cast<unsigned int>(d3d12Format_));
    paramMap_.Set("DLSSG.Reset", isReset ? 1u : 0u);
    paramMap_.Set("DLSSG.MultiFrameCount", 1u);
    paramMap_.Set("DLSSG.MultiFrameIndex", 1u);
    paramMap_.Set("DLSSG.Backbuffer", d3d12Backbuffer_);
    paramMap_.Set("DLSSG.HUDLess", static_cast<ID3D12Resource*>(nullptr));
    paramMap_.Set("DLSSG.UI", static_cast<ID3D12Resource*>(nullptr));
    paramMap_.Set("DLSSG.UIAlpha", static_cast<ID3D12Resource*>(nullptr));
    paramMap_.Set("DLSSG.Depth", d3d12Depth_);
    paramMap_.Set("DLSSG.MVecs", d3d12MVecs_);
    paramMap_.Set("DLSSG.DepthInverted", 1u);
    paramMap_.Set("DLSSG.MvecScaleX", 1.0f);
    paramMap_.Set("DLSSG.MvecScaleY", 1.0f);
    paramMap_.Set("DLSSG.CameraNear", 0.15f);
    paramMap_.Set("DLSSG.CameraFar", 10000.0f);
    paramMap_.Set("DLSSG.CameraFOV", 1.0f);
    paramMap_.Set("DLSSG.CameraAspectRatio", static_cast<float>(width_) / static_cast<float>(height_));
    paramMap_.Set("DLSSG.OutputInterpolated", d3d12OutInterp_);
    paramMap_.Set("DLSSG.OutputReal", d3d12OutReal_);

    NvxResult r = ngxEvaluate_(cmdList_, fgHandle_, &paramMap_, nullptr);
    if (!NvxSucceeded(r)) {
        PAWS_LOG("ANR FG: NVSDK_NGX_D3D12_EvaluateFeature failed (0x%08X) — disabling ANR FG.", static_cast<unsigned>(r));
        failedPermanently_ = true;
        ready_ = false;
        return false;
    }

    // 3. Copy D3D12 d3d12OutInterp_ -> DX11 outInterpTex
    if (useSharedHandles_) {
        Transition(d3d12OutInterp_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Transition(d3d12Backbuffer_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!SubmitAndWait()) return false;
        if (!isReset && d3d11Ctx && outInterpTex && dx11SharedOut_) {
            d3d11Ctx->CopyResource(outInterpTex, dx11SharedOut_);
        }
    } else {
        if (!isReset) {
            Transition(d3d12OutInterp_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = d3d12ReadbackBuf_;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dstLoc.PlacedFootprint = outFootprint_;
            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = d3d12OutInterp_;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmdList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
            Transition(d3d12OutInterp_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        Transition(d3d12Backbuffer_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!SubmitAndWait()) return false;

        if (!isReset && d3d11Ctx && outInterpTex && dx11StagingWrite_) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(d3d11Ctx->Map(dx11StagingWrite_, 0, D3D11_MAP_WRITE, 0, &mapped))) {
                const size_t rowBytes = static_cast<size_t>(width_) * 4;
                for (uint32_t y = 0; y < height_; ++y) {
                    const uint8_t* src = readbackPtr_ + outFootprint_.Offset + y * outFootprint_.Footprint.RowPitch;
                    uint8_t* dst = static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                    std::memcpy(dst, src, rowBytes);
                    if (swapRB_) {
                        for (size_t x = 0; x < rowBytes; x += 4) std::swap(dst[x], dst[x + 2]);
                    }
                }
                d3d11Ctx->Unmap(dx11StagingWrite_, 0);
                d3d11Ctx->CopyResource(outInterpTex, dx11StagingWrite_);
            }
        }
    }

    ++frameCount_;
    return !isReset;
}

void AnrFrameGen::OnResize() {
    ReleaseResources();
}

void AnrFrameGen::Shutdown() {
    ReleaseResources();
    if (d3d12Initialized_ && ngxShutdown_ && d3d12Dev_) {
        ngxShutdown_(d3d12Dev_);
    }
    SafeCloseHandle(fenceEvent_);
    SafeRelease(fence_);
    SafeRelease(cmdList_);
    SafeRelease(cmdAlloc_);
    SafeRelease(cmdQueue_);
    SafeRelease(d3d12Dev_);
    if (anrMod_) {
        FreeLibrary(anrMod_);
        anrMod_ = nullptr;
    }
    if (d3d12Mod_) {
        FreeLibrary(d3d12Mod_);
        d3d12Mod_ = nullptr;
    }
    d3d12Initialized_ = false;
}

} // namespace paws
