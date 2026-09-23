#pragma once

#include "config.h"
#include "engine.h"

// One fog layer in shader order: four float4 registers (see vf_march.hlsl AccumulateLayer).
struct FogLayer
{
    float start;
    float density;
    float g;
    float unused;
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
    FogLayer layers[2];
    float lightColor[3];
    float lightVisibility;
    float maxDistance;
    float horizonStart;
    float farClip;
    float referenceZ;
};

void UnpackColor(uint32_t argb, float* rgb);
FogParams BuildFogParams(const FrameInputs& in, const Config& cfg);

void Mul4x4(const float* a, const float* b, float* out);
bool Invert4x4(const float* m, float* out);
void TransformDirection(const float* v, const float* m, float* out);
