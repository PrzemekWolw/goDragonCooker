//go:build windows

package main

import (
	"fmt"
	"runtime"
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

func platformSystemInfo() []string {
	return []string{
		windowsCPUInfo(),
		windowsMemoryInfo(),
	}
}

func windowsCPUInfo() string {
	key, err := registry.OpenKey(
		registry.LOCAL_MACHINE,
		`HARDWARE\DESCRIPTION\System\CentralProcessor\0`,
		registry.QUERY_VALUE,
	)
	if err == nil {
		defer key.Close()
		if name, _, valueErr := key.GetStringValue("ProcessorNameString"); valueErr == nil {
			return fmt.Sprintf("CPU: %s (%d logical processors)", strings.TrimSpace(name), runtime.NumCPU())
		}
	}
	return fmt.Sprintf("CPU: %d logical processors", runtime.NumCPU())
}

var kernel32System = windows.NewLazySystemDLL("kernel32.dll")

type memoryStatusEx struct {
	length                 uint32
	memoryLoad             uint32
	totalPhysical          uint64
	availablePhysical      uint64
	totalPageFile          uint64
	availablePageFile      uint64
	totalVirtual           uint64
	availableVirtual       uint64
	availableExtended      uint64
}

func windowsMemoryInfo() string {
	status := memoryStatusEx{length: uint32(unsafe.Sizeof(memoryStatusEx{}))}
	proc := kernel32System.NewProc("GlobalMemoryStatusEx")
	ret, _, _ := proc.Call(uintptr(unsafe.Pointer(&status)))
	if ret == 0 {
		return "RAM: unavailable"
	}
	return fmt.Sprintf("RAM: %.1f GB total, %.1f GB available",
		float64(status.totalPhysical)/(1024*1024*1024),
		float64(status.availablePhysical)/(1024*1024*1024))
}
