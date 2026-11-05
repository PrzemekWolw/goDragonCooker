#pragma once

#include <string>

#include "core/platform.h"

enum class ColorType { Unknown, Grayscale, RGB, Palette, GrayAlpha, RGBA };
enum class ColorSpace { Unknown, Linear, SRGB };

struct ImgInfo {
    std::string format;
    int bits = 0;
    ColorType colorType = ColorType::Unknown;
    ColorSpace colorSpace = ColorSpace::Unknown;
};

std::string imageFormat(const std::string& name);
bool readImageHeader(const fs::path& path, ImgInfo& info, std::string& error);
