#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Rgba8 {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;
};

// PNG, JPEG, BMP, TGA, HDR or EXR as 8-bit RGBA. Float pixels are clamped to 0..1; EXR alpha is opaque.
bool loadRgba8(const std::string& path, Rgba8& image, std::string& error);

uint8_t srgbToLinear(uint8_t value);
uint8_t linearToSrgb(uint8_t value);
