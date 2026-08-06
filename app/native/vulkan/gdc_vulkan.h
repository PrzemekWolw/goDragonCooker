#ifndef GDC_VULKAN_H
#define GDC_VULKAN_H

#include <stdint.h>

#if defined(_WIN32)
#define GDC_API __declspec(dllexport)
#else
#define GDC_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDC_BC4_UNORM = 1,
    GDC_BC5_UNORM = 2,
    GDC_BC6H_UF16 = 3,
    GDC_BC7_UNORM = 4,
    GDC_BC7_UNORM_SRGB = 5
};

enum {
    GDC_FLAG_INPUT_SRGB = 1,
    GDC_FLAG_IGNORE_SRGB = 2,
    GDC_FLAG_SEPARATE_ALPHA = 4,
    GDC_FLAG_OUTPUT_SRGB = 8
};

typedef struct gdc_vulkan_context gdc_vulkan_context;

GDC_API const char* gdc_vulkan_version(void);
GDC_API int gdc_vulkan_device_count(char* error, uint32_t error_size);
GDC_API int gdc_vulkan_device_name(uint32_t index, char* name, uint32_t name_size);
GDC_API gdc_vulkan_context* gdc_vulkan_create(
    int32_t device_index,
    const char* shader_directory,
    uint32_t worker_count,
    char* error,
    uint32_t error_size);
GDC_API int gdc_vulkan_context_device_name(
    gdc_vulkan_context* context,
    char* name,
    uint32_t name_size);
GDC_API void gdc_vulkan_destroy(gdc_vulkan_context* context);
GDC_API int gdc_vulkan_compress(
    gdc_vulkan_context* context,
    const char* source_path,
    const char* destination_path,
    uint32_t format,
    uint32_t flags,
    char* error,
    uint32_t error_size);

#ifdef __cplusplus
}
#endif

#endif
