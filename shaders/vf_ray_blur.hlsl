// One radial blur pass toward the sun.
float4 cRayBlur : register(c9);   // xy = sun position in uv, z = step fraction, w = normalisation
float4 cRayTex  : register(c10);  // zw = 1 / target size

sampler2D sSource : register(s0);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * cRayTex.zw;
    float2 d = (cRayBlur.xy - uv) * cRayBlur.z;
    float3 sum = 0;
    float decay = 1;
    [unroll] for (int k = 0; k < 8; k++)
    {
        sum += tex2Dlod(sSource, float4(uv + d * k, 0, 0)).rgb * decay;
        decay *= 0.9;
    }
    return float4(sum * cRayBlur.w, 1);
}
