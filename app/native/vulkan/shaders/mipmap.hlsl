cbuffer cbCS : register(b0)
{
    uint g_source_width;
    uint g_source_height;
    uint g_destination_width;
    uint g_destination_height;
    uint g_flags;
};

Texture2D<float4> g_Input : register(t0);
RWTexture2D<float4> g_Output : register(u0);

static const uint FLAG_INPUT_SRGB = 1u;
static const uint FLAG_SEPARATE_ALPHA = 4u;
static const uint FLAG_OUTPUT_SRGB = 8u;

float3 SrgbToLinear(float3 value)
{
    float3 low = value / 12.92;
    float3 high = pow((value + 0.055) / 1.055, 2.4);
    return value <= 0.04045 ? low : high;
}

float3 LinearToSrgb(float3 value)
{
    value = max(value, 0.0);
    float3 low = value * 12.92;
    float3 high = 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    return value <= 0.0031308 ? low : high;
}

float4 LoadLinear(uint2 coordinate)
{
    float4 value = g_Input.Load(uint3(
        min(coordinate.x, g_source_width - 1u),
        min(coordinate.y, g_source_height - 1u),
        0));
    if (g_flags & FLAG_INPUT_SRGB)
        value.rgb = SrgbToLinear(value.rgb);
    if (!(g_flags & FLAG_SEPARATE_ALPHA))
        value.rgb *= value.a;
    return value;
}

[numthreads(8, 8, 1)]
void GenerateMip(uint3 dispatchID : SV_DispatchThreadID)
{
    if (dispatchID.x >= g_destination_width || dispatchID.y >= g_destination_height)
        return;

    float2 sourcePosition =
        (float2(dispatchID.xy) + 0.5) *
        float2(g_source_width, g_source_height) /
        float2(g_destination_width, g_destination_height) - 0.5;
    int2 base = int2(floor(sourcePosition));
    float2 fraction = sourcePosition - base;
    uint2 p00 = uint2(max(base, 0));
    uint2 p10 = uint2(max(base + int2(1, 0), 0));
    uint2 p01 = uint2(max(base + int2(0, 1), 0));
    uint2 p11 = uint2(max(base + int2(1, 1), 0));
    float4 top = lerp(LoadLinear(p00), LoadLinear(p10), fraction.x);
    float4 bottom = lerp(LoadLinear(p01), LoadLinear(p11), fraction.x);
    float4 result = lerp(top, bottom, fraction.y);

    if (!(g_flags & FLAG_SEPARATE_ALPHA) && result.a > 1e-8)
        result.rgb /= result.a;
    if (g_flags & FLAG_OUTPUT_SRGB)
        result.rgb = LinearToSrgb(result.rgb);
    g_Output[dispatchID.xy] = result;
}
