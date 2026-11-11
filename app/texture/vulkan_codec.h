#pragma once

#include <string>
#include <vector>

constexpr unsigned vulkanWorkerCount = 4;
constexpr unsigned vulkanPipelineWorkerCount = vulkanWorkerCount * 2;

struct CookJob {
    std::string srcFile;
    std::string format;
    bool srgb = false;
    bool inputSRGB = false;
    bool ignoreSRGB = false;
    bool sepa = false;
    // Set when srcFile is a temporary corrected image. The DDS is written here, named from srcFile.
    std::string outputDir;
    // Temporary directory holding srcFile. Removed when the cook finishes.
    std::string cleanupDir;
};

std::string vulkanVersion();
std::string vulkanDevices(std::vector<std::string>& devices);
std::string ensureVulkanContext(int deviceIndex);
std::string vulkanContextDeviceName();
std::string vulkanCompress(const CookJob& job, int deviceIndex);
