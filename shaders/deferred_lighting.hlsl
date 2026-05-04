#include "common.hlsli"

Texture2D gAlbedo : register(t0);
Texture2D gNormal : register(t1);
Texture2D gWorldPosition : register(t2);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float3 albedo = gAlbedo.Sample(linearSampler, input.uv).rgb;
    float3 normal = gNormal.Sample(linearSampler, input.uv).xyz;
    float3 worldPosition = gWorldPosition.Sample(linearSampler, input.uv).xyz;
    float3 color = ComputeLambertLighting(albedo, normal, worldPosition);
    return float4(color, 1.0f);
}
