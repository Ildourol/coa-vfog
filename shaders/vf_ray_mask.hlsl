// God-ray source: bright sky around the sun, taken from a downsampled copy of the frame.
#include "vf_common.hlsli"

float4 cRayMask : register(c9);   // xyz = toward-sun direction (view space), w = angular falloff exponent
float4 cRaySrc  : register(c10);  // x = full pixels per ray texel, y = luminance threshold, zw = 1 / ray size

sampler2D sDepth : register(s0);
sampler2D sScene : register(s1);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 pc = min(cRect.xy + (vpos + 0.5) * cRaySrc.x, cRect.xy + cRect.zw - 0.5);
    float sky = IsSky(SampleDepth(sDepth, pc)) ? 1 : 0;
    float3 V = normalize(ViewRay(pc));
    float falloff = pow(saturate(dot(V, cRayMask.xyz)), cRayMask.w);
    float3 c = tex2Dlod(sScene, float4((vpos + 0.5) * cRaySrc.zw, 0, 0)).rgb;
    float lum = dot(c, float3(0.299, 0.587, 0.114));
    return float4(c * (sky * falloff * saturate((lum - cRaySrc.y) * 4)), 1);
}
