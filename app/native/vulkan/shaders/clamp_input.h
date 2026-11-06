// Encoders read whole 4x4 blocks; clamp reads to the extent of 1x1 and 2x2 mips.
uint3 ClampTexel(Texture2D<float4> input, uint3 position)
{
    uint width, height;
    input.GetDimensions(width, height);
    return uint3(min(position.x, width - 1), min(position.y, height - 1), 0);
}

#define Load(position) Load(ClampTexel(g_Input, position))
