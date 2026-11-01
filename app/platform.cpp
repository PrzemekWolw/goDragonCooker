#include "app.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <dxgi.h>
#else
#include <dlfcn.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

std::string format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int size = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string result(size > 0 ? size_t(size) : 0, '\0');
    if (size > 0) {
        std::vsnprintf(result.data(), result.size() + 1, fmt, args);
    }
    va_end(args);
    return result;
}

std::string toLower(std::string s)
{
    for (char& c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool hasPrefix(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool hasSuffix(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

fs::path u8path(const std::string& s)
{
    return fs::u8path(s);
}

std::string u8str(const fs::path& p)
{
    return p.u8string();
}

fs::path exeDir()
{
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    return fs::path(std::wstring(buffer, length)).parent_path();
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return fs::current_path();
    }
    std::error_code ec;
    return fs::canonical(buffer, ec).parent_path();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

fs::path userConfigDir()
{
#if defined(_WIN32)
    const wchar_t* appData = _wgetenv(L"APPDATA");
    return appData ? fs::path(appData) : fs::path();
#else
    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    return home ? fs::path(home) / "Library" / "Application Support" : fs::path();
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return xdg;
    }
    return home ? fs::path(home) / ".config" : fs::path();
#endif
#endif
}

std::string targetName()
{
#if defined(_WIN32)
    std::string platform = "windows";
#elif defined(__APPLE__)
    std::string platform = "macos";
#else
    std::string platform = "linux";
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
    return platform + "-arm64";
#else
    return platform + "-x64";
#endif
}

// Looks in bin/<target>/ next to the executable, then relative to the working directory.
fs::path findAsset(const std::string& name)
{
    for (const fs::path& base : {exeDir(), fs::path()}) {
        std::error_code ec;
        const fs::path candidate = fs::absolute(base / "bin" / targetName() / name, ec);
        if (!ec && fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Dynamic libraries
// ---------------------------------------------------------------------------

void* loadLibrary(const fs::path& path, std::string& error)
{
#if defined(_WIN32)
    HMODULE module = LoadLibraryExW(
        path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        const DWORD code = GetLastError();
        wchar_t message[512] = {};
        DWORD length = FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, message,
            DWORD(std::size(message)), nullptr);
        while (length > 0 && (message[length - 1] == L'\r' || message[length - 1] == L'\n')) {
            message[--length] = L'\0';
        }
        const std::string text = u8str(fs::path(message));
        error = text.empty() ? format("Windows error %lu", (unsigned long)code)
                             : format("Windows error %lu: %s", (unsigned long)code, text.c_str());
    }
    return module;
#else
    dlerror();
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* message = dlerror();
        error = message ? message : "dlopen failed";
    }
    return handle;
#endif
}

void* librarySymbol(void* library, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

// ---------------------------------------------------------------------------
// System info
// ---------------------------------------------------------------------------

static unsigned cpuCount()
{
    return std::max(1u, std::thread::hardware_concurrency());
}

static std::string cpuInfo(const std::string& name)
{
    if (name.empty()) {
        return format("CPU: %u logical processors", cpuCount());
    }
    return format("CPU: %s (%u logical processors)", name.c_str(), cpuCount());
}

#if defined(_WIN32)

static std::vector<std::string> platformSystemInfo()
{
    std::string name;
    wchar_t buffer[256] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
        name = trim(u8str(fs::path(buffer)));
    }

    std::string memory = "RAM: unavailable";
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        constexpr double gb = 1024.0 * 1024.0 * 1024.0;
        memory = format(
            "RAM: %.1f GB total, %.1f GB available", double(status.ullTotalPhys) / gb,
            double(status.ullAvailPhys) / gb);
    }
    return {cpuInfo(name), memory};
}

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
        report.push_back("GPU hardware: no display adapters reported");
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
    std::vector<std::string> options = {"Automatic (best)"};
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

static std::vector<std::string> platformSystemInfo()
{
    std::string name;
    for (const std::string& line : readLines("/proc/cpuinfo")) {
        if (hasPrefix(line, "model name")) {
            if (const auto colon = line.find(':'); colon != std::string::npos) {
                name = trim(line.substr(colon + 1));
            }
            break;
        }
    }

    std::string memory = "RAM: unavailable";
    for (const std::string& line : readLines("/proc/meminfo")) {
        std::istringstream fields(line);
        std::string key;
        unsigned long long kb = 0;
        if (fields >> key >> kb && key == "MemTotal:") {
            memory = format("RAM: %.1f GB total", double(kb) / (1024.0 * 1024.0));
            break;
        }
    }
    return {cpuInfo(name), memory};
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
        report.push_back("GPU hardware: no display adapters reported");
    }
    return report;
}

std::vector<std::string> preferredGPUArgs() { return {}; }
std::vector<std::string> gpuAdapterOptions() { return {"Automatic (best)"}; }
int gpuAdapterOptionIndex(int) { return 0; }
int gpuAdapterIndexForOption(int) { return -1; }
std::vector<int> gpuAdapterCandidates() { return {}; }
std::string gpuAdapterName(int) { return "unknown"; }
void setSelectedGPU(int) {}

#endif
