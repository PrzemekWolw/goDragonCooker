#include "app.h"

#include <cstdint>
#include <shared_mutex>

#include "native/vulkan/gdc_vulkan.h"

namespace {

struct VulkanCodec {
    std::mutex mu;
    std::shared_mutex contextMu;
    bool loaded = false;
    gdc_vulkan_context* context = nullptr;
    int deviceIndex = -2;

    decltype(&gdc_vulkan_version) version = nullptr;
    decltype(&gdc_vulkan_device_count) deviceCount = nullptr;
    decltype(&gdc_vulkan_device_name) deviceName = nullptr;
    decltype(&gdc_vulkan_create) create = nullptr;
    decltype(&gdc_vulkan_context_device_name) contextDeviceName = nullptr;
    decltype(&gdc_vulkan_destroy) destroy = nullptr;
    decltype(&gdc_vulkan_compress) compress = nullptr;
};

VulkanCodec codec;

template <typename T>
void loadSymbol(void* library, const char* name, T& fn)
{
    fn = reinterpret_cast<T>(librarySymbol(library, name));
}

std::string vulkanInit()
{
    std::lock_guard lock(codec.mu);
    if (codec.loaded) {
        return "";
    }
#if defined(_WIN32)
    const fs::path path = findAsset("gdc_vulkan.dll");
#elif defined(__APPLE__)
    const fs::path path = findAsset("libgdc_vulkan.dylib");
#else
    const fs::path path = findAsset("libgdc_vulkan.so");
#endif
    if (path.empty()) {
        return "Compressonator Vulkan library not found";
    }
    std::string error;
    void* library = loadLibrary(path, error);
    if (library) {
        loadSymbol(library, "gdc_vulkan_version", codec.version);
        loadSymbol(library, "gdc_vulkan_device_count", codec.deviceCount);
        loadSymbol(library, "gdc_vulkan_device_name", codec.deviceName);
        loadSymbol(library, "gdc_vulkan_create", codec.create);
        loadSymbol(library, "gdc_vulkan_context_device_name", codec.contextDeviceName);
        loadSymbol(library, "gdc_vulkan_destroy", codec.destroy);
        loadSymbol(library, "gdc_vulkan_compress", codec.compress);
        if (!codec.version || !codec.deviceCount || !codec.deviceName || !codec.create ||
            !codec.contextDeviceName || !codec.destroy || !codec.compress) {
            error = "required Vulkan codec exports are missing";
            library = nullptr;
        }
    }
    if (!library) {
        return format("failed to load Compressonator Vulkan library %s: %s", u8str(path).c_str(), error.c_str());
    }
    codec.loaded = true;
    return "";
}

uint32_t vulkanFormat(const std::string& format)
{
    if (format == "BC4_UNORM") return GDC_BC4_UNORM;
    if (format == "BC5_UNORM") return GDC_BC5_UNORM;
    if (format == "BC6H_UF16") return GDC_BC6H_UF16;
    if (format == "BC7_UNORM") return GDC_BC7_UNORM;
    if (format == "BC7_UNORM_SRGB") return GDC_BC7_UNORM_SRGB;
    return 0;
}

} // namespace

std::string vulkanVersion()
{
    if (!vulkanInit().empty()) {
        return "unavailable";
    }
    std::lock_guard lock(codec.mu);
    const char* version = codec.version();
    return version ? version : "unknown";
}

std::string vulkanDevices(std::vector<std::string>& devices)
{
    devices.clear();
    if (std::string error = vulkanInit(); !error.empty()) {
        return error;
    }
    char error[1024] = {};
    const int count = codec.deviceCount(error, sizeof(error));
    if (count < 0) {
        return error;
    }
    for (int index = 0; index < count; ++index) {
        char name[256] = {};
        if (codec.deviceName(uint32_t(index), name, sizeof(name)) == 0) {
            devices.push_back(name);
        }
    }
    return "";
}

std::string ensureVulkanContext(int deviceIndex)
{
    if (std::string error = vulkanInit(); !error.empty()) {
        return error;
    }
    std::lock_guard lock(codec.mu);
    std::unique_lock contextLock(codec.contextMu);
    if (codec.context && codec.deviceIndex == deviceIndex) {
        return "";
    }
    if (codec.context) {
        codec.destroy(codec.context);
        codec.context = nullptr;
    }
    const fs::path shaders = findAsset("shaders");
    std::error_code ec;
    if (shaders.empty() || !fs::is_directory(shaders, ec)) {
        return "Compressonator Vulkan shaders not found";
    }
    char error[1024] = {};
    codec.context = codec.create(deviceIndex, u8str(shaders).c_str(), vulkanWorkerCount, error, sizeof(error));
    if (!codec.context) {
        return error;
    }
    codec.deviceIndex = deviceIndex;
    return "";
}

std::string vulkanContextDeviceName()
{
    std::shared_lock lock(codec.contextMu);
    if (!codec.context) {
        return "";
    }
    char name[256] = {};
    if (codec.contextDeviceName(codec.context, name, sizeof(name)) != 0) {
        return "";
    }
    return name;
}

std::string vulkanCompress(const CookJob& job, int deviceIndex)
{
    if (std::string error = ensureVulkanContext(deviceIndex); !error.empty()) {
        return error;
    }
    const uint32_t fmt = vulkanFormat(job.format);
    if (fmt == 0) {
        return "unsupported Compressonator Vulkan target format: " + job.format;
    }
    const std::string destination = u8str(u8path(job.srcFile).replace_extension(".dds"));
    uint32_t flags = 0;
    if (job.inputSRGB) flags |= GDC_FLAG_INPUT_SRGB;
    if (job.ignoreSRGB) flags |= GDC_FLAG_IGNORE_SRGB;
    if (job.sepa) flags |= GDC_FLAG_SEPARATE_ALPHA;
    if (job.srgb) flags |= GDC_FLAG_OUTPUT_SRGB;

    char error[1024] = {};
    std::shared_lock lock(codec.contextMu);
    const int result = codec.compress(
        codec.context, job.srcFile.c_str(), destination.c_str(), fmt, flags, error, sizeof(error));
    if (result != 0) {
        return error[0] ? error : format("Compressonator Vulkan returned code %d", result);
    }
    return "";
}
