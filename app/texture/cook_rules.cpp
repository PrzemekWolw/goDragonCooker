#include "texture/cook_rules.h"

#include "core/strings.h"

std::string textureRole(const std::string& name)
{
    const std::string lower = toLower(name);
    if (imageFormat(lower).empty()) {
        return "";
    }
    const std::string ext = u8str(u8path(lower).extension());
    for (const char* role : {"color", "data", "normal"}) {
        if (hasSuffix(lower, "." + std::string(role) + ext)) {
            return role;
        }
    }
    return "";
}

bool isFloatHDR(const std::string& name)
{
    const std::string lower = toLower(name);
    return hasSuffix(lower, ".exr") || hasSuffix(lower, ".hdr");
}

bool isExpensiveCodec(const std::string& format)
{
    return hasPrefix(format, "BC6H_") || hasPrefix(format, "BC7_");
}

std::string resolveDataFormat(const ImgInfo& info, bool autoDetect)
{
    if (autoDetect || info.colorType == ColorType::Grayscale || info.colorType == ColorType::GrayAlpha) {
        return "BC4_UNORM";
    }
    return "BC7_UNORM";
}

const char* colorSpaceName(ColorSpace space)
{
    switch (space) {
    case ColorSpace::Linear: return "linear";
    case ColorSpace::SRGB: return "sRGB";
    default: return "unknown";
    }
}

bool makeCookJob(const std::string& srcFile, const std::string& role, const ImgInfo& img, bool autoDetectFmt,
                 CookJob& job)
{
    job = CookJob{};
    job.srcFile = srcFile;
    if (role == "color") {
        job.format = "BC7_UNORM_SRGB";
        job.srgb = job.sepa = true;
        job.inputSRGB = img.colorSpace != ColorSpace::Linear;
    } else if (role == "data") {
        job.format = isFloatHDR(srcFile) ? "BC7_UNORM" : resolveDataFormat(img, autoDetectFmt);
        job.ignoreSRGB = true;
    } else if (role == "normal") {
        job.format = "BC5_UNORM";
        job.ignoreSRGB = true;
    } else {
        return false;
    }
    return true;
}

std::vector<std::string> cookArgs(const CookJob& job, const std::vector<std::string>& gpuArgs,
                                  const std::string& outDir)
{
    std::vector<std::string> args = {"-y", "-pow2"};
    if (job.sepa) args.push_back("-sepalpha");
    if (job.ignoreSRGB) args.push_back("--ignore-srgb");
    if (job.inputSRGB) args.push_back("-srgbi");
    if (job.srgb) args.push_back("-srgbo");
    if (hasPrefix(job.format, "BC7_")) {
        args.insert(args.end(), {"-bc", "x"});
    }
    if (isExpensiveCodec(job.format)) {
        args.insert(args.end(), gpuArgs.begin(), gpuArgs.end());
    }
    const std::string dir = outDir.empty() ? u8str(u8path(job.srcFile).parent_path()) : outDir;
    args.insert(args.end(), {"-o", dir, "-f", job.format, "--", job.srcFile});
    return args;
}
