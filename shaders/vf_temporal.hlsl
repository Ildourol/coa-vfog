#include "vf_common.hlsli"

row_major float4x4 cViewToPreviousClip : register(c9);
float4 cTemporal : register(c13);

sampler2D sCurrent : register(s0);
sampler2D sHistory : register(s1);
sampler2D sDepth : register(s2);

float HistoryWeight()
{
    return cTemporal.x;
}

bool HistoryInvalid()
{
    return cTemporal.y <= 0;
}

void CurrentNeighbourhoodRange(float2 uv, float4 centre, out float4 lowest, out float4 highest)
{
    lowest = centre;
    highest = centre;
    static const float2 kNeighbourOffsets[8] = {
        float2(-1, -1), float2(0, -1), float2(1, -1), float2(-1, 0),
        float2(1, 0), float2(-1, 1), float2(0, 1), float2(1, 1)
    };
    [unroll] for (int k = 0; k < 8; k++)
    {
        float4 neighbour = tex2Dlod(sCurrent, float4(uv + kNeighbourOffsets[k] * LowResTexelSize(), 0, 0));
        lowest = min(lowest, neighbour);
        highest = max(highest, neighbour);
    }
}

float4 main(float2 lowResTexel : VPOS) : COLOR0
{
    float2 uv = LowResTexelToUv(lowResTexel);
    float4 current = tex2Dlod(sCurrent, float4(uv, 0, 0));
    [branch] if (HistoryInvalid())
        return current;

    float4 lowest;
    float4 highest;
    CurrentNeighbourhoodRange(uv, current, lowest, highest);

    float2 pixel = LowResTexelToFullPixel(lowResTexel);
    float viewZ = LinearDepth(SampleDepth(sDepth, pixel));
    float4 previousClip = mul(float4(ViewRayAtUnitDepth(pixel) * viewZ, 1), cViewToPreviousClip);
    [branch] if (previousClip.w <= 1e-3)
        return current;
    float2 previousPixel = NdcToPixel(previousClip.xy / previousClip.w);
    float2 previousUv = LowResTexelToUv(FullPixelToLowResTexel(previousPixel));
    [branch] if (any(previousUv < 0) || any(previousUv > 1))
        return current;
    float4 history = clamp(tex2Dlod(sHistory, float4(previousUv, 0, 0)), lowest, highest);
    return lerp(current, history, HistoryWeight());
}
