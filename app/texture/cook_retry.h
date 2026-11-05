#pragma once

#include <string>
#include <vector>

#include "texture/cook_validate.h"

struct CookRetry {
    bool attempted = false;
    bool improved = false;
    std::string note;
    // Quality of the DDS that was kept.
    CookQuality quality;
};

// Re-cooks with texconv into a temporary folder and replaces the DDS only when the retry is clearly better.
// firstWasTexconv forces the CPU codec, since the first run may already have used the GPU one.
CookRetry retryCook(const CookJob& job, const CookQuality& first, bool firstWasTexconv,
                    const std::vector<std::string>& gpuArgs, const CookThresholds& thresholds);
