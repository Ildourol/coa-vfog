#include "config.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace
{
constexpr const char* kSection = "CoAVolFog";

unsigned long long FileStamp(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
        return 0;
    return (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}

float ReadFloat(const std::string& path, const char* key, float fallback, float lo, float hi)
{
    char buf[64];
    GetPrivateProfileStringA(kSection, key, "", buf, sizeof(buf), path.c_str());
    if (!buf[0])
        return fallback;
    char* end = nullptr;
    float v = std::strtof(buf, &end);
    if (end == buf || v != v)
        return fallback;
    return std::clamp(v, lo, hi);
}

int ReadInt(const std::string& path, const char* key, int fallback, int lo, int hi)
{
    int v = static_cast<int>(GetPrivateProfileIntA(kSection, key, fallback, path.c_str()));
    return std::clamp(v, lo, hi);
}
}

void ConfigStore::Load(const std::string& path)
{
    m_path = path;
    m_stamp = FileStamp(path);
    Read();
}

bool ConfigStore::ReloadIfChanged()
{
    unsigned long long stamp = FileStamp(m_path);
    if (stamp == m_stamp)
        return false;
    m_stamp = stamp;
    bool enable = m_config.enable;
    bool hooks = m_config.hooks;
    Read();
    m_config.enable = enable;
    m_config.hooks = hooks;
    VF_LOG_INFO("config reloaded: density=%.2f haze=%.2f ground=%.2f far=%.2f stockfog=%d sun=%.2f rays=%.2f "
                "glow=%d farclipmax=%.0f quality=%d debug=%d",
                m_config.density, m_config.haze, m_config.groundFog, m_config.farFog, m_config.stockFog,
                m_config.sunScatter, m_config.godRays, m_config.glowCompensation ? 1 : 0, m_config.farClipMax,
                m_config.quality, m_config.debugView);
    return true;
}

void ConfigStore::Read()
{
    Config c;
    const std::string& p = m_path;
    c.enable = ReadInt(p, "Enable", 1, 0, 1) != 0;
    c.hooks = ReadInt(p, "EngineHooks", 1, 0, 1) != 0;
    c.quality = ReadInt(p, "Quality", c.quality, 1, 3);
    c.density = ReadFloat(p, "Density", c.density, 0.0f, 10.0f);
    c.haze = ReadFloat(p, "Haze", c.haze, 0.0f, 10.0f);
    c.groundFog = ReadFloat(p, "GroundFog", c.groundFog, 0.0f, 10.0f);
    c.farFog = ReadFloat(p, "FarFog", c.farFog, 0.0f, 10.0f);
    c.stockFog = ReadInt(p, "StockFog", c.stockFog, 0, 1);
    c.dataMode = ReadInt(p, "DataMode", c.dataMode, 0, 1);
    c.colorSpace = ReadInt(p, "ColorSpace", c.colorSpace, 0, 1);
    c.sunScatter = ReadFloat(p, "SunScatter", c.sunScatter, 0.0f, 10.0f);
    c.ambient = ReadFloat(p, "Ambient", c.ambient, 0.0f, 10.0f);
    c.exposure = ReadFloat(p, "Exposure", c.exposure, 0.0f, 10.0f);
    c.classicExposure = ReadFloat(p, "ClassicExposure", c.classicExposure, 0.0f, 10.0f);
    c.lightShafts = ReadInt(p, "LightShafts", 1, 0, 1) != 0;
    c.godRays = ReadFloat(p, "GodRays", c.godRays, 0.0f, 4.0f);
    c.glowCompensation = ReadInt(p, "GlowCompensation", 1, 0, 1) != 0;
    c.farClipMax = ReadFloat(p, "FarClipMax", c.farClipMax, 0.0f, kEngineFarClipMax);
    if (c.farClipMax < kEngineFarClipMin)
        c.farClipMax = kFarClipMaxKeepsClientCap;
    c.maxDistance = ReadFloat(p, "MaxDistance", c.maxDistance, 200.0f, 5000.0f);
    c.temporal = ReadFloat(p, "Temporal", c.temporal, 0.0f, 0.97f);
    c.underwater = ReadInt(p, "Underwater", 0, 0, 1) != 0;
    c.liquidDepth = ReadInt(p, "LiquidDepth", 1, 0, 1) != 0;
    c.debugView = ReadInt(p, "DebugView", 0, 0, 3);
    c.sunMarker = ReadInt(p, "SunMarker", 0, 0, 1) != 0;
    c.logLevel = ReadInt(p, "LogLevel", c.logLevel, 0, 2);
    m_config = c;
    LogSetLevel(c.logLevel);
}

ConfigStore& GlobalConfig()
{
    static ConfigStore store;
    return store;
}
