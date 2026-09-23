// Depth-aware upsample of the low-res fog onto the world viewport. Blend: ONE, INVSRCALPHA.
#include "vf_common.hlsli"

float4 cComposite : register(c9);   // x = exposure, y = god-ray strength, z = debug view, w = linear fog radiance
float4 cRayColor  : register(c10);  // rgb = god-ray colour
float4 cSunPx     : register(c11);  // xy = sun position in render-target pixels, z = sun marker enabled

sampler2D sDepth : register(s0);
sampler2D sFog   : register(s1);
sampler2D sRays  : register(s2);

// Compresses HDR scattering (Classic intensities reach 10) by luminance so the hue survives: the brightness
// rolls off toward 1 above the knee, and only colours that still leave the gamut are desaturated.
float3 ToneMap(float3 c)
{
    const float knee = 0.45;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    float lt = l <= knee ? l : knee + (1 - knee) * (1 - exp(-(l - knee) / (1 - knee)));
    float3 r = c * (lt / max(l, 1e-5));
    float m = max(r.r, max(r.g, r.b));
    return m > 1 ? lt + (r - lt) * ((1 - lt) / max(m - lt, 1e-5)) : r;
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 pc = vpos + 0.5;
    float z0 = LinearDepth(SampleDepth(sDepth, pc));
    float2 lowc = FullToLow(pc);
    float2 base = floor(lowc);
    float2 f = lowc - base;

    static const float2 kTaps[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
    float4 acc = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 4; j++)
    {
        float2 o = kTaps[j];
        float2 t = clamp(base + o, 0, cLowSize.xy - 1);
        float zt = LinearDepth(SampleDepth(sDepth, LowToFull(t)));
        float2 bw2 = lerp(1 - f, f, o);
        float w = bw2.x * bw2.y / (1e-3 + abs(zt - z0) / max(z0, 1e-3)) + 1e-6;
        acc += tex2Dlod(sFog, float4((t + 0.5) * cLowSize.zw, 0, 0)) * w;
        wsum += w;
    }
    float4 fog = acc / wsum;
    fog.rgb *= cComposite.x;
    [branch] if (cComposite.w > 0)
    {
        float a = max(fog.a, 1e-4);
        fog.rgb = pow(ToneMap(fog.rgb / a), 1.0 / 2.2) * fog.a;
    }

    float3 rays = 0;
    [branch] if (cComposite.y > 0)
        rays = tex2Dlod(sRays, float4((pc - cRect.xy) / cRect.zw, 0, 0)).rgb * cRayColor.rgb * cComposite.y;

    [branch] if (cComposite.z > 0.5)
    {
        if (cComposite.z < 1.5)
            return float4(fog.rgb + rays, 1);
        if (cComposite.z < 2.5)
            return float4((1 - fog.aaa), 1);
        if (cComposite.z < 3.5)
            return float4(saturate(z0 / cDepthLin.z).xxx, 1);
    }
    [branch] if (cSunPx.z > 0 && distance(pc, cSunPx.xy) < 6)
        return float4(1, 0, 0, 1);
    return float4(fog.rgb + rays, fog.a);
}
