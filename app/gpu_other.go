//go:build !windows

package main

func platformGPUInfo() []string {
	report := platformSystemInfo()
	return append(report, otherGPUInfo()...)
}

func preferredGPUArgs() []string {
	return nil
}

func gpuAdapterOptions() []string {
	return []string{"Automatic (best)"}
}

func gpuAdapterOptionIndex(adapter int) int {
	return 0
}

func gpuAdapterIndexForOption(option int) int {
	return -1
}

func gpuAdapterCandidates() []int {
	return nil
}

func gpuAdapterName(index int) string {
	return "unknown"
}

func setSelectedGPU(index int) {}
