#include "vf_common.hlsli"

float4 cComposite : register(c9);
float4 cGodRayColour : register(c10);
float4 cSun : register(c11);

sampler2D sDepth : register(s0);
sampler2D sFog : register(s1);
sampler2D sGodRays : register(s2);
sampler2D sSceneBeforeFog : register(s3);

static const float kHighlightKnee = 0.8;
static const float kSunMarkerRadius = 6;
static const float4 kSunMarkerColour = float4(1, 0, 0, 1);
static const float kDebugRadiance = 1;
static const float kDebugTransmittance = 2;
static const float kDebugLinearDepth = 3;

float Exposure()
{
    return cComposite.x;
}

float GodRayStrength()
{
    return cComposite.y;
}

float DebugView()
{
    return cComposite.z;
}

bool DebugViewEnabled()
{
    return DebugView() > 0.5;
}

bool DebugViewAtMost(float view)
{
    return DebugView() < view + 0.5;
}

float BlendMode()
{
    return cComposite.w;
}

bool BlendsInLinearLight()
{
    return BlendMode() > 0.5;
}

bool BlendsOverSceneCopy()
{
    return BlendMode() > 0.5 && BlendMode() < 1.5;
}

float2 SunPixel()
{
    return cSun.xy;
}

bool SunMarkerEnabled()
{
    return cSun.z > 0;
}

float ClientGlowToCompensate()
{
    return cSun.w;
}

float3 GammaToLinear(float3 colour)
{
    return pow(colour, 2.2);
}

float3 LinearToGamma(float3 colour)
{
    return pow(colour, 1 / 2.2);
}

float3 RollOffHighlights(float3 colour, float3 knee)
{
    float3 span = max(1 - knee, 1e-4);
    return colour <= knee ? colour : knee + span * (1 - exp(-(colour - knee) / span));
}

float3 BeforeClientGlow(float3 onScreen, float glow)
{
    return (sqrt(1 + 4 * glow * onScreen) - 1) / (2 * glow);
}

float4 DepthAwareUpsample(float2 pixel, float viewZ)
{
    float2 lowResCoord = FullPixelToLowResTexel(pixel);
    float2 baseTexel = floor(lowResCoord);
    float2 bilinearFraction = lowResCoord - baseTexel;

    static const float2 kTapOffsets[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
    float4 weightedFog = 0;
    float weightSum = 0;
    [unroll] for (int j = 0; j < 4; j++)
    {
        float2 tapOffset = kTapOffsets[j];
        float2 tapTexel = clamp(baseTexel + tapOffset, 0, LowResSize() - 1);
        float tapViewZ = LinearDepth(SampleDepth(sDepth, LowResTexelToFullPixel(tapTexel)));
        float2 bilinearWeight = lerp(1 - bilinearFraction, bilinearFraction, tapOffset);
        float relativeDepthDifference = abs(tapViewZ - viewZ) / max(viewZ, 1e-3);
        float weight = bilinearWeight.x * bilinearWeight.y / (1e-3 + relativeDepthDifference) + 1e-6;
        weightedFog += tex2Dlod(sFog, float4(LowResTexelToUv(tapTexel), 0, 0)) * weight;
        weightSum += weight;
    }
    return weightedFog / weightSum;
}

float3 DisplaySpaceGodRays(float2 viewportUv)
{
    float3 godRays = 0;
    [branch] if (GodRayStrength() > 0)
        godRays = tex2Dlod(sGodRays, float4(viewportUv, 0, 0)).rgb * cGodRayColour.rgb * GodRayStrength();
    return godRays;
}

float4 BlendOverSceneCopy(float2 viewportUv, float4 fog, float3 godRays)
{
    float3 scene = GammaToLinear(tex2Dlod(sSceneBeforeFog, float4(viewportUv, 0, 0)).rgb);
    float3 colour = LinearToGamma(RollOffHighlights(scene * (1 - fog.a) + fog.rgb, max(kHighlightKnee, scene)));
    [branch] if (ClientGlowToCompensate() > 0)
        colour = lerp(colour, BeforeClientGlow(colour, ClientGlowToCompensate()), fog.a);
    return float4(colour + godRays, 1);
}

float4 PremultipliedForFixedFunctionBlend(float4 fog, float3 godRays, bool linearLight)
{
    float opacity = max(fog.a, 1e-4);
    float3 unpremultiplied = RollOffHighlights(fog.rgb / opacity, kHighlightKnee);
    if (linearLight)
        unpremultiplied = LinearToGamma(unpremultiplied);
    return float4(unpremultiplied * fog.a + godRays, fog.a);
}

float4 main(float2 pixelIndex : VPOS) : COLOR0
{
    float2 pixel = pixelIndex + 0.5;
    float depth = SampleDepth(sDepth, pixel);
    float viewZ = LinearDepth(depth);
    float4 fog;
    [branch] if (BeyondFarClip(depth))
        fog = tex2Dlod(sFog, float4(LowResTexelToUv(FullPixelToLowResTexel(pixel)), 0, 0));
    else
        fog = DepthAwareUpsample(pixel, viewZ);
    fog.rgb *= Exposure();

    float2 viewportUv = (pixel - ViewportOrigin()) / ViewportSize();
    float3 godRays = DisplaySpaceGodRays(viewportUv);
    const bool linearLight = BlendsInLinearLight();

    [branch] if (DebugViewEnabled())
    {
        if (DebugViewAtMost(kDebugRadiance))
            return float4((linearLight ? LinearToGamma(saturate(fog.rgb)) : fog.rgb) + godRays, 1);
        if (DebugViewAtMost(kDebugTransmittance))
            return float4(1 - fog.aaa, 1);
        if (DebugViewAtMost(kDebugLinearDepth))
            return float4(saturate(viewZ / MaxFogDistance()).xxx, 1);
    }
    [branch] if (SunMarkerEnabled() && distance(pixel, SunPixel()) < kSunMarkerRadius)
        return kSunMarkerColour;

    [branch] if (BlendsOverSceneCopy())
        return BlendOverSceneCopy(viewportUv, fog, godRays);
    return PremultipliedForFixedFunctionBlend(fog, godRays, linearLight);
}
