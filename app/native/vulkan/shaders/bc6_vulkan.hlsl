cbuffer cbCS : register(b0)
{
    uint g_tex_width;
    uint g_num_block_x;
    uint g_format;
    uint g_mode_id;
    uint g_start_block_id;
    uint g_num_total_blocks;
    float g_alpha_weight;
    float g_quality;
};

Texture2D<float4> g_Input : register(t0);
StructuredBuffer<uint4> g_InBuff : register(t1);
RWStructuredBuffer<uint4> g_OutBuff : register(u0);

void SetBits(inout uint4 block, uint offset, uint count, uint value)
{
    for (uint bit = 0; bit < count; ++bit)
    {
        uint target = offset + bit;
        block[target >> 5] |= ((value >> bit) & 1u) << (target & 31u);
    }
}

uint QuantizeEndpoint(float value)
{
    uint halfBits = f32tof16(max(value, 0.0));
    return min(1023u, ((halfBits << 10u) + 15872u) / 31744u);
}

[numthreads(64, 1, 1)]
void EncodeBlocks(uint3 dispatchID : SV_DispatchThreadID)
{
    uint blockID = g_start_block_id + dispatchID.x;
    if (blockID >= g_num_total_blocks)
        return;

    uint blockX = blockID % g_num_block_x;
    uint blockY = blockID / g_num_block_x;
    uint height = ((g_num_total_blocks + g_num_block_x - 1u) / g_num_block_x) * 4u;
    float3 pixels[16];
    float3 low = float3(65504.0, 65504.0, 65504.0);
    float3 high = 0.0;
    for (uint i = 0; i < 16; ++i)
    {
        uint2 p = uint2(
            min(blockX * 4u + (i & 3u), g_tex_width - 1u),
            min(blockY * 4u + (i >> 2u), height - 1u));
        pixels[i] = max(g_Input.Load(uint3(p, 0)).rgb, 0.0);
        low = min(low, pixels[i]);
        high = max(high, pixels[i]);
    }

    float3 axis = high - low;
    float denominator = max(dot(axis, axis), 1e-20);
    uint indices[16];
    for (uint j = 0; j < 16; ++j)
        indices[j] = (uint)round(saturate(dot(pixels[j] - low, axis) / denominator) * 15.0);

    if (indices[0] >= 8u)
    {
        float3 swap = low;
        low = high;
        high = swap;
        for (uint k = 0; k < 16; ++k)
            indices[k] = 15u - indices[k];
    }

    uint4 result = 0u;
    SetBits(result, 0u, 5u, 0x03u);
    SetBits(result, 5u, 10u, QuantizeEndpoint(low.r));
    SetBits(result, 15u, 10u, QuantizeEndpoint(low.g));
    SetBits(result, 25u, 10u, QuantizeEndpoint(low.b));
    SetBits(result, 35u, 10u, QuantizeEndpoint(high.r));
    SetBits(result, 45u, 10u, QuantizeEndpoint(high.g));
    SetBits(result, 55u, 10u, QuantizeEndpoint(high.b));
    SetBits(result, 65u, 3u, indices[0]);
    uint indexOffset = 68u;
    for (uint n = 1; n < 16; ++n)
    {
        SetBits(result, indexOffset, 4u, indices[n]);
        indexOffset += 4u;
    }
    g_OutBuff[blockID] = result;
}
