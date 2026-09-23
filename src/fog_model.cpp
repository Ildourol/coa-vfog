#include "fog_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
constexpr float kHazeDensity = 0.00035f;
constexpr float kHazeG = 0.75f;
constexpr float kHazeIsotropic = 0.3f;
constexpr float kHazeFalloff = 1.0f / 220.0f;
constexpr float kHazeSun = 1.3f;
constexpr float kHazeAmbient = 0.8f;
constexpr float kHazeShadowDensity = 0.75f;

constexpr float kGroundStart = 12.0f;
constexpr float kGroundOpticalDepth = 1.1f;
constexpr float kGroundG = 0.4f;
constexpr float kGroundIsotropic = 0.5f;
constexpr float kGroundFalloff = 1.0f / 18.0f;
constexpr float kGroundSun = 0.8f;
constexpr float kGroundAmbient = 0.75f;
constexpr float kGroundShadowDensity = 0.85f;

constexpr float kFarOpticalDepth = 3.0f;
constexpr float kFarRamp = 60.0f;
constexpr float kFarStartFraction = 0.25f;
constexpr float kFarG = 0.2f;
constexpr float kFarIsotropic = 0.7f;
constexpr float kFarSun = 0.45f;
constexpr float kFarAmbient = 0.6f;
constexpr float kFarSkyFalloff = 10.0f;
// The Classic far wall sits beyond the 3.3.5 far clip; the distance fog borrows its colours, scaled
// so its intensity-10 scattering stays in range at the shorter distance.
constexpr float kFarWallStart = 1000.0f;
constexpr float kFarWallIntensityScale = 0.1f;

constexpr float kClassicUnits = 0.01f;
constexpr float kFogRangeMargin = 300.0f;
constexpr uint32_t kFlagShadowed = 0x1;
constexpr uint32_t kFlagRelativeHeights = 0x2;

// Range the derived haze and mist were tuned for; Classic layers use the full fog range like the modern client.
constexpr float kDerivedRange = 1500.0f;

constexpr float kMoonLight = 0.35f;
constexpr float kReferenceFarTarget = 200.0f;
constexpr float kNoLimit = 1.0e9f;

float SmoothStep(float e0, float e1, float x)
{
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void Scale(const float* rgb, float s, float* out)
{
    out[0] = rgb[0] * s;
    out[1] = rgb[1] * s;
    out[2] = rgb[2] * s;
}

void Encode(float* rgb, bool linear)
{
    if (linear)
        for (int i = 0; i < 3; ++i)
            rgb[i] = std::pow(std::max(rgb[i], 0.0f), 2.2f);
}

float ReferenceZ(const FrameInputs& in)
{
    float dx = in.camTarget[0] - in.camPos[0];
    float dy = in.camTarget[1] - in.camPos[1];
    float dz = in.camTarget[2] - in.camPos[2];
    if (dx * dx + dy * dy + dz * dz > kReferenceFarTarget * kReferenceFarTarget)
        return in.camPos[2] - 2.0f;
    return std::min(in.camPos[2], in.camTarget[2]) - 1.0f;
}

void Unbounded(FogLayer& l)
{
    l.skyFalloff = 0.0f;
    l.limit = kNoLimit;
}

void DerivedLayers(const FrameInputs& in, const Config& cfg, const FogParams& p, float sun, float fogDistance,
                   const float* fogColor, FogLayer* out)
{
    float foggyZone = std::clamp(700.0f / fogDistance, 0.6f, 2.5f);
    float shadowed = cfg.lightShafts ? 1.0f : 0.0f;

    FogLayer& haze = out[0];
    haze.start = 0.0f;
    haze.density = kHazeDensity * cfg.haze * cfg.density * foggyZone;
    haze.g = kHazeG;
    haze.isotropic = kHazeIsotropic;
    Scale(fogColor, kHazeAmbient * cfg.ambient, haze.emissive);
    Scale(p.lightColor, kHazeSun * sun, haze.diffuse);
    haze.exponent = 1.0f;
    haze.upperHeight = p.referenceZ;
    haze.upperFalloff = kHazeFalloff;
    std::memcpy(haze.shadowEmissive, haze.emissive, sizeof(haze.emissive));
    haze.shadowDensity = kHazeShadowDensity;
    haze.shadowed = shadowed;
    Unbounded(haze);
    haze.limit = kDerivedRange;

    FogLayer& ground = out[1];
    ground.start = kGroundStart;
    ground.density = kGroundOpticalDepth * cfg.groundFog * cfg.density / std::max(fogDistance, 250.0f);
    ground.g = kGroundG;
    ground.isotropic = kGroundIsotropic;
    Scale(fogColor, kGroundAmbient * cfg.ambient, ground.emissive);
    Scale(p.lightColor, kGroundSun * sun, ground.diffuse);
    ground.exponent = 1.0f;
    ground.upperHeight = p.referenceZ;
    ground.upperFalloff = kGroundFalloff;
    std::memcpy(ground.shadowEmissive, ground.emissive, sizeof(ground.emissive));
    ground.shadowDensity = kGroundShadowDensity;
    ground.shadowed = shadowed;
    Unbounded(ground);
    ground.limit = kDerivedRange;

    out[2] = FogLayer{};
    Unbounded(out[2]);
}

void AuthoredLayers(const AuthoredFog& fog, const Config& cfg, const FogParams& p, float sun, FogLayer* out)
{
    for (int i = 0; i < 3; ++i)
    {
        FogLayer& l = out[i];
        l = FogLayer{};
        Unbounded(l);
        if (i >= fog.layerCount)
            continue;
        const AuthoredLayer& a = fog.layers[i];
        const float heightBase = a.flags & kFlagRelativeHeights ? p.referenceZ : 0.0f;
        l.start = std::min(p.maxDistance - kFogRangeMargin, a.start);
        l.density = a.density * kClassicUnits * cfg.density;
        l.g = std::clamp(a.g, -0.99f, 0.99f);
        l.isotropic = 0.0f;
        Scale(a.emissive, cfg.ambient, l.emissive);
        l.strength = a.strength;
        Scale(a.diffuse, 1.0f, l.diffuse);
        l.exponent = a.exponent > 0.0f ? a.exponent : 1.0f;
        l.upperHeight = a.upperHeight + heightBase;
        l.upperFalloff = a.upperDensity > 0.0f ? a.upperDensity * kClassicUnits : 0.0f;
        l.lowerHeight = a.lowerHeight + heightBase;
        l.lowerFalloff = a.lowerDensity > 0.0f ? a.lowerDensity * kClassicUnits : 0.0f;
        Scale(a.shadowEmissive, cfg.ambient, l.shadowEmissive);
        l.shadowDensity = a.shadowMultiplier;
        l.shadowed = (a.flags & kFlagShadowed) && cfg.lightShafts ? 1.0f : 0.0f;
        Encode(l.emissive, p.linear);
        Encode(l.diffuse, p.linear);
        Encode(l.shadowEmissive, p.linear);
        Scale(l.diffuse, a.intensity * sun, l.diffuse);
    }
}

void DistanceLayer(const FrameInputs& in, const Config& cfg, const FogParams& p, const AuthoredFog* authored,
                   float sun, const float* fogColor, FogLayer& out)
{
    out = FogLayer{};
    float range = p.maxDistance;
    float farStart = std::min(std::max(in.fogStart, 0.0f), p.farLimit * kFarStartFraction);
    float us = farStart / range;
    float uf = p.farLimit / range;
    float shape = range * ((uf - us) + kFarRamp * (uf * uf * uf - us * us * us) / 3.0f);
    out.start = farStart;
    out.density = cfg.stockFog == 1 && shape > 0.0f ? kFarOpticalDepth * cfg.farFog * cfg.density / shape : 0.0f;
    out.g = kFarG;
    out.isotropic = kFarIsotropic;
    out.strength = kFarRamp;
    out.exponent = 2.0f;
    out.shadowDensity = 1.0f;
    out.skyFalloff = kFarSkyFalloff;
    out.limit = p.farLimit;

    const AuthoredLayer* wall = nullptr;
    if (authored)
        for (int i = 0; i < authored->layerCount; ++i)
            if (authored->layers[i].start >= kFarWallStart)
                wall = &authored->layers[i];
    if (wall)
    {
        out.g = std::clamp(wall->g, -0.99f, 0.99f);
        out.isotropic = 0.0f;
        Scale(wall->emissive, cfg.ambient, out.emissive);
        Scale(wall->diffuse, 1.0f, out.diffuse);
        Encode(out.emissive, p.linear);
        Encode(out.diffuse, p.linear);
        Scale(out.diffuse, wall->intensity * kFarWallIntensityScale * sun, out.diffuse);
    }
    else
    {
        Scale(fogColor, kFarAmbient * cfg.ambient, out.emissive);
        Scale(p.lightColor, kFarSun * sun, out.diffuse);
    }
    std::memcpy(out.shadowEmissive, out.emissive, sizeof(out.emissive));
}
}

void UnpackColor(uint32_t argb, float* rgb)
{
    rgb[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    rgb[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    rgb[2] = static_cast<float>(argb & 0xFF) / 255.0f;
}

FogParams BuildFogParams(const FrameInputs& in, const Config& cfg, const AuthoredFog* authored)
{
    FogParams p = {};
    p.linear = cfg.colorSpace == 1;
    p.authored = authored && authored->layerCount > 0;
    float fogColor[3];
    UnpackColor(in.fogColor, fogColor);
    UnpackColor(in.directColor, p.lightColor);
    UnpackColor(in.lightIsMoon ? in.directColor : in.sunColor, p.rayColor);
    Encode(fogColor, p.linear);
    Encode(p.lightColor, p.linear);

    float elevation = SmoothStep(-0.03f, 0.10f, in.toLight[2]);
    p.lightVisibility = elevation * (in.lightIsMoon ? kMoonLight : 1.0f);
    p.farClip = in.farClip;
    p.maxDistance = std::max(cfg.maxDistance, in.farClip);
    p.horizonStart = in.farClip * 0.85f;
    p.referenceZ = ReferenceZ(in);
    p.farLimit = std::clamp(in.fogEnd, 100.0f, in.farClip);

    float fogDistance = in.zoneFogDistance > 50.0f && in.zoneFogDistance < 20000.0f ? in.zoneFogDistance : in.fogEnd;
    fogDistance = std::clamp(fogDistance, 50.0f, 5000.0f);
    float sun = p.lightVisibility * cfg.sunScatter;

    if (p.authored)
        AuthoredLayers(*authored, cfg, p, elevation * cfg.sunScatter, p.layers);
    else
        DerivedLayers(in, cfg, p, sun, fogDistance, fogColor, p.layers);
    DistanceLayer(in, cfg, p, p.authored ? authored : nullptr, p.authored ? elevation * cfg.sunScatter : sun,
                  fogColor, p.layers[3]);
    return p;
}
void Mul4x4(const float* a, const float* b, float* out)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    std::memcpy(out, r, sizeof(r));
}

bool Invert4x4(const float* m, float* out)
{
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-12)
        return false;
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<float>(inv[i] / det);
    return true;
}

void TransformDirection(const float* v, const float* m, float* out)
{
    float r[3];
    for (int j = 0; j < 3; ++j)
        r[j] = v[0] * m[0 * 4 + j] + v[1] * m[1 * 4 + j] + v[2] * m[2 * 4 + j];
    std::memcpy(out, r, sizeof(r));
}
