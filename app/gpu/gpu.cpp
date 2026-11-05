#include "gpu/gpu.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <thread>

#include "core/platform.h"
#include "core/settings.h"
#include "core/strings.h"
#include "core/task.h"
#include "texture/texconv.h"
#include "texture/vulkan_codec.h"

#if defined(_WIN32)
#include <windows.h>
#include <dxgi.h>
#endif

AppState app;

void registerGpuSettings()
{
    registerSettings(
        [](const SettingsJson& settings) {
            std::lock_guard lock(app.mu);
            const std::string backend = settings.value("textureBackend", "");
            if (backend == textureBackendTexconv || backend == textureBackendCompressonator) {
                app.textureBackend = backend;
            }
            app.gpuAdapter = settings.value("texconvGPU", 0);
            app.vulkanDevice = settings.value("vulkanGPU", 0);
        },
        [](SettingsJson& settings) {
            std::lock_guard lock(app.mu);
            settings["textureBackend"] = app.textureBackend;
            settings["texconvGPU"] = app.gpuAdapter;
            settings["vulkanGPU"] = app.vulkanDevice;
        });
}

// ---------------------------------------------------------------------------
// Adapters
// ---------------------------------------------------------------------------

#if defined(_WIN32)

struct GpuAdapter {
    int index;
    std::string name;
};

static std::vector<GpuAdapter> enumerateDXGIAdapters()
{
    std::vector<GpuAdapter> adapters;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        return adapters;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        if (!adapter) {
            break;
        }
        DXGI_ADAPTER_DESC1 description = {};
        if (SUCCEEDED(adapter->GetDesc1(&description))) {
            std::string name = u8str(fs::path(description.Description));
            if (!name.empty()) {
                adapters.push_back({int(index), name});
            }
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return adapters;
}

static const std::vector<GpuAdapter>& availableGPUAdapters()
{
    static const std::vector<GpuAdapter> adapters = [] {
        std::vector<GpuAdapter> result;
        std::set<std::string> seen;
        for (const GpuAdapter& adapter : enumerateDXGIAdapters()) {
            const std::string lower = toLower(adapter.name);
            if (lower == "microsoft basic display adapter" || lower == "microsoft basic render driver") {
                continue;
            }
            if (seen.insert(adapter.name).second) {
                result.push_back(adapter);
            }
        }
        return result;
    }();
    return adapters;
}

std::vector<std::string> platformGPUInfo()
{
    std::vector<std::string> report = platformSystemInfo();
    const auto& adapters = availableGPUAdapters();
    for (const GpuAdapter& adapter : adapters) {
        report.push_back(format("GPU adapter %d: %s", adapter.index, adapter.name.c_str()));
    }
    if (adapters.empty()) {
        report.push_back("gdc.gpu.noAdapters");
    }
    return report;
}

static std::mutex gpuIndexMu;
static int selectedGPU = -1;

std::string gpuAdapterName(int index)
{
    for (const GpuAdapter& adapter : availableGPUAdapters()) {
        if (adapter.index == index) {
            return adapter.name;
        }
    }
    return "unknown";
}

std::vector<std::string> gpuAdapterOptions()
{
    std::vector<std::string> options = {"gdc.common.automaticBest"};
    for (const GpuAdapter& adapter : availableGPUAdapters()) {
        options.push_back(format("%d: %s", adapter.index, adapter.name.c_str()));
    }
    return options;
}

int gpuAdapterOptionIndex(int adapter)
{
    if (adapter < 0) {
        return 0;
    }
    const auto& adapters = availableGPUAdapters();
    for (size_t option = 0; option < adapters.size(); ++option) {
        if (adapters[option].index == adapter) {
            return int(option) + 1;
        }
    }
    return 0;
}

int gpuAdapterIndexForOption(int option)
{
    const auto& adapters = availableGPUAdapters();
    if (option <= 0 || option - 1 >= int(adapters.size())) {
        return -1;
    }
    return adapters[option - 1].index;
}

static int gpuScore(const std::string& name)
{
    const std::string lower = toLower(name);
    int score = 1;
    if (lower.find("nvidia") != std::string::npos) {
        score = 100;
    } else if (lower.find("amd") != std::string::npos || lower.find("radeon") != std::string::npos) {
        score = 70;
    } else if (lower.find("intel") != std::string::npos) {
        score = 10;
    }
    if (lower.find("780m") != std::string::npos || lower.find("680m") != std::string::npos ||
        lower.find("integrated") != std::string::npos) {
        score -= 40;
    }
    return score;
}

std::vector<int> gpuAdapterCandidates()
{
    std::vector<int> indices;
    for (const GpuAdapter& adapter : availableGPUAdapters()) {
        indices.push_back(adapter.index);
    }
    std::stable_sort(indices.begin(), indices.end(), [](int a, int b) {
        return gpuScore(gpuAdapterName(a)) > gpuScore(gpuAdapterName(b));
    });
    return indices;
}

std::vector<std::string> preferredGPUArgs()
{
    int configuredGPU;
    {
        std::lock_guard lock(app.mu);
        configuredGPU = app.gpuAdapter;
    }
    if (configuredGPU >= 0) {
        return {"-gpu", std::to_string(configuredGPU)};
    }
    {
        std::lock_guard lock(gpuIndexMu);
        if (selectedGPU >= 0) {
            return {"-gpu", std::to_string(selectedGPU)};
        }
    }
    const std::vector<int> indices = gpuAdapterCandidates();
    return {"-gpu", std::to_string(indices.empty() ? 0 : indices.front())};
}

void setSelectedGPU(int index)
{
    std::lock_guard lock(gpuIndexMu);
    selectedGPU = index;
}

#else

static std::vector<std::string> readLines(const fs::path& path)
{
    std::vector<std::string> lines;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> platformGPUInfo()
{
    std::vector<std::string> report = platformSystemInfo();
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
        const fs::path uevent = entry.path() / "device" / "uevent";
        if (hasPrefix(entry.path().filename().string(), "card") && fs::exists(uevent, ec)) {
            files.push_back(uevent);
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        std::string driver, pciID;
        for (const std::string& line : readLines(file)) {
            if (hasPrefix(line, "DRIVER=")) {
                driver = line.substr(7);
            } else if (hasPrefix(line, "PCI_ID=")) {
                pciID = line.substr(7);
            }
        }
        report.push_back(format("GPU: driver %s, PCI %s", driver.c_str(), pciID.c_str()));
    }
    if (files.empty()) {
        report.push_back("gdc.gpu.noAdapters");
    }
    return report;
}

std::vector<std::string> preferredGPUArgs() { return {}; }
std::vector<std::string> gpuAdapterOptions() { return {"gdc.common.automaticBest"}; }
int gpuAdapterOptionIndex(int) { return 0; }
int gpuAdapterIndexForOption(int) { return -1; }
std::vector<int> gpuAdapterCandidates() { return {}; }
std::string gpuAdapterName(int) { return "unknown"; }
void setSelectedGPU(int) {}

#endif

// ---------------------------------------------------------------------------
// GPU check
// ---------------------------------------------------------------------------

static std::vector<std::string> checkTexconvGPU()
{
    std::error_code ec;
    const auto stamp = (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path(ec) / format("go-cooker-gpu-check-%llu", stamp);
    fs::create_directories(dir, ec);
    if (ec) {
        return {"Texconv BC7: FAILED (" + ec.message() + ")"};
    }
    const fs::path input = dir / "gpu-check.tga";
    {
        // Uncompressed 4x4 32-bit TGA, top-left origin, BGRA (64, 96, 128, 255).
        std::vector<unsigned char> tga = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 4, 0, 32, 0x28};
        for (int i = 0; i < 16; ++i) {
            tga.insert(tga.end(), {64, 96, 128, 255});
        }
        std::ofstream file(input, std::ios::binary);
        file.write(reinterpret_cast<const char*>(tga.data()), std::streamsize(tga.size()));
        if (!file) {
            fs::remove_all(dir, ec);
            return {"Texconv BC7: FAILED (cannot write test image)"};
        }
    }

    int configuredGPU;
    {
        std::lock_guard lock(app.mu);
        configuredGPU = app.gpuAdapter;
    }
    std::vector<int> indices = configuredGPU >= 0 ? std::vector<int>{configuredGPU} : gpuAdapterCandidates();
    if (indices.empty()) {
        indices = {-1};
    }

    std::vector<std::string> result;
    std::string lastError;
    for (int index : indices) {
        std::vector<std::string> args = {"-y", "-f", "BC7_UNORM", "-bc", "x"};
        if (index >= 0) {
            args.insert(args.end(), {"-gpu", std::to_string(index)});
        }
        args.insert(args.end(), {"-o", u8str(dir), "--", u8str(input)});
        lastError = texconvRun(args);
        if (lastError.empty()) {
#if defined(_WIN32)
            setSelectedGPU(index);
            result = {format("Texconv BC7 GPU: OK (adapter %d: %s)", index, gpuAdapterName(index).c_str())};
#else
            result = {"Texconv BC7: OK"};
#endif
            break;
        }
    }
    fs::remove_all(dir, ec);
    if (result.empty()) {
        result = {"Texconv BC7 GPU: FAILED", "  " + lastError};
    }
    return result;
}

static std::vector<std::string> runGPUCheck()
{
    std::vector<std::string> report = platformGPUInfo();
    report.push_back("Texconv version: " + texconvVersion());
    for (const std::string& line : checkTexconvGPU()) {
        report.push_back(line);
    }

    std::vector<std::string> devices;
    const std::string deviceError = vulkanDevices(devices);
    int selectedDevice;
    {
        std::lock_guard lock(app.mu);
        app.vulkanDevices = devices;
        selectedDevice = app.vulkanDevice;
    }
    report.push_back("Compressonator version: " + vulkanVersion());
    std::string contextError;
    if (!deviceError.empty()) {
        report.push_back("Compressonator Vulkan: FAILED (" + deviceError + ")");
    } else if (!(contextError = ensureVulkanContext(selectedDevice)).empty()) {
        report.push_back("Compressonator Vulkan: FAILED (" + contextError + ")");
    } else {
        std::string deviceName = vulkanContextDeviceName();
        if (deviceName.empty() && selectedDevice >= 0 && selectedDevice < int(devices.size())) {
            deviceName = devices[selectedDevice];
        }
        report.push_back("Compressonator Vulkan: OK (" + (deviceName.empty() ? "unknown" : deviceName) + ")");
    }
    return report;
}

void runStartupGPUCheck()
{
    {
        std::lock_guard lock(app.mu);
        app.gpuChecking = true;
    }
    if (std::string error = texconvInit(); !error.empty()) {
        std::lock_guard lock(app.mu);
        app.texconvWarn = error;
        app.warnShown = true;
    }
    std::vector<std::string> report = runGPUCheck();
    {
        std::lock_guard lock(app.mu);
        app.gpuReport = report;
        app.gpuChecking = false;
    }
    requestRedraw();
}

void rerunGPUCheck()
{
    {
        std::lock_guard lock(app.mu);
        if (app.gpuChecking) {
            return;
        }
        app.gpuChecking = true;
        app.gpuReport = {"gdc.gpu.testingSelected"};
    }
    std::thread([] {
        std::vector<std::string> report = runGPUCheck();
        {
            std::lock_guard lock(app.mu);
            app.gpuReport = report;
            app.gpuChecking = false;
        }
        requestRedraw();
    }).detach();
}
