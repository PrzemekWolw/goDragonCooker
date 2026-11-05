#pragma once

#include <string>
#include <vector>

#include "texture/vulkan_codec.h"

// Target PSNR per codec family in dB. Below target is a quality warning, below target - floorMargin a failure.
struct CookThresholds {
    double bc7 = 38;
    double bc5 = 40;
    double bc4 = 42;
    double floorMargin = 10;
};

enum class CookVerdict { Ok, HeaderOnly, Warn, Fail };
const char* cookVerdictName(CookVerdict verdict);

struct CookQuality {
    CookVerdict verdict = CookVerdict::Fail;
    std::string error;
    std::vector<std::string> issues;
    std::string format;
    int width = 0, height = 0, mips = 0;
    int srcWidth = 0, srcHeight = 0;
    bool resized = false;
    bool compared = false;
    // BC4 from a color source: "red" or "luma", whichever the codec matched.
    std::string reference;
    // Decision metric: the lowest of the color and alpha PSNR, capped at 99.
    double psnr = 0;
    double alphaPsnr = 0;
    std::vector<double> channelPsnr;
    int maxError = 0;
    double ssim = 1;
    size_t blocks = 0, badBlocks = 0;
    double worstBlockPsnr = 0;
    int worstBlockX = 0, worstBlockY = 0;
    size_t normalOverflow = 0;
    double target = 0, floor = 0;
    // True when the pixels are below target or the DDS is unreadable, so a re-cook might help.
    bool lowQuality = false;
};

std::string ddsPathFor(const CookJob& job);
CookQuality validateCook(const CookJob& job, const std::string& ddsPath, const CookThresholds& thresholds);
std::string describeQuality(const CookQuality& quality);
