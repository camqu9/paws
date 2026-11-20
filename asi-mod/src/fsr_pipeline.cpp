#include "fsr_pipeline.h"
#include "motion_vectors_shader.h"
#include "temporal_upscale_shader.h"

#include <d3dcompiler.h>
#include <cstring>

namespace paws {

namespace {

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

static const char* kDebugVS = R"HLSL(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = p;
    return o;
}
)HLSL";

static const char* kDebugPS = R"HLSL(
Texture2D<float>  g_DepthTex : register(t0);
Texture2D<float2> g_MotionTex : register(t1);
SamplerState      g_Sampler : register(s0);

cbuffer DebugCB : register(b0) {
    int g_Mode; // 1 = Depth Buffer, 2 = Motion Vectors
    float3 g_Pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    if (g_Mode == 1) {
        float d = g_DepthTex.SampleLevel(g_Sampler, uv, 0);
        // Reversed-Z contrast boost for visibility in GTA V
        float vis = pow(saturate(d * 8.0), 0.35);
        return float4(vis, vis, vis, 1.0);
    } else if (g_Mode == 2) {
        float2 mv = g_MotionTex.SampleLevel(g_Sampler, uv, 0);
        float2 scaled = mv * 32.0;
        float r = saturate(0.5 + scaled.x);
        float g = saturate(0.5 + scaled.y);
        float b = saturate(length(scaled));
        return float4(r, g, b, 1.0);
    }
    return float4(0.0, 0.0, 0.0, 1.0);
}
)HLSL";

struct alignas(16) DebugCBData {
    int mode;
    float pad[3];
};

struct alignas(16) TemporalCBData {
    float renderSize[2];
    float invRenderSize[2];
    float displaySize[2];
    float invDisplaySize[2];
    float sharpness;
    int resetHistory;
    float pad[2];
};

bool CompileHlsl(const char* src, const char* entry, const char* target, ID3DBlob** outBlob) {
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            outBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            PAWS_LOG("HLSL compile error (%s): %s", target, static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        return false;
    }
    if (err) err->Release();
    return true;
}

} // namespace

DXGI_FORMAT FsrPipeline::DepthToTypelessFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24G8_TYPELESS;
        default:
            return DXGI_FORMAT_R32_TYPELESS;
    }
}

DXGI_FORMAT FsrPipeline::DepthToSrvFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        default:
            return DXGI_FORMAT_R32_FLOAT;
    }
}

bool FsrPipeline::Init(ID3D11Device* device, ID3D11DeviceContext* /*context*/) {
    if (initialized_) return true;
    device_ = device;

    ID3DBlob* csBlob = nullptr;
    ID3DBlob* tuBlob = nullptr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;

    if (!CompileHlsl(kMotionVectorsHlsl, "main", "cs_5_0", &csBlob) ||
        !CompileHlsl(kTemporalUpscaleHlsl, "main", "cs_5_0", &tuBlob) ||
        !CompileHlsl(kDebugVS, "main", "vs_5_0", &vsBlob) ||
        !CompileHlsl(kDebugPS, "main", "ps_5_0", &psBlob)) {
        SafeRelease(csBlob);
        SafeRelease(tuBlob);
        SafeRelease(vsBlob);
        SafeRelease(psBlob);
        return false;
    }

    HRESULT hr1 = device_->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &motionVecCS_);
    HRESULT hrT = device_->CreateComputeShader(tuBlob->GetBufferPointer(), tuBlob->GetBufferSize(), nullptr, &temporalUpscaleCS_);
    HRESULT hr2 = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &debugVS_);
    HRESULT hr3 = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &debugPS_);
    SafeRelease(csBlob);
    SafeRelease(tuBlob);
    SafeRelease(vsBlob);
    SafeRelease(psBlob);

    if (FAILED(hr1) || FAILED(hrT) || FAILED(hr2) || FAILED(hr3)) {
        PAWS_LOG("Failed to create DX11 shaders (CS=0x%08X TU=0x%08X VS=0x%08X PS=0x%08X)", hr1, hrT, hr2, hr3);
        Shutdown();
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.ByteWidth = sizeof(MotionVectorCameraCB);
    device_->CreateBuffer(&cbDesc, nullptr, &cameraCB_);

    cbDesc.ByteWidth = sizeof(TemporalCBData);
    device_->CreateBuffer(&cbDesc, nullptr, &temporalCB_);

    cbDesc.ByteWidth = sizeof(DebugCBData);
    device_->CreateBuffer(&cbDesc, nullptr, &debugCB_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&sd, &linearSampler_);

    initialized_ = true;
    PAWS_LOG("FsrPipeline initialized successfully.");
    return true;
}

void FsrPipeline::Shutdown() {
    ReleaseRenderResources();
    SafeRelease(motionVecCS_);
    SafeRelease(cameraCB_);
    SafeRelease(temporalUpscaleCS_);
    SafeRelease(temporalCB_);
    SafeRelease(debugVS_);
    SafeRelease(debugPS_);
    SafeRelease(debugCB_);
    SafeRelease(linearSampler_);
    device_ = nullptr;
    initialized_ = false;
}

void FsrPipeline::OnResize() {
    ReleaseRenderResources();
    capturedThisFrame_ = false;
}

void FsrPipeline::ReleaseHistoryResources() {
    for (int i = 0; i < 2; ++i) {
        SafeRelease(historySRV_[i]);
        SafeRelease(historyUAV_[i]);
        SafeRelease(historyTex_[i]);
    }
    historyIndex_ = 0;
    historyValid_ = false;
    displayWidth_ = 0;
    displayHeight_ = 0;
    historyFormat_ = DXGI_FORMAT_UNKNOWN;
}

void FsrPipeline::ReleaseRenderResources() {
#ifdef HAS_FFX_SDK
    DestroyFfxContext();
#endif
    SafeRelease(colorCopySRV_);
    SafeRelease(colorCopyTex_);
    SafeRelease(depthCopySRV_);
    SafeRelease(depthCopyTex_);
    SafeRelease(motionVecUAV_);
    SafeRelease(motionVecSRV_);
    SafeRelease(motionVecTex_);
    SafeRelease(fsrOutputUAV_);
    SafeRelease(fsrOutputTex_);
    ReleaseHistoryResources();
    renderWidth_ = 0;
    renderHeight_ = 0;
    colorFormat_ = DXGI_FORMAT_UNKNOWN;
    depthFormat_ = DXGI_FORMAT_UNKNOWN;
}

bool FsrPipeline::EnsureHistoryResources(uint32_t displayW, uint32_t displayH, DXGI_FORMAT bbFmt) {
    if (!device_ || displayW == 0 || displayH == 0) return false;
    DXGI_FORMAT fmt = (bbFmt == DXGI_FORMAT_B8G8R8A8_UNORM || bbFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                          ? DXGI_FORMAT_B8G8R8A8_UNORM
                          : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (displayWidth_ == displayW && displayHeight_ == displayH && historyFormat_ == fmt &&
        historyTex_[0] && historyTex_[1]) {
        return true;
    }

    ReleaseHistoryResources();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = displayW;
    td.Height = displayH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &historyTex_[i]))) {
            ReleaseHistoryResources();
            return false;
        }
        if (FAILED(device_->CreateShaderResourceView(historyTex_[i], nullptr, &historySRV_[i])) ||
            FAILED(device_->CreateUnorderedAccessView(historyTex_[i], nullptr, &historyUAV_[i]))) {
            ReleaseHistoryResources();
            return false;
        }
    }

    displayWidth_ = displayW;
    displayHeight_ = displayH;
    historyFormat_ = fmt;
    PAWS_LOG("Allocated temporal upscaler history buffers: %ux%u (fmt=%d)",
             displayW, displayH, static_cast<int>(fmt));
    return true;
}

bool FsrPipeline::EnsureRenderResources(uint32_t width, uint32_t height, DXGI_FORMAT colorFmt, DXGI_FORMAT depthFmt) {
    if (!device_ || width == 0 || height == 0) return false;
    if (renderWidth_ == width && renderHeight_ == height &&
        colorFormat_ == colorFmt && depthFormat_ == depthFmt &&
        colorCopyTex_ && depthCopyTex_ && motionVecTex_) {
        return true;
    }

    ReleaseRenderResources();

    // Color copy texture
    D3D11_TEXTURE2D_DESC ctd{};
    ctd.Width = width;
    ctd.Height = height;
    ctd.MipLevels = 1;
    ctd.ArraySize = 1;
    ctd.Format = colorFmt;
    ctd.SampleDesc.Count = 1;
    ctd.Usage = D3D11_USAGE_DEFAULT;
    ctd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&ctd, nullptr, &colorCopyTex_))) return false;
    device_->CreateShaderResourceView(colorCopyTex_, nullptr, &colorCopySRV_);

    // Depth copy texture (typeless so we can bind SRV)
    D3D11_TEXTURE2D_DESC dtd{};
    dtd.Width = width;
    dtd.Height = height;
    dtd.MipLevels = 1;
    dtd.ArraySize = 1;
    dtd.Format = DepthToTypelessFormat(depthFmt);
    dtd.SampleDesc.Count = 1;
    dtd.Usage = D3D11_USAGE_DEFAULT;
    dtd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&dtd, nullptr, &depthCopyTex_))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC dsrv{};
    dsrv.Format = DepthToSrvFormat(depthFmt);
    dsrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    dsrv.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(depthCopyTex_, &dsrv, &depthCopySRV_))) return false;

    // Motion vectors texture: DXGI_FORMAT_R16G16_FLOAT
    D3D11_TEXTURE2D_DESC mvd{};
    mvd.Width = width;
    mvd.Height = height;
    mvd.MipLevels = 1;
    mvd.ArraySize = 1;
    mvd.Format = DXGI_FORMAT_R16G16_FLOAT;
    mvd.SampleDesc.Count = 1;
    mvd.Usage = D3D11_USAGE_DEFAULT;
    mvd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&mvd, nullptr, &motionVecTex_))) return false;
    device_->CreateUnorderedAccessView(motionVecTex_, nullptr, &motionVecUAV_);
    device_->CreateShaderResourceView(motionVecTex_, nullptr, &motionVecSRV_);

    // FSR output texture
    D3D11_TEXTURE2D_DESC otd = ctd;
    otd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    otd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(device_->CreateTexture2D(&otd, nullptr, &fsrOutputTex_))) {
        device_->CreateUnorderedAccessView(fsrOutputTex_, nullptr, &fsrOutputUAV_);
    }

    renderWidth_ = width;
    renderHeight_ = height;
    colorFormat_ = colorFmt;
    depthFormat_ = depthFmt;

    PAWS_LOG("Allocated scene capture & motion vector resources: %ux%u (ColorFmt=%d, DepthFmt=%d, MV=R16G16_FLOAT)",
             width, height, static_cast<int>(colorFmt), static_cast<int>(depthFmt));
    return true;
}

void FsrPipeline::CaptureSceneBuffers(ID3D11DeviceContext* context,
                                      ID3D11Texture2D* colorTex,
                                      ID3D11Texture2D* depthTex) {
    if (!initialized_ || !context || !colorTex || !depthTex) return;

    D3D11_TEXTURE2D_DESC cDesc{}, dDesc{};
    colorTex->GetDesc(&cDesc);
    depthTex->GetDesc(&dDesc);

    if (cDesc.SampleDesc.Count > 1 || dDesc.SampleDesc.Count > 1) {
        return; // MSAA buffers require resolve first
    }

    if (!EnsureRenderResources(cDesc.Width, cDesc.Height, cDesc.Format, dDesc.Format)) {
        return;
    }

    context->CopyResource(colorCopyTex_, colorTex);
    context->CopyResource(depthCopyTex_, depthTex);
    capturedThisFrame_ = true;
}

#ifdef HAS_FFX_SDK
bool FsrPipeline::InitFfxContext(uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH) {
    if (ffxContextCreated_) return true;

    const size_t scratchSize = ffxGetScratchMemorySizeDX11();
    ffxScratchBuffer_.resize(scratchSize);

    FfxFsr2ContextDescription desc{};
    desc.flags = FFX_FSR2_ENABLE_DEPTH_INVERTED |
                 FFX_FSR2_ENABLE_AUTO_EXPOSURE |
                 FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    desc.maxRenderSize.width = renderW;
    desc.maxRenderSize.height = renderH;
    desc.displaySize.width = displayW;
    desc.displaySize.height = displayH;

    FfxErrorCode err = ffxGetInterfaceDX11(&desc.backendInterface, device_,
                                           ffxScratchBuffer_.data(), ffxScratchBuffer_.size());
    if (err != FFX_OK) {
        PAWS_LOG("FFX SDK: ffxGetInterfaceDX11 failed (%d)", static_cast<int>(err));
        return false;
    }

    err = ffxFsr2ContextCreate(&ffxContext_, &desc);
    if (err != FFX_OK) {
        PAWS_LOG("FFX SDK: ffxFsr2ContextCreate failed (%d)", static_cast<int>(err));
        return false;
    }

    ffxContextCreated_ = true;
    PAWS_LOG("FFX SDK: FSR 2/3 context created (%ux%u -> %ux%u)", renderW, renderH, displayW, displayH);
    return true;
}

void FsrPipeline::DestroyFfxContext() {
    if (ffxContextCreated_) {
        ffxFsr2ContextDestroy(&ffxContext_);
        ffxContextCreated_ = false;
    }
    ffxScratchBuffer_.clear();
}
#endif

void FsrPipeline::Execute(ID3D11DeviceContext* context,
                          ID3D11Texture2D* backbufferTex,
                          const CameraTracker& camera,
                          bool fsrEnabled,
                          DebugViewMode debugMode) {
    if (!initialized_ || !context || !backbufferTex) {
        return;
    }

    ID3D11ShaderResourceView* nullSrv[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUav[1] = { nullptr };

    // 1. Dispatch Motion Vector Compute Shader -> DXGI_FORMAT_R16G16_FLOAT
    //    Only run when needed (FFX SDK enabled or Motion Vectors debug view active) to save GPU time.
#ifdef HAS_FFX_SDK
    const bool needMotionVectors = fsrEnabled || (debugMode == DebugViewMode::MotionVectors);
#else
    const bool needMotionVectors = (debugMode == DebugViewMode::MotionVectors);
#endif
    if (needMotionVectors && capturedThisFrame_ && depthCopySRV_ && motionVecUAV_) {
        MotionVectorCameraCB cbData = camera.BuildMotionVectorCB();
        context->UpdateSubresource(cameraCB_, 0, nullptr, &cbData, 0, 0);

        ID3D11ShaderResourceView* csSrvs[1] = { depthCopySRV_ };
        ID3D11UnorderedAccessView* csUavs[1] = { motionVecUAV_ };
        ID3D11Buffer* csCbs[1] = { cameraCB_ };

        context->CSSetShader(motionVecCS_, nullptr, 0);
        context->CSSetConstantBuffers(0, 1, csCbs);
        context->CSSetShaderResources(0, 1, csSrvs);
        context->CSSetUnorderedAccessViews(0, 1, csUavs, nullptr);

        UINT groupsX = (renderWidth_ + 7) / 8;
        UINT groupsY = (renderHeight_ + 7) / 8;
        context->Dispatch(groupsX, groupsY, 1);

        context->CSSetShaderResources(0, 1, nullSrv);
        context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }

    // 2. Run FSR upscaling / RCAS sharpening if enabled
    if (fsrEnabled) {
#ifdef HAS_FFX_SDK
        if (capturedThisFrame_ && colorCopyTex_ && depthCopyTex_ && motionVecTex_) {
            D3D11_TEXTURE2D_DESC bbDesc{};
            backbufferTex->GetDesc(&bbDesc);
            if (InitFfxContext(renderWidth_, renderHeight_, bbDesc.Width, bbDesc.Height)) {
                FfxFsr2DispatchDescription dd{};
                dd.commandList = ffxGetCommandListDX11(context);
                dd.color = ffxGetResourceDX11(&ffxContext_, colorCopyTex_, L"paws_color", FFX_RESOURCE_STATE_COMPUTE_READ);
                dd.depth = ffxGetResourceDX11(&ffxContext_, depthCopyTex_, L"paws_depth", FFX_RESOURCE_STATE_COMPUTE_READ);
                dd.motionVectors = ffxGetResourceDX11(&ffxContext_, motionVecTex_, L"paws_mv", FFX_RESOURCE_STATE_COMPUTE_READ);
                dd.output = ffxGetResourceDX11(&ffxContext_, fsrOutputTex_, L"paws_out", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
                dd.jitterOffset.x = camera.GetJitterPixelsX();
                dd.jitterOffset.y = camera.GetJitterPixelsY();
                dd.motionVectorScale.x = static_cast<float>(renderWidth_);
                dd.motionVectorScale.y = static_cast<float>(renderHeight_);
                dd.renderSize.width = renderWidth_;
                dd.renderSize.height = renderHeight_;
                dd.enableSharpening = true;
                dd.sharpness = 0.2f;
                dd.frameTimeDelta = camera.GetDeltaTimeMs();
                dd.preExposure = 1.0f;
                dd.reset = !camera.HasValidHistory();
                dd.cameraNear = camera.GetNearPlane();
                dd.cameraFar = camera.GetFarPlane();
                dd.cameraFovAngleVertical = camera.GetFovYRadians();

                if (ffxFsr2ContextDispatch(&ffxContext_, &dd) == FFX_OK && fsrOutputTex_) {
                    context->CopyResource(backbufferTex, fsrOutputTex_);
                }
            }
        }
#else
        if (!loggedSdkStatus_) {
            PAWS_LOG("Native DX11 FSR RCAS sharpening active on final lit backbuffer.");
            loggedSdkStatus_ = true;
        }

        D3D11_TEXTURE2D_DESC bbDesc{};
        backbufferTex->GetDesc(&bbDesc);
        if (EnsureHistoryResources(bbDesc.Width, bbDesc.Height, bbDesc.Format) &&
            temporalUpscaleCS_ && historyTex_[0] && historySRV_[0] && historyUAV_[1] &&
            bbDesc.Format == historyFormat_) {

            // Copy GTA V's final lit + skybox + post-FX + HUD backbuffer as RCAS input
            context->CopyResource(historyTex_[0], backbufferTex);

            TemporalCBData tcb{};
            tcb.renderSize[0] = static_cast<float>(displayWidth_);
            tcb.renderSize[1] = static_cast<float>(displayHeight_);
            tcb.invRenderSize[0] = 1.0f / tcb.renderSize[0];
            tcb.invRenderSize[1] = 1.0f / tcb.renderSize[1];
            tcb.displaySize[0] = static_cast<float>(displayWidth_);
            tcb.displaySize[1] = static_cast<float>(displayHeight_);
            tcb.invDisplaySize[0] = tcb.invRenderSize[0];
            tcb.invDisplaySize[1] = tcb.invRenderSize[1];
            tcb.sharpness = 0.2f;
            tcb.resetHistory = 0;
            context->UpdateSubresource(temporalCB_, 0, nullptr, &tcb, 0, 0);

            ID3D11ShaderResourceView* tuSrvs[1] = { historySRV_[0] };
            ID3D11UnorderedAccessView* tuUavs[1] = { historyUAV_[1] };
            ID3D11Buffer* tuCbs[1] = { temporalCB_ };

            context->CSSetShader(temporalUpscaleCS_, nullptr, 0);
            context->CSSetConstantBuffers(0, 1, tuCbs);
            context->CSSetShaderResources(0, 1, tuSrvs);
            context->CSSetUnorderedAccessViews(0, 1, tuUavs, nullptr);
            context->Dispatch((displayWidth_ + 7) / 8, (displayHeight_ + 7) / 8, 1);

            context->CSSetShaderResources(0, 1, nullSrv);
            context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            context->CSSetShader(nullptr, nullptr, 0);

            context->CopyResource(backbufferTex, historyTex_[1]);
        }
#endif
    }

    // 3. Render Debug View Overlay (Depth Buffer or Motion Vectors) if selected via F8
    if (debugMode != DebugViewMode::Normal && capturedThisFrame_ && depthCopySRV_ && motionVecSRV_) {
        ID3D11RenderTargetView* bbRTV = nullptr;
        if (SUCCEEDED(device_->CreateRenderTargetView(backbufferTex, nullptr, &bbRTV))) {
            D3D11_TEXTURE2D_DESC bbDesc{};
            backbufferTex->GetDesc(&bbDesc);

            // Save minimal state
            ID3D11RenderTargetView* prevRTV = nullptr;
            ID3D11DepthStencilView* prevDSV = nullptr;
            context->OMGetRenderTargets(1, &prevRTV, &prevDSV);

            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<float>(bbDesc.Width);
            vp.Height = static_cast<float>(bbDesc.Height);
            vp.MaxDepth = 1.0f;

            DebugCBData dcb{};
            dcb.mode = static_cast<int>(debugMode);
            context->UpdateSubresource(debugCB_, 0, nullptr, &dcb, 0, 0);

            ID3D11ShaderResourceView* psSrvs[2] = { depthCopySRV_, motionVecSRV_ };
            context->OMSetRenderTargets(1, &bbRTV, nullptr);
            context->RSSetViewports(1, &vp);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(debugVS_, nullptr, 0);
            context->PSSetShader(debugPS_, nullptr, 0);
            context->PSSetConstantBuffers(0, 1, &debugCB_);
            context->PSSetShaderResources(0, 2, psSrvs);
            context->PSSetSamplers(0, 1, &linearSampler_);
            context->Draw(3, 0);

            // Restore state
            context->PSSetShaderResources(0, 2, nullSrv);
            context->OMSetRenderTargets(1, &prevRTV, prevDSV);
            SafeRelease(prevRTV);
            SafeRelease(prevDSV);
            SafeRelease(bbRTV);
        }
    }
}

} // namespace paws
