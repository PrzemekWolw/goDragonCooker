#include "texture/texconv.h"

#include <cstdlib>
#include <cwchar>
#include <mutex>

#include "core/platform.h"
#include "core/strings.h"

#if defined(_WIN32)
#include <windows.h>

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
}
#endif

using TexconvFn = int (*)(int argc, wchar_t* argv[], bool verbose, bool initCOM, bool allowSlowCodec,
                          wchar_t* errBuf, int errBufSize);
using TexconvVersionFn = int (*)();

static std::mutex texconvInitMu;
// Serializes BC6H/BC7 calls because they use the GPU or all CPU cores.
static std::mutex texconvCodecMu;
static TexconvFn texconvFn = nullptr;
static TexconvVersionFn texconvVersionFn = nullptr;

static std::wstring toWide(const std::string& s)
{
#if defined(_WIN32)
    return u8path(s).wstring();
#else
    std::wstring wide(s.size() + 1, L'\0');
    const size_t n = std::mbstowcs(wide.data(), s.c_str(), wide.size());
    if (n == size_t(-1)) {
        for (size_t i = 0; i < s.size(); ++i) {
            wide[i] = wchar_t(static_cast<unsigned char>(s[i]));
        }
        wide.resize(s.size());
    } else {
        wide.resize(n);
    }
    return wide;
#endif
}

static std::string fromWide(const wchar_t* s)
{
#if defined(_WIN32)
    return u8str(fs::path(s));
#else
    char buffer[1024];
    const size_t n = std::wcstombs(buffer, s, sizeof(buffer) - 1);
    if (n == size_t(-1)) {
        return "";
    }
    return std::string(buffer, n);
#endif
}

std::string texconvInit()
{
    std::lock_guard lock(texconvInitMu);
    if (texconvFn) {
        return "";
    }
#if defined(_WIN32)
    const char* name = "texconv.dll";
#elif defined(__APPLE__)
    const char* name = "libtexconv.dylib";
#else
    const char* name = "libtexconv.so";
#endif
    const fs::path path = findAsset(name);
    if (path.empty()) {
        return "texconv library not found (looked in bin/ next to executable and cwd)";
    }
    std::string error;
    void* library = loadLibrary(path, error);
    if (!library) {
        return format("failed to load texconv library %s (%s)", u8str(path).c_str(), error.c_str());
    }
    texconvVersionFn = reinterpret_cast<TexconvVersionFn>(librarySymbol(library, "texconv_version"));
    texconvFn = reinterpret_cast<TexconvFn>(librarySymbol(library, "texconv"));
    if (!texconvFn) {
        return format("failed to load texconv library %s (missing texconv export)", u8str(path).c_str());
    }
    return "";
}

std::string texconvVersion()
{
    {
        std::lock_guard lock(texconvInitMu);
        if (!texconvFn) {
            return "unavailable";
        }
    }
    const int version = texconvVersionFn ? texconvVersionFn() : 0;
    return version == 0 ? "unknown" : format("%03d (DirectXTex)", version);
}

static bool containsGPUCodecFormat(const std::vector<std::string>& args)
{
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "-f") {
            const std::string fmt = args[i + 1];
            if (hasPrefix(fmt, "BC6H_") || hasPrefix(fmt, "BC7_")) {
                return true;
            }
        }
    }
    return false;
}

std::string texconvRun(const std::vector<std::string>& args, bool forceCPU)
{
    if (std::string error = texconvInit(); !error.empty()) {
        return error;
    }
    if (args.empty()) {
        return "no arguments provided to texconv";
    }

    const bool gpuCodec = containsGPUCodecFormat(args);
#if defined(_WIN32)
    const bool initCOM = true;
    const bool allowSlowCodec = !(gpuCodec && !forceCPU);
#else
    const bool initCOM = false;
    const bool allowSlowCodec = true;
#endif

    std::unique_lock<std::mutex> codecLock(texconvCodecMu, std::defer_lock);
    if (gpuCodec) {
        codecLock.lock();
    }

    std::vector<std::wstring> wideArgs;
    std::vector<wchar_t*> argv;
    for (const std::string& arg : args) {
        wideArgs.push_back(toWide(arg));
    }
    for (std::wstring& arg : wideArgs) {
        argv.push_back(arg.data());
    }

    wchar_t errBuf[256] = {};
    const int ret = texconvFn(int(argv.size()), argv.data(), false, initCOM, allowSlowCodec, errBuf, 256);
    if (ret == 0) {
        return "";
    }
    const std::string message = fromWide(errBuf);
    if (message.empty()) {
        return format("texconv returned error code %d", ret);
    }
    return "Texconv Error: " + message;
}
