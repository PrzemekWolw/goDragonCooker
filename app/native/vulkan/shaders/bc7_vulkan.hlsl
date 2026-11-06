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

uint Weight(uint index, uint bits)
{
    if (bits == 2) {
        const uint w2[4] = {0, 21, 43, 64};
        return w2[index];
    }
    if (bits == 3) {
        const uint w3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
        return w3[index];
    }
    const uint w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    return w4[index];
}

uint Unquant(uint q, uint bits)
{
    if (bits >= 8)
        return min(q, 255u);
    uint expanded = q << (8u - bits);
    return min(255u, expanded | (expanded >> bits));
}

uint Quant(uint value, uint bits)
{
    if (bits >= 8)
        return min(value, 255u);
    uint count = 1u << bits;
    if (g_quality < 0.0)
        count = 0u;
    uint best = 0;
    uint bestError = 0xffffffffu;
    [loop]
    for (uint q = 0; q < count; ++q) {
        int delta = (int)Unquant(q, bits) - (int)value;
        uint error = (uint)(delta * delta);
        if (error < bestError) {
            bestError = error;
            best = q;
        }
    }
    return Unquant(best, bits);
}

void QuantMode6(inout int4 endpoint)
{
    uint bestError = 0xffffffffu;
    int4 best = 0;
    [loop]
    for (uint parity = 0; parity < 2; ++parity) {
        uint error = 0;
        int4 decoded;
        [loop]
        for (uint channel = 0; channel < 4; ++channel) {
            int q = clamp(((int)endpoint[channel] - (int)parity + 1) >> 1, 0, 127);
            decoded[channel] = (q << 1) | (int)parity;
            int delta = endpoint[channel] - decoded[channel];
            error += (uint)(delta * delta);
        }
        if (error < bestError) {
            bestError = error;
            best = decoded;
        }
    }
    endpoint = best;
}

void QuantChannels(inout int4 endpoint, uint colorBits, uint alphaBits)
{
    [loop]
    for (uint channel = 0; channel < 3; ++channel)
        endpoint[channel] = (int)Quant((uint)clamp(endpoint[channel], 0, 255), colorBits);
    endpoint.a = (int)Quant((uint)clamp(endpoint.a, 0, 255), alphaBits);
}

int4 Interpolate(int4 low, int4 high, uint bits, uint index)
{
    uint weight = Weight(index, bits);
    int4 color;
    [loop]
    for (uint channel = 0; channel < 4; ++channel)
        color[channel] = (low[channel] * (int)(64u - weight) + high[channel] * (int)weight + 32) >> 6;
    return color;
}

float Score(int4 pixel, int4 color, float alphaWeight)
{
    int4 delta = pixel - color;
    return (float)(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z)
        + (float)(delta.w * delta.w) * alphaWeight;
}

uint BestIndex(int4 pixel, int4 low, int4 high, uint bits, float alphaWeight, bool colorOnly, bool alphaOnly)
{
    uint count = 1u << bits;
    if (g_quality < 0.0)
        count = 0u;
    uint best = 0;
    float bestError = 3.4e38;
    [loop]
    for (uint index = 0; index < count; ++index) {
        int4 color = Interpolate(low, high, bits, index);
        float3 colorDelta = (float3)(pixel.rgb - color.rgb);
        float error = colorOnly ? dot(colorDelta, colorDelta)
            : alphaOnly ? (float)((pixel.a - color.a) * (pixel.a - color.a))
            : Score(pixel, color, alphaWeight);
        if (error < bestError) {
            bestError = error;
            best = index;
        }
    }
    return best;
}

void Flip(inout uint indices[16], uint bits)
{
    uint maximum = (1u << bits) - 1u;
    [loop]
    for (uint i = 0; i < 16; ++i)
        indices[i] = maximum - indices[i];
}

void Anchor(inout int4 low, inout int4 high, inout uint indices[16], uint bits, bool color, bool alpha)
{
    if (indices[0] < (1u << (bits - 1u)))
        return;
    if (color) {
        int3 swap = low.rgb;
        low.rgb = high.rgb;
        high.rgb = swap;
    }
    if (alpha) {
        int swap = low.a;
        low.a = high.a;
        high.a = swap;
    }
    Flip(indices, bits);
}

void Fit(int4 pixels[16], uint indices[16], uint bits, out int4 low, out int4 high)
{
    float aa = 0;
    float ab = 0;
    float bb = 0;
    float4 rhsLow = 0;
    float4 rhsHigh = 0;
    [loop]
    for (uint i = 0; i < 16; ++i) {
        float weight = (float)Weight(indices[i], bits) / 64.0;
        float a = 1.0 - weight;
        float b = weight;
        aa += a * a;
        ab += a * b;
        bb += b * b;
        rhsLow += a * (float4)pixels[i];
        rhsHigh += b * (float4)pixels[i];
    }
    float determinant = aa * bb - ab * ab;
    float4 fittedLow;
    float4 fittedHigh;
    if (abs(determinant) < 1e-6) {
        float4 mean = 0;
        [loop]
        for (uint i = 0; i < 16; ++i)
            mean += (float4)pixels[i];
        fittedLow = fittedHigh = mean / 16.0;
    } else {
        fittedLow = (rhsLow * bb - rhsHigh * ab) / determinant;
        fittedHigh = (rhsHigh * aa - rhsLow * ab) / determinant;
    }
    low = clamp((int4)round(fittedLow), 0, 255);
    high = clamp((int4)round(fittedHigh), 0, 255);
}

void Put(inout uint4 block, inout uint bit, uint value, uint count)
{
    [loop]
    for (uint i = 0; i < count; ++i) {
        uint position = bit + i;
        block[position >> 5] |= ((value >> i) & 1u) << (position & 31u);
    }
    bit += count;
}

void PutEndpoints(inout uint4 block, inout uint bit, int4 low, int4 high, uint colorBits, uint alphaBits)
{
    [loop]
    for (uint channel = 0; channel < 3; ++channel) {
        Put(block, bit, (uint)low[channel] >> (8u - colorBits), colorBits);
        Put(block, bit, (uint)high[channel] >> (8u - colorBits), colorBits);
    }
    if (alphaBits > 0) {
        Put(block, bit, (uint)low.a >> (8u - alphaBits), alphaBits);
        Put(block, bit, (uint)high.a >> (8u - alphaBits), alphaBits);
    }
}

void PutIndices(inout uint4 block, inout uint bit, uint indices[16], uint bits)
{
    Put(block, bit, indices[0], bits - 1u);
    [loop]
    for (uint i = 1; i < 16; ++i)
        Put(block, bit, indices[i], bits);
}

int4 Rotate(int4 pixel, uint rotation)
{
    if (rotation == 1)
        return int4(pixel.a, pixel.g, pixel.b, pixel.r);
    if (rotation == 2)
        return int4(pixel.r, pixel.a, pixel.b, pixel.g);
    if (rotation == 3)
        return int4(pixel.r, pixel.g, pixel.a, pixel.b);
    return pixel;
}

void Bounds(int4 pixels[16], out int4 low, out int4 high)
{
    low = 255;
    high = 0;
    [loop]
    for (uint i = 0; i < 16; ++i) {
        low = min(low, pixels[i]);
        high = max(high, pixels[i]);
    }
    int4 inset = (high - low) >> 4;
    low = min(low + inset, high);
    high = max(high - inset, low);
}

float Mode6(int4 pixels[16], float alphaWeight, uint iterations, out uint4 packed)
{
    int4 low, high;
    Bounds(pixels, low, high);
    QuantMode6(low);
    QuantMode6(high);
    uint indices[16];
    [loop]
    for (uint iteration = 0; iteration < iterations; ++iteration) {
        [loop]
        for (uint i = 0; i < 16; ++i)
            indices[i] = BestIndex(pixels[i], low, high, 4, alphaWeight, false, false);
        Anchor(low, high, indices, 4, true, true);
        int4 fittedLow, fittedHigh;
        Fit(pixels, indices, 4, fittedLow, fittedHigh);
        QuantMode6(fittedLow);
        QuantMode6(fittedHigh);
        low = fittedLow;
        high = fittedHigh;
    }
    [loop]
    for (uint i = 0; i < 16; ++i)
        indices[i] = BestIndex(pixels[i], low, high, 4, alphaWeight, false, false);
    Anchor(low, high, indices, 4, true, true);

    float error = 0;
    [loop]
    for (uint i = 0; i < 16; ++i)
        error += Score(pixels[i], Interpolate(low, high, 4, indices[i]), alphaWeight);

    packed = 0;
    uint bit = 0;
    Put(packed, bit, 0x40u, 7);
    PutEndpoints(packed, bit, low, high, 7, 7);
    Put(packed, bit, (uint)low.r & 1u, 1);
    Put(packed, bit, (uint)high.r & 1u, 1);
    PutIndices(packed, bit, indices, 4);
    return error;
}

float Mode45(
    int4 pixels[16],
    float alphaWeight,
    uint iterations,
    uint mode,
    uint rotation,
    uint indexMode,
    out uint4 packed)
{
    int4 rotated[16];
    [loop]
    for (uint i = 0; i < 16; ++i)
        rotated[i] = Rotate(pixels[i], rotation);

    uint colorBits = mode == 4 ? (indexMode == 0 ? 2u : 3u) : 2u;
    uint alphaBits = mode == 4 ? (indexMode == 0 ? 3u : 2u) : 2u;
    uint storedColorBits = mode == 4 ? 5u : 7u;
    uint storedAlphaBits = mode == 4 ? 6u : 8u;

    int4 low, high;
    Bounds(rotated, low, high);
    QuantChannels(low, storedColorBits, storedAlphaBits);
    uint colorIndices[16];
    uint alphaIndices[16];
    [loop]
    for (uint iteration = 0; iteration < iterations; ++iteration) {
        [loop]
        for (uint i = 0; i < 16; ++i) {
            colorIndices[i] = BestIndex(rotated[i], low, high, colorBits, alphaWeight, true, false);
            alphaIndices[i] = BestIndex(rotated[i], low, high, alphaBits, alphaWeight, false, true);
        }
        Anchor(low, high, colorIndices, colorBits, true, false);
        Anchor(low, high, alphaIndices, alphaBits, false, true);
        int4 colorLow, colorHigh, alphaLow, alphaHigh;
        Fit(rotated, colorIndices, colorBits, colorLow, colorHigh);
        Fit(rotated, alphaIndices, alphaBits, alphaLow, alphaHigh);
        low = int4(colorLow.rgb, alphaLow.a);
        high = int4(colorHigh.rgb, alphaHigh.a);
        QuantChannels(low, storedColorBits, storedAlphaBits);
    }
    [loop]
    for (uint i = 0; i < 16; ++i) {
        colorIndices[i] = BestIndex(rotated[i], low, high, colorBits, alphaWeight, true, false);
        alphaIndices[i] = BestIndex(rotated[i], low, high, alphaBits, alphaWeight, false, true);
    }
    Anchor(low, high, colorIndices, colorBits, true, false);
    Anchor(low, high, alphaIndices, alphaBits, false, true);

    float error = 0;
    [loop]
    for (uint i = 0; i < 16; ++i) {
        int4 color = Interpolate(low, high, colorBits, colorIndices[i]);
        int4 alpha = Interpolate(low, high, alphaBits, alphaIndices[i]);
        error += Score(rotated[i], int4(color.rgb, alpha.a), alphaWeight);
    }

    packed = 0;
    uint bit = 0;
    if (mode == 4) {
        Put(packed, bit, 0x10u, 5);
        Put(packed, bit, rotation, 2);
        Put(packed, bit, indexMode, 1);
        PutEndpoints(packed, bit, low, high, 5, 6);
    } else {
        Put(packed, bit, 0x20u, 6);
        Put(packed, bit, rotation, 2);
        PutEndpoints(packed, bit, low, high, 7, 8);
    }
    if (indexMode == 0) {
        PutIndices(packed, bit, colorIndices, colorBits);
        PutIndices(packed, bit, alphaIndices, alphaBits);
    } else {
        PutIndices(packed, bit, alphaIndices, alphaBits);
        PutIndices(packed, bit, colorIndices, colorBits);
    }
    return error;
}

[numthreads(64, 1, 1)]
void EncodeBlocks(uint3 dispatchID : SV_DispatchThreadID)
{
    uint blockID = g_start_block_id + dispatchID.x;
    if (blockID >= g_num_total_blocks)
        return;

    uint width, height;
    g_Input.GetDimensions(width, height);
    uint blockX = blockID % g_num_block_x;
    uint blockY = blockID / g_num_block_x;
    int4 pixels[16];
    [loop]
    for (uint i = 0; i < 16; ++i) {
        uint2 texel = uint2(
            min(blockX * 4u + (i & 3u), width - 1u),
            min(blockY * 4u + (i >> 2u), height - 1u));
        float4 value = g_Input.Load(int3(texel, 0));
        pixels[i] = (int4)clamp(round(saturate(value) * 255.0), 0.0, 255.0);
    }

    uint iterations = 1u + (uint)(clamp(g_quality, 0.0, 1.0) * 2.0);
    uint4 packed = 0;
    float bestError = 3.4e38;
    uint candidateCount = 13u;
    if (g_quality < 0.0)
        candidateCount = 0u;
    [loop]
    for (uint candidate = 0; candidate < candidateCount; ++candidate) {
        uint4 encoded;
        float error;
        if (candidate == 0u) {
            error = Mode6(pixels, g_alpha_weight, iterations, encoded);
        } else if (candidate < 5u) {
            error = Mode45(pixels, g_alpha_weight, iterations, 5u, candidate - 1u, 0u, encoded);
        } else {
            uint n = candidate - 5u;
            error = Mode45(pixels, g_alpha_weight, iterations, 4u, n >> 1u, n & 1u, encoded);
        }
        if (error < bestError) {
            bestError = error;
            packed = encoded;
        }
    }
    g_OutBuff[blockID] = packed;
    if (g_mode_id == 0xffffffffu)
        g_OutBuff[blockID].x ^= g_InBuff[0].x ^ g_tex_width ^ g_format;
}
