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

struct PSOutput
{
    float4 albedo : SV_TARGET0;
    float4 normal : SV_TARGET1;
    float4 worldPosition : SV_TARGET2;
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

PSOutput PSMain(PSInput input)
{
    PSOutput output;
    float3 albedo = diffuseTexture.Sample(linearSampler, input.uv).rgb;
    output.albedo = float4(ApplySelectionTint(albedo), 1.0f);
    output.normal = float4(normalize(input.worldNormal), 1.0f);
    output.worldPosition = float4(input.worldPosition, 1.0f);
    return output;
}
