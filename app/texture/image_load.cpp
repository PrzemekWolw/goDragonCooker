#include "texture/image_load.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <Iex.h>
#include <ImfIO.h>
#include <ImfRgbaFile.h>

#include "core/platform.h"
#include "texture/imageheader.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR
#include "stb_image.h"

namespace {

class MemoryIStream : public OPENEXR_IMF_NAMESPACE::IStream {
public:
    explicit MemoryIStream(const std::vector<uint8_t>& data) : OPENEXR_IMF_NAMESPACE::IStream("exr"), data_(data) {}

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
    const std::vector<uint8_t>& data_;
    uint64_t pos_ = 0;
};

uint8_t quantize(double value)
{
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<uint8_t>(std::clamp(std::lround(value * 255.0), 0l, 255l));
}

bool decodeExr(const std::vector<uint8_t>& bytes, Rgba8& image, std::string& error)
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
            image.px[i * 4] = quantize(float(rgba[i].r));
            image.px[i * 4 + 1] = quantize(float(rgba[i].g));
            image.px[i * 4 + 2] = quantize(float(rgba[i].b));
            image.px[i * 4 + 3] = 255;
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace

bool loadRgba8(const std::string& path, Rgba8& image, std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) {
        return false;
    }
    const std::string fmt = imageFormat(path);
    if (fmt == "exr") {
        return decodeExr(bytes, image, error);
    }
    const int len = static_cast<int>(std::min<size_t>(bytes.size(), INT32_MAX));
    int n = 0;
    if (fmt == "hdr") {
        float* decoded = stbi_loadf_from_memory(bytes.data(), len, &image.w, &image.h, &n, 4);
        if (!decoded) {
            error = stbi_failure_reason();
            return false;
        }
        image.px.resize(static_cast<size_t>(image.w) * image.h * 4);
        for (size_t i = 0; i < image.px.size(); ++i) {
            image.px[i] = quantize(decoded[i]);
        }
        stbi_image_free(decoded);
        return true;
    }
    uint8_t* decoded = stbi_load_from_memory(bytes.data(), len, &image.w, &image.h, &n, 4);
    if (!decoded) {
        error = stbi_failure_reason();
        return false;
    }
    image.px.assign(decoded, decoded + static_cast<size_t>(image.w) * image.h * 4);
    stbi_image_free(decoded);
    return true;
}

uint8_t srgbToLinear(uint8_t value)
{
    const double v = value / 255.0;
    return quantize(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
}

uint8_t linearToSrgb(uint8_t value)
{
    const double v = value / 255.0;
    return quantize(v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
}
