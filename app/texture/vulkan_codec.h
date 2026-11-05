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
};

std::string vulkanVersion();
std::string vulkanDevices(std::vector<std::string>& devices);
std::string ensureVulkanContext(int deviceIndex);
std::string vulkanContextDeviceName();
std::string vulkanCompress(const CookJob& job, int deviceIndex);
