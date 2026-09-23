// Low-resolution ray march through the fog layers with screen-space sun visibility.
// Output: rgb = in-scattered radiance, a = 1 - transmittance.
#include "vf_common.hlsli"

#ifndef STEPS
#define STEPS 24
#endif
#define SHADOW_STEPS 6
#define LAYER_REGS 6

float4 cToLight  : register(c9);   // xyz toward-light direction (view space), w = light visibility
float4 cShadow   : register(c10);  // x = min step (yd), y = step per yard of distance, z = enabled, w = thickness in steps
float4 cMarch    : register(c11);  // x = jitter, y = distance-curve range, z = horizon blend start, w = far clip
float4 cLayer[24] : register(c12); // four layers of six float4, see FogLayer in fog_model.h

sampler2D sDepth : register(s0);

float PhaseHG(float g, float c)
{
    float r = (1 - g) / sqrt(max(1 + g * g - 2 * g * c, 1e-6));
    return r * r * r;
}

float SunVisibility(float3 p, float t, float jitter)
{
    float stepLen = max(cShadow.x, t * cShadow.y);
    float thickness = stepLen * cShadow.w;
    float3 stepV = cToLight.xyz * stepLen;
    float3 q = p + stepV * jitter;
    float vis = 1;
    [loop] for (int k = 0; k < SHADOW_STEPS; k++)
    {
        q += stepV;
        [branch] if (q.z < 0.3)
            break;
        float2 px = ViewToPixel(q);
        [branch] if (any(px < cRect.xy) || any(px > cRect.xy + cRect.zw))
            break;
        float sz = LinearDepth(SampleDepth(sDepth, px));
        [branch] if (sz < q.z - 0.3 && sz > q.z - thickness)
        {
            vis = 0;
            break;
        }
    }
    return vis;
}

// Layer registers: L0 (start, density, g, isotropic), L1 (emissive, strength), L2 (diffuse, exponent),
// L3 (upper height, upper falloff, lower height, lower falloff), L4 (shadow emissive, shadow density),
// L5 (shadowed, sky falloff, limit, -).
void AccumulateLayer(int b, float phase, float scale, float ta, float t, float dt, float h, float vis,
                     inout float3 src, inout float tau)
{
    float4 l0 = cLayer[b];
    float4 l1 = cLayer[b + 1];
    float4 l2 = cLayer[b + 2];
    float4 l3 = cLayer[b + 3];
    float4 l4 = cLayer[b + 4];
    float4 l5 = cLayer[b + 5];
    float cover = saturate((t - l0.x) / max(dt, 1e-3)) * saturate((l5.z - ta) / max(dt, 1e-3));
    float curve = 1 + l1.w * pow(saturate(t / cMarch.y) + 1e-6, l2.w);
    float heightF = saturate(exp((l3.x - h) * l3.y)) * saturate(exp((h - l3.z) * l3.w));
    float shadowed = l5.x;
    float lit = lerp(1, vis, shadowed);
    float e = l0.y * scale * dt * cover * curve * heightF * lerp(1, lerp(l4.w, 1, vis), shadowed);
    float3 emissive = lerp(l1.rgb, lerp(l4.rgb, l1.rgb, vis), shadowed);
    src += (l2.rgb * (lit * phase) + emissive) * e;
    tau += e;
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 pc = LowToFull(vpos);
    float3 ray = ViewRay(pc);
    float rayLen = length(ray);
    float3 V = ray / rayLen;
    float d = SampleDepth(sDepth, pc);
    float sky = d >= cDepthLin.w ? 1 : 0;
    float z = LinearDepth(d);
    float horizon = max(sky, smoothstep(cMarch.z, cMarch.w, z));
    z = lerp(z, cDepthLin.z, horizon);
    float tMax = min(z * rayLen, cDepthLin.z);

    float jitter = cMarch.x > 0 ? frac(InterleavedGradientNoise(vpos) + cLow.y * 0.618034) : 0.5;
    float3 camW = cInvView[3].xyz;
    float3 dirW = mul(V, (float3x3)cInvView);
    float cosT = dot(cToLight.xyz, V);
    float up = max(dirW.z, 0);
    // Pixels without depth below the horizon (beyond the far clip, or water that wrote none) and geometry
    // fading into the horizon are marched as level rays, so they meet the sky at eye level seamlessly.
    float dirZ = lerp(dirW.z, up, horizon);

    float phase[4];
    float scale[4];
    [unroll] for (int i = 0; i < 4; i++)
    {
        float4 l0 = cLayer[i * LAYER_REGS];
        float4 l5 = cLayer[i * LAYER_REGS + 5];
        phase[i] = lerp(PhaseHG(l0.z, cosT), 1, l0.w);
        scale[i] = sky > 0 ? exp(-up * l5.y) : 1;
    }

    float3 L = 0;
    float T = 1;
    const float invN = 1.0 / STEPS;
    [loop] for (int s = 0; s < STEPS; s++)
    {
        float u0 = s * invN;
        float u1 = u0 + invN;
        float ta = tMax * u0 * u0;
        float tb = tMax * u1 * u1;
        float dt = tb - ta;
        float t = lerp(ta, tb, jitter);
        float h = camW.z + dirZ * t;
        float vis = 1;
        [branch] if (cShadow.z > 0)
            vis = SunVisibility(V * t, t, jitter);
        float3 src = 0;
        float tau = 0;
        [unroll] for (int j = 0; j < 4; j++)
            AccumulateLayer(j * LAYER_REGS, phase[j], scale[j], ta, t, dt, h, vis, src, tau);
        [branch] if (tau > 1e-6)
        {
            float tr = exp(-tau);
            L += T * src * ((1 - tr) / tau);
            T *= tr;
        }
    }
    return float4(L, 1 - T);
}
