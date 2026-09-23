// Depth-aware upsample of the low-res fog onto the world viewport.
//
// Linear mode (cComposite.w = 1): the fog is blended over a copy of the scene in linear light,
// scene * T + L, the way the modern client adds its premultiplied volume to a linear frame. Highlights
// above a knee roll off per channel, so saturated sunset colours clip toward yellow like the modern frame.
// Fallback (w = 2, linear fog without a scene copy) and gamma mode (w = 0) use the fixed-function blend
// ONE, INVSRCALPHA.
#include "vf_common.hlsli"

float4 cComposite : register(c9);   // x = exposure, y = god-ray strength, z = debug view, w = blend mode (above)
float4 cRayColor  : register(c10);  // rgb = god-ray colour
float4 cSunPx     : register(c11);  // xy = sun position in render-target pixels, z = sun marker enabled,
                                    // w = the client's glow amount to compensate (0 = none)

sampler2D sDepth : register(s0);
sampler2D sFog   : register(s1);
sampler2D sRays  : register(s2);
sampler2D sScene : register(s3);    // the world viewport before the fog (linear mode)

static const float kKnee = 0.8;

float3 RollOff(float3 x, float3 knee)
{
    float3 span = max(1 - knee, 1e-4);
    return x <= knee ? x : knee + span * (1 - exp(-(x - knee) / span));
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

    // God rays are a display-space overlay on top of the fogged image (their source is the gamma frame).
    float2 uv = (pc - cRect.xy) / cRect.zw;
    float3 rays = 0;
    [branch] if (cComposite.y > 0)
        rays = tex2Dlod(sRays, float4(uv, 0, 0)).rgb * cRayColor.rgb * cComposite.y;
    const bool linearLight = cComposite.w > 0.5;

    [branch] if (cComposite.z > 0.5)
    {
        if (cComposite.z < 1.5)
            return float4((linearLight ? pow(saturate(fog.rgb), 1 / 2.2) : fog.rgb) + rays, 1);
        if (cComposite.z < 2.5)
            return float4((1 - fog.aaa), 1);
        if (cComposite.z < 3.5)
            return float4(saturate(z0 / cDepthLin.z).xxx, 1);
    }
    [branch] if (cSunPx.z > 0 && distance(pc, cSunPx.xy) < 6)
        return float4(1, 0, 0, 1);

    [branch] if (cComposite.w > 0.5 && cComposite.w < 1.5)
    {
        float3 scene = pow(tex2Dlod(sScene, float4(uv, 0, 0)).rgb, 2.2);
        float3 c = pow(RollOff(scene * (1 - fog.a) + fog.rgb, max(kKnee, scene)), 1 / 2.2);
        // The client's glow runs after the fog and adds g * blur^2, which bleaches bright fog to white. On fogged
        // pixels, solve c' + g c'^2 = c so the fog lands on screen as composited.
        [branch] if (cSunPx.w > 0)
            c = lerp(c, (sqrt(1 + 4 * cSunPx.w * c) - 1) / (2 * cSunPx.w), fog.a);
        return float4(c + rays, 1);
    }
    // Fixed-function blend: premultiplied fog, rolled off per channel like the linear path.
    float a = max(fog.a, 1e-4);
    float3 unit = RollOff(fog.rgb / a, kKnee);
    if (linearLight)
        unit = pow(unit, 1 / 2.2);
    return float4(unit * fog.a + rays, fog.a);
}
