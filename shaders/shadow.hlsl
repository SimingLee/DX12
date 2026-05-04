#include "common.hlsli"

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

float4 VSMain(VSInput input) : SV_POSITION
{
    float4 worldPosition = mul(float4(input.position, 1.0f), world);
    return mul(mul(worldPosition, view), proj);
}
