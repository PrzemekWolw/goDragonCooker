#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
static HMODULE vk_codec_handle;
#else
static void* vk_codec_handle;
#endif

typedef void gdc_context;
typedef const char* (*version_fn)(void);
typedef int (*device_count_fn)(char*, uint32_t);
typedef int (*device_name_fn)(uint32_t, char*, uint32_t);
typedef gdc_context* (*create_fn)(int32_t, const char*, uint32_t, char*, uint32_t);
typedef int (*context_device_name_fn)(gdc_context*, char*, uint32_t);
typedef void (*destroy_fn)(gdc_context*);
typedef int (*compress_fn)(gdc_context*, const char*, const char*, uint32_t, uint32_t, char*, uint32_t);

static version_fn codec_version;
static device_count_fn codec_device_count;
static device_name_fn codec_device_name;
static create_fn codec_create;
static context_device_name_fn codec_context_device_name;
static destroy_fn codec_destroy;
static compress_fn codec_compress;

static void set_load_error(char* error, uint32_t error_size, const char* message)
{
    if (!error || error_size == 0)
        return;
    snprintf(error, error_size, "%s", message ? message : "");
}

#if defined(_WIN32)
static wchar_t* utf8_to_wide(const char* value)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (length <= 0)
        return NULL;
    wchar_t* result = (wchar_t*)malloc((size_t)length * sizeof(wchar_t));
    if (!result)
        return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, length)) {
        free(result);
        return NULL;
    }
    return result;
}

static void set_windows_load_error(char* error, uint32_t error_size, DWORD code)
{
    if (!error || error_size == 0)
        return;
    wchar_t message[512] = {0};
    char utf8[1024] = {0};
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        code,
        0,
        message,
        (DWORD)(sizeof(message) / sizeof(message[0])),
        NULL);
    if (length > 0) {
        while (length > 0 && (message[length - 1] == L'\r' || message[length - 1] == L'\n'))
            message[--length] = L'\0';
        WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, sizeof(utf8), NULL, NULL);
    }
    if (utf8[0])
        snprintf(error, error_size, "Windows error %lu: %s", (unsigned long)code, utf8);
    else
        snprintf(error, error_size, "Windows error %lu", (unsigned long)code);
}
#endif

static void* load_symbol(const char* name)
{
#if defined(_WIN32)
    return (void*)GetProcAddress(vk_codec_handle, name);
#else
    return dlsym(vk_codec_handle, name);
#endif
}

void unload_vulkan_codec(void)
{
    if (vk_codec_handle) {
#if defined(_WIN32)
        FreeLibrary(vk_codec_handle);
#else
        dlclose(vk_codec_handle);
#endif
    }
    vk_codec_handle = 0;
    codec_version = 0;
    codec_device_count = 0;
    codec_device_name = 0;
    codec_create = 0;
    codec_context_device_name = 0;
    codec_destroy = 0;
    codec_compress = 0;
}

int load_vulkan_codec(const char* path, char* error, uint32_t error_size)
{
    unload_vulkan_codec();
    set_load_error(error, error_size, "");
#if defined(_WIN32)
    wchar_t* wide_path = utf8_to_wide(path);
    if (!wide_path) {
        set_load_error(error, error_size, "invalid library path");
        return -1;
    }
    vk_codec_handle = LoadLibraryExW(
        wide_path,
        NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    DWORD load_error = vk_codec_handle ? ERROR_SUCCESS : GetLastError();
    free(wide_path);
    if (!vk_codec_handle) {
        set_windows_load_error(error, error_size, load_error);
        return -1;
    }
#else
    dlerror();
    vk_codec_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!vk_codec_handle) {
        set_load_error(error, error_size, dlerror());
        return -1;
    }
#endif
    codec_version = (version_fn)load_symbol("gdc_vulkan_version");
    codec_device_count = (device_count_fn)load_symbol("gdc_vulkan_device_count");
    codec_device_name = (device_name_fn)load_symbol("gdc_vulkan_device_name");
    codec_create = (create_fn)load_symbol("gdc_vulkan_create");
    codec_context_device_name = (context_device_name_fn)load_symbol("gdc_vulkan_context_device_name");
    codec_destroy = (destroy_fn)load_symbol("gdc_vulkan_destroy");
    codec_compress = (compress_fn)load_symbol("gdc_vulkan_compress");
    if (!codec_version || !codec_device_count || !codec_device_name || !codec_create
        || !codec_context_device_name
        || !codec_destroy || !codec_compress) {
        set_load_error(error, error_size, "required Vulkan codec exports are missing");
        unload_vulkan_codec();
        return -2;
    }
    return 0;
}

const char* call_vulkan_version(void)
{
    return codec_version ? codec_version() : 0;
}

int call_vulkan_device_count(char* error, uint32_t error_size)
{
    return codec_device_count ? codec_device_count(error, error_size) : -99;
}

int call_vulkan_device_name(uint32_t index, char* name, uint32_t name_size)
{
    return codec_device_name ? codec_device_name(index, name, name_size) : -99;
}

gdc_context* call_vulkan_create(
    int32_t index, const char* shaders, uint32_t workers, char* error, uint32_t error_size)
{
    return codec_create ? codec_create(index, shaders, workers, error, error_size) : 0;
}

int call_vulkan_context_device_name(gdc_context* context, char* name, uint32_t name_size)
{
    return codec_context_device_name ? codec_context_device_name(context, name, name_size) : -99;
}

void call_vulkan_destroy(gdc_context* context)
{
    if (codec_destroy)
        codec_destroy(context);
}

int call_vulkan_compress(
    gdc_context* context,
    const char* source,
    const char* destination,
    uint32_t format,
    uint32_t flags,
    char* error,
    uint32_t error_size)
{
    return codec_compress
        ? codec_compress(context, source, destination, format, flags, error, error_size)
        : -99;
}
