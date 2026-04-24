cbuffer SceneConstants : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
    float3 cameraPos;
    float _padding0;
    float3 lightDir;
    float _padding1;
};

Texture2D diffuseTexture : register(t0);
SamplerState linearSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(float4(input.position, 1.0f), world);
    float4 viewPos = mul(worldPos, view);
    output.position = mul(viewPos, proj);
    output.worldNormal = normalize(mul(float4(input.normal, 0.0f), world).xyz);
    output.uv = input.uv;

    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 albedo = diffuseTexture.Sample(linearSampler, input.uv).rgb;
    float3 n = normalize(input.worldNormal);
    float3 l = normalize(-lightDir);
    float diffuse = saturate(dot(n, l));
    float3 ambient = 0.18f.xxx;
    float3 color = albedo * (ambient + diffuse);
    return float4(color, 1.0f);
}
