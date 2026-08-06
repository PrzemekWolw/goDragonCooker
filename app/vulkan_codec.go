package main

/*
#cgo linux LDFLAGS: -ldl
#include <stdint.h>
#include <stdlib.h>

typedef void gdc_context;
int load_vulkan_codec(const char *path, char *error, uint32_t error_size);
void unload_vulkan_codec(void);
const char *call_vulkan_version(void);
int call_vulkan_device_count(char *error, uint32_t error_size);
int call_vulkan_device_name(uint32_t index, char *name, uint32_t name_size);
gdc_context *call_vulkan_create(int32_t index, const char *shaders, uint32_t workers, char *error, uint32_t error_size);
int call_vulkan_context_device_name(gdc_context *context, char *name, uint32_t name_size);
void call_vulkan_destroy(gdc_context *context);
int call_vulkan_compress(gdc_context *context, const char *source, const char *destination,
                         uint32_t format, uint32_t flags, char *error, uint32_t error_size);
*/
import "C"

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"unsafe"
)

const (
	textureBackendCompressonator = "compressonator"
	textureBackendTexconv        = "texconv"
	vulkanWorkerCount            = 4
	vulkanPipelineWorkerCount    = vulkanWorkerCount * 2
)

var vulkanCodec struct {
	sync.Mutex
	contextMu   sync.RWMutex
	loaded      bool
	context     unsafe.Pointer
	deviceIndex int32
}

func vulkanInit() error {
	vulkanCodec.Lock()
	defer vulkanCodec.Unlock()
	if vulkanCodec.loaded {
		return nil
	}
	path := findVulkanCodecLib()
	if path == "" {
		return fmt.Errorf("Compressonator Vulkan library not found")
	}
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	errBuf := (*C.char)(C.calloc(1024, 1))
	defer C.free(unsafe.Pointer(errBuf))
	if result := C.load_vulkan_codec(cPath, errBuf, 1024); result != 0 {
		detail := C.GoString(errBuf)
		if detail == "" {
			detail = fmt.Sprintf("code %d", int(result))
		}
		return fmt.Errorf("failed to load Compressonator Vulkan library %s: %s", path, detail)
	}
	vulkanCodec.loaded = true
	vulkanCodec.deviceIndex = -2
	return nil
}

func vulkanVersion() string {
	if err := vulkanInit(); err != nil {
		return "unavailable"
	}
	vulkanCodec.Lock()
	defer vulkanCodec.Unlock()
	version := C.call_vulkan_version()
	if version == nil {
		return "unknown"
	}
	return C.GoString(version)
}

func vulkanDevices() ([]string, error) {
	if err := vulkanInit(); err != nil {
		return nil, err
	}
	errBuf := (*C.char)(C.calloc(1024, 1))
	defer C.free(unsafe.Pointer(errBuf))
	count := int(C.call_vulkan_device_count(errBuf, 1024))
	if count < 0 {
		return nil, fmt.Errorf("%s", C.GoString(errBuf))
	}
	devices := make([]string, 0, count)
	for index := 0; index < count; index++ {
		nameBuf := (*C.char)(C.calloc(256, 1))
		result := C.call_vulkan_device_name(C.uint32_t(index), nameBuf, 256)
		name := C.GoString(nameBuf)
		C.free(unsafe.Pointer(nameBuf))
		if result == 0 {
			devices = append(devices, name)
		}
	}
	return devices, nil
}

func ensureVulkanContext(deviceIndex int32) error {
	if err := vulkanInit(); err != nil {
		return err
	}
	vulkanCodec.Lock()
	defer vulkanCodec.Unlock()
	vulkanCodec.contextMu.Lock()
	defer vulkanCodec.contextMu.Unlock()
	if vulkanCodec.context != nil && vulkanCodec.deviceIndex == deviceIndex {
		return nil
	}
	if vulkanCodec.context != nil {
		C.call_vulkan_destroy(vulkanCodec.context)
		vulkanCodec.context = nil
	}
	shaderPath := findVulkanShaderDir()
	if shaderPath == "" {
		return fmt.Errorf("Compressonator Vulkan shaders not found")
	}
	cShaders := C.CString(shaderPath)
	defer C.free(unsafe.Pointer(cShaders))
	errBuf := (*C.char)(C.calloc(1024, 1))
	defer C.free(unsafe.Pointer(errBuf))
	context := C.call_vulkan_create(
		C.int32_t(deviceIndex), cShaders, vulkanWorkerCount, errBuf, 1024)
	if context == nil {
		return fmt.Errorf("%s", C.GoString(errBuf))
	}
	vulkanCodec.context = context
	vulkanCodec.deviceIndex = deviceIndex
	return nil
}

func vulkanContextDeviceName() string {
	vulkanCodec.contextMu.RLock()
	defer vulkanCodec.contextMu.RUnlock()
	if vulkanCodec.context == nil {
		return ""
	}
	nameBuf := (*C.char)(C.calloc(256, 1))
	defer C.free(unsafe.Pointer(nameBuf))
	if C.call_vulkan_context_device_name(vulkanCodec.context, nameBuf, 256) != 0 {
		return ""
	}
	return C.GoString(nameBuf)
}

func vulkanCompress(job cookJob, deviceIndex int32) string {
	if err := ensureVulkanContext(deviceIndex); err != nil {
		return err.Error()
	}
	format, ok := vulkanFormat(job.format)
	if !ok {
		return "unsupported Compressonator Vulkan target format: " + job.format
	}
	destination := strings.TrimSuffix(job.srcFile, filepath.Ext(job.srcFile)) + ".dds"
	flags := uint32(0)
	if job.inputSRGB {
		flags |= 1
	}
	if job.ignoreSRGB {
		flags |= 2
	}
	if job.sepa {
		flags |= 4
	}
	if job.srgb {
		flags |= 8
	}

	cSource := C.CString(job.srcFile)
	cDestination := C.CString(destination)
	defer C.free(unsafe.Pointer(cSource))
	defer C.free(unsafe.Pointer(cDestination))
	errBuf := (*C.char)(C.calloc(1024, 1))
	defer C.free(unsafe.Pointer(errBuf))

	vulkanCodec.contextMu.RLock()
	context := vulkanCodec.context
	defer vulkanCodec.contextMu.RUnlock()
	result := C.call_vulkan_compress(
		context, cSource, cDestination, C.uint32_t(format), C.uint32_t(flags), errBuf, 1024)
	if result != 0 {
		message := C.GoString(errBuf)
		if message == "" {
			message = fmt.Sprintf("Compressonator Vulkan returned code %d", int(result))
		}
		return message
	}
	return ""
}

func vulkanFormat(format string) (uint32, bool) {
	switch format {
	case "BC4_UNORM":
		return 1, true
	case "BC5_UNORM":
		return 2, true
	case "BC6H_UF16":
		return 3, true
	case "BC7_UNORM":
		return 4, true
	case "BC7_UNORM_SRGB":
		return 5, true
	default:
		return 0, false
	}
}

func findVulkanCodecLib() string {
	name := "libgdc_vulkan.so"
	if isWindows() {
		name = "gdc_vulkan.dll"
	}
	return findVulkanAsset(name)
}

func findVulkanShaderDir() string {
	path := findVulkanAsset("shaders")
	if info, err := os.Stat(path); err == nil && info.IsDir() {
		return path
	}
	return ""
}

func findVulkanAsset(name string) string {
	platformName := runtime.GOOS
	archName := runtime.GOARCH
	if archName == "amd64" {
		archName = "x64"
	}
	targetName := platformName + "-" + archName
	executable, _ := os.Executable()
	candidates := []string{
		filepath.Join(filepath.Dir(executable), "bin", targetName, name),
		filepath.Join("bin", targetName, name),
	}
	for _, candidate := range candidates {
		if absolute, err := filepath.Abs(candidate); err == nil {
			if _, statErr := os.Stat(absolute); statErr == nil {
				return absolute
			}
		}
	}
	return ""
}
