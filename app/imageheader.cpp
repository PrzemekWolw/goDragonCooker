#include "app.h"

#include <cstdint>
#include <cstring>
#include <fstream>

static const std::map<std::string, std::string> supportedImageExtensions = {
    {".bmp", "bmp"},   {".exr", "exr"}, {".hdr", "hdr"}, {".jpeg", "jpeg"}, {".jpg", "jpeg"},
    {".png", "png"},   {".tga", "tga"}, {".tif", "tiff"}, {".tiff", "tiff"},
};

std::string imageFormat(const std::string& name)
{
    const auto it = supportedImageExtensions.find(toLower(u8str(u8path(name).extension())));
    return it == supportedImageExtensions.end() ? "" : it->second;
}

static uint32_t bigEndian32(const uint8_t* data)
{
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}

static bool readBytes(std::ifstream& file, void* data, size_t size)
{
    return bool(file.read(static_cast<char*>(data), std::streamsize(size)));
}

static bool isJPEGStartOfFrame(uint8_t marker)
{
    return marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
}

static bool readJPEGHeader(std::ifstream& file, ImgInfo& info, std::string& error)
{
    uint8_t soi[2];
    if (!readBytes(file, soi, 2) || soi[0] != 0xff || soi[1] != 0xd8) {
        error = "invalid JPEG header";
        return false;
    }
    for (;;) {
        uint8_t marker[2];
        if (!readBytes(file, marker, 2)) {
            error = "failed to read JPEG header";
            return false;
        }
        if (marker[0] != 0xff) {
            continue;
        }
        while (marker[1] == 0xff) {
            if (!readBytes(file, &marker[1], 1)) {
                error = "failed to read JPEG marker";
                return false;
            }
        }
        if (marker[1] == 0xd8 || marker[1] == 0xd9) {
            continue;
        }
        uint8_t lengthBuf[2];
        if (!readBytes(file, lengthBuf, 2)) {
            error = "failed to read JPEG segment";
            return false;
        }
        const int length = (lengthBuf[0] << 8) | lengthBuf[1];
        if (length < 2) {
            error = "invalid JPEG segment length";
            return false;
        }
        if (isJPEGStartOfFrame(marker[1])) {
            uint8_t header[6];
            if (!readBytes(file, header, 6)) {
                error = "failed to read JPEG frame";
                return false;
            }
            info = {"jpeg", header[0], header[5] == 1 ? ColorType::Grayscale : ColorType::RGB, ColorSpace::SRGB};
            return true;
        }
        if (!file.seekg(length - 2, std::ios::cur)) {
            error = "failed to skip JPEG segment";
            return false;
        }
    }
}

static bool readPNGHeader(std::ifstream& file, ImgInfo& info, std::string& error)
{
    uint8_t sig[8];
    if (!readBytes(file, sig, 8)) {
        error = "bad PNG signature";
        return false;
    }
    if (std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
        error = "not a PNG file";
        return false;
    }

    info = {"png", 0, ColorType::Unknown, ColorSpace::Unknown};
    for (;;) {
        uint8_t chunkHeader[8];
        if (!readBytes(file, chunkHeader, 8)) {
            error = "unexpected end of PNG";
            return false;
        }
        const uint32_t chunkLen = bigEndian32(chunkHeader);
        const std::string chunkType(reinterpret_cast<char*>(chunkHeader + 4), 4);

        if (chunkType == "IHDR") {
            uint8_t data[13];
            if (chunkLen != 13 || !readBytes(file, data, 13)) {
                error = "IHDR too short";
                return false;
            }
            static const ColorType colorTypes[] = {
                ColorType::Grayscale, ColorType::Unknown, ColorType::RGB, ColorType::Palette,
                ColorType::GrayAlpha, ColorType::Unknown, ColorType::RGBA};
            if (data[9] > 6 || colorTypes[data[9]] == ColorType::Unknown) {
                error = format("unsupported PNG color type: %d", data[9]);
                return false;
            }
            info.bits = data[8];
            info.colorType = colorTypes[data[9]];
        } else if (chunkType == "sRGB" && chunkLen == 1) {
            uint8_t data;
            if (!readBytes(file, &data, 1)) {
                error = "unexpected end of PNG";
                return false;
            }
            info.colorSpace = ColorSpace::SRGB;
        } else if (chunkType == "gAMA" && chunkLen == 4) {
            uint8_t data[4];
            if (!readBytes(file, data, 4)) {
                error = "unexpected end of PNG";
                return false;
            }
            const uint32_t gamma = bigEndian32(data);
            if (gamma == 100000) {
                info.colorSpace = ColorSpace::Linear;
            } else if (gamma == 45455) {
                info.colorSpace = ColorSpace::SRGB;
            }
        } else if (!file.seekg(chunkLen, std::ios::cur)) {
            error = "unexpected end of PNG";
            return false;
        }

        uint8_t crc[4];
        if (!readBytes(file, crc, 4)) {
            error = "unexpected end of PNG";
            return false;
        }
        if (chunkType == "IDAT" || chunkType == "IEND") {
            break;
        }
    }
    if (info.bits == 0) {
        error = "IHDR not found";
        return false;
    }
    return true;
}

// Reads only the source header and metadata needed for format and color-space selection.
bool readImageHeader(const fs::path& path, ImgInfo& info, std::string& error)
{
    const std::string fmt = imageFormat(u8str(path));
    if (fmt.empty()) {
        error = "unsupported image format: " + u8str(path.extension());
        return false;
    }
    if (fmt == "exr" || fmt == "hdr") {
        info = {fmt, 32, ColorType::RGB, ColorSpace::Linear};
        return true;
    }
    if (fmt != "png" && fmt != "jpeg") {
        info = {fmt, 0, ColorType::Unknown, ColorSpace::Unknown};
        return true;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open file";
        return false;
    }
    return fmt == "png" ? readPNGHeader(file, info, error) : readJPEGHeader(file, info, error);
}
