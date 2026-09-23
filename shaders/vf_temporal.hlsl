// Reprojects the previous accumulated fog into the current low-res grid and blends it with the new march.
#include "vf_common.hlsli"

row_major float4x4 cReproj : register(c9);  // c9..c12 current view -> previous clip
float4 cTemporal  : register(c13);  // x = history weight, y = history valid

sampler2D sCurrent : register(s0);
sampler2D sHistory : register(s1);
sampler2D sDepth   : register(s2);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * cLowSize.zw;
    float4 cur = tex2Dlod(sCurrent, float4(uv, 0, 0));
    [branch] if (cTemporal.y <= 0)
        return cur;

    float4 lo = cur;
    float4 hi = cur;
    static const float2 kNeighbours[8] = {
        float2(-1, -1), float2(0, -1), float2(1, -1), float2(-1, 0),
        float2(1, 0), float2(-1, 1), float2(0, 1), float2(1, 1)
    };
    [unroll] for (int k = 0; k < 8; k++)
    {
        float4 n = tex2Dlod(sCurrent, float4(uv + kNeighbours[k] * cLowSize.zw, 0, 0));
        lo = min(lo, n);
        hi = max(hi, n);
    }

    float2 pc = LowToFull(vpos);
    float z = LinearDepth(SampleDepth(sDepth, pc));
    float4 prev = mul(float4(ViewRay(pc) * z, 1), cReproj);
    [branch] if (prev.w <= 1e-3)
        return cur;
    float2 ndc = prev.xy / prev.w;
    float2 prevPx = cRect.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * cRect.zw;
    float2 prevUv = (FullToLow(prevPx) + 0.5) * cLowSize.zw;
    [branch] if (any(prevUv < 0) || any(prevUv > 1))
        return cur;
    float4 hist = clamp(tex2Dlod(sHistory, float4(prevUv, 0, 0)), lo, hi);
    return lerp(cur, hist, cTemporal.x);
}
