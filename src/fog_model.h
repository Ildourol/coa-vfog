#pragma once

#include "config.h"
#include "engine.h"

// One fog layer in shader order: four float4 registers (see vf_march.hlsl AccumulateLayer).
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
    float heightBase;
    float heightFalloff;
    float shadowed;
    float shadowDensity;
};

struct FogParams
{
    // Haze, ground mist, and the distance fog that stands in for the stock fog.
    FogLayer layers[3];
    float lightColor[3];
    float rayColor[3];
    float lightVisibility;
    float maxDistance;
    float horizonStart;
    float farClip;
    float referenceZ;
    // Sky rays only: the distance fog fades with ray elevation as exp(-k * dir.z), leaving a horizon band.
    float farSkyFalloff;
    // The distance fog stops accumulating beyond this distance.
    float farLimit;
};

void UnpackColor(uint32_t argb, float* rgb);
FogParams BuildFogParams(const FrameInputs& in, const Config& cfg);

void Mul4x4(const float* a, const float* b, float* out);
bool Invert4x4(const float* m, float* out);
void TransformDirection(const float* v, const float* m, float* out);
