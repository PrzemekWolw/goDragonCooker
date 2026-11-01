#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

inline const std::string textureBackendCompressonator = "compressonator";
inline const std::string textureBackendTexconv = "texconv";
constexpr unsigned vulkanWorkerCount = 4;
constexpr unsigned vulkanPipelineWorkerCount = vulkanWorkerCount * 2;

constexpr ImVec4 colBg(0.09f, 0.12f, 0.16f, 1.0f);
constexpr ImVec4 colBar(0.13f, 0.15f, 0.20f, 1.0f);
constexpr ImVec4 colText(0.80f, 0.80f, 0.83f, 1.0f);
constexpr ImVec4 colTextDim(0.50f, 0.54f, 0.60f, 1.0f);
constexpr ImVec4 colCookOk(60 / 255.0f, 179 / 255.0f, 113 / 255.0f, 1.0f);
constexpr ImVec4 colCookErr(0.76f, 0.28f, 0.28f, 1.0f);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Shared between the UI thread and one background run.
struct TaskState {
    std::mutex mu;
    std::string status;
    std::vector<std::string> logLines;
    bool running = false;
    std::atomic<bool> stop{false};

    void log(const std::string& msg);
    void setStatus(const std::string& status);
    void begin(const std::string& status);
    void finish(const std::string& status);
};

struct TexEntry {
    std::string suffix;
    std::string ext;
    std::string stageKey;
};

struct MatGenSettings {
    std::vector<TexEntry> suffixes;
    std::vector<TexEntry> customSuffixes;
    double detailScaleX = 0.1;
    double detailScaleY = 0.1;
    bool instanceOpacity = false;
    bool instanceEmissive = false;
    bool enableDetailMaps = false;
};

struct PatternEditBuf {
    std::string suffix;
    int extIdx = 0;
    bool dirty = false;
};

// UI-thread-only fields are plain; runs get copies.
struct CookTab {
    TaskState task;
    std::string directory;
    bool autoDetectFmt = true;
};

struct MatGenTab {
    TaskState task;
    std::string directory;
    MatGenSettings settings;
    std::map<std::string, PatternEditBuf> patternBufs;
};

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
extern CookTab cookTab;
extern MatGenTab matGenTab;

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

// main.cpp
void requestRedraw();
extern float uiScale;
float px(float value);
void saveSettings();
void openDirPicker(std::string* target);
bool comboStrings(const char* id, int* index, const std::vector<std::string>& items, float width);
void tooltip(const char* text);

// cooker.cpp
void cookTabUI();
void runStartupGPUCheck();

// materialgen.cpp
MatGenSettings defaultMatGenSettings();
void matGenTabUI();

// strings
std::string format(const char* fmt, ...);
std::string toLower(std::string s);
bool hasPrefix(const std::string& s, const std::string& prefix);
bool hasSuffix(const std::string& s, const std::string& suffix);
std::string trim(const std::string& s);

// platform.cpp
fs::path u8path(const std::string& s);
std::string u8str(const fs::path& p);
fs::path exeDir();
fs::path userConfigDir();
std::string targetName();
fs::path findAsset(const std::string& name);
void* loadLibrary(const fs::path& path, std::string& error);
void* librarySymbol(void* library, const char* name);
std::vector<std::string> platformGPUInfo();
std::vector<std::string> preferredGPUArgs();
std::vector<std::string> gpuAdapterOptions();
int gpuAdapterOptionIndex(int adapter);
int gpuAdapterIndexForOption(int option);
std::vector<int> gpuAdapterCandidates();
std::string gpuAdapterName(int index);
void setSelectedGPU(int index);

// imageheader.cpp
enum class ColorType { Unknown, Grayscale, RGB, Palette, GrayAlpha, RGBA };
enum class ColorSpace { Unknown, Linear, SRGB };

struct ImgInfo {
    std::string format;
    int bits = 0;
    ColorType colorType = ColorType::Unknown;
    ColorSpace colorSpace = ColorSpace::Unknown;
};

std::string imageFormat(const std::string& name);
bool readImageHeader(const fs::path& path, ImgInfo& info, std::string& error);

// texconv.cpp
std::string texconvInit();
std::string texconvVersion();
std::string texconvRun(const std::vector<std::string>& args, bool forceCPU = false);

// vulkan_codec.cpp
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
