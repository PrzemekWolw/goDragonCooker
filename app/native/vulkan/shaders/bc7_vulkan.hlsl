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

static const uint WEIGHTS[16] = {
    0u, 4u, 9u, 13u, 17u, 21u, 26u, 30u,
    34u, 38u, 43u, 47u, 51u, 55u, 60u, 64u
};

void SetBits(inout uint4 block, uint offset, uint count, uint value)
{
    for (uint bit = 0; bit < count; ++bit)
    {
        uint target = offset + bit;
        block[target >> 5] |= ((value >> bit) & 1u) << (target & 31u);
    }
}

uint EndpointError(uint4 value, uint pbit)
{
    uint4 expanded = ((value >> 1u) << 1u) | pbit;
    int4 difference = int4(expanded) - int4(value);
    return dot(difference, difference);
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
    uint4 pixels[16];
    uint4 low = 255u;
    uint4 high = 0u;
    for (uint i = 0; i < 16; ++i)
    {
        uint2 p = uint2(
            min(blockX * 4u + (i & 3u), g_tex_width - 1u),
            min(blockY * 4u + (i >> 2u), height - 1u));
        pixels[i] = uint4(saturate(g_Input.Load(uint3(p, 0))) * 255.0 + 0.5);
        low = min(low, pixels[i]);
        high = max(high, pixels[i]);
    }

    float4 axis = float4(high - low);
    float denominator = max(dot(axis, axis), 1e-10);
    uint indices[16];
    for (uint j = 0; j < 16; ++j)
    {
        float position = saturate(dot(float4(pixels[j] - low), axis) / denominator);
        uint bestIndex = 0;
        float bestError = 1e20;
        for (uint candidate = 0; candidate < 16; ++candidate)
        {
            float difference = position - WEIGHTS[candidate] / 64.0;
            float error = difference * difference;
            if (error < bestError)
            {
                bestError = error;
                bestIndex = candidate;
            }
        }
        indices[j] = bestIndex;
    }

    if (indices[0] >= 8u)
    {
        uint4 swap = low;
        low = high;
        high = swap;
        for (uint k = 0; k < 16; ++k)
            indices[k] = 15u - indices[k];
    }

    uint lowPbit = EndpointError(low, 0u) <= EndpointError(low, 1u) ? 0u : 1u;
    uint highPbit = EndpointError(high, 0u) <= EndpointError(high, 1u) ? 0u : 1u;
    uint4 lowEndpoint = low >> 1u;
    uint4 highEndpoint = high >> 1u;

    uint4 result = 0u;
    SetBits(result, 0u, 7u, 0x40u);
    uint offset = 7u;
    for (uint channel = 0; channel < 4; ++channel)
    {
        SetBits(result, offset, 7u, lowEndpoint[channel]);
        offset += 7u;
        SetBits(result, offset, 7u, highEndpoint[channel]);
        offset += 7u;
    }
    SetBits(result, offset++, 1u, lowPbit);
    SetBits(result, offset++, 1u, highPbit);
    SetBits(result, offset, 3u, indices[0]);
    offset += 3u;
    for (uint n = 1; n < 16; ++n)
    {
        SetBits(result, offset, 4u, indices[n]);
        offset += 4u;
    }
    g_OutBuff[blockID] = result;
}
