static const uint LIGHT_TYPE_DIRECTIONAL = 0;
static const uint LIGHT_TYPE_POINT = 1;
static const uint SHADOW_TYPE_NONE = 0;
static const uint SHADOW_TYPE_DIRECTIONAL = 1;
static const uint SHADOW_TYPE_POINT = 2;
static const uint MAX_LIGHT_COUNT = 16;
static const float POINT_SHADOW_NEAR = 0.1f;

cbuffer ObjectConstants : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
    float3 cameraPos;
    float _objectPadding0;
    float4 tint;
};

struct GpuLight
{
    float4 positionRange;
    float4 directionType;
    float4 colorIntensity;
    float4 shadowData;
};

cbuffer LightingConstants : register(b1)
{
    float4 ambientColor;
    uint lightCount;
    float3 _lightingPadding0;
    GpuLight lights[MAX_LIGHT_COUNT];
    float4x4 directionalShadowViewProj[MAX_LIGHT_COUNT];
};

SamplerState linearSampler : register(s0);
SamplerComparisonState shadowSampler : register(s1);

Texture2DArray directionalShadowMaps : register(t3);
TextureCubeArray pointShadowMaps : register(t4);

float3 ApplySelectionTint(float3 color)
{
    return lerp(color, color * tint.rgb + tint.rgb * 0.25f, tint.a);
}

float SampleShadowForLight(GpuLight light, uint lightIndex, float3 worldPosition)
{
    uint shadowType = (uint)round(light.shadowData.x);
    if (shadowType == SHADOW_TYPE_NONE)
    {
        return 1.0f;
    }

    uint shadowSlot = (uint)round(light.shadowData.y);
    float shadowStrength = saturate(light.shadowData.z);
    float shadowBias = light.shadowData.w;
    if (shadowStrength <= 0.0f)
    {
        return 1.0f;
    }

    if (shadowType == SHADOW_TYPE_DIRECTIONAL)
    {
        float4 shadowPosition = mul(float4(worldPosition, 1.0f), directionalShadowViewProj[shadowSlot]);
        shadowPosition.xyz /= shadowPosition.w;
        float2 shadowUv = shadowPosition.xy * float2(0.5f, -0.5f) + 0.5f;
        if (all(shadowUv >= 0.0f.xx) && all(shadowUv <= 1.0f.xx) && shadowPosition.z >= 0.0f && shadowPosition.z <= 1.0f)
        {
            float visibility = directionalShadowMaps.SampleCmpLevelZero(shadowSampler, float3(shadowUv, shadowSlot), shadowPosition.z - shadowBias);
            return lerp(1.0f, visibility, shadowStrength);
        }
    }
    else if (shadowType == SHADOW_TYPE_POINT)
    {
        float3 fromLight = worldPosition - light.positionRange.xyz;
        float majorDistance = max(max(abs(fromLight.x), abs(fromLight.y)), abs(fromLight.z));
        float range = max(light.positionRange.w, POINT_SHADOW_NEAR + 0.001f);
        if (majorDistance > POINT_SHADOW_NEAR && majorDistance <= range)
        {
            float compareDepth = range / (range - POINT_SHADOW_NEAR) -
                (POINT_SHADOW_NEAR * range) / (range - POINT_SHADOW_NEAR) / majorDistance;
            float visibility = pointShadowMaps.SampleCmpLevelZero(shadowSampler, float4(fromLight, shadowSlot), compareDepth - shadowBias);
            return lerp(1.0f, visibility, shadowStrength);
        }
    }

    return 1.0f;
}

float3 ComputeLambertLighting(float3 albedo, float3 worldNormal, float3 worldPosition)
{
    float3 n = normalize(worldNormal);
    float3 lit = albedo * ambientColor.rgb;

    [loop]
    for (uint i = 0; i < min(lightCount, MAX_LIGHT_COUNT); ++i)
    {
        GpuLight light = lights[i];
        uint type = (uint)round(light.directionType.w);
        float3 lightColor = light.colorIntensity.rgb;
        float intensity = light.colorIntensity.w;
        float3 l = 0.0f.xxx;
        float attenuation = 1.0f;

        if (type == LIGHT_TYPE_POINT)
        {
            float3 toLight = light.positionRange.xyz - worldPosition;
            float distanceToLight = length(toLight);
            float range = max(light.positionRange.w, 0.001f);
            l = distanceToLight > 0.0001f ? toLight / distanceToLight : float3(0.0f, 1.0f, 0.0f);
            float rangeFactor = saturate(1.0f - distanceToLight / range);
            attenuation = rangeFactor * rangeFactor;
        }
        else
        {
            l = normalize(-light.directionType.xyz);
        }

        float diffuse = saturate(dot(n, l));
        float shadow = SampleShadowForLight(light, i, worldPosition);
        lit += albedo * lightColor * (diffuse * intensity * attenuation * shadow);
    }

    return lit;
}
