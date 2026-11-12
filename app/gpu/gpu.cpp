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
            app.gpuAdapter = settings.value("texconvGPU", app.gpuAdapter);
            app.vulkanDevice = settings.value("vulkanGPU", app.vulkanDevice);
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
    int index = 0;
    std::string name;
    uint64_t dedicatedMemory = 0;
    uint64_t sharedMemory = 0;
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
        // 0x1414 is Microsoft's basic display and render adapter, not a GPU.
        if (SUCCEEDED(adapter->GetDesc1(&description)) && (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            description.VendorId != 0x1414) {
            std::string name = u8str(fs::path(description.Description));
            if (!name.empty()) {
                adapters.push_back({int(index), name, uint64_t(description.DedicatedVideoMemory),
                                    uint64_t(description.SharedSystemMemory)});
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

static bool isDiscreteAdapter(const GpuAdapter& a)
{
    return a.dedicatedMemory >= (1ull << 30);
}

static bool betterAdapter(const GpuAdapter& a, const GpuAdapter& b)
{
    const bool da = isDiscreteAdapter(a), db = isDiscreteAdapter(b);
    if (da != db) {
        return da;
    }
    if (a.dedicatedMemory != b.dedicatedMemory) {
        return a.dedicatedMemory > b.dedicatedMemory;
    }
    return a.sharedMemory > b.sharedMemory;
}

std::vector<int> gpuAdapterCandidates()
{
    std::vector<GpuAdapter> sorted = availableGPUAdapters();
    std::stable_sort(sorted.begin(), sorted.end(), betterAdapter);
    std::vector<int> indices;
    for (const GpuAdapter& adapter : sorted) {
        indices.push_back(adapter.index);
    }
    return indices;
}

std::vector<std::string> preferredGPUArgs()
{
    int configuredGPU;
    {
        std::lock_guard lock(app.mu);
        configuredGPU = app.gpuAdapter;
    }
    auto known = [](int index) { return gpuAdapterName(index) != "unknown"; };
    if (configuredGPU >= 0 && known(configuredGPU)) {
        return {"-gpu", std::to_string(configuredGPU)};
    }
    {
        std::lock_guard lock(gpuIndexMu);
        if (selectedGPU >= 0 && known(selectedGPU)) {
            return {"-gpu", std::to_string(selectedGPU)};
        }
    }
    const std::vector<int> indices = gpuAdapterCandidates();
    if (indices.empty()) {
        return {"-nogpu"};
    }
    return {"-gpu", std::to_string(indices.front())};
}

void setSelectedGPU(int index)
{
    std::lock_guard lock(gpuIndexMu);
    selectedGPU = index;
}

#else

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
    int gpuCount = 0;
    for (const fs::path& file : files) {
        std::string driver, pciID;
        for (const std::string& line : readLines(file)) {
            if (hasPrefix(line, "DRIVER=")) {
                driver = line.substr(7);
            } else if (hasPrefix(line, "PCI_ID=")) {
                pciID = line.substr(7);
            }
        }
        if (pciID.empty()) {
            continue;
        }
        report.push_back(format("GPU: driver %s, PCI %s", driver.c_str(), pciID.c_str()));
        ++gpuCount;
    }
    if (gpuCount == 0) {
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
    std::vector<int> indices = configuredGPU >= 0 && gpuAdapterName(configuredGPU) != "unknown"
                                   ? std::vector<int>{configuredGPU}
                                   : gpuAdapterCandidates();
    if (indices.empty()) {
#if defined(_WIN32)
        return {"Texconv BC7 GPU: no adapter"};
#else
        indices = {-1};
#endif
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
