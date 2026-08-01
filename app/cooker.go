package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	g "github.com/AllenDang/giu"
)

// ---------------------------------------------------------------------------
// Texture Cooker
// ---------------------------------------------------------------------------

type cookTab struct {
	mu            sync.Mutex
	directory     string
	status        string
	logLines      []string
	running       bool
	stopCook      chan struct{}
	autoDetectFmt bool // auto-detect .data format (true = legacy grayscale BC4, false = inspect source)
}

func (t *cookTab) log(msg string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.logLines = append(t.logLines, msg)
	if len(t.logLines) > 500 {
		t.logLines = t.logLines[len(t.logLines)-500:]
	}
}

type cookJob struct {
	srcFile    string
	format     string
	srgb       bool
	inputSRGB  bool
	ignoreSRGB bool
	sepa       bool
}

// cookResult carries back the outcome of a single conversion.
type cookResult struct {
	name string
	err  string
}

// isCookableImage returns true for formats supported by the bundled texconv.
func isCookableImage(name string) bool {
	return imageFormat(name) != ""
}

// textureRole matches the file suffix to a material slot (color/data/normal).
// Returns empty string if no role matched.
func textureRole(name string) string {
	lower := strings.ToLower(name)
	ext := filepath.Ext(lower)
	if imageFormat(lower) == "" {
		return ""
	}
	for _, role := range []string{"color", "data", "normal"} {
		if strings.HasSuffix(lower, "."+role+ext) {
			return role
		}
	}
	return ""
}

func isFloatHDR(name string) bool {
	lower := strings.ToLower(name)
	return strings.HasSuffix(lower, ".exr") || strings.HasSuffix(lower, ".hdr")
}

func runCooker(directory string, tab *cookTab) {
	tab.mu.Lock()
	tab.running = true
	tab.status = "Seeking files..."
	stopCook := tab.stopCook
	autoDetectFmt := tab.autoDetectFmt
	tab.mu.Unlock()
	g.Update()

	tab.log("Starting processing")
	tab.log("Seeking files")

	var jobs []cookJob

	err := filepath.Walk(directory, func(path string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() {
			return nil
		}
		name := info.Name()
		if !isCookableImage(name) {
			return nil
		}

		rRole := textureRole(name)
		if rRole == "" {
			return nil
		}

		img, err := readImageHeader(path)
		if err != nil {
			// EXR isn't supported by Go's stdlib image reader - that's fine.
			if !isFloatHDR(name) {
				tab.log(fmt.Sprintf("  SKIP: %s (%v)", name, err))
				return nil
			}
		}
		if img.bits != 0 && !img.bit8() && !isFloatHDR(name) {
			tab.log(fmt.Sprintf("  SKIP: %s (not 8-bit, got %d-bit)", name, img.bits))
			return nil
		}

		var format string
		var srgb, inputSRGB, ignoreSRGB, sepa bool
		switch rRole {
		case "color":
			format, srgb, sepa = "BC7_UNORM_SRGB", true, true
			inputSRGB = img.colorSpace != csLinear
		case "data":
			if isFloatHDR(name) {
				format, srgb, sepa = "BC7_UNORM", false, false
			} else {
				format, srgb, sepa = resolveDataFormat(img, autoDetectFmt)
			}
			ignoreSRGB = true
		case "normal":
			format, srgb, sepa = "BC5_UNORM", false, false
			ignoreSRGB = true
		}

		dts := strings.TrimSuffix(path, filepath.Ext(path)) + ".dds"
		if _, err := os.Stat(dts); err == nil {
			return nil
		}

		jobs = append(jobs, cookJob{
			srcFile: path, format: format, srgb: srgb, inputSRGB: inputSRGB,
			ignoreSRGB: ignoreSRGB, sepa: sepa,
		})
		tab.log(fmt.Sprintf("  Queue: %s (%s, %s)", name, format, colorSpaceName(img.colorSpace)))
		return nil
	})

	if err != nil {
		tab.log(fmt.Sprintf("Walk error: %v", err))
		finishCook(tab, "Error")
		return
	}

	total := len(jobs)
	if total == 0 {
		tab.log("No files to cook.")
		finishCook(tab, "Nothing to do")
		return
	}

	var codecJobs []cookJob
	var cpuJobs []cookJob
	for _, j := range jobs {
		if isExpensiveCodec(j.format) {
			codecJobs = append(codecJobs, j)
		} else {
			cpuJobs = append(cpuJobs, j)
		}
	}

	tab.mu.Lock()
	tab.status = "Cooking textures..."
	tab.mu.Unlock()
	g.Update()

	t0 := time.Now()
	done := 0
	cooked := 0
	failed := 0
	batchCount := 0

	recordResult := func(r cookResult) {
		done++
		if r.err != "" {
			tab.log(fmt.Sprintf("  FAIL: %s - %s", r.name, strings.TrimSpace(r.err)))
			failed++
		} else {
			tab.log(fmt.Sprintf("  OK: %s", r.name))
			cooked++
		}

		// Batch UI updates (every 16 results) instead of per-file.
		batchCount++
		if batchCount >= 16 || done == total {
			tab.mu.Lock()
			tab.status = fmt.Sprintf("Cooking: %d/%d (%d cooked, %d failed)", done, total, cooked, failed)
			tab.mu.Unlock()
			g.Update()
			batchCount = 0
		}
	}

	tab.log(fmt.Sprintf("Cooking %d files: %d BC6H/BC7 first, then %d CPU-batch", total, len(codecJobs), len(cpuJobs)))

	useCPUCodec := false
	for _, j := range codecJobs {
		select {
		case <-stopCook:
			tab.log("Cooking cancelled.")
			finishCook(tab, "Cancelled")
			return
		default:
		}

		args := cookArgs(j)
		var errMsg string
		if useCPUCodec {
			errMsg = texconvRunCPU(args)
		} else {
			errMsg = texconvRun(args)
			if errMsg != "" {
				useCPUCodec = true
				if fallbackErr := texconvRunCPU(args); fallbackErr == "" {
					errMsg = ""
					tab.log(fmt.Sprintf("  GPU codec failed for %s; using CPU codec for remaining BC6H/BC7 textures.", filepath.Base(j.srcFile)))
				} else {
					errMsg += "; CPU fallback: " + fallbackErr
				}
			}
		}
		recordResult(cookResult{name: filepath.Base(j.srcFile), err: errMsg})
	}

	if len(cpuJobs) > 0 {
		numWorkers := runtime.NumCPU()
		if numWorkers > len(cpuJobs) {
			numWorkers = len(cpuJobs)
		}
		if numWorkers < 1 {
			numWorkers = 1
		}
		tab.log(fmt.Sprintf("Running %d CPU-batch textures with %d workers.", len(cpuJobs), numWorkers))

		jobCh := make(chan cookJob, len(cpuJobs))
		for _, j := range cpuJobs {
			jobCh <- j
		}
		close(jobCh)

		results := make(chan cookResult, len(cpuJobs))
		var wg sync.WaitGroup
		wg.Add(numWorkers)
		for i := 0; i < numWorkers; i++ {
			go func() {
				defer wg.Done()
				for {
					select {
					case <-stopCook:
						return
					case j, ok := <-jobCh:
						if !ok {
							return
						}
						results <- cookResult{name: filepath.Base(j.srcFile), err: texconvRun(cookArgs(j))}
					}
				}
			}()
		}

		go func() {
			wg.Wait()
			close(results)
		}()

		for r := range results {
			recordResult(r)
		}
	}

	select {
	case <-stopCook:
		tab.log("Cooking cancelled.")
		finishCook(tab, "Cancelled")
		return
	default:
	}

	elapsed := time.Since(t0).Seconds()
	tab.log(fmt.Sprintf("Finished in %.2fs (%d/%d textures cooked, %d failed)", elapsed, cooked, total, failed))
	finishCook(tab, fmt.Sprintf("Done: %d/%d cooked", cooked, total))
}

func isExpensiveCodec(format string) bool {
	return strings.HasPrefix(format, "BC6H_") || strings.HasPrefix(format, "BC7_")
}

func cookArgs(j cookJob) []string {
	args := []string{"-y", "-pow2"}
	if j.sepa {
		args = append(args, "-sepalpha")
	}
	if j.ignoreSRGB {
		args = append(args, "--ignore-srgb")
	}
	if j.inputSRGB {
		args = append(args, "-srgbi")
	}
	if j.srgb {
		args = append(args, "-srgbo")
	}
	if strings.HasPrefix(j.format, "BC7_") {
		args = append(args, "-bc", "x")
	}
	if isExpensiveCodec(j.format) {
		args = append(args, preferredGPUArgs()...)
	}
	return append(args, "-o", filepath.Dir(j.srcFile),
		"-f", j.format,
		"--", j.srcFile)
}

func finishCook(tab *cookTab, status string) {
	tab.mu.Lock()
	tab.running = false
	tab.status = status
	tab.mu.Unlock()
	g.Update()
}

// resolveDataFormat picks the texconv format for a .data texture.
// When autoDetect is true (default/legacy), it always uses BC4_UNORM (grayscale).
// When autoDetect is false, it inspects the image header:
//   - grayscale / grayscale+alpha  -> BC4_UNORM
//   - rgb / rgba                   -> BC7_UNORM
func resolveDataFormat(info imgInfo, autoDetect bool) (string, bool, bool) {
	if autoDetect {
		return "BC4_UNORM", false, false // legacy: assume grayscale
	}

	switch info.colorType {
	case ctGrayscale, ctGrayAlpha:
		return "BC4_UNORM", false, false
	default:
		// RGB, RGBA, palette -> full BC7 linear (no srgb)
		return "BC7_UNORM", false, false
	}
}

func colorSpaceName(space colorSpace) string {
	switch space {
	case csLinear:
		return "linear"
	case csSRGB:
		return "sRGB"
	default:
		return "unknown"
	}
}
