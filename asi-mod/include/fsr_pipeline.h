#pragma once

#include "camera.h"
#include "paws_common.h"

#include <d3d11.h>
#include <cstdint>

#ifdef HAS_FFX_SDK
#include <FidelityFX/host/ffx_fsr2.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <vector>
#endif

namespace paws {

class FsrPipeline {
public:
    FsrPipeline() = default;
    ~FsrPipeline() { Shutdown(); }

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();
    void OnResize();

    // Copies the captured 3D scene color & depth textures before HUD draw
    void CaptureSceneBuffers(ID3D11DeviceContext* context,
                             ID3D11Texture2D* colorTex,
                             ID3D11Texture2D* depthTex);

    // Executes Motion Vector CS -> FSR 2/3 (if enabled) -> Debug Overlay (if active)
    void Execute(ID3D11DeviceContext* context,
                 ID3D11Texture2D* backbufferTex,
                 const CameraTracker& camera,
                 bool fsrEnabled,
                 DebugViewMode debugMode);

    bool HasCapturedFrame() const { return capturedThisFrame_; }
    void ResetFrameCaptureFlag() { capturedThisFrame_ = false; }

private:
    bool EnsureRenderResources(uint32_t width, uint32_t height, DXGI_FORMAT colorFmt, DXGI_FORMAT depthFmt);
    void ReleaseRenderResources();

    static DXGI_FORMAT DepthToSrvFormat(DXGI_FORMAT fmt);
    static DXGI_FORMAT DepthToTypelessFormat(DXGI_FORMAT fmt);

    ID3D11Device* device_ = nullptr;
    bool initialized_ = false;
    bool loggedSdkStatus_ = false;
    bool capturedThisFrame_ = false;

    uint32_t renderWidth_ = 0;
    uint32_t renderHeight_ = 0;
    DXGI_FORMAT colorFormat_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depthFormat_ = DXGI_FORMAT_UNKNOWN;

    // Shaders & pipeline states
    ID3D11ComputeShader* motionVecCS_ = nullptr;
    ID3D11Buffer* cameraCB_ = nullptr;

    ID3D11ComputeShader* temporalUpscaleCS_ = nullptr;
    ID3D11Buffer* temporalCB_ = nullptr;

    ID3D11VertexShader* debugVS_ = nullptr;
    ID3D11PixelShader* debugPS_ = nullptr;
    ID3D11Buffer* debugCB_ = nullptr;
    ID3D11SamplerState* linearSampler_ = nullptr;

    // Captured scene buffers + motion vectors + ping-pong temporal history
    ID3D11Texture2D* colorCopyTex_ = nullptr;
    ID3D11ShaderResourceView* colorCopySRV_ = nullptr;

    ID3D11Texture2D* depthCopyTex_ = nullptr;
    ID3D11ShaderResourceView* depthCopySRV_ = nullptr;

    ID3D11Texture2D* motionVecTex_ = nullptr;
    ID3D11UnorderedAccessView* motionVecUAV_ = nullptr;
    ID3D11ShaderResourceView* motionVecSRV_ = nullptr;

    ID3D11Texture2D* fsrOutputTex_ = nullptr;
    ID3D11UnorderedAccessView* fsrOutputUAV_ = nullptr;

    ID3D11Texture2D* historyTex_[2] = { nullptr, nullptr };
    ID3D11ShaderResourceView* historySRV_[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* historyUAV_[2] = { nullptr, nullptr };
    uint32_t historyIndex_ = 0;
    bool historyValid_ = false;

#ifdef HAS_FFX_SDK
    bool ffxContextCreated_ = false;
    FfxFsr2Context ffxContext_{};
    std::vector<uint8_t> ffxScratchBuffer_;
    bool InitFfxContext(uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH);
    void DestroyFfxContext();
#endif
};

} // namespace paws
