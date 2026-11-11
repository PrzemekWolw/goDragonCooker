#include "texture/cook_validate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

#include "core/platform.h"
#include "core/strings.h"
#include "texture/imageheader.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR
#include "stb_image.h"

#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

namespace {

constexpr double kPsnrCap = 99;

struct DxgiInfo {
    uint32_t id;
    const char* name;
    int blockBytes;
};

constexpr DxgiInfo kFormats[] = {
    {71, "BC1_UNORM", 8},  {72, "BC1_UNORM_SRGB", 8},  {74, "BC2_UNORM", 16}, {75, "BC2_UNORM_SRGB", 16},
    {77, "BC3_UNORM", 16}, {78, "BC3_UNORM_SRGB", 16}, {80, "BC4_UNORM", 8},  {81, "BC4_SNORM", 8},
    {83, "BC5_UNORM", 16}, {84, "BC5_SNORM", 16},      {95, "BC6H_UF16", 16}, {96, "BC6H_SF16", 16},
    {98, "BC7_UNORM", 16}, {99, "BC7_UNORM_SRGB", 16},
};

const DxgiInfo* findDxgi(uint32_t id)
{
    for (const DxgiInfo& f : kFormats) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

uint32_t rd32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

constexpr uint32_t fourCC(const char (&s)[5])
{
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

uint32_t legacyDxgi(uint32_t cc)
{
    if (cc == fourCC("DXT1")) return 71;
    if (cc == fourCC("DXT2") || cc == fourCC("DXT3")) return 74;
    if (cc == fourCC("DXT4") || cc == fourCC("DXT5")) return 77;
    if (cc == fourCC("ATI1") || cc == fourCC("BC4U")) return 80;
    if (cc == fourCC("BC4S")) return 81;
    if (cc == fourCC("ATI2") || cc == fourCC("BC5U")) return 83;
    if (cc == fourCC("BC5S")) return 84;
    return 0;
}

struct Image {
    int w = 0, h = 0, ch = 0;
    std::vector<uint8_t> px;
    uint8_t& at(int x, int y, int c) { return px[(size_t(y) * w + x) * ch + c]; }
    uint8_t at(int x, int y, int c) const { return px[(size_t(y) * w + x) * ch + c]; }
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
    out.resize(size_t(size));
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(out.data()), size));
}

bool isPow2(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

int fullChain(int w, int h)
{
    int n = 1;
    while (w > 1 || h > 1) {
        w = std::max(1, w >> 1);
        h = std::max(1, h >> 1);
        ++n;
    }
    return n;
}

bool decodeTop(const uint8_t* data, size_t size, const DxgiInfo& f, int w, int h, Image& out, std::string& error)
{
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (size < size_t(bw) * bh * f.blockBytes) {
        error = "DDS data is truncated";
        return false;
    }
    int ch = 4;
    switch (f.id) {
    case 71: case 72: case 74: case 75: case 77: case 78: case 98: case 99: break;
    case 80: ch = 1; break;
    case 83: ch = 2; break;
    default: error = format("pixels not compared (%s decode not supported)", f.name); return false;
    }
    const int pw = bw * 4;
    std::vector<uint8_t> buf(size_t(pw) * bh * 4 * ch);
    const int pitch = pw * ch;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* block = data + (size_t(by) * bw + bx) * f.blockBytes;
            uint8_t* dst = &buf[(size_t(by) * 4 * pw + size_t(bx) * 4) * ch];
            switch (f.id) {
            case 71: case 72: bcdec_bc1(block, dst, pitch); break;
            case 74: case 75: bcdec_bc2(block, dst, pitch); break;
            case 77: case 78: bcdec_bc3(block, dst, pitch); break;
            case 80: bcdec_bc4(block, dst, pitch); break;
            case 83: bcdec_bc5(block, dst, pitch); break;
            default: bcdec_bc7(block, dst, pitch); break;
            }
        }
    }
    out.w = w;
    out.h = h;
    out.ch = ch;
    out.px.resize(size_t(w) * h * ch);
    for (int y = 0; y < h; ++y) {
        std::memcpy(&out.px[size_t(y) * w * ch], &buf[size_t(y) * pitch], size_t(w) * ch);
    }
    return true;
}

bool loadSource(const std::string& path, Image& img, std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) {
        error = "cannot read source";
        return false;
    }
    int w = 0, h = 0, n = 0;
    const int len = int(std::min<size_t>(bytes.size(), INT32_MAX));
    if (imageFormat(path) == "hdr") {
        float* f = stbi_loadf_from_memory(bytes.data(), len, &w, &h, &n, 4);
        if (!f) {
            error = std::string("cannot decode source: ") + stbi_failure_reason();
            return false;
        }
        img.px.resize(size_t(w) * h * 4);
        for (size_t i = 0; i < img.px.size(); ++i) {
            img.px[i] = uint8_t(std::clamp(f[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        stbi_image_free(f);
    } else {
        uint8_t* p = stbi_load_from_memory(bytes.data(), len, &w, &h, &n, 4);
        if (!p) {
            error = std::string("cannot decode source: ") + stbi_failure_reason();
            return false;
        }
        img.px.assign(p, p + size_t(w) * h * 4);
        stbi_image_free(p);
    }
    img.w = w;
    img.h = h;
    img.ch = 4;
    return true;
}

struct Tap {
    int index;
    float weight;
};

std::vector<std::vector<Tap>> resampleTaps(int nIn, int nOut)
{
    std::vector<std::vector<Tap>> taps(nOut);
    const double scale = double(nIn) / nOut;
    for (int o = 0; o < nOut; ++o) {
        if (nOut <= nIn) {
            const double a = o * scale, b = a + scale;
            for (int i = int(std::floor(a)); i < int(std::ceil(b)) && i < nIn; ++i) {
                const double w = std::min(b, double(i + 1)) - std::max(a, double(i));
                if (w > 0) {
                    taps[o].push_back({i, float(w / scale)});
                }
            }
        } else {
            const double c = (o + 0.5) * scale - 0.5;
            const int i0 = int(std::floor(c));
            const float f = float(c - i0);
            taps[o].push_back({std::clamp(i0, 0, nIn - 1), 1 - f});
            taps[o].push_back({std::clamp(i0 + 1, 0, nIn - 1), f});
        }
    }
    return taps;
}

Image resample(const Image& src, int dw, int dh)
{
    const auto tx = resampleTaps(src.w, dw), ty = resampleTaps(src.h, dh);
    std::vector<float> rows(size_t(dw) * src.h * src.ch);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < dw; ++x) {
            for (int c = 0; c < src.ch; ++c) {
                float sum = 0;
                for (const Tap& t : tx[x]) {
                    sum += t.weight * src.at(t.index, y, c);
                }
                rows[(size_t(y) * dw + x) * src.ch + c] = sum;
            }
        }
    }
    Image out{dw, dh, src.ch, std::vector<uint8_t>(size_t(dw) * dh * src.ch)};
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            for (int c = 0; c < src.ch; ++c) {
                float sum = 0;
                for (const Tap& t : ty[y]) {
                    sum += t.weight * rows[(size_t(t.index) * dw + x) * src.ch + c];
                }
                out.at(x, y, c) = uint8_t(std::clamp(sum + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return out;
}

void linearToSRGB(Image& img)
{
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) {
        const double v = i / 255.0;
        const double s = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
        lut[i] = uint8_t(std::clamp(s * 255 + 0.5, 0.0, 255.0));
    }
    for (size_t i = 0; i < img.px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            img.px[i + c] = lut[img.px[i + c]];
        }
    }
}

double psnr(double mse)
{
    return mse <= 0 ? kPsnrCap : std::min(kPsnrCap, 10 * std::log10(255.0 * 255.0 / mse));
}

// Reduces the 4-channel source to the channels the cooked format stores.
Image referenceFor(const Image& src, int ch, bool luma)
{
    Image out{src.w, src.h, ch, std::vector<uint8_t>(size_t(src.w) * src.h * ch)};
    for (size_t i = 0, n = size_t(src.w) * src.h; i < n; ++i) {
        const uint8_t* s = &src.px[i * 4];
        uint8_t* d = &out.px[i * ch];
        if (ch == 1) {
            d[0] = luma ? uint8_t(std::clamp(0.2125 * s[0] + 0.7154 * s[1] + 0.0721 * s[2] + 0.5, 0.0, 255.0)) : s[0];
        } else {
            std::memcpy(d, s, size_t(ch));
        }
    }
    return out;
}

double lumaAt(const Image& img, int x, int y)
{
    if (img.ch == 1) return img.at(x, y, 0);
    if (img.ch == 2) return 0.5 * (img.at(x, y, 0) + img.at(x, y, 1));
    return 0.299 * img.at(x, y, 0) + 0.587 * img.at(x, y, 1) + 0.114 * img.at(x, y, 2);
}

double lumaSSIM(const Image& a, const Image& b)
{
    constexpr double c1 = (0.01 * 255) * (0.01 * 255), c2 = (0.03 * 255) * (0.03 * 255);
    const int win = std::min({8, a.w, a.h});
    double total = 0;
    size_t count = 0;
    for (int y0 = 0; y0 + win <= a.h; y0 += win) {
        for (int x0 = 0; x0 + win <= a.w; x0 += win) {
            double ma = 0, mb = 0, va = 0, vb = 0, cov = 0;
            for (int y = y0; y < y0 + win; ++y) {
                for (int x = x0; x < x0 + win; ++x) {
                    const double la = lumaAt(a, x, y), lb = lumaAt(b, x, y);
                    ma += la;
                    mb += lb;
                    va += la * la;
                    vb += lb * lb;
                    cov += la * lb;
                }
            }
            const double n = double(win) * win;
            ma /= n;
            mb /= n;
            va = va / n - ma * ma;
            vb = vb / n - mb * mb;
            cov = cov / n - ma * mb;
            total += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++count;
        }
    }
    return count ? total / count : 1;
}

struct Metrics {
    std::vector<double> channelPsnr;
    double colorPsnr = kPsnrCap, alphaPsnr = kPsnrCap;
    int maxError = 0;
    size_t blocks = 0, badBlocks = 0;
    double worstBlockPsnr = kPsnrCap;
    int worstX = 0, worstY = 0;
};

Metrics compare(const Image& got, const Image& ref, bool relaxed)
{
    Metrics m;
    const int ch = got.ch;
    std::vector<double> sse(ch, 0);
    for (size_t i = 0; i < got.px.size(); ++i) {
        const int d = std::abs(int(got.px[i]) - int(ref.px[i]));
        sse[i % ch] += double(d) * d;
        m.maxError = std::max(m.maxError, d);
    }
    const double pixels = double(got.w) * got.h;
    const int colorChannels = ch == 4 ? 3 : ch;
    double colorSse = 0;
    for (int c = 0; c < ch; ++c) {
        m.channelPsnr.push_back(psnr(sse[c] / pixels));
        if (c < colorChannels) {
            colorSse += sse[c];
        }
    }
    m.colorPsnr = psnr(colorSse / (pixels * colorChannels));
    if (ch == 4) {
        m.alphaPsnr = m.channelPsnr[3];
    }

    // A codec bug tends to leave whole blocks black, flat or garbled, which the image PSNR averages away.
    const int badError = relaxed ? 192 : 128;
    const double badPsnr = relaxed ? 10 : 15;
    for (int by = 0; by < got.h; by += 4) {
        for (int bx = 0; bx < got.w; bx += 4) {
            double blockSse = 0;
            int blockMax = 0, n = 0;
            for (int y = by; y < std::min(by + 4, got.h); ++y) {
                for (int x = bx; x < std::min(bx + 4, got.w); ++x) {
                    for (int c = 0; c < ch; ++c) {
                        const int d = std::abs(int(got.at(x, y, c)) - int(ref.at(x, y, c)));
                        blockSse += double(d) * d;
                        blockMax = std::max(blockMax, d);
                        ++n;
                    }
                }
            }
            const double p = psnr(blockSse / n);
            ++m.blocks;
            if (blockMax > badError || p < badPsnr) {
                ++m.badBlocks;
            }
            if (p < m.worstBlockPsnr) {
                m.worstBlockPsnr = p;
                m.worstX = bx;
                m.worstY = by;
            }
        }
    }
    return m;
}

size_t normalOverflow(const Image& got, const Image& ref)
{
    auto overflows = [](const Image& img, int x, int y) {
        const double nx = img.at(x, y, 0) / 127.5 - 1, ny = img.at(x, y, 1) / 127.5 - 1;
        return nx * nx + ny * ny > 1.04;
    };
    size_t count = 0;
    for (int y = 0; y < got.h; ++y) {
        for (int x = 0; x < got.w; ++x) {
            count += overflows(got, x, y) && !overflows(ref, x, y);
        }
    }
    return count;
}

} // namespace

const char* cookVerdictName(CookVerdict verdict)
{
    switch (verdict) {
    case CookVerdict::Ok: return "ok";
    case CookVerdict::HeaderOnly: return "header only";
    case CookVerdict::Warn: return "quality warning";
    default: return "quality failure";
    }
}

std::string ddsPathFor(const CookJob& job)
{
    const fs::path src = u8path(job.srcFile);
    const fs::path dir = job.outputDir.empty() ? src.parent_path() : u8path(job.outputDir);
    return u8str(dir / (u8str(src.stem()) + ".dds"));
}

CookQuality validateCook(const CookJob& job, const std::string& ddsPath, const CookThresholds& thresholds)
{
    CookQuality q;
    if (hasPrefix(job.format, "BC5_")) {
        q.target = thresholds.bc5;
    } else if (hasPrefix(job.format, "BC4_")) {
        q.target = thresholds.bc4;
    } else {
        q.target = thresholds.bc7;
    }
    q.floor = q.target - thresholds.floorMargin;
    q.verdict = CookVerdict::Fail;
    q.lowQuality = true;

    std::vector<uint8_t> dds;
    if (!readFile(ddsPath, dds)) {
        q.error = "cannot read the DDS";
        return q;
    }
    if (dds.size() < 128 || rd32(dds.data()) != fourCC("DDS ") || rd32(&dds[4]) != 124) {
        q.error = "not a DDS file";
        return q;
    }
    q.height = int(rd32(&dds[12]));
    q.width = int(rd32(&dds[16]));
    q.mips = std::max(1, int(rd32(&dds[28])));
    const uint32_t cc = rd32(&dds[84]);
    size_t offset = 128;
    uint32_t dxgi = 0;
    if (cc == fourCC("DX10")) {
        if (dds.size() < 148) {
            q.error = "DDS DX10 header is truncated";
            return q;
        }
        dxgi = rd32(&dds[128]);
        offset = 148;
    } else {
        dxgi = legacyDxgi(cc);
    }
    const DxgiInfo* fmt = findDxgi(dxgi);
    if (!fmt) {
        q.error = format("unsupported DDS format (DXGI %u)", dxgi);
        return q;
    }
    q.format = fmt->name;
    if (q.format != job.format) {
        q.error = format("format %s, expected %s", fmt->name, job.format.c_str());
        return q;
    }
    if (q.width <= 0 || q.height <= 0) {
        q.error = "DDS has no size";
        return q;
    }
    if (!isPow2(q.width) || !isPow2(q.height)) {
        q.issues.push_back(format("size %dx%d is not a power of two", q.width, q.height));
    }
    if (q.mips != fullChain(q.width, q.height)) {
        q.issues.push_back(format("%d mips, expected %d", q.mips, fullChain(q.width, q.height)));
    }

    const std::string srcFormat = imageFormat(job.srcFile);
    if (srcFormat == "exr" || srcFormat == "tiff") {
        q.lowQuality = false;
        q.verdict = q.issues.empty() ? CookVerdict::HeaderOnly : CookVerdict::Warn;
        q.issues.push_back(format("pixels not compared (%s source)", srcFormat.c_str()));
        return q;
    }

    Image got;
    std::string error;
    if (!decodeTop(dds.data() + offset, dds.size() - offset, *fmt, q.width, q.height, got, error)) {
        if (hasPrefix(error, "pixels not compared")) {
            q.lowQuality = false;
            q.verdict = q.issues.empty() ? CookVerdict::HeaderOnly : CookVerdict::Warn;
            q.issues.push_back(error);
        } else {
            q.error = error;
        }
        return q;
    }
    Image src;
    if (!loadSource(job.srcFile, src, error)) {
        q.lowQuality = false;
        q.verdict = CookVerdict::Warn;
        q.issues.push_back(error);
        return q;
    }
    q.srcWidth = src.w;
    q.srcHeight = src.h;
    if (job.srgb && !job.inputSRGB) {
        linearToSRGB(src);
    }
    if (src.w != q.width || src.h != q.height) {
        q.resized = true;
        q.target -= 3;
        q.floor -= 3;
        src = resample(src, q.width, q.height);
    }

    Metrics m;
    Image ref;
    if (got.ch == 1) {
        Image red = referenceFor(src, 1, false), luma = referenceFor(src, 1, true);
        const Metrics mRed = compare(got, red, q.resized), mLuma = compare(got, luma, q.resized);
        const bool useLuma = mLuma.colorPsnr > mRed.colorPsnr;
        if (red.px != luma.px) {
            q.reference = useLuma ? "luma" : "red";
        }
        m = useLuma ? mLuma : mRed;
        ref = useLuma ? std::move(luma) : std::move(red);
    } else {
        ref = referenceFor(src, got.ch, false);
        m = compare(got, ref, q.resized);
    }

    q.compared = true;
    q.channelPsnr = m.channelPsnr;
    q.alphaPsnr = m.alphaPsnr;
    q.psnr = std::min(m.colorPsnr, m.alphaPsnr);
    q.maxError = m.maxError;
    q.blocks = m.blocks;
    q.badBlocks = m.badBlocks;
    q.worstBlockPsnr = m.worstBlockPsnr;
    q.worstBlockX = m.worstX;
    q.worstBlockY = m.worstY;
    q.ssim = lumaSSIM(got, ref);
    if (got.ch == 2) {
        q.normalOverflow = normalOverflow(got, ref);
        if (q.normalOverflow * 1000 > size_t(got.w) * got.h) {
            q.issues.push_back(format("%zu texels decode to a normal longer than 1", q.normalOverflow));
        }
    }

    q.lowQuality = q.psnr < q.target || q.badBlocks > 0;
    if (q.psnr < q.floor || q.badBlocks > 0) {
        q.verdict = CookVerdict::Fail;
    } else if (q.psnr < q.target || !q.issues.empty()) {
        q.verdict = CookVerdict::Warn;
    } else {
        q.verdict = CookVerdict::Ok;
    }
    return q;
}

std::string describeQuality(const CookQuality& q)
{
    std::string text;
    if (!q.error.empty()) {
        text = q.error;
    } else if (!q.compared) {
        text = format("header ok (%s %dx%d, %d mips)", q.format.c_str(), q.width, q.height, q.mips);
    } else {
        text = format("PSNR %.1f dB (target %.0f)", q.psnr, q.target);
        if (q.channelPsnr.size() == 4 && q.alphaPsnr < kPsnrCap) {
            text += format(", alpha %.1f dB", q.alphaPsnr);
        }
        text += format(", max err %d, SSIM %.4f, worst block %.1f dB at (%d,%d)", q.maxError, q.ssim,
                       q.worstBlockPsnr, q.worstBlockX, q.worstBlockY);
        if (q.badBlocks) {
            text += format(", %zu/%zu bad blocks", q.badBlocks, q.blocks);
        }
        if (!q.reference.empty()) {
            text += ", vs " + q.reference;
        }
        if (q.resized) {
            text += format(", resized %dx%d -> %dx%d", q.srcWidth, q.srcHeight, q.width, q.height);
        }
    }
    for (const std::string& issue : q.issues) {
        text += "; " + issue;
    }
    return text;
}
