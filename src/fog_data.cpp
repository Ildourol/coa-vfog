#include "fog_data.h"

#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
constexpr char kFileMagic[4] = {'V', 'F', 'D', '1'};
constexpr uint32_t kFormatVersion = 1;
constexpr float kHalfMinutesPerDay = 2880.0f;
constexpr uint32_t kClearWeatherSlot = 0;

struct Header
{
    char magic[4];
    uint32_t version;
    uint32_t lightCount;
    uint32_t paramsCount;
    uint32_t keyCount;
    uint32_t layerCount;
};

template <typename T>
bool ReadArray(std::FILE* f, std::vector<T>& out, uint32_t count)
{
    out.resize(count);
    return count == 0 || std::fread(out.data(), sizeof(T), count, f) == count;
}

void UnpackRgb(uint32_t rgb, float* out)
{
    out[0] = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(rgb & 0xFF) / 255.0f;
}

void Lerp3(const float* a, const float* b, float t, float* out)
{
    for (int i = 0; i < 3; ++i)
        out[i] = a[i] + (b[i] - a[i]) * t;
}

float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

void LerpPackedRgb(uint32_t a, uint32_t b, float t, float* out)
{
    float rgbA[3];
    float rgbB[3];
    UnpackRgb(a, rgbA);
    UnpackRgb(b, rgbB);
    Lerp3(rgbA, rgbB, t, out);
}

void AddScaled(AuthoredLayer& acc, const AuthoredLayer& l, float w)
{
    for (int i = 0; i < 3; ++i)
    {
        acc.diffuse[i] += l.diffuse[i] * w;
        acc.emissive[i] += l.emissive[i] * w;
        acc.shadowEmissive[i] += l.shadowEmissive[i] * w;
    }
    acc.start += l.start * w;
    acc.density += l.density * w;
    acc.shadowMultiplier += l.shadowMultiplier * w;
    acc.upperDensity += l.upperDensity * w;
    acc.upperHeight += l.upperHeight * w;
    acc.lowerDensity += l.lowerDensity * w;
    acc.lowerHeight += l.lowerHeight * w;
    acc.intensity += l.intensity * w;
    acc.g += l.g * w;
    acc.strength += l.strength * w;
    acc.exponent += l.exponent * w;
    acc.flags |= l.flags;
}
}

static_assert(sizeof(Header) == 24, "fogdata header");

bool FogData::Load(const std::string& path)
{
    m_lights.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
    {
        VF_LOG_INFO("no Classic fog data at %s; derived layers only", path.c_str());
        return false;
    }
    Header h = {};
    bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, kFileMagic, sizeof(kFileMagic)) == 0 &&
              h.version == kFormatVersion && ReadArray(f, m_lights, h.lightCount) &&
              ReadArray(f, m_params, h.paramsCount) && ReadArray(f, m_keys, h.keyCount) &&
              ReadArray(f, m_layers, h.layerCount);
    std::fclose(f);
    for (const Params& p : m_params)
        ok = ok && p.firstKey + p.keyCount <= m_keys.size();
    for (const Key& k : m_keys)
        ok = ok && k.firstLayer + k.layerCount <= m_layers.size() && k.halfMinuteOfDay < kHalfMinutesPerDay;
    if (!ok)
    {
        VF_LOG_ERROR("Classic fog data %s is invalid; derived layers only", path.c_str());
        m_lights.clear();
        return false;
    }
    VF_LOG_INFO("Classic fog data: %u lights, %u light params, %u keys, %u layers", h.lightCount, h.paramsCount,
                h.keyCount, h.layerCount);
    return true;
}

bool FogData::IsMapWide(const Light& light)
{
    return light.falloffEnd <= 0.0f && light.position[0] == 0.0f && light.position[1] == 0.0f;
}

float FogData::SphereWeight(const Light& light, const float* position)
{
    const float dx = position[0] - light.position[0];
    const float dy = position[1] - light.position[1];
    const float dz = position[2] - light.position[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance <= light.falloffStart)
        return 1.0f;
    if (distance < light.falloffEnd)
        return (light.falloffEnd - distance) / (light.falloffEnd - light.falloffStart);
    return 0.0f;
}

const FogData::Params* FogData::FindParams(uint32_t id) const
{
    auto it = std::lower_bound(m_params.begin(), m_params.end(), id,
                               [](const Params& p, uint32_t v) { return p.id < v; });
    return it != m_params.end() && it->id == id ? &*it : nullptr;
}

const FogData::Params* FogData::SlotParams(const Light& light, int lightParamsSlot) const
{
    const bool slotInRange = lightParamsSlot >= 0 && lightParamsSlot < kLightParamsSlots;
    const Params* params = FindParams(slotInRange ? light.paramsBySlot[lightParamsSlot] : 0);
    return params ? params : FindParams(light.paramsBySlot[kClearWeatherSlot]);
}

int FogData::InterpolateKeys(const Params& params, float halfMinuteOfDay, AuthoredLayer* out) const
{
    const Key* keys = &m_keys[params.firstKey];
    const uint32_t count = params.keyCount;
    if (count == 0)
        return 0;

    uint32_t next = 0;
    while (next < count && keys[next].halfMinuteOfDay <= halfMinuteOfDay)
        ++next;
    const uint32_t prev = next == 0 ? count - 1 : next - 1;
    next = next == count ? 0 : next;
    float prevTime = keys[prev].halfMinuteOfDay;
    float nextTime = keys[next].halfMinuteOfDay;
    if (prevTime > halfMinuteOfDay)
        prevTime -= kHalfMinutesPerDay;
    if (nextTime < halfMinuteOfDay || (nextTime == prevTime && count > 1))
        nextTime += kHalfMinutesPerDay;
    const float fraction =
        nextTime > prevTime ? std::clamp((halfMinuteOfDay - prevTime) / (nextTime - prevTime), 0.0f, 1.0f) : 0.0f;

    const Key& a = keys[prev];
    const Key& b = keys[next];
    const int layerCount = std::min<int>(std::max(a.layerCount, b.layerCount), kMaxAuthoredLayers);
    for (int i = 0; i < layerCount; ++i)
    {
        static const Layer kAbsentLayer = {};
        const bool hasA = i < a.layerCount;
        const bool hasB = i < b.layerCount;
        const Layer& la = hasA ? m_layers[a.firstLayer + i] : kAbsentLayer;
        const Layer& lb = hasB ? m_layers[b.firstLayer + i] : kAbsentLayer;
        AuthoredLayer& layer = out[i];
        LerpPackedRgb(la.diffuseRgb, lb.diffuseRgb, fraction, layer.diffuse);
        LerpPackedRgb(la.emissiveRgb, lb.emissiveRgb, fraction, layer.emissive);
        LerpPackedRgb(la.shadowEmissiveRgb, lb.shadowEmissiveRgb, fraction, layer.shadowEmissive);
        layer.start = Lerp(la.start, lb.start, fraction);
        layer.density = Lerp(la.density, lb.density, fraction);
        layer.shadowMultiplier = Lerp(la.shadowMultiplier, lb.shadowMultiplier, fraction);
        layer.upperDensity = Lerp(la.upperDensity, lb.upperDensity, fraction);
        layer.upperHeight = Lerp(la.upperHeight, lb.upperHeight, fraction);
        layer.lowerDensity = Lerp(la.lowerDensity, lb.lowerDensity, fraction);
        layer.lowerHeight = Lerp(la.lowerHeight, lb.lowerHeight, fraction);
        layer.intensity = Lerp(la.intensity, lb.intensity, fraction);
        layer.g = Lerp(la.g, lb.g, fraction);
        layer.strength = Lerp(la.strength, lb.strength, fraction);
        layer.exponent = Lerp(la.exponent, lb.exponent, fraction);
        layer.flags = !hasB || (hasA && fraction < 0.5f) ? la.flags : lb.flags;
    }
    return layerCount;
}

bool FogData::Resolve(int mapId, const float* position, float dayFraction, int lightParamsSlot, AuthoredFog& out) const
{
    std::memset(&out, 0, sizeof(out));
    if (m_lights.empty() || mapId < 0)
        return false;

    struct Contribution
    {
        const Light* light;
        float weight;
    };
    const auto byWeight = [](const Contribution& a, const Contribution& b) { return a.weight < b.weight; };
    Contribution local[kMaxBlendedLights] = {};
    int localCount = 0;
    const Light* mapWide = nullptr;
    float localWeight = 0.0f;
    for (const Light& light : m_lights)
    {
        if (light.mapId != mapId)
            continue;
        if (IsMapWide(light))
        {
            if (!mapWide)
                mapWide = &light;
            continue;
        }
        const float weight = SphereWeight(light, position);
        if (weight <= 0.0f)
            continue;
        if (localCount < kMaxBlendedLights - 1)
            local[localCount++] = {&light, weight};
        else
        {
            auto weakest = std::min_element(local, local + localCount, byWeight);
            if (weakest->weight < weight)
                *weakest = {&light, weight};
        }
    }
    for (int i = 0; i < localCount; ++i)
        localWeight += local[i].weight;
    if (localWeight > 1.0f)
        for (int i = 0; i < localCount; ++i)
            local[i].weight /= localWeight;
    Contribution blended[kMaxBlendedLights] = {};
    int blendedCount = 0;
    for (int i = 0; i < localCount; ++i)
        blended[blendedCount++] = local[i];
    if (mapWide && localWeight < 1.0f)
        blended[blendedCount++] = {mapWide, 1.0f - localWeight};

    const float halfMinuteOfDay = std::fmod(std::max(dayFraction, 0.0f), 1.0f) * kHalfMinutesPerDay;
    float fogLightWeight = 0.0f;
    AuthoredLayer layersByLight[kMaxBlendedLights][kMaxAuthoredLayers] = {};
    int layerCounts[kMaxBlendedLights] = {};
    for (int i = 0; i < blendedCount; ++i)
    {
        const Light& light = *blended[i].light;
        const Params* params = SlotParams(light, lightParamsSlot);
        if (!params)
            continue;
        layerCounts[i] = InterpolateKeys(*params, halfMinuteOfDay, layersByLight[i]);
        if (layerCounts[i] == 0)
            continue;
        fogLightWeight += blended[i].weight;
        out.lightIds[out.lightCount] = light.id;
        out.lightWeights[out.lightCount] = blended[i].weight;
        ++out.lightCount;
    }
    if (fogLightWeight < kMinimumFogCoverage)
    {
        std::memset(&out, 0, sizeof(out));
        return false;
    }
    out.coverage = fogLightWeight;
    for (int i = 0; i < out.lightCount; ++i)
        out.lightWeights[i] /= fogLightWeight;
    for (int i = 0; i < blendedCount; ++i)
    {
        if (layerCounts[i] == 0)
            continue;
        const float weight = blended[i].weight / fogLightWeight;
        for (int j = 0; j < layerCounts[i]; ++j)
            AddScaled(out.layers[j], layersByLight[i][j], weight);
        out.layerCount = std::max(out.layerCount, layerCounts[i]);
    }
    return true;
}

FogData& GlobalFogData()
{
    static FogData data;
    return data;
}
