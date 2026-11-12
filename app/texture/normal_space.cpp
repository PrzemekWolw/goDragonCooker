#include "texture/normal_space.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/platform.h"
#include "texture/image_load.h"

namespace {

constexpr int kSamples = 4000;

struct Center {
    double r = 0;
    double g = 0;
};

double medianOf(std::vector<uint8_t>& values)
{
    if (values.empty()) {
        return 0;
    }
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    return values[mid];
}

// The flat normal is the median of each axis. sRGB is monotonic, so a map centered on
// linear 0.5 stays centered on the byte that encodes 0.5: 128 linear, 188 sRGB, 55 double-linear.
Center centerOf(const Rgba8& image, int step, uint8_t (*map)(uint8_t))
{
    std::vector<uint8_t> red, green;
    for (int y = step / 2; y < image.h; y += step) {
        for (int x = step / 2; x < image.w; x += step) {
            const uint8_t* pixel = &image.px[(static_cast<size_t>(y) * image.w + x) * 4];
            red.push_back(map(pixel[0]));
            green.push_back(map(pixel[1]));
        }
    }
    return {medianOf(red), medianOf(green)};
}

uint8_t identity(uint8_t value)
{
    return value;
}

bool near(const Center& center, double target)
{
    return std::abs(center.r - target) <= 24 && std::abs(center.g - target) <= 24;
}

NormalEncoding classify(const Rgba8& image)
{
    const int span = std::min(image.w, image.h);
    const int step = std::max(1, static_cast<int>(std::sqrt(double(span) * span / kSamples)));
    const Center raw = centerOf(image, step, identity);
    if (raw.r == 0 && raw.g == 0 && image.w * image.h < 64) {
        return NormalEncoding::Unknown;
    }
    if (near(raw, 128)) {
        return NormalEncoding::Linear;
    }
    if (near(raw, 188) && near(centerOf(image, step, srgbToLinear), 128)) {
        return NormalEncoding::Srgb;
    }
    if (near(raw, 55) && near(centerOf(image, step, linearToSrgb), 128)) {
        return NormalEncoding::DoubleLinear;
    }
    return NormalEncoding::Unknown;
}

bool writeBmp(const std::string& path, const Rgba8& image, std::string& error)
{
    const int row = (image.w * 3 + 3) & ~3;
    const uint32_t pixels = static_cast<uint32_t>(row) * static_cast<uint32_t>(image.h);
    const uint32_t offset = 54;
    std::vector<uint8_t> file(offset + pixels, 0);
    auto put32 = [&](size_t index, uint32_t value) {
        file[index] = static_cast<uint8_t>(value);
        file[index + 1] = static_cast<uint8_t>(value >> 8);
        file[index + 2] = static_cast<uint8_t>(value >> 16);
        file[index + 3] = static_cast<uint8_t>(value >> 24);
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, offset + pixels);
    put32(10, offset);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(image.w));
    put32(22, static_cast<uint32_t>(image.h));
    file[26] = 1;
    file[28] = 24;
    for (int y = 0; y < image.h; ++y) {
        uint8_t* dst = file.data() + offset + static_cast<size_t>(image.h - 1 - y) * row;
        const uint8_t* src = image.px.data() + static_cast<size_t>(y) * image.w * 4;
        for (int x = 0; x < image.w; ++x) {
            dst[x * 3] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4];
        }
    }
    if (!writeFileBytes(path, file, error)) {
        error = "cannot write corrected normal map: " + path;
        return false;
    }
    return true;
}

} // namespace

const char* normalEncodingName(NormalEncoding encoding)
{
    switch (encoding) {
    case NormalEncoding::Linear: return "linear";
    case NormalEncoding::Srgb: return "sRGB";
    case NormalEncoding::DoubleLinear: return "double-linear";
    default: return "unknown";
    }
}

NormalEncoding detectNormalEncoding(const std::string& path)
{
    Rgba8 image;
    std::string error;
    return loadRgba8(path, image, error) ? classify(image) : NormalEncoding::Unknown;
}

std::string linearizeNormalMap(const std::string& path, NormalEncoding encoding, const std::string& destination,
                               std::string& error)
{
    Rgba8 image;
    if (!loadRgba8(path, image, error)) {
        error = "cannot decode normal map: " + error;
        return "";
    }
    uint8_t (*map)(uint8_t) = encoding == NormalEncoding::Srgb ? srgbToLinear : linearToSrgb;
    for (size_t i = 0, count = static_cast<size_t>(image.w) * image.h; i < count; ++i) {
        image.px[i * 4] = map(image.px[i * 4]);
        image.px[i * 4 + 1] = map(image.px[i * 4 + 1]);
    }
    return writeBmp(destination, image, error) ? destination : "";
}
