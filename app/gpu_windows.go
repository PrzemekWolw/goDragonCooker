//go:build windows

package main

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
	"sync"
)

func platformGPUInfo() []string {
	report := platformSystemInfo()
	report = append(report, gpuAdapterInfo()...)
	return report
}

var (
	gpuIndexMu      sync.Mutex
	selectedGPU     = -1
	gpuAdaptersOnce sync.Once
	gpuAdapters     []gpuAdapter
)

func gpuAdapterInfo() []string {
	report := []string{}
	for _, adapter := range availableGPUAdapters() {
		report = append(report, fmt.Sprintf("GPU adapter %d: %s", adapter.index, adapter.name))
	}
	if len(report) == 0 {
		return []string{"GPU hardware: no display adapters reported"}
	}
	return report
}

func availableGPUAdapters() []gpuAdapter {
	gpuAdaptersOnce.Do(func() {
		seen := make(map[string]struct{})
		for _, adapter := range dxgiGPUAdapters() {
			if strings.EqualFold(adapter.name, "Microsoft Basic Display Adapter") ||
				strings.EqualFold(adapter.name, "Microsoft Basic Render Driver") {
				continue
			}
			if _, exists := seen[adapter.name]; !exists {
				seen[adapter.name] = struct{}{}
				gpuAdapters = append(gpuAdapters, adapter)
			}
		}
	})
	return gpuAdapters
}

func gpuAdapterName(index int) string {
	for _, adapter := range availableGPUAdapters() {
		if adapter.index == index {
			return adapter.name
		}
	}
	return "unknown"
}

func gpuAdapterOptions() []string {
	options := []string{"Automatic (best)"}
	for _, adapter := range availableGPUAdapters() {
		options = append(options, fmt.Sprintf("%d: %s", adapter.index, adapter.name))
	}
	return options
}

func gpuAdapterOptionIndex(adapter int) int {
	if adapter < 0 {
		return 0
	}
	for option, candidate := range availableGPUAdapters() {
		if candidate.index == adapter {
			return option + 1
		}
	}
	return 0
}

func gpuAdapterIndexForOption(option int) int {
	if option <= 0 {
		return -1
	}
	adapters := availableGPUAdapters()
	if option-1 < len(adapters) {
		return adapters[option-1].index
	}
	return -1
}

func preferredGPUArgs() []string {
	app.mu.RLock()
	configuredGPU := app.gpuAdapter
	app.mu.RUnlock()
	if configuredGPU >= 0 {
		return []string{"-gpu", strconv.Itoa(int(configuredGPU))}
	}

	gpuIndexMu.Lock()
	if selectedGPU >= 0 {
		index := selectedGPU
		gpuIndexMu.Unlock()
		return []string{"-gpu", strconv.Itoa(index)}
	}
	gpuIndexMu.Unlock()

	indices := gpuAdapterCandidates()
	if len(indices) == 0 {
		return []string{"-gpu", "0"}
	}
	return []string{"-gpu", strconv.Itoa(indices[0])}
}

func setSelectedGPU(index int) {
	gpuIndexMu.Lock()
	selectedGPU = index
	gpuIndexMu.Unlock()
}

func gpuAdapterCandidates() []int {
	adapters := availableGPUAdapters()
	indices := make([]int, len(adapters))
	for index, adapter := range adapters {
		indices[index] = adapter.index
	}
	sort.SliceStable(indices, func(i, j int) bool {
		return gpuScore(gpuAdapterName(indices[i])) > gpuScore(gpuAdapterName(indices[j]))
	})
	return indices
}

func gpuScore(name string) int {
	lower := strings.ToLower(name)
	score := 1
	switch {
	case strings.Contains(lower, "nvidia"):
		score = 100
	case strings.Contains(lower, "amd"), strings.Contains(lower, "radeon"):
		score = 70
	case strings.Contains(lower, "intel"):
		score = 10
	}
	if strings.Contains(lower, "780m") ||
		strings.Contains(lower, "680m") ||
		strings.Contains(lower, "integrated") {
		score -= 40
	}
	return score
}
