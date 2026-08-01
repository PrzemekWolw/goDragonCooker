package main

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"

	g "github.com/AllenDang/giu"
)

var startupGPUCheckStarted bool

func runStartupGPUCheck() {
	app.mu.Lock()
	app.gpuChecking = true
	app.mu.Unlock()
	if err := texconvInit(); err != nil {
		app.mu.Lock()
		app.texconvWarn = err.Error()
		app.warnShown = true
		app.mu.Unlock()
	}
	report := runGPUCheck()
	app.mu.Lock()
	app.gpuReport = report
	app.gpuChecking = false
	app.mu.Unlock()
	g.Update()
}

func rerunGPUCheck() {
	app.mu.Lock()
	if app.gpuChecking {
		app.mu.Unlock()
		return
	}
	app.gpuChecking = true
	app.gpuReport = []string{"Testing selected GPU..."}
	app.mu.Unlock()
	g.Update()

	go func() {
		report := runGPUCheck()
		app.mu.Lock()
		app.gpuReport = report
		app.gpuChecking = false
		app.mu.Unlock()
		g.Update()
	}()
}

func runGPUCheck() []string {
	report := platformGPUInfo()
	report = append(report, "Texconv version: "+texconvVersion())
	report = append(report, checkTexconvGPU()...)
	return report
}

func checkTexconvGPU() []string {
	dir, err := os.MkdirTemp("", "go-cooker-gpu-check")
	if err != nil {
		return []string{fmt.Sprintf("Texconv BC7: FAILED (%v)", err)}
	}
	defer os.RemoveAll(dir)

	input := filepath.Join(dir, "gpu-check.png")
	file, err := os.Create(input)
	if err != nil {
		return []string{fmt.Sprintf("Texconv BC7: FAILED (%v)", err)}
	}

	img := image.NewRGBA(image.Rect(0, 0, 4, 4))
	for y := 0; y < 4; y++ {
		for x := 0; x < 4; x++ {
			img.SetRGBA(x, y, color.RGBA{R: 128, G: 96, B: 64, A: 255})
		}
	}
	encodeErr := png.Encode(file, img)
	closeErr := file.Close()
	if encodeErr != nil {
		return []string{fmt.Sprintf("Texconv BC7: FAILED (%v)", encodeErr)}
	}
	if closeErr != nil {
		return []string{fmt.Sprintf("Texconv BC7: FAILED (%v)", closeErr)}
	}

	baseArgs := []string{
		"-y",
		"-f", "BC7_UNORM",
		"-bc", "x",
	}
	app.mu.RLock()
	configuredGPU := app.gpuAdapter
	app.mu.RUnlock()
	indices := gpuAdapterCandidates()
	if configuredGPU >= 0 {
		indices = []int{int(configuredGPU)}
	}
	if len(indices) == 0 {
		indices = []int{-1}
	}
	var lastErr string
	for _, index := range indices {
		args := append([]string(nil), baseArgs...)
		if index >= 0 {
			args = append(args, "-gpu", fmt.Sprintf("%d", index))
		}
		args = append(args, "-o", dir, "--", input)
		if errText := texconvRun(args); errText == "" {
			if isWindows() {
				setSelectedGPU(index)
				return []string{fmt.Sprintf("Texconv BC7 GPU: OK (adapter %d: %s)", index, gpuAdapterName(index))}
			}
			return []string{"Texconv BC7: OK"}
		} else {
			lastErr = errText
		}
	}
	return []string{"Texconv BC7 GPU: FAILED", "  " + lastErr}
}
