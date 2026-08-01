package main

/*
#cgo linux LDFLAGS: -ldl
#include <stdlib.h>
#include <string.h>

extern int load_texconv_lib(const char *lib_path);
extern void unload_texconv_lib(void);
extern int get_texconv_version(void);
extern int run_texconv(int argc, const char **argv,
                       int verbose, int init_com, int allow_slow,
                       char *err_buf, int buf_size);
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

var texconvLoaded bool

// texconvCodecMu serializes BC6H/BC7 calls because they use the GPU or all CPU cores.
var texconvCodecMu sync.Mutex
var texconvInitMu sync.Mutex

// texconvInit loads the texconv shared library. Call once at startup.
func texconvInit() error {
	texconvInitMu.Lock()
	defer texconvInitMu.Unlock()
	if texconvLoaded {
		return nil
	}

	libPath := findTexconvLib()
	if libPath == "" {
		return fmt.Errorf("texconv library not found (looked in bin/ next to executable and cwd)")
	}

	cPath := C.CString(libPath)
	defer C.free(unsafe.Pointer(cPath))

	ret := C.load_texconv_lib(cPath)
	if ret != 0 {
		return fmt.Errorf("failed to load texconv library %s (code %d)", libPath, ret)
	}

	texconvLoaded = true
	runtime.SetFinalizer(new(struct{}), func(interface{}) { C.unload_texconv_lib() })
	return nil
}

func texconvVersion() string {
	if !texconvLoaded {
		return "unavailable"
	}
	version := int(C.get_texconv_version())
	if version == 0 {
		return "unknown"
	}
	return fmt.Sprintf("%03d (DirectXTex)", version)
}

// texconvRun calls texconv with the given string arguments.
// Returns an error message (empty string means success).
//
// BC6H/BC7 calls are serialized; other formats may run concurrently.
func texconvRun(args []string) string {
	return texconvRunMode(args, false)
}

func texconvRunCPU(args []string) string {
	return texconvRunMode(args, true)
}

func texconvRunMode(args []string, forceCPU bool) string {
	if err := texconvInit(); err != nil {
		return err.Error()
	}

	if len(args) == 0 {
		return "no arguments provided to texconv"
	}

	allowSlowCodec := C.int(1)
	initCOM := C.int(0)
	if isWindows() {
		initCOM = 1
	}
	if isWindows() && containsGPUCodecFormat(args) && !forceCPU {
		allowSlowCodec = 0
	}

	if containsGPUCodecFormat(args) {
		texconvCodecMu.Lock()
		defer texconvCodecMu.Unlock()
	}

	// Build C string array.
	cArgs := make([]*C.char, len(args))
	for i, s := range args {
		cArgs[i] = C.CString(s)
	}

	// Error buffer.
	errBuf := (*C.char)(C.calloc(512, 1))

	ret := C.run_texconv(
		C.int(len(args)),
		(**C.char)(unsafe.Pointer(&cArgs[0])),
		C.int(0), // verbose = false (caller handles logging)
		initCOM,
		allowSlowCodec,
		errBuf,
		C.int(512),
	)

	// Capture error message before freeing C memory.
	var errMsg string
	if ret != 0 {
		errMsg = C.GoString(errBuf)
	}

	// Free C strings.
	for _, p := range cArgs {
		C.free(unsafe.Pointer(p))
	}
	C.free(unsafe.Pointer(errBuf))

	if ret != 0 {
		if errMsg == "" {
			return fmt.Sprintf("texconv returned error code %d", ret)
		}
		return "Texconv Error: " + errMsg
	}
	return ""
}

func containsGPUCodecFormat(args []string) bool {
	for i := 0; i+1 < len(args); i++ {
		if args[i] != "-f" {
			continue
		}
		format := strings.ToUpper(args[i+1])
		if strings.HasPrefix(format, "BC6H_") || strings.HasPrefix(format, "BC7_") {
			return true
		}
	}
	return false
}

// findTexconvLib locates the texconv shared library for the current platform.
func findTexconvLib() string {
	name := "libtexconv.so"
	if isWindows() {
		name = "texconv.dll"
	} else if isDarwin() {
		name = "libtexconv.dylib"
	}

	platformName := runtime.GOOS
	if isDarwin() {
		platformName = "macos"
	}
	archName := runtime.GOARCH
	if runtime.GOARCH == "amd64" {
		archName = "x64"
	}
	targetName := platformName + "-" + archName

	exePath, _ := os.Executable()
	candidates := []string{
		filepath.Join(filepath.Dir(exePath), "bin", targetName, name),
		filepath.Join("bin", targetName, name),
	}

	for _, c := range candidates {
		if abs, err := filepath.Abs(c); err == nil {
			if info, serr := os.Stat(abs); serr == nil && !info.IsDir() {
				return abs
			}
		}
	}
	return ""
}
