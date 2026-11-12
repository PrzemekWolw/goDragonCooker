#include "core/platform.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/strings.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

fs::path findLibrary(const std::string& stem)
{
#if defined(_WIN32)
    return findAsset(stem + ".dll");
#elif defined(__APPLE__)
    return findAsset("lib" + stem + ".dylib");
#else
    return findAsset("lib" + stem + ".so");
#endif
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out, std::string& error)
{
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(size_t(std::max<std::streamoff>(size, 0)));
    if (size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size)) {
        error = "cannot read " + path;
        return false;
    }
    return true;
}

bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& data, std::string& error)
{
    std::error_code ec;
    const fs::path p = u8path(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

std::vector<std::string> readLines(const fs::path& path)
{
    std::vector<std::string> lines;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        lines.push_back(line);
    }
    return lines;
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

std::vector<std::string> platformSystemInfo()
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

#else

std::vector<std::string> platformSystemInfo()
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

#endif
