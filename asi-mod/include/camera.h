#pragma once

#include <DirectXMath.h>
#include <cstdint>

namespace paws {

struct alignas(16) MotionVectorCameraCB {
    DirectX::XMFLOAT4X4 currentInvViewProj;
    DirectX::XMFLOAT4X4 prevViewProj;
    float resolution[2];
    float invResolution[2];
    float currentJitterUV[2];
    float prevJitterUV[2];
};

class CameraTracker {
public:
    static constexpr uint32_t kJitterPhaseCount = 16;

    CameraTracker();

    // Halton sequence helper: base-b radical inverse for 1-based index
    static float Halton(uint32_t index, uint32_t base);

    // Returns 16-phase Halton (2, 3) sub-pixel jitter in [-0.5, 0.5]
    void GetHaltonJitter(uint32_t phaseIndex, float& outX, float& outY) const;

    // Applies sub-pixel jitter to a DX11 projection matrix (in-place)
    static void ApplyJitterToProjection(DirectX::XMFLOAT4X4& proj,
                                        float jitterPixelsX,
                                        float jitterPixelsY,
                                        uint32_t renderWidth,
                                        uint32_t renderHeight);

    // Advances frame state: shifts currentViewProj -> prevViewProj, computes next Halton jitter
    void BeginFrame(uint32_t renderWidth, uint32_t renderHeight, float deltaTimeMs);

    // Updates camera matrices when observed from the game's view/projection matrices
    void UpdateCameraMatrices(const DirectX::XMFLOAT4X4& view,
                              const DirectX::XMFLOAT4X4& proj,
                              float nearPlane,
                              float farPlane,
                              float fovYRadians);

    // Builds the constant buffer payload for motion_vectors.hlsl
    MotionVectorCameraCB BuildMotionVectorCB() const;

    uint32_t GetFrameIndex() const { return frameIndex_; }
    float GetJitterPixelsX() const { return jitterPixelsX_; }
    float GetJitterPixelsY() const { return jitterPixelsY_; }
    float GetDeltaTimeMs() const { return deltaTimeMs_; }
    float GetNearPlane() const { return nearPlane_; }
    float GetFarPlane() const { return farPlane_; }
    float GetFovYRadians() const { return fovYRadians_; }
    bool HasValidHistory() const { return hasValidHistory_; }

    const DirectX::XMFLOAT4X4& GetCurrentInvViewProj() const { return currentInvViewProj_; }
    const DirectX::XMFLOAT4X4& GetPrevViewProj() const { return prevViewProj_; }

private:
    uint32_t frameIndex_ = 0;
    uint32_t renderWidth_ = 1920;
    uint32_t renderHeight_ = 1080;

    float jitterPixelsX_ = 0.0f;
    float jitterPixelsY_ = 0.0f;
    float prevJitterPixelsX_ = 0.0f;
    float prevJitterPixelsY_ = 0.0f;

    float deltaTimeMs_ = 16.6667f;
    float nearPlane_ = 0.15f;
    float farPlane_ = 10000.0f;
    float fovYRadians_ = DirectX::XMConvertToRadians(55.0f);

    bool hasValidHistory_ = false;
    bool matricesUpdatedThisFrame_ = false;

    DirectX::XMFLOAT4X4 currentViewProj_{};
    DirectX::XMFLOAT4X4 currentInvViewProj_{};
    DirectX::XMFLOAT4X4 prevViewProj_{};
};

} // namespace paws
