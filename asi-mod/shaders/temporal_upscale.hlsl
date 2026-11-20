// paws_gtav.asi - Native DX11 Post-Upscale FSR RCAS Compute Shader (SM 5.0)
// Applies AMD FidelityFX Robust Contrast-Adaptive Sharpening (RCAS) directly to
// GTA V's final lit + tone-mapped backbuffer after Frame Scaling upscaling,
// preserving full lighting, skybox, post-FX, and crisp HUD with zero ghosting.

Texture2D<float4> g_ColorTex : register(t0);
RWTexture2D<float4> g_OutputTex : register(u0);

cbuffer TemporalCB : register(b0) {
    float2 g_RenderSize;
    float2 g_InvRenderSize;
    float2 g_DisplaySize;
    float2 g_InvDisplaySize;
    float  g_Sharpness;
    int    g_ResetHistory;
    float2 g_Pad;
};

float Luma(float3 c) {
    return 0.5 * c.g + 0.25 * (c.r + c.b);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    int2 maxCoord = int2((int)g_DisplaySize.x - 1, (int)g_DisplaySize.y - 1);
    if ((int)DTid.x > maxCoord.x || (int)DTid.y > maxCoord.y) {
        return;
    }

    int2 p = int2(DTid.xy);
    float4 center = g_ColorTex.Load(int3(p, 0));
    float3 e = center.rgb;

    float3 b = g_ColorTex.Load(int3(clamp(p + int2( 0, -1), int2(0, 0), maxCoord), 0)).rgb;
    float3 d = g_ColorTex.Load(int3(clamp(p + int2(-1,  0), int2(0, 0), maxCoord), 0)).rgb;
    float3 f = g_ColorTex.Load(int3(clamp(p + int2( 1,  0), int2(0, 0), maxCoord), 0)).rgb;
    float3 h = g_ColorTex.Load(int3(clamp(p + int2( 0,  1), int2(0, 0), maxCoord), 0)).rgb;

    float bL = Luma(b), dL = Luma(d), eL = Luma(e), fL = Luma(f), hL = Luma(h);

    // FSR RCAS noise detection
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float maxL = max(max(max(bL, dL), max(fL, hL)), eL);
    float minL = min(min(min(bL, dL), min(fL, hL)), eL);
    nz = saturate(abs(nz) / max(maxL - minL, 1e-5));
    nz = -0.5 * nz + 1.0;

    // Anti-ringing contrast-adaptive lobe
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = mn4 / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0));

    float sharpScale = exp2(-clamp(g_Sharpness, 0.0, 2.0));
    lobe *= sharpScale * nz;

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    float3 outColor = saturate((lobe * (b + d + f + h) + e) * rcpL);
    g_OutputTex[DTid.xy] = float4(outColor, center.a);
}
