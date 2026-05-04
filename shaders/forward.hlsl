#include "common.hlsli"

Texture2D diffuseTexture : register(t0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPosition = mul(float4(input.position, 1.0f), world);
    output.worldPosition = worldPosition.xyz;
    output.position = mul(mul(worldPosition, view), proj);
    output.worldNormal = normalize(mul(float4(input.normal, 0.0f), world).xyz);
    output.uv = input.uv;

    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 albedo = diffuseTexture.Sample(linearSampler, input.uv).rgb;
    albedo = ApplySelectionTint(albedo);
    float3 color = ComputeLambertLighting(albedo, input.worldNormal, input.worldPosition);
    return float4(color, 1.0f);
}
