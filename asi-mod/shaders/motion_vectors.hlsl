// paws_gtav.asi - Camera Motion Vector Compute Shader (HLSL SM 5.0)
// Reconstructs 3D world position from the captured depth buffer and reprojects
// into the previous frame to output DXGI_FORMAT_R16G16_FLOAT motion vectors.

Texture2D<float> g_DepthTex : register(t0);
RWTexture2D<float2> g_MotionVectors : register(u0);

cbuffer CameraCB : register(b0) {
    row_major float4x4 g_CurrentInvViewProj;
    row_major float4x4 g_PrevViewProj;
    float2 g_Resolution;
    float2 g_InvResolution;
    float2 g_CurrentJitterUV;
    float2 g_PrevJitterUV;
};

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint width  = (uint)g_Resolution.x;
    uint height = (uint)g_Resolution.y;
    if (DTid.x >= width || DTid.y >= height) {
        return;
    }

    float depth = g_DepthTex.Load(int3(DTid.xy, 0));

    // Pixel center UV in [0, 1]
    float2 uv = (float2(DTid.xy) + 0.5) * g_InvResolution;

    // Remove current-frame Halton sub-pixel jitter before world reconstruction
    float2 unjitteredUV = uv - g_CurrentJitterUV;

    // Convert UV [0, 1] -> DirectX NDC [-1, 1] (Y-up in NDC, Y-down in UV)
    float2 ndc = float2(unjitteredUV.x * 2.0 - 1.0, 1.0 - unjitteredUV.y * 2.0);

    // Reconstruct world-space position
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos = mul(clipPos, g_CurrentInvViewProj);
    if (abs(worldPos.w) < 1e-6) {
        g_MotionVectors[DTid.xy] = float2(0.0, 0.0);
        return;
    }
    worldPos /= worldPos.w;

    // Reproject world position into previous frame clip space
    float4 prevClip = mul(worldPos, g_PrevViewProj);
    if (abs(prevClip.w) < 1e-6) {
        g_MotionVectors[DTid.xy] = float2(0.0, 0.0);
        return;
    }
    float2 prevNDC = prevClip.xy / prevClip.w;

    // Convert previous NDC -> previous UV [0, 1]
    float2 prevUV = float2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5);

    // Screen-space velocity in UV space (prevUV - currentUnjitteredUV)
    // FSR 2/3 expects motion vectors pointing from current pixel to previous pixel
    // (or scaled via motionVectorScale in ffxFsr2DispatchDescription).
    float2 motionVec = prevUV - unjitteredUV;
    g_MotionVectors[DTid.xy] = motionVec;
}
