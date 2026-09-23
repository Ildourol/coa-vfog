#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One Classic LightDataGlobalVolumeFog layer after key interpolation and light blending.
// Colours are 0..1 in the table's own (gamma) encoding; the rest are the table's raw values.
struct AuthoredLayer
{
    float diffuse[3];
    float emissive[3];
    float shadowEmissive[3];
    float start;
    float density;
    float shadowMultiplier;
    float upperDensity;
    float upperHeight;
    float lowerDensity;
    float lowerHeight;
    float intensity;
    float g;
    float strength;
    float exponent;
    uint32_t flags;
};

constexpr int kMaxAuthoredLayers = 3;
constexpr int kMaxBlendedLights = 4;

struct AuthoredFog
{
    int layerCount;
    AuthoredLayer layers[kMaxAuthoredLayers];
    int lightCount;
    uint32_t lightIds[kMaxBlendedLights];
    float lightWeights[kMaxBlendedLights];
};

class FogData
{
public:
    bool Load(const std::string& path);
    bool Loaded() const { return !m_lights.empty(); }

    // Blends the lights covering pos on the map and interpolates their keys at the day fraction.
    // False when the map has no Classic light with fog data.
    bool Resolve(int map, const float* pos, float dayFraction, int slot, AuthoredFog& out) const;

private:
    struct Light
    {
        uint32_t id;
        int32_t map;
        float pos[3];
        float falloffStart;
        float falloffEnd;
        uint32_t params[8];
    };
    struct Params
    {
        uint32_t id;
        uint32_t firstKey;
        uint32_t keyCount;
    };
    struct Key
    {
        uint16_t time;
        uint16_t layerCount;
        uint32_t firstLayer;
    };
    struct Layer
    {
        uint32_t diffuse;
        uint32_t emissive;
        uint32_t shadowEmissive;
        uint32_t flags;
        float start;
        float density;
        float shadowMultiplier;
        float upperDensity;
        float upperHeight;
        float lowerDensity;
        float lowerHeight;
        float intensity;
        float g;
        float strength;
        float exponent;
    };

    const Params* FindParams(uint32_t id) const;
    int Evaluate(const Params& params, float time, AuthoredLayer* out) const;

    std::vector<Light> m_lights;
    std::vector<Params> m_params;
    std::vector<Key> m_keys;
    std::vector<Layer> m_layers;
};

FogData& GlobalFogData();
