//go:build !windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
)

func platformSystemInfo() []string {
	return []string{
		otherCPUInfo(),
		otherMemoryInfo(),
	}
}

func otherCPUInfo() string {
	name := ""
	if data, err := os.ReadFile("/proc/cpuinfo"); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "model name") {
				if parts := strings.SplitN(line, ":", 2); len(parts) == 2 {
					name = strings.TrimSpace(parts[1])
				}
				break
			}
		}
	}
	if name == "" {
		return fmt.Sprintf("CPU: %d logical processors", runtime.NumCPU())
	}
	return fmt.Sprintf("CPU: %s (%d logical processors)", name, runtime.NumCPU())
}

func otherMemoryInfo() string {
	data, err := os.ReadFile("/proc/meminfo")
	if err != nil {
		return "RAM: unavailable"
	}
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) >= 2 && fields[0] == "MemTotal:" {
			kb, err := strconv.ParseUint(fields[1], 10, 64)
			if err == nil {
				return fmt.Sprintf("RAM: %.1f GB total", float64(kb*1024)/(1024*1024*1024))
			}
		}
	}
	return "RAM: unavailable"
}

func otherGPUInfo() []string {
	files, _ := filepath.Glob("/sys/class/drm/card*/device/uevent")
	report := make([]string, 0, len(files))
	for _, file := range files {
		data, err := os.ReadFile(file)
		if err != nil {
			continue
		}
		driver := ""
		pciID := ""
		for _, line := range strings.Split(string(data), "\n") {
			switch {
			case strings.HasPrefix(line, "DRIVER="):
				driver = strings.TrimPrefix(line, "DRIVER=")
			case strings.HasPrefix(line, "PCI_ID="):
				pciID = strings.TrimPrefix(line, "PCI_ID=")
			}
		}
		report = append(report, fmt.Sprintf("GPU: driver %s, PCI %s", driver, pciID))
	}
	if len(report) == 0 {
		return []string{"GPU hardware: no display adapters reported"}
	}
	return report
}
