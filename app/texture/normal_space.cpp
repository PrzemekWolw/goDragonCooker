#include "texture/normal_space.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include <Iex.h>
#include <ImfIO.h>
#include <ImfRgbaFile.h>

#include "core/platform.h"
#include "core/strings.h"
#include "texture/imageheader.h"

#include "stb_image.h"

namespace {

constexpr int kSamples = 4000;

struct Image8 {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;
};

class MemoryIStream : public OPENEXR_IMF_NAMESPACE::IStream {
public:
    explicit MemoryIStream(std::vector<uint8_t> data)
        : OPENEXR_IMF_NAMESPACE::IStream("exr"), data_(std::move(data))
    {
    }

    bool read(char buffer[], int n) override
    {
        if (pos_ + static_cast<uint64_t>(n) > data_.size()) {
            throw IEX_NAMESPACE::InputExc("EXR read past end of file");
        }
        std::memcpy(buffer, data_.data() + pos_, static_cast<size_t>(n));
        pos_ += static_cast<uint64_t>(n);
        return true;
    }

    uint64_t tellg() override { return pos_; }

    void seekg(uint64_t pos) override { pos_ = pos; }

private:
    std::vector<uint8_t> data_;
    uint64_t pos_ = 0;
};

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream file(u8path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(out.data()), size));
}

uint8_t quantize(float value)
{
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<uint8_t>(std::clamp(std::lround(double(value) * 255.0), 0l, 255l));
}

uint8_t srgbToLinear(uint8_t value)
{
    const double v = value / 255.0;
    const double linear = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    return static_cast<uint8_t>(std::clamp(std::lround(linear * 255.0), 0l, 255l));
}

uint8_t linearToSrgb(uint8_t value)
{
    const double v = value / 255.0;
    const double srgb = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::clamp(std::lround(srgb * 255.0), 0l, 255l));
}

bool decodeExr(const std::vector<uint8_t>& bytes, Image8& image, std::string& error)
{
    try {
        MemoryIStream stream(bytes);
        OPENEXR_IMF_NAMESPACE::RgbaInputFile file(stream);
        const IMATH_NAMESPACE::Box2i window = file.dataWindow();
        const int w = window.max.x - window.min.x + 1;
        const int h = window.max.y - window.min.y + 1;
        if (w <= 0 || h <= 0) {
            error = "EXR has invalid dimensions";
            return false;
        }
        std::vector<OPENEXR_IMF_NAMESPACE::Rgba> rgba(static_cast<size_t>(w) * h);
        file.setFrameBuffer(rgba.data() - window.min.x - static_cast<ptrdiff_t>(window.min.y) * w, 1, w);
        file.readPixels(window.min.y, window.max.y);
        image.w = w;
        image.h = h;
        image.px.resize(rgba.size() * 4);
        for (size_t i = 0; i < rgba.size(); ++i) {
            image.px[i * 4] = quantize(static_cast<float>(rgba[i].r));
            image.px[i * 4 + 1] = quantize(static_cast<float>(rgba[i].g));
            image.px[i * 4 + 2] = quantize(static_cast<float>(rgba[i].b));
            image.px[i * 4 + 3] = 255;
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool decodeBytes(const std::string& fmt, const std::vector<uint8_t>& bytes, Image8& image, std::string& error)
{
    const int len = static_cast<int>(std::min<size_t>(bytes.size(), INT32_MAX));
    int w = 0, h = 0, n = 0;
    if (fmt == "hdr") {
        float* decoded = stbi_loadf_from_memory(bytes.data(), len, &w, &h, &n, 4);
        if (!decoded) {
            error = std::string("cannot decode normal map: ") + stbi_failure_reason();
            return false;
        }
        image.w = w;
        image.h = h;
        image.px.resize(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < image.px.size(); ++i) {
            image.px[i] = quantize(decoded[i]);
        }
        stbi_image_free(decoded);
        return true;
    }
    uint8_t* decoded = stbi_load_from_memory(bytes.data(), len, &w, &h, &n, 4);
    if (!decoded) {
        error = std::string("cannot decode normal map: ") + stbi_failure_reason();
        return false;
    }
    image.w = w;
    image.h = h;
    image.px.assign(decoded, decoded + static_cast<size_t>(w) * h * 4);
    stbi_image_free(decoded);
    return true;
}

bool decodeNormalImage(const std::string& path, Image8& image, std::string& error)
{
    const std::string fmt = imageFormat(path);
    if (fmt.empty() || fmt == "tiff") {
        error = "unsupported normal map format";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) {
        error = "cannot read normal map";
        return false;
    }
    if (fmt == "exr") {
        return decodeExr(bytes, image, error);
    }
    return decodeBytes(fmt, bytes, image, error);
}

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
Center centerOf(const Image8& image, int step, uint8_t (*map)(uint8_t))
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

NormalEncoding classify(const Image8& image)
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

bool writeBmp(const std::string& path, const Image8& image, std::string& error)
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
    std::ofstream output(u8path(path), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!output) {
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
    Image8 image;
    std::string error;
    if (!decodeNormalImage(path, image, error)) {
        return NormalEncoding::Unknown;
    }
    return classify(image);
}

std::string linearizeNormalMap(const std::string& path, NormalEncoding encoding, const std::string& destination,
                               std::string& error)
{
    Image8 image;
    if (!decodeNormalImage(path, image, error)) {
        return "";
    }
    uint8_t (*map)(uint8_t) = encoding == NormalEncoding::Srgb ? srgbToLinear : linearToSrgb;
    for (size_t i = 0, count = static_cast<size_t>(image.w) * image.h; i < count; ++i) {
        image.px[i * 4] = map(image.px[i * 4]);
        image.px[i * 4 + 1] = map(image.px[i * 4 + 1]);
    }
    if (!writeBmp(destination, image, error)) {
        return "";
    }
    return destination;
}
