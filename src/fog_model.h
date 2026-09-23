#pragma once

#include "config.h"
#include "engine.h"
#include "fog_data.h"

// One fog layer in shader order: six float4 registers (see AccumulateLayer in vf_march.hlsl).
struct FogLayer
{
    float start;
    float density;
    float g;
    float isotropic;
    float emissive[3];
    float strength;
    float diffuse[3];
    float exponent;
    float upperHeight;
    float upperFalloff;
    float lowerHeight;
    float lowerFalloff;
    float shadowEmissive[3];
    float shadowDensity;
    float shadowed;
    // Sky rays only: density scales by exp(-skyFalloff * max(dir.z, 0)), leaving a horizon band.
    float skyFalloff;
    // Distance beyond which the layer stops accumulating.
    float limit;
    float unused;
};

constexpr int kFogLayers = 4;

struct FogParams
{
    // Three scene layers (Classic-authored or derived) and the distance fog that replaces the stock fog (with
    // Classic layers only where they thin out).
    FogLayer layers[kFogLayers];
    float lightColor[3];
    float rayColor[3];
    float lightVisibility;
    // The modern sunAboveHorizon: shadowed Classic layers treat a light below the horizon as shadow.
    float lightAboveHorizon;
    // Multiplies the shadow-march visibility of shadowed layers (lightAboveHorizon for Classic layers).
    float shadowLight;
    float maxDistance;
    float horizonStart;
    float farClip;
    float referenceZ;
    float farLimit;
    bool linear;
    bool authored;
};

void UnpackColor(uint32_t argb, float* rgb);
FogParams BuildFogParams(const FrameInputs& in, const Config& cfg, const AuthoredFog* authored);

void Mul4x4(const float* a, const float* b, float* out);
bool Invert4x4(const float* m, float* out);
void TransformDirection(const float* v, const float* m, float* out);
