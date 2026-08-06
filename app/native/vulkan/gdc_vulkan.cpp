#include "gdc_vulkan.h"

#include <vulkan/vulkan.h>

#include <cmath>
#include <OpenEXR/ImfRgbaFile.h>
#include <tiffio.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "stb_image.h"

namespace {

constexpr const char* kVersion = "Compressonator 4.5.0 Vulkan";
constexpr uint32_t kMaxWorkers = 4;

struct Constants {
    uint32_t width;
    uint32_t blocksX;
    uint32_t format;
    uint32_t mode;
    uint32_t startBlock;
    uint32_t totalBlocks;
    float alphaWeight;
    float quality;
};

struct MipConstants {
    uint32_t sourceWidth;
    uint32_t sourceHeight;
    uint32_t destinationWidth;
    uint32_t destinationHeight;
    uint32_t flags;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
};

struct Worker {
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    Buffer staging;
    Buffer output;
    Buffer readback;
    Buffer dummy;
    Buffer constants;
    Buffer mipConstants;
    Image sourceImage;
    Image mipImage;
    VkImageView sourceView = VK_NULL_HANDLE;
    std::vector<VkImageView> mipViews;
    std::vector<VkDescriptorSet> descriptorSets;
    std::vector<VkDescriptorSet> mipDescriptorSets;
    uint32_t descriptorCapacity = 0;
    bool imagesInitialized = false;
    bool busy = false;
};

struct Pipeline {
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct gdc_vulkan_context_impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::array<Pipeline, 4> pipelines;
    VkDescriptorSetLayout mipDescriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout mipPipelineLayout = VK_NULL_HANDLE;
    Pipeline mipPipeline;
    std::vector<Worker> workers;
    std::mutex workerMutex;
    std::condition_variable workerReady;
    std::mutex queueMutex;
};

void writeString(char* output, uint32_t size, const std::string& value)
{
    if (!output || size == 0)
        return;
    std::snprintf(output, size, "%s", value.c_str());
}

std::string vkError(const char* operation, VkResult result)
{
    return std::string(operation) + " failed (Vulkan error " + std::to_string(result) + ")";
}

bool check(VkResult result, const char* operation, std::string& error)
{
    if (result == VK_SUCCESS)
        return true;
    error = vkError(operation, result);
    return false;
}

VkInstance createInstance(std::string& error)
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "goDragonCooker";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "Compressonator";
    app.engineVersion = VK_MAKE_VERSION(4, 5, 0);
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (!check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance", error))
        return VK_NULL_HANDLE;
    return instance;
}

std::vector<VkPhysicalDevice> enumerateDevices(VkInstance instance, std::string& error)
{
    uint32_t count = 0;
    if (!check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices", error))
        return {};
    std::vector<VkPhysicalDevice> devices(count);
    if (count && !check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices", error))
        return {};
    return devices;
}

int deviceScore(VkPhysicalDevice device)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : 100;
    score += static_cast<int>(properties.limits.maxComputeSharedMemorySize / 1024);
    return score;
}

int selectDevice(const std::vector<VkPhysicalDevice>& devices, int requested)
{
    if (requested >= 0)
        return requested < static_cast<int>(devices.size()) ? requested : -1;
    int best = -1;
    int bestScore = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        int score = deviceScore(devices[i]);
        if (score > bestScore) {
            best = static_cast<int>(i);
            bestScore = score;
        }
    }
    return best;
}

uint32_t findMemoryType(VkPhysicalDevice device, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

bool createBuffer(
    gdc_vulkan_context_impl& context,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memoryFlags,
    Buffer& output,
    std::string& error,
    bool persistentMap = false)
{
    output = {};
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(context.device, &bufferInfo, nullptr, &output.buffer), "vkCreateBuffer", error))
        return false;
    output.size = size;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, output.buffer, &requirements);
    uint32_t memoryType = findMemoryType(context.physicalDevice, requirements.memoryTypeBits, memoryFlags);
    if (memoryType == UINT32_MAX) {
        error = "required Vulkan buffer memory type is unavailable";
        vkDestroyBuffer(context.device, output.buffer, nullptr);
        output = {};
        return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (!check(vkAllocateMemory(context.device, &allocation, nullptr, &output.memory), "vkAllocateMemory", error)) {
        vkDestroyBuffer(context.device, output.buffer, nullptr);
        output = {};
        return false;
    }
    if (!check(vkBindBufferMemory(context.device, output.buffer, output.memory, 0), "vkBindBufferMemory", error)) {
        vkDestroyBuffer(context.device, output.buffer, nullptr);
        vkFreeMemory(context.device, output.memory, nullptr);
        output = {};
        return false;
    }
    if (persistentMap
        && !check(vkMapMemory(context.device, output.memory, 0, size, 0, &output.mapped), "vkMapMemory", error)) {
        vkDestroyBuffer(context.device, output.buffer, nullptr);
        vkFreeMemory(context.device, output.memory, nullptr);
        output = {};
        return false;
    }
    return true;
}

void destroyBuffer(VkDevice device, Buffer& buffer)
{
    if (buffer.mapped)
        vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer)
        vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}

bool createImage(
    gdc_vulkan_context_impl& context,
    uint32_t width,
    uint32_t height,
    uint32_t levels,
    VkImageUsageFlags usage,
    Image& output,
    std::string& error)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    info.extent = {width, height, 1};
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateImage(context.device, &info, nullptr, &output.image), "vkCreateImage", error))
        return false;
    output.width = width;
    output.height = height;
    output.levels = levels;

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context.device, output.image, &requirements);
    uint32_t memoryType = findMemoryType(
        context.physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == UINT32_MAX) {
        error = "device-local Vulkan image memory is unavailable";
        vkDestroyImage(context.device, output.image, nullptr);
        output = {};
        return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (!check(vkAllocateMemory(context.device, &allocation, nullptr, &output.memory), "vkAllocateMemory", error)) {
        vkDestroyImage(context.device, output.image, nullptr);
        output = {};
        return false;
    }
    if (!check(vkBindImageMemory(context.device, output.image, output.memory, 0), "vkBindImageMemory", error)) {
        vkDestroyImage(context.device, output.image, nullptr);
        vkFreeMemory(context.device, output.memory, nullptr);
        output = {};
        return false;
    }
    return true;
}

void destroyImage(VkDevice device, Image& image)
{
    if (image.image)
        vkDestroyImage(device, image.image, nullptr);
    if (image.memory)
        vkFreeMemory(device, image.memory, nullptr);
    image = {};
}

void imageBarrier(
    VkCommandBuffer commandBuffer,
    VkImage image,
    uint32_t baseLevel,
    uint32_t levelCount,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkAccessFlags sourceAccess,
    VkAccessFlags destinationAccess,
    VkPipelineStageFlags sourceStage,
    VkPipelineStageFlags destinationStage)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseLevel, levelCount, 0, 1};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    vkCmdPipelineBarrier(
        commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

uint32_t nearestPowerOfTwo(uint32_t value)
{
    if (value <= 1)
        return 1;
    uint32_t upper = 1;
    while (upper < value && upper < (1u << 30))
        upper <<= 1;
    uint32_t lower = upper >> 1;
    return value - lower < upper - value ? lower : upper;
}

uint32_t mipCount(uint32_t width, uint32_t height)
{
    uint32_t count = 1;
    while (width > 1 || height > 1) {
        width = std::max(1u, width >> 1);
        height = std::max(1u, height >> 1);
        ++count;
    }
    return count;
}

uint32_t blockSize(uint32_t format)
{
    return format == GDC_BC4_UNORM ? 8 : 16;
}

uint32_t blocksPerGroup(uint32_t format)
{
    return format == GDC_BC6H_UF16 || format == GDC_BC7_UNORM
            || format == GDC_BC7_UNORM_SRGB
        ? 64
        : 4;
}

uint32_t pipelineIndex(uint32_t format)
{
    switch (format) {
    case GDC_BC4_UNORM:
        return 0;
    case GDC_BC5_UNORM:
        return 1;
    case GDC_BC6H_UF16:
        return 2;
    default:
        return 3;
    }
}

uint32_t dxgiFormat(uint32_t format)
{
    switch (format) {
    case GDC_BC4_UNORM:
        return 80;
    case GDC_BC5_UNORM:
        return 83;
    case GDC_BC6H_UF16:
        return 95;
    case GDC_BC7_UNORM:
        return 98;
    case GDC_BC7_UNORM_SRGB:
        return 99;
    default:
        return 0;
    }
}

std::vector<uint32_t> loadShader(const std::string& path, std::string& error)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "shader not found: " + path;
        return {};
    }
    std::streamsize size = stream.tellg();
    if (size <= 0 || (size & 3) != 0) {
        error = "invalid SPIR-V shader: " + path;
        return {};
    }
    stream.seekg(0);
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    if (!stream.read(reinterpret_cast<char*>(words.data()), size)) {
        error = "failed to read shader: " + path;
        return {};
    }
    return words;
}

bool createPipelines(gdc_vulkan_context_impl& context, const std::string& directory, std::string& error)
{
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptorInfo.pBindings = bindings.data();
    if (!check(
            vkCreateDescriptorSetLayout(
                context.device, &descriptorInfo, nullptr, &context.descriptorLayout),
            "vkCreateDescriptorSetLayout",
            error))
        return false;

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &context.descriptorLayout;
    if (!check(
            vkCreatePipelineLayout(context.device, &layoutInfo, nullptr, &context.pipelineLayout),
            "vkCreatePipelineLayout",
            error))
        return false;

    const std::array<const char*, 4> names{"bc4.spv", "bc5.spv", "bc6.spv", "bc7.spv"};
    for (size_t i = 0; i < names.size(); ++i) {
        std::string path = directory + "/" + names[i];
        auto words = loadShader(path, error);
        if (words.empty())
            return false;
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = words.size() * sizeof(uint32_t);
        shaderInfo.pCode = words.data();
        if (!check(
                vkCreateShaderModule(context.device, &shaderInfo, nullptr, &context.pipelines[i].shader),
                "vkCreateShaderModule",
                error))
            return false;
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = context.pipelines[i].shader;
        pipelineInfo.stage.pName = "EncodeBlocks";
        pipelineInfo.layout = context.pipelineLayout;
        if (!check(
                vkCreateComputePipelines(
                    context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &context.pipelines[i].pipeline),
                "vkCreateComputePipelines",
                error))
            return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 3> mipBindings{};
    mipBindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    mipBindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    mipBindings[2] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    descriptorInfo.bindingCount = static_cast<uint32_t>(mipBindings.size());
    descriptorInfo.pBindings = mipBindings.data();
    if (!check(
            vkCreateDescriptorSetLayout(
                context.device, &descriptorInfo, nullptr, &context.mipDescriptorLayout),
            "vkCreateDescriptorSetLayout",
            error))
        return false;
    layoutInfo.pSetLayouts = &context.mipDescriptorLayout;
    if (!check(
            vkCreatePipelineLayout(context.device, &layoutInfo, nullptr, &context.mipPipelineLayout),
            "vkCreatePipelineLayout",
            error))
        return false;
    auto mipWords = loadShader(directory + "/mipmap.spv", error);
    if (mipWords.empty())
        return false;
    VkShaderModuleCreateInfo mipShaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mipShaderInfo.codeSize = mipWords.size() * sizeof(uint32_t);
    mipShaderInfo.pCode = mipWords.data();
    if (!check(
            vkCreateShaderModule(
                context.device, &mipShaderInfo, nullptr, &context.mipPipeline.shader),
            "vkCreateShaderModule",
            error))
        return false;
    VkComputePipelineCreateInfo mipPipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    mipPipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    mipPipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    mipPipelineInfo.stage.module = context.mipPipeline.shader;
    mipPipelineInfo.stage.pName = "GenerateMip";
    mipPipelineInfo.layout = context.mipPipelineLayout;
    if (!check(
            vkCreateComputePipelines(
                context.device,
                VK_NULL_HANDLE,
                1,
                &mipPipelineInfo,
                nullptr,
                &context.mipPipeline.pipeline),
            "vkCreateComputePipelines",
            error))
        return false;
    return true;
}

bool createWorkers(gdc_vulkan_context_impl& context, uint32_t count, std::string& error)
{
    context.workers.resize(std::clamp(count, 1u, kMaxWorkers));
    for (Worker& worker : context.workers) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = context.queueFamily;
        if (!check(
                vkCreateCommandPool(context.device, &poolInfo, nullptr, &worker.commandPool),
                "vkCreateCommandPool",
                error))
            return false;
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = worker.commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        if (!check(
                vkAllocateCommandBuffers(context.device, &commandInfo, &worker.commandBuffer),
                "vkAllocateCommandBuffers",
                error))
            return false;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!check(vkCreateFence(context.device, &fenceInfo, nullptr, &worker.fence), "vkCreateFence", error))
            return false;
        std::array<VkDescriptorPoolSize, 4> sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32}};
        VkDescriptorPoolCreateInfo descriptorPoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorPoolInfo.maxSets = 64;
        descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        descriptorPoolInfo.pPoolSizes = sizes.data();
        if (!check(
                vkCreateDescriptorPool(
                    context.device, &descriptorPoolInfo, nullptr, &worker.descriptorPool),
                "vkCreateDescriptorPool",
                error))
            return false;
    }
    return true;
}

Worker& acquireWorker(gdc_vulkan_context_impl& context)
{
    std::unique_lock lock(context.workerMutex);
    Worker* available = nullptr;
    context.workerReady.wait(lock, [&] {
        auto it = std::find_if(
            context.workers.begin(), context.workers.end(), [](const Worker& worker) {
                return !worker.busy;
            });
        if (it == context.workers.end())
            return false;
        available = &*it;
        return true;
    });
    available->busy = true;
    return *available;
}

void releaseWorker(gdc_vulkan_context_impl& context, Worker& worker)
{
    {
        std::lock_guard lock(context.workerMutex);
        worker.busy = false;
    }
    context.workerReady.notify_one();
}

struct WorkerGuard {
    gdc_vulkan_context_impl& context;
    Worker& worker;
    ~WorkerGuard() { releaseWorker(context, worker); }
};

VkDeviceSize growCapacity(VkDeviceSize required)
{
    VkDeviceSize capacity = 256;
    while (capacity < required)
        capacity *= 2;
    return capacity;
}

bool ensureBuffer(
    gdc_vulkan_context_impl& context,
    Buffer& buffer,
    VkDeviceSize required,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memoryFlags,
    bool persistentMap,
    std::string& error)
{
    if (buffer.size >= required)
        return true;
    destroyBuffer(context.device, buffer);
    return createBuffer(
        context, growCapacity(required), usage, memoryFlags, buffer, error, persistentMap);
}

void destroyWorkerImages(gdc_vulkan_context_impl& context, Worker& worker)
{
    if (worker.sourceView)
        vkDestroyImageView(context.device, worker.sourceView, nullptr);
    worker.sourceView = VK_NULL_HANDLE;
    for (VkImageView view : worker.mipViews)
        if (view)
            vkDestroyImageView(context.device, view, nullptr);
    worker.mipViews.clear();
    destroyImage(context.device, worker.sourceImage);
    destroyImage(context.device, worker.mipImage);
    worker.imagesInitialized = false;
}

bool createView(
    gdc_vulkan_context_impl& context,
    VkImage image,
    uint32_t level,
    VkImageView& view,
    std::string& error)
{
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    return check(
        vkCreateImageView(context.device, &info, nullptr, &view),
        "vkCreateImageView",
        error);
}

bool ensureImages(
    gdc_vulkan_context_impl& context,
    Worker& worker,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    uint32_t width,
    uint32_t height,
    uint32_t levels,
    std::string& error)
{
    if (worker.sourceImage.width == sourceWidth
        && worker.sourceImage.height == sourceHeight
        && worker.mipImage.width == width
        && worker.mipImage.height == height
        && worker.mipImage.levels == levels)
        return true;

    destroyWorkerImages(context, worker);
    if (!createImage(
            context,
            sourceWidth,
            sourceHeight,
            1,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            worker.sourceImage,
            error)
        || !createImage(
            context,
            width,
            height,
            levels,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            worker.mipImage,
            error)
        || !createView(context, worker.sourceImage.image, 0, worker.sourceView, error)) {
        destroyWorkerImages(context, worker);
        return false;
    }
    worker.mipViews.resize(levels, VK_NULL_HANDLE);
    for (uint32_t level = 0; level < levels; ++level) {
        if (!createView(
                context,
                worker.mipImage.image,
                level,
                worker.mipViews[level],
                error)) {
            destroyWorkerImages(context, worker);
            return false;
        }
    }
    return true;
}

bool ensureDescriptorSets(
    gdc_vulkan_context_impl& context,
    Worker& worker,
    uint32_t levels,
    std::string& error)
{
    if (worker.descriptorCapacity >= levels)
        return true;
    if (!check(
            vkResetDescriptorPool(context.device, worker.descriptorPool, 0),
            "vkResetDescriptorPool",
            error))
        return false;
    worker.descriptorCapacity = 0;
    worker.descriptorSets.clear();
    worker.mipDescriptorSets.clear();

    std::vector<VkDescriptorSet> descriptorSets(levels);
    std::vector<VkDescriptorSet> mipDescriptorSets(levels);
    std::vector<VkDescriptorSetLayout> layouts(levels, context.descriptorLayout);
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = worker.descriptorPool;
    info.descriptorSetCount = levels;
    info.pSetLayouts = layouts.data();
    if (!check(
            vkAllocateDescriptorSets(context.device, &info, descriptorSets.data()),
            "vkAllocateDescriptorSets",
            error))
        return false;
    std::fill(layouts.begin(), layouts.end(), context.mipDescriptorLayout);
    info.pSetLayouts = layouts.data();
    if (!check(
            vkAllocateDescriptorSets(context.device, &info, mipDescriptorSets.data()),
            "vkAllocateDescriptorSets",
            error))
        return false;
    worker.descriptorSets = std::move(descriptorSets);
    worker.mipDescriptorSets = std::move(mipDescriptorSets);
    worker.descriptorCapacity = levels;
    return true;
}

bool writeDDS(
    const char* path,
    uint32_t width,
    uint32_t height,
    uint32_t levels,
    uint32_t format,
    const void* data,
    size_t dataSize,
    std::string& error)
{
    struct PixelFormat {
        uint32_t size, flags, fourCC, rgbBits, rMask, gMask, bMask, aMask;
    };
    struct Header {
        uint32_t size, flags, height, width, pitch, depth, mipCount, reserved[11];
        PixelFormat pixelFormat;
        uint32_t caps, caps2, caps3, caps4, reserved2;
    };
    struct DX10 {
        uint32_t format, dimension, miscFlag, arraySize, miscFlags2;
    };
    Header header{};
    header.size = 124;
    header.flags = 0x00021007;
    header.height = height;
    header.width = width;
    header.pitch = std::max(1u, (width + 3) / 4) * blockSize(format);
    header.mipCount = levels;
    header.pixelFormat = {32, 0x4, 0x30315844, 0, 0, 0, 0, 0};
    header.caps = 0x401008;
    DX10 dx10{dxgiFormat(format), 3, 0, 1, 0};

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    uint32_t magic = 0x20534444;
    output.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&dx10), sizeof(dx10));
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(dataSize));
    if (!output) {
        error = std::string("failed to write DDS: ") + path;
        return false;
    }
    return true;
}

bool loadTiff(
    const char* path,
    int& width,
    int& height,
    std::vector<float>& pixels,
    std::string& error)
{
    TIFF* file = TIFFOpen(path, "r");
    if (!file) {
        error = std::string("failed to open TIFF: ") + path;
        return false;
    }
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    TIFFGetField(file, TIFFTAG_IMAGEWIDTH, &imageWidth);
    TIFFGetField(file, TIFFTAG_IMAGELENGTH, &imageHeight);
    if (imageWidth == 0 || imageHeight == 0) {
        TIFFClose(file);
        error = "TIFF has invalid dimensions";
        return false;
    }
    std::vector<uint32_t> rgba(static_cast<size_t>(imageWidth) * imageHeight);
    if (!TIFFReadRGBAImageOriented(
            file, imageWidth, imageHeight, rgba.data(), ORIENTATION_TOPLEFT, 0)) {
        TIFFClose(file);
        error = "failed to decode TIFF pixels";
        return false;
    }
    TIFFClose(file);
    width = static_cast<int>(imageWidth);
    height = static_cast<int>(imageHeight);
    pixels.resize(rgba.size() * 4);
    for (size_t i = 0; i < rgba.size(); ++i) {
        pixels[i * 4] = TIFFGetR(rgba[i]) / 255.0f;
        pixels[i * 4 + 1] = TIFFGetG(rgba[i]) / 255.0f;
        pixels[i * 4 + 2] = TIFFGetB(rgba[i]) / 255.0f;
        pixels[i * 4 + 3] = TIFFGetA(rgba[i]) / 255.0f;
    }
    return true;
}

bool loadExr(
    const char* path,
    int& width,
    int& height,
    std::vector<float>& pixels,
    std::string& error)
{
    try {
        OPENEXR_IMF_NAMESPACE::RgbaInputFile file(path);
        IMATH_NAMESPACE::Box2i window = file.dataWindow();
        width = window.max.x - window.min.x + 1;
        height = window.max.y - window.min.y + 1;
        if (width <= 0 || height <= 0) {
            error = "EXR has invalid dimensions";
            return false;
        }
        std::vector<OPENEXR_IMF_NAMESPACE::Rgba> rgba(
            static_cast<size_t>(width) * height);
        file.setFrameBuffer(
            rgba.data() - window.min.x - static_cast<ptrdiff_t>(window.min.y) * width,
            1,
            width);
        file.readPixels(window.min.y, window.max.y);
        pixels.resize(rgba.size() * 4);
        for (size_t i = 0; i < rgba.size(); ++i) {
            pixels[i * 4] = static_cast<float>(rgba[i].r);
            pixels[i * 4 + 1] = static_cast<float>(rgba[i].g);
            pixels[i * 4 + 2] = static_cast<float>(rgba[i].b);
            pixels[i * 4 + 3] = static_cast<float>(rgba[i].a);
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("failed to decode EXR: ") + exception.what();
        return false;
    }
}

bool loadPixels(
    const char* path,
    int& width,
    int& height,
    std::vector<float>& pixels,
    std::string& error)
{
    std::string extension = path;
    size_t dot = extension.find_last_of('.');
    extension = dot == std::string::npos ? "" : extension.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension == ".tif" || extension == ".tiff")
        return loadTiff(path, width, height, pixels, error);
    if (extension == ".exr")
        return loadExr(path, width, height, pixels, error);

    if (stbi_is_hdr(path)) {
        float* decoded = stbi_loadf(path, &width, &height, nullptr, 4);
        if (!decoded) {
            error = std::string("failed to load source image: ") + stbi_failure_reason();
            return false;
        }
        size_t count = static_cast<size_t>(width) * height * 4;
        pixels.assign(decoded, decoded + count);
        stbi_image_free(decoded);
        return true;
    }

    stbi_uc* decoded = stbi_load(path, &width, &height, nullptr, 4);
    if (!decoded) {
        error = std::string("failed to load source image: ") + stbi_failure_reason();
        return false;
    }
    size_t count = static_cast<size_t>(width) * height * 4;
    pixels.resize(count);
    for (size_t i = 0; i < count; ++i)
        pixels[i] = decoded[i] / 255.0f;
    stbi_image_free(decoded);
    return true;
}

struct CompressionOutput {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
    std::vector<uint8_t> bytes;
};

struct LevelInfo {
    uint32_t width;
    uint32_t height;
    uint32_t blocksX;
    uint32_t blocksY;
    VkDeviceSize offset;
    VkDeviceSize size;
    VkDeviceSize storageSize;
};

bool compressWithWorker(
    gdc_vulkan_context_impl& context,
    Worker& worker,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    const std::vector<float>& pixels,
    uint32_t format,
    uint32_t flags,
    CompressionOutput& result,
    std::string& error)
{
    const uint32_t width = nearestPowerOfTwo(sourceWidth);
    const uint32_t height = nearestPowerOfTwo(sourceHeight);
    const uint32_t levels = mipCount(width, height);
    std::vector<LevelInfo> levelInfo(levels);

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.physicalDevice, &properties);
    const VkDeviceSize storageAlignment =
        std::max<VkDeviceSize>(1, properties.limits.minStorageBufferOffsetAlignment);
    const VkDeviceSize uniformAlignment =
        std::max<VkDeviceSize>(1, properties.limits.minUniformBufferOffsetAlignment);
    const VkDeviceSize constantsStride =
        (sizeof(Constants) + uniformAlignment - 1) & ~(uniformAlignment - 1);
    const VkDeviceSize mipConstantsStride =
        (sizeof(MipConstants) + uniformAlignment - 1) & ~(uniformAlignment - 1);

    VkDeviceSize outputSize = 0;
    size_t ddsSize = 0;
    for (uint32_t level = 0, w = width, h = height; level < levels; ++level) {
        outputSize = (outputSize + storageAlignment - 1) & ~(storageAlignment - 1);
        LevelInfo& info = levelInfo[level];
        info.width = w;
        info.height = h;
        info.blocksX = std::max(1u, (w + 3) / 4);
        info.blocksY = std::max(1u, (h + 3) / 4);
        const uint32_t blocks = info.blocksX * info.blocksY;
        const uint32_t groupSize = blocksPerGroup(format);
        const uint32_t storageBlocks =
            ((blocks + groupSize - 1) / groupSize) * groupSize;
        info.offset = outputSize;
        info.size = static_cast<VkDeviceSize>(blocks) * blockSize(format);
        info.storageSize =
            static_cast<VkDeviceSize>(storageBlocks) * blockSize(format);
        outputSize += info.storageSize;
        ddsSize += static_cast<size_t>(info.size);
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
    }

    const VkDeviceSize sourceBytes =
        static_cast<VkDeviceSize>(pixels.size() * sizeof(float));
    const VkMemoryPropertyFlags hostMemory =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!ensureBuffer(
            context,
            worker.staging,
            sourceBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            hostMemory,
            true,
            error)
        || !ensureBuffer(
            context,
            worker.output,
            outputSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            false,
            error)
        || !ensureBuffer(
            context,
            worker.readback,
            outputSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            hostMemory,
            true,
            error)
        || !ensureBuffer(
            context,
            worker.dummy,
            std::max<VkDeviceSize>(32, levelInfo[0].storageSize * 2),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            hostMemory,
            true,
            error)
        || !ensureBuffer(
            context,
            worker.constants,
            constantsStride * levels,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            hostMemory,
            true,
            error)
        || !ensureBuffer(
            context,
            worker.mipConstants,
            mipConstantsStride * levels,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            hostMemory,
            true,
            error)
        || !ensureImages(
            context,
            worker,
            sourceWidth,
            sourceHeight,
            width,
            height,
            levels,
            error)
        || !ensureDescriptorSets(context, worker, levels, error))
        return false;

    std::memcpy(worker.staging.mapped, pixels.data(), static_cast<size_t>(sourceBytes));
    std::memset(worker.dummy.mapped, 0, static_cast<size_t>(worker.dummy.size));

    for (uint32_t level = 0; level < levels; ++level) {
        const LevelInfo& info = levelInfo[level];
        const Constants values{
            info.width,
            info.blocksX,
            format,
            0,
            0,
            info.blocksX * info.blocksY,
            1.0f,
            1.0f};
        std::memcpy(
            static_cast<uint8_t*>(worker.constants.mapped) + constantsStride * level,
            &values,
            sizeof(values));

        const uint32_t sourceLevelWidth =
            level == 0 ? sourceWidth : levelInfo[level - 1].width;
        const uint32_t sourceLevelHeight =
            level == 0 ? sourceHeight : levelInfo[level - 1].height;
        uint32_t mipFlags = flags;
        if (flags & GDC_FLAG_IGNORE_SRGB)
            mipFlags &= ~(GDC_FLAG_INPUT_SRGB | GDC_FLAG_OUTPUT_SRGB);
        else if (level > 0) {
            mipFlags &= ~GDC_FLAG_INPUT_SRGB;
            if (flags & GDC_FLAG_OUTPUT_SRGB)
                mipFlags |= GDC_FLAG_INPUT_SRGB;
        }
        const MipConstants mipValues{
            sourceLevelWidth, sourceLevelHeight, info.width, info.height, mipFlags};
        std::memcpy(
            static_cast<uint8_t*>(worker.mipConstants.mapped) + mipConstantsStride * level,
            &mipValues,
            sizeof(mipValues));

        const VkDescriptorBufferInfo constantsInfo{
            worker.constants.buffer, constantsStride * level, sizeof(Constants)};
        const VkDescriptorImageInfo imageInfo{
            VK_NULL_HANDLE,
            worker.mipViews[level],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo dummyInfo{
            worker.dummy.buffer, 0, worker.dummy.size};
        const VkDescriptorBufferInfo outputInfo{
            worker.output.buffer, info.offset, info.storageSize};
        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            worker.descriptorSets[level], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            nullptr, &constantsInfo, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            worker.descriptorSets[level], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            &imageInfo, nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            worker.descriptorSets[level], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr, &dummyInfo, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            worker.descriptorSets[level], 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr, &outputInfo, nullptr};
        vkUpdateDescriptorSets(
            context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        const VkDescriptorBufferInfo mipConstantsInfo{
            worker.mipConstants.buffer, mipConstantsStride * level, sizeof(MipConstants)};
        const VkDescriptorImageInfo mipInputInfo{
            VK_NULL_HANDLE,
            level == 0 ? worker.sourceView : worker.mipViews[level - 1],
            level == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo mipOutputInfo{
            VK_NULL_HANDLE, worker.mipViews[level], VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 3> mipWrites{};
        mipWrites[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            worker.mipDescriptorSets[level], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            nullptr, &mipConstantsInfo, nullptr};
        mipWrites[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            worker.mipDescriptorSets[level], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            &mipInputInfo, nullptr, nullptr};
        mipWrites[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            worker.mipDescriptorSets[level], 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            &mipOutputInfo, nullptr, nullptr};
        vkUpdateDescriptorSets(
            context.device,
            static_cast<uint32_t>(mipWrites.size()),
            mipWrites.data(),
            0,
            nullptr);
    }

    if (!check(
            vkResetCommandPool(context.device, worker.commandPool, 0),
            "vkResetCommandPool",
            error))
        return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(vkBeginCommandBuffer(worker.commandBuffer, &begin), "vkBeginCommandBuffer", error))
        return false;

    const bool initialized = worker.imagesInitialized;
    imageBarrier(
        worker.commandBuffer,
        worker.sourceImage.image,
        0,
        1,
        initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        initialized ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy sourceCopy{};
    sourceCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    sourceCopy.imageExtent = {sourceWidth, sourceHeight, 1};
    vkCmdCopyBufferToImage(
        worker.commandBuffer,
        worker.staging.buffer,
        worker.sourceImage.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &sourceCopy);
    imageBarrier(
        worker.commandBuffer,
        worker.sourceImage.image,
        0,
        1,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    imageBarrier(
        worker.commandBuffer,
        worker.mipImage.image,
        0,
        levels,
        initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        initialized ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkBufferMemoryBarrier outputReuseBarrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    outputReuseBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    outputReuseBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputReuseBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputReuseBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputReuseBarrier.buffer = worker.output.buffer;
    outputReuseBarrier.offset = 0;
    outputReuseBarrier.size = outputSize;
    vkCmdPipelineBarrier(
        worker.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        1,
        &outputReuseBarrier,
        0,
        nullptr);

    vkCmdBindPipeline(
        worker.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, context.mipPipeline.pipeline);
    for (uint32_t level = 0; level < levels; ++level) {
        const LevelInfo& info = levelInfo[level];
        vkCmdBindDescriptorSets(
            worker.commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            context.mipPipelineLayout,
            0,
            1,
            &worker.mipDescriptorSets[level],
            0,
            nullptr);
        vkCmdDispatch(
            worker.commandBuffer, (info.width + 7) / 8, (info.height + 7) / 8, 1);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = worker.mipImage.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        vkCmdPipelineBarrier(
            worker.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
    }
    imageBarrier(
        worker.commandBuffer,
        worker.mipImage.image,
        0,
        levels,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(
        worker.commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        context.pipelines[pipelineIndex(format)].pipeline);
    for (uint32_t level = 0; level < levels; ++level) {
        const LevelInfo& info = levelInfo[level];
        const uint32_t totalBlocks = info.blocksX * info.blocksY;
        vkCmdBindDescriptorSets(
            worker.commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            context.pipelineLayout,
            0,
            1,
            &worker.descriptorSets[level],
            0,
            nullptr);
        const uint32_t groupSize = blocksPerGroup(format);
        vkCmdDispatch(
            worker.commandBuffer, (totalBlocks + groupSize - 1) / groupSize, 1, 1);
    }

    VkBufferMemoryBarrier outputBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    outputBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    outputBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.buffer = worker.output.buffer;
    outputBarrier.offset = 0;
    outputBarrier.size = outputSize;
    vkCmdPipelineBarrier(
        worker.commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        1,
        &outputBarrier,
        0,
        nullptr);
    const VkBufferCopy outputCopy{0, 0, outputSize};
    vkCmdCopyBuffer(
        worker.commandBuffer, worker.output.buffer, worker.readback.buffer, 1, &outputCopy);
    VkBufferMemoryBarrier readbackBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    readbackBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    readbackBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readbackBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readbackBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readbackBarrier.buffer = worker.readback.buffer;
    readbackBarrier.offset = 0;
    readbackBarrier.size = outputSize;
    vkCmdPipelineBarrier(
        worker.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0,
        nullptr,
        1,
        &readbackBarrier,
        0,
        nullptr);
    if (!check(vkEndCommandBuffer(worker.commandBuffer), "vkEndCommandBuffer", error))
        return false;

    if (!check(vkResetFences(context.device, 1, &worker.fence), "vkResetFences", error))
        return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &worker.commandBuffer;
    {
        std::lock_guard lock(context.queueMutex);
        if (!check(
                vkQueueSubmit(context.queue, 1, &submit, worker.fence),
                "vkQueueSubmit",
                error))
            return false;
    }
    const VkResult waitResult =
        vkWaitForFences(context.device, 1, &worker.fence, VK_TRUE, 120000000000ull);
    if (waitResult != VK_SUCCESS) {
        {
            std::lock_guard lock(context.queueMutex);
            vkQueueWaitIdle(context.queue);
        }
        check(waitResult, "vkWaitForFences", error);
        return false;
    }
    worker.imagesInitialized = true;

    result.width = width;
    result.height = height;
    result.levels = levels;
    result.bytes.resize(ddsSize);
    size_t destinationOffset = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        const LevelInfo& info = levelInfo[level];
        std::memcpy(
            result.bytes.data() + destinationOffset,
            static_cast<const uint8_t*>(worker.readback.mapped) + info.offset,
            static_cast<size_t>(info.size));
        destinationOffset += static_cast<size_t>(info.size);
    }
    return true;
}

} // namespace

struct gdc_vulkan_context {
    gdc_vulkan_context_impl implementation;
};

extern "C" {

const char* gdc_vulkan_version(void)
{
    return kVersion;
}

int gdc_vulkan_device_count(char* error, uint32_t errorSize)
{
    std::string message;
    VkInstance instance = createInstance(message);
    if (!instance) {
        writeString(error, errorSize, message);
        return -1;
    }
    auto devices = enumerateDevices(instance, message);
    vkDestroyInstance(instance, nullptr);
    if (!message.empty()) {
        writeString(error, errorSize, message);
        return -1;
    }
    return static_cast<int>(devices.size());
}

int gdc_vulkan_device_name(uint32_t index, char* name, uint32_t nameSize)
{
    std::string error;
    VkInstance instance = createInstance(error);
    if (!instance)
        return -1;
    auto devices = enumerateDevices(instance, error);
    if (index >= devices.size()) {
        vkDestroyInstance(instance, nullptr);
        return -2;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(devices[index], &properties);
    writeString(name, nameSize, properties.deviceName);
    vkDestroyInstance(instance, nullptr);
    return 0;
}

gdc_vulkan_context* gdc_vulkan_create(
    int32_t deviceIndex,
    const char* shaderDirectory,
    uint32_t workerCount,
    char* error,
    uint32_t errorSize)
{
    auto result = std::make_unique<gdc_vulkan_context>();
    auto& context = result->implementation;
    std::string message;
    context.instance = createInstance(message);
    if (!context.instance)
        goto failed;
    {
        auto devices = enumerateDevices(context.instance, message);
        int selected = selectDevice(devices, deviceIndex);
        if (selected < 0) {
            message = devices.empty() ? "no Vulkan physical devices found" : "invalid Vulkan device index";
            goto failed;
        }
        context.physicalDevice = devices[static_cast<size_t>(selected)];
    }
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &count, families.data());
        context.queueFamily = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                context.queueFamily = i;
                break;
            }
        }
        if (context.queueFamily == UINT32_MAX) {
            message = "selected Vulkan device has no compute queue";
            goto failed;
        }
    }
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = context.queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        if (!check(vkCreateDevice(context.physicalDevice, &deviceInfo, nullptr, &context.device), "vkCreateDevice", message))
            goto failed;
        vkGetDeviceQueue(context.device, context.queueFamily, 0, &context.queue);
    }
    if (!shaderDirectory || !createPipelines(context, shaderDirectory, message)
        || !createWorkers(context, workerCount, message))
        goto failed;
    return result.release();

failed:
    writeString(error, errorSize, message);
    gdc_vulkan_destroy(result.release());
    return nullptr;
}

int gdc_vulkan_context_device_name(gdc_vulkan_context* opaque, char* name, uint32_t nameSize)
{
    if (!opaque || !opaque->implementation.physicalDevice)
        return -1;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(opaque->implementation.physicalDevice, &properties);
    writeString(name, nameSize, properties.deviceName);
    return 0;
}

void gdc_vulkan_destroy(gdc_vulkan_context* opaque)
{
    if (!opaque)
        return;
    auto& context = opaque->implementation;
    if (context.device)
        vkDeviceWaitIdle(context.device);
    for (Worker& worker : context.workers) {
        destroyWorkerImages(context, worker);
        destroyBuffer(context.device, worker.mipConstants);
        destroyBuffer(context.device, worker.constants);
        destroyBuffer(context.device, worker.dummy);
        destroyBuffer(context.device, worker.readback);
        destroyBuffer(context.device, worker.output);
        destroyBuffer(context.device, worker.staging);
        if (worker.descriptorPool)
            vkDestroyDescriptorPool(context.device, worker.descriptorPool, nullptr);
        if (worker.fence)
            vkDestroyFence(context.device, worker.fence, nullptr);
        if (worker.commandPool)
            vkDestroyCommandPool(context.device, worker.commandPool, nullptr);
    }
    for (Pipeline& pipeline : context.pipelines) {
        if (pipeline.pipeline)
            vkDestroyPipeline(context.device, pipeline.pipeline, nullptr);
        if (pipeline.shader)
            vkDestroyShaderModule(context.device, pipeline.shader, nullptr);
    }
    if (context.mipPipeline.pipeline)
        vkDestroyPipeline(context.device, context.mipPipeline.pipeline, nullptr);
    if (context.mipPipeline.shader)
        vkDestroyShaderModule(context.device, context.mipPipeline.shader, nullptr);
    if (context.mipPipelineLayout)
        vkDestroyPipelineLayout(context.device, context.mipPipelineLayout, nullptr);
    if (context.mipDescriptorLayout)
        vkDestroyDescriptorSetLayout(context.device, context.mipDescriptorLayout, nullptr);
    if (context.pipelineLayout)
        vkDestroyPipelineLayout(context.device, context.pipelineLayout, nullptr);
    if (context.descriptorLayout)
        vkDestroyDescriptorSetLayout(context.device, context.descriptorLayout, nullptr);
    if (context.device)
        vkDestroyDevice(context.device, nullptr);
    if (context.instance)
        vkDestroyInstance(context.instance, nullptr);
    delete opaque;
}

int gdc_vulkan_compress(
    gdc_vulkan_context* opaque,
    const char* sourcePath,
    const char* destinationPath,
    uint32_t format,
    uint32_t flags,
    char* error,
    uint32_t errorSize)
{
    if (!opaque || !sourcePath || !destinationPath) {
        writeString(error, errorSize, "invalid Vulkan compression arguments");
        return -1;
    }
    if (dxgiFormat(format) == 0) {
        writeString(error, errorSize, "unsupported BC target format");
        return -2;
    }

    int sourceWidth = 0;
    int sourceHeight = 0;
    std::vector<float> pixels;
    std::string message;
    if (!loadPixels(sourcePath, sourceWidth, sourceHeight, pixels, message)
        || sourceWidth <= 0
        || sourceHeight <= 0) {
        if (message.empty())
            message = "source image has invalid dimensions";
        writeString(error, errorSize, message);
        return -2;
    }

    auto& context = opaque->implementation;
    Worker& worker = acquireWorker(context);
    CompressionOutput output;
    bool compressed = false;
    {
        WorkerGuard guard{context, worker};
        compressed = compressWithWorker(
            context,
            worker,
            static_cast<uint32_t>(sourceWidth),
            static_cast<uint32_t>(sourceHeight),
            pixels,
            format,
            flags,
            output,
            message);
    }
    if (!compressed) {
        writeString(error, errorSize, message);
        return -2;
    }

    if (!writeDDS(
            destinationPath,
            output.width,
            output.height,
            output.levels,
            format,
            output.bytes.data(),
            output.bytes.size(),
            message)) {
        writeString(error, errorSize, message);
        return -2;
    }
    if (error && errorSize)
        error[0] = '\0';
    return 0;
}

} // extern "C"
