#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
constexpr float kMinimumFogCoverage = 0.5f;
constexpr int kMaxBlendedLights = 4;

struct AuthoredFog
{
    int layerCount;
    AuthoredLayer layers[kMaxAuthoredLayers];
    int lightCount;
    uint32_t lightIds[kMaxBlendedLights];
    float lightWeights[kMaxBlendedLights];
    float coverage;
};

class FogData
{
public:
    bool Load(const std::string& path);
    bool Loaded() const { return !m_lights.empty(); }

    bool Resolve(int mapId, const float* position, float dayFraction, int lightParamsSlot, AuthoredFog& out) const;

private:
    static constexpr int kLightParamsSlots = 8;

    struct Light
    {
        uint32_t id;
        int32_t mapId;
        float position[3];
        float falloffStart;
        float falloffEnd;
        uint32_t paramsBySlot[kLightParamsSlots];
    };
    struct Params
    {
        uint32_t id;
        uint32_t firstKey;
        uint32_t keyCount;
    };
    struct Key
    {
        uint16_t halfMinuteOfDay;
        uint16_t layerCount;
        uint32_t firstLayer;
    };
    struct Layer
    {
        uint32_t diffuseRgb;
        uint32_t emissiveRgb;
        uint32_t shadowEmissiveRgb;
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
    static_assert(sizeof(Light) == 60 && sizeof(Params) == 12 && sizeof(Key) == 8 && sizeof(Layer) == 60,
                  "records match the struct formats of tools/convert_classic_fog.py");

    static bool IsMapWide(const Light& light);
    static float SphereWeight(const Light& light, const float* position);
    const Params* FindParams(uint32_t id) const;
    const Params* SlotParams(const Light& light, int lightParamsSlot) const;
    int InterpolateKeys(const Params& params, float halfMinuteOfDay, AuthoredLayer* out) const;

    std::vector<Light> m_lights;
    std::vector<Params> m_params;
    std::vector<Key> m_keys;
    std::vector<Layer> m_layers;
};

FogData& GlobalFogData();
