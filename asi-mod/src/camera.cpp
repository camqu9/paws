#include "camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace paws {

CameraTracker::CameraTracker() {
    XMStoreFloat4x4(&currentViewProj_, XMMatrixIdentity());
    XMStoreFloat4x4(&currentInvViewProj_, XMMatrixIdentity());
    XMStoreFloat4x4(&prevViewProj_, XMMatrixIdentity());
}

float CameraTracker::Halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    uint32_t i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

void CameraTracker::GetHaltonJitter(uint32_t phaseIndex, float& outX, float& outY) const {
    uint32_t idx = (phaseIndex % kJitterPhaseCount) + 1; // 1..16
    // Map [0, 1) -> [-0.5, 0.5] sub-pixel offsets
    outX = Halton(idx, 2) - 0.5f;
    outY = Halton(idx, 3) - 0.5f;
}

void CameraTracker::ApplyJitterToProjection(XMFLOAT4X4& proj,
                                            float jitterPixelsX,
                                            float jitterPixelsY,
                                            uint32_t renderWidth,
                                            uint32_t renderHeight) {
    if (renderWidth == 0 || renderHeight == 0) return;
    // DirectX NDC spans [-1, 1] (width 2), with Y flipped relative to pixel coordinates
    float offsetX = (2.0f * jitterPixelsX) / static_cast<float>(renderWidth);
    float offsetY = (-2.0f * jitterPixelsY) / static_cast<float>(renderHeight);
    proj._31 += offsetX;
    proj._32 += offsetY;
}

void CameraTracker::BeginFrame(uint32_t renderWidth, uint32_t renderHeight, float deltaTimeMs) {
    renderWidth_ = std::max(renderWidth, 1u);
    renderHeight_ = std::max(renderHeight, 1u);
    deltaTimeMs_ = std::clamp(deltaTimeMs, 0.1f, 250.0f);

    if (matricesUpdatedThisFrame_) {
        prevViewProj_ = currentViewProj_;
        hasValidHistory_ = true;
    } else if (!hasValidHistory_) {
        // Initialize a sane default perspective matrix until the game's camera CB is observed
        float aspect = static_cast<float>(renderWidth_) / static_cast<float>(renderHeight_);
        XMMATRIX view = XMMatrixIdentity();
        // GTA V Legacy uses reversed-Z depth (near -> 1.0, far -> 0.0)
        XMMATRIX proj = XMMatrixPerspectiveFovLH(fovYRadians_, aspect, farPlane_, nearPlane_);
        XMMATRIX vp = XMMatrixMultiply(view, proj);
        XMVECTOR det{};
        XMMATRIX invVp = XMMatrixInverse(&det, vp);
        XMStoreFloat4x4(&currentViewProj_, vp);
        XMStoreFloat4x4(&currentInvViewProj_, invVp);
        prevViewProj_ = currentViewProj_;
    }

    prevJitterPixelsX_ = jitterPixelsX_;
    prevJitterPixelsY_ = jitterPixelsY_;

    GetHaltonJitter(frameIndex_, jitterPixelsX_, jitterPixelsY_);
    ++frameIndex_;
    matricesUpdatedThisFrame_ = false;
}

void CameraTracker::UpdateCameraMatrices(const XMFLOAT4X4& view,
                                         const XMFLOAT4X4& proj,
                                         float nearPlane,
                                         float farPlane,
                                         float fovYRadians) {
    if (nearPlane > 0.0f) nearPlane_ = nearPlane;
    if (farPlane > nearPlane_) farPlane_ = farPlane;
    if (fovYRadians > 0.05f && fovYRadians < XM_PI) fovYRadians_ = fovYRadians;

    // Apply current frame's 16-phase Halton jitter to projection
    XMFLOAT4X4 jitteredProj = proj;
    ApplyJitterToProjection(jitteredProj, jitterPixelsX_, jitterPixelsY_, renderWidth_, renderHeight_);

    XMMATRIX mView = XMLoadFloat4x4(&view);
    XMMATRIX mProj = XMLoadFloat4x4(&proj); // Unjittered VP for clean motion vector reprojection
    XMMATRIX mVP = XMMatrixMultiply(mView, mProj);

    XMVECTOR det{};
    XMMATRIX mInvVP = XMMatrixInverse(&det, mVP);
    if (std::fabs(XMVectorGetX(det)) > 1e-12f) {
        XMStoreFloat4x4(&currentViewProj_, mVP);
        XMStoreFloat4x4(&currentInvViewProj_, mInvVP);
        matricesUpdatedThisFrame_ = true;
    }
}

MotionVectorCameraCB CameraTracker::BuildMotionVectorCB() const {
    MotionVectorCameraCB cb{};
    cb.currentInvViewProj = currentInvViewProj_;
    cb.prevViewProj = prevViewProj_;
    cb.resolution[0] = static_cast<float>(renderWidth_);
    cb.resolution[1] = static_cast<float>(renderHeight_);
    cb.invResolution[0] = 1.0f / cb.resolution[0];
    cb.invResolution[1] = 1.0f / cb.resolution[1];
    cb.currentJitterUV[0] = jitterPixelsX_ * cb.invResolution[0];
    cb.currentJitterUV[1] = jitterPixelsY_ * cb.invResolution[1];
    cb.prevJitterUV[0] = prevJitterPixelsX_ * cb.invResolution[0];
    cb.prevJitterUV[1] = prevJitterPixelsY_ * cb.invResolution[1];
    return cb;
}

} // namespace paws
