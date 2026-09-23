// Diagnostic readback, one texel per point of a 5x5 grid over the world viewport:
// raw depth, linear depth, fog opacity, class (0 world, 1 beyond the far clip, 2 sky).
#include "vf_common.hlsli"

sampler2D sDepth : register(s0);
sampler2D sFog   : register(s1);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float i = floor(vpos.x);
    float2 grid = float2(fmod(i, 5), floor(i / 5));
    float2 px = floor(cRect.xy + (grid * 0.2 + 0.1) * cRect.zw) + 0.5;
    float d = SampleDepth(sDepth, px);
    float4 fog = tex2Dlod(sFog, float4((FullToLow(px) + 0.5) * cLowSize.zw, 0, 0));
    return float4(d, LinearDepth(d), fog.a, IsSky(d) ? 2 : (BeyondWorld(d) ? 1 : 0));
}
