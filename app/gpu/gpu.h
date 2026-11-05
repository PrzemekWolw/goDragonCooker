#pragma once

#include <mutex>
#include <string>
#include <vector>

inline const std::string textureBackendCompressonator = "compressonator";
inline const std::string textureBackendTexconv = "texconv";

// Texture backend selection and the hardware report, shared by the UI and cook runs.
struct AppState {
    std::mutex mu;
    std::string texconvWarn;
    bool warnShown = false;
    std::vector<std::string> gpuReport;
    int gpuAdapter = -1;
    int vulkanDevice = -1;
    std::string textureBackend = textureBackendCompressonator;
    std::vector<std::string> vulkanDevices;
    bool gpuChecking = false;
};

extern AppState app;

// Registers textureBackend/texconvGPU/vulkanGPU with settings.json.
void registerGpuSettings();

std::vector<std::string> platformGPUInfo();
std::vector<std::string> preferredGPUArgs();
std::vector<std::string> gpuAdapterOptions();
int gpuAdapterOptionIndex(int adapter);
int gpuAdapterIndexForOption(int option);
std::vector<int> gpuAdapterCandidates();
std::string gpuAdapterName(int index);
void setSelectedGPU(int index);

void runStartupGPUCheck();
void rerunGPUCheck();
