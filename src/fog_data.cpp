#include "fog_data.h"

#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
constexpr uint32_t kVersion = 1;
constexpr float kHalfMinutesPerDay = 2880.0f;
constexpr uint32_t kSlotClear = 0;

struct Header
{
    char magic[4];
    uint32_t version;
    uint32_t lights;
    uint32_t params;
    uint32_t keys;
    uint32_t layers;
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
    bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, "VFD1", 4) == 0 &&
              h.version == kVersion && ReadArray(f, m_lights, h.lights) && ReadArray(f, m_params, h.params) &&
              ReadArray(f, m_keys, h.keys) && ReadArray(f, m_layers, h.layers);
    std::fclose(f);
    for (const Params& p : m_params)
        ok = ok && p.firstKey + p.keyCount <= m_keys.size();
    for (const Key& k : m_keys)
        ok = ok && k.firstLayer + k.layerCount <= m_layers.size() && k.time < kHalfMinutesPerDay;
    if (!ok)
    {
        VF_LOG_ERROR("Classic fog data %s is invalid; derived layers only", path.c_str());
        m_lights.clear();
        return false;
    }
    VF_LOG_INFO("Classic fog data: %u lights, %u light params, %u keys, %u layers", h.lights, h.params, h.keys,
                h.layers);
    return true;
}

const FogData::Params* FogData::FindParams(uint32_t id) const
{
    auto it = std::lower_bound(m_params.begin(), m_params.end(), id,
                               [](const Params& p, uint32_t v) { return p.id < v; });
    return it != m_params.end() && it->id == id ? &*it : nullptr;
}

int FogData::Evaluate(const Params& params, float time, AuthoredLayer* out) const
{
    const Key* keys = &m_keys[params.firstKey];
    const uint32_t count = params.keyCount;
    if (count == 0)
        return 0;

    uint32_t next = 0;
    while (next < count && keys[next].time <= time)
        ++next;
    const uint32_t prev = next == 0 ? count - 1 : next - 1;
    next = next == count ? 0 : next;
    float t0 = keys[prev].time;
    float t1 = keys[next].time;
    if (t0 > time)
        t0 -= kHalfMinutesPerDay;
    if (t1 < time || (t1 == t0 && count > 1))
        t1 += kHalfMinutesPerDay;
    const float frac = t1 > t0 ? std::clamp((time - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;

    const Key& a = keys[prev];
    const Key& b = keys[next];
    const int n = std::min<int>(std::max(a.layerCount, b.layerCount), kMaxAuthoredLayers);
    for (int i = 0; i < n; ++i)
    {
        static const Layer kZero = {};
        const bool hasA = i < a.layerCount;
        const bool hasB = i < b.layerCount;
        const Layer& la = hasA ? m_layers[a.firstLayer + i] : kZero;
        const Layer& lb = hasB ? m_layers[b.firstLayer + i] : kZero;
        AuthoredLayer& o = out[i];
        float ca[3];
        float cb[3];
        UnpackRgb(la.diffuse, ca);
        UnpackRgb(lb.diffuse, cb);
        Lerp3(ca, cb, frac, o.diffuse);
        UnpackRgb(la.emissive, ca);
        UnpackRgb(lb.emissive, cb);
        Lerp3(ca, cb, frac, o.emissive);
        UnpackRgb(la.shadowEmissive, ca);
        UnpackRgb(lb.shadowEmissive, cb);
        Lerp3(ca, cb, frac, o.shadowEmissive);
        o.start = Lerp(la.start, lb.start, frac);
        o.density = Lerp(la.density, lb.density, frac);
        o.shadowMultiplier = Lerp(la.shadowMultiplier, lb.shadowMultiplier, frac);
        o.upperDensity = Lerp(la.upperDensity, lb.upperDensity, frac);
        o.upperHeight = Lerp(la.upperHeight, lb.upperHeight, frac);
        o.lowerDensity = Lerp(la.lowerDensity, lb.lowerDensity, frac);
        o.lowerHeight = Lerp(la.lowerHeight, lb.lowerHeight, frac);
        o.intensity = Lerp(la.intensity, lb.intensity, frac);
        o.g = Lerp(la.g, lb.g, frac);
        o.strength = Lerp(la.strength, lb.strength, frac);
        o.exponent = Lerp(la.exponent, lb.exponent, frac);
        o.flags = !hasB || (hasA && frac < 0.5f) ? la.flags : lb.flags;
    }
    return n;
}

bool FogData::Resolve(int map, const float* pos, float dayFraction, int slot, AuthoredFog& out) const
{
    std::memset(&out, 0, sizeof(out));
    if (m_lights.empty() || map < 0)
        return false;

    struct Contribution
    {
        const Light* light;
        float weight;
    };
    Contribution local[kMaxBlendedLights] = {};
    int localCount = 0;
    const Light* global = nullptr;
    float localSum = 0.0f;
    for (const Light& l : m_lights)
    {
        if (l.map != map)
            continue;
        if (l.falloffEnd <= 0.0f && l.pos[0] == 0.0f && l.pos[1] == 0.0f)
        {
            if (!global)
                global = &l;
            continue;
        }
        const float dx = pos[0] - l.pos[0];
        const float dy = pos[1] - l.pos[1];
        const float dz = pos[2] - l.pos[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        float w = 0.0f;
        if (d <= l.falloffStart)
            w = 1.0f;
        else if (d < l.falloffEnd)
            w = (l.falloffEnd - d) / (l.falloffEnd - l.falloffStart);
        if (w <= 0.0f)
            continue;
        if (localCount < kMaxBlendedLights - 1)
            local[localCount++] = {&l, w};
        else
        {
            auto weakest = std::min_element(local, local + localCount,
                                            [](const Contribution& a, const Contribution& b) { return a.weight < b.weight; });
            if (weakest->weight < w)
                *weakest = {&l, w};
        }
    }
    for (int i = 0; i < localCount; ++i)
        localSum += local[i].weight;
    if (localSum > 1.0f)
        for (int i = 0; i < localCount; ++i)
            local[i].weight /= localSum;
    Contribution all[kMaxBlendedLights] = {};
    int allCount = 0;
    for (int i = 0; i < localCount; ++i)
        all[allCount++] = local[i];
    if (global && localSum < 1.0f)
        all[allCount++] = {global, 1.0f - localSum};

    const float time = std::fmod(std::max(dayFraction, 0.0f), 1.0f) * kHalfMinutesPerDay;
    float covered = 0.0f;
    AuthoredLayer evaluated[kMaxBlendedLights][kMaxAuthoredLayers] = {};
    int counts[kMaxBlendedLights] = {};
    for (int i = 0; i < allCount; ++i)
    {
        const Light& l = *all[i].light;
        const uint32_t slotParams = slot >= 0 && slot < 8 ? l.params[slot] : 0;
        const Params* p = FindParams(slotParams);
        if (!p)
            p = FindParams(l.params[kSlotClear]);
        if (!p)
            continue;
        counts[i] = Evaluate(*p, time, evaluated[i]);
        if (counts[i] == 0)
            continue;
        covered += all[i].weight;
        out.lightIds[out.lightCount] = l.id;
        out.lightWeights[out.lightCount] = all[i].weight;
        ++out.lightCount;
    }
    if (covered < kMinimumFogCoverage)
    {
        std::memset(&out, 0, sizeof(out));
        return false;
    }
    out.coverage = covered;
    for (int i = 0; i < out.lightCount; ++i)
        out.lightWeights[i] /= covered;
    for (int i = 0; i < allCount; ++i)
    {
        if (counts[i] == 0)
            continue;
        const float w = all[i].weight / covered;
        for (int j = 0; j < counts[i]; ++j)
            AddScaled(out.layers[j], evaluated[i][j], w);
        out.layerCount = std::max(out.layerCount, counts[i]);
    }
    return true;
}

FogData& GlobalFogData()
{
    static FogData data;
    return data;
}
