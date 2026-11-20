// paws_gtav.asi - Native DX11 Temporal Upscaling + YCoCg Clamping + RCAS Compute Shader (SM 5.0)
// Runs when AMD FidelityFX SDK (DX12/Vulkan only) is not compiled in, providing
// real temporal accumulation from captured color + depth-reprojected R16G16_FLOAT motion vectors.

Texture2D<float4> g_ColorTex   : register(t0);
Texture2D<float2> g_MotionTex  : register(t1);
Texture2D<float4> g_HistoryTex : register(t2);
SamplerState      g_LinearClamp : register(s0);

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

float3 RgbToYCoCg(float3 c) {
    float y  =  0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    float co =  0.5  * c.r             - 0.5  * c.b;
    float cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return float3(y, co, cg);
}

float3 YCoCgToRgb(float3 ycc) {
    float tmp = ycc.x - ycc.z;
    return float3(tmp + ycc.y, ycc.x + ycc.z, tmp - ycc.y);
}

float Luma(float3 c) {
    return 0.5 * c.g + 0.25 * (c.r + c.b);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint outW = (uint)g_DisplaySize.x;
    uint outH = (uint)g_DisplaySize.y;
    if (DTid.x >= outW || DTid.y >= outH) {
        return;
    }

    float2 uv = (float2(DTid.xy) + 0.5) * g_InvDisplaySize;
    float3 current = g_ColorTex.SampleLevel(g_LinearClamp, uv, 0).rgb;

    // 3x3 neighborhood YCoCg variance clamping (reduces temporal ghosting)
    float3 m1 = float3(0.0, 0.0, 0.0);
    float3 m2 = float3(0.0, 0.0, 0.0);
    float3 minN = float3( 1e5,  1e5,  1e5);
    float3 maxN = float3(-1e5, -1e5, -1e5);

    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            float2 sUV = uv + float2(dx, dy) * g_InvRenderSize;
            float3 sYcc = RgbToYCoCg(g_ColorTex.SampleLevel(g_LinearClamp, sUV, 0).rgb);
            m1 += sYcc;
            m2 += sYcc * sYcc;
            minN = min(minN, sYcc);
            maxN = max(maxN, sYcc);
        }
    }

    float3 mean = m1 / 9.0;
    float3 stddev = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    float3 clipMin = max(minN, mean - 1.25 * stddev);
    float3 clipMax = min(maxN, mean + 1.25 * stddev);

    float2 mv = g_MotionTex.SampleLevel(g_LinearClamp, uv, 0);
    float2 prevUV = uv + mv;
    bool validHistory = (g_ResetHistory == 0) &&
                        (prevUV.x >= 0.0 && prevUV.x <= 1.0 && prevUV.y >= 0.0 && prevUV.y <= 1.0);

    float3 accumulated = current;
    if (validHistory) {
        float3 histRgb = g_HistoryTex.SampleLevel(g_LinearClamp, prevUV, 0).rgb;
        float3 histYcc = clamp(RgbToYCoCg(histRgb), clipMin, clipMax);
        float3 clampedHist = saturate(YCoCgToRgb(histYcc));

        // Velocity-adaptive blend factor: responsive under motion, stable when still
        float speed = length(mv * g_DisplaySize);
        float alpha = clamp(0.10 + speed * 0.03, 0.10, 0.50);
        accumulated = lerp(clampedHist, current, alpha);
    }

    // Lightweight RCAS contrast-adaptive sharpening against local cross taps
    float3 b = g_ColorTex.SampleLevel(g_LinearClamp, uv + float2( 0.0, -1.0) * g_InvRenderSize, 0).rgb;
    float3 d = g_ColorTex.SampleLevel(g_LinearClamp, uv + float2(-1.0,  0.0) * g_InvRenderSize, 0).rgb;
    float3 f = g_ColorTex.SampleLevel(g_LinearClamp, uv + float2( 1.0,  0.0) * g_InvRenderSize, 0).rgb;
    float3 h = g_ColorTex.SampleLevel(g_LinearClamp, uv + float2( 0.0,  1.0) * g_InvRenderSize, 0).rgb;

    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = mn4 / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0));
    lobe *= exp2(-clamp(g_Sharpness, 0.0, 2.0));

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    float3 sharpened = saturate((lobe * (b + d + f + h) + accumulated) * rcpL);

    g_OutputTex[DTid.xy] = float4(sharpened, 1.0);
}
