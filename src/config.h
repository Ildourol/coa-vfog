#pragma once

#include <string>

constexpr float kEngineFarClipMin = 183.333f;
constexpr float kEngineFarClipMax = 1583.33f;
constexpr float kFarClipMaxKeepsClientCap = 0.0f;

struct Config
{
    bool enable = true;
    bool hooks = true;
    int quality = 2;
    float density = 1.0f;
    float haze = 1.0f;
    float groundFog = 0.6f;
    float farFog = 1.0f;
    int stockFog = 1;
    int dataMode = 1;
    int colorSpace = 1;
    float sunScatter = 1.0f;
    float ambient = 1.0f;
    float exposure = 1.0f;
    float classicExposure = 1.0f;
    bool lightShafts = true;
    float godRays = 0.0f;
    bool glowCompensation = true;
    float farClipMax = kFarClipMaxKeepsClientCap;
    float maxDistance = 5000.0f;
    float temporal = 0.85f;
    bool underwater = false;
    bool liquidDepth = true;
    int debugView = 0;
    bool sunMarker = false;
    int logLevel = 1;
};

class ConfigStore
{
public:
    void Load(const std::string& path);
    bool ReloadIfChanged();
    void Override(const Config& config) { m_config = config; }
    const Config& Get() const { return m_config; }

private:
    void Read();

    std::string m_path;
    unsigned long long m_stamp = 0;
    Config m_config;
};

ConfigStore& GlobalConfig();
