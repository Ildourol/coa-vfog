// Shared constants and helpers. Register layout mirrors src/renderer.cpp.
//
// Conventions: row vectors (v * M), world Z up, view space +Z forward.
// cProj holds the view->clip terms of the engine projection; depth is linearised
// from the D3D [0,1] depth the engine rasterises.

float4 cRect      : register(c0);  // world viewport in render-target pixels: x, y, w, h
float4 cLow       : register(c1);  // x = full pixels per low-res texel, y = frame index, zw = 1 / depth size
float4 cProj      : register(c2);  // P00, P11, P20, P21
float4 cDepthLin  : register(c3);  // viewZ = y / (d - x); z = max distance; w = sky depth threshold
row_major float4x4 cInvView : register(c4);  // c4..c7 view -> world
float4 cLowSize   : register(c8);  // low-res target: w, h, 1/w, 1/h

float SampleDepth(sampler2D s, float2 px)
{
    return tex2Dlod(s, float4(px * cLow.zw, 0, 0)).r;
}

float LinearDepth(float d)
{
    return d >= cDepthLin.w ? cDepthLin.z : min(cDepthLin.y / (d - cDepthLin.x), cDepthLin.z);
}

// Full-resolution pixel centre represented by a low-res texel.
float2 LowToFull(float2 texel)
{
    float2 px = cRect.xy + texel * cLow.x + floor(cLow.x * 0.5) + 0.5;
    return min(px, cRect.xy + cRect.zw - 0.5);
}

// Continuous low-res texel coordinate (texel centres are integers) of a full-resolution position.
float2 FullToLow(float2 px)
{
    return (px - cRect.xy - floor(cLow.x * 0.5) - 0.5) / cLow.x;
}

// View-space ray through a pixel centre, scaled so that z = 1.
float3 ViewRay(float2 px)
{
    float2 ndc = float2((px.x - cRect.x) / cRect.z * 2 - 1, 1 - (px.y - cRect.y) / cRect.w * 2);
    return float3((ndc.x - cProj.z) / cProj.x, (ndc.y - cProj.w) / cProj.y, 1);
}

float2 ViewToPixel(float3 q)
{
    float2 ndc = float2(q.x / q.z * cProj.x + cProj.z, q.y / q.z * cProj.y + cProj.w);
    return cRect.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * cRect.zw;
}

float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}
