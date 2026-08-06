package main

import (
	"bytes"
	_ "embed"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	_ "image/png"
	"runtime"
	"sync"

	g "github.com/AllenDang/giu"
)

var appVersion = "dev"

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

type appState struct {
	mu             sync.RWMutex
	tabCook        *cookTab
	tabMatGen      *matGenTab
	dirPicker      *dirPickerState
	texconvWarn    string
	warnShown      bool
	gpuReport      []string
	gpuAdapter     int32
	vulkanDevice   int32
	textureBackend string
	vulkanDevices  []string
	gpuChecking    bool
}

var app = &appState{
	tabCook:        &cookTab{stopCook: make(chan struct{}), autoDetectFmt: true},
	tabMatGen:      &matGenTab{stopGenerate: make(chan struct{}), settings: defaultMatGenSettings(), patternBufs: make(map[string]*patternEditBuf)},
	dirPicker:      newDirPicker(),
	gpuAdapter:     -1,
	vulkanDevice:   -1,
	textureBackend: textureBackendCompressonator,
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

func main() {
	runtime.LockOSThread()
	loadSettings()

	title := fmt.Sprintf("Materials Tools (GoDragonPBR) v%s", appVersion)
	wnd := g.NewMasterWindow(title, 1280, 900, 0)
	loadFont()
	setAppIcon(wnd)
	wnd.SetStyle(applyTheme())
	setTitleBarColor(title, colBar, colText)
	wnd.Run(loop)
}

//go:embed dragonpbr.ico
var appIcon []byte

func setAppIcon(wnd *g.MasterWindow) {
	icon, err := decodeIcon(appIcon)
	if err == nil {
		wnd.SetIcon(icon)
	}
}

func decodeIcon(data []byte) (image.Image, error) {
	if len(data) < 6 || binary.LittleEndian.Uint16(data) != 0 || binary.LittleEndian.Uint16(data[2:]) != 1 {
		return nil, fmt.Errorf("invalid ICO header")
	}

	count := int(binary.LittleEndian.Uint16(data[4:]))
	if count == 0 || len(data) < 6+count*16 {
		return nil, fmt.Errorf("invalid ICO entries")
	}

	bestOffset, bestSize := 0, 0
	for i := 0; i < count; i++ {
		entry := data[6+i*16:]
		width := int(entry[0])
		if width == 0 {
			width = 256
		}
		height := int(entry[1])
		if height == 0 {
			height = 256
		}
		size := int(binary.LittleEndian.Uint32(entry[8:]))
		offset := int(binary.LittleEndian.Uint32(entry[12:]))
		if offset < 0 || size <= 0 || offset+size > len(data) {
			continue
		}
		if width*height > bestSize {
			bestOffset, bestSize = offset, width*height
		}
	}
	if bestSize == 0 {
		return nil, fmt.Errorf("ICO contains no valid image")
	}

	icon, _, err := image.Decode(bytes.NewReader(data[bestOffset:]))
	if err == nil {
		return icon, nil
	}
	return decodeICOBMP(data[bestOffset:])
}

func decodeICOBMP(data []byte) (image.Image, error) {
	if len(data) < 40 {
		return nil, fmt.Errorf("invalid ICO bitmap")
	}

	headerSize := int(binary.LittleEndian.Uint32(data))
	width := int(int32(binary.LittleEndian.Uint32(data[4:])))
	height := int(int32(binary.LittleEndian.Uint32(data[8:]))) / 2
	bits := int(binary.LittleEndian.Uint16(data[14:]))
	if headerSize < 40 || width <= 0 || height <= 0 || (bits != 8 && bits != 24 && bits != 32) {
		return nil, fmt.Errorf("unsupported ICO bitmap")
	}

	colors := 0
	if bits <= 8 {
		colors = int(binary.LittleEndian.Uint32(data[32:]))
		if colors == 0 {
			colors = 1 << bits
		}
	}
	paletteOffset := headerSize
	pixelOffset := paletteOffset + colors*4
	rowSize := ((width*bits + 31) / 32) * 4
	maskRowSize := ((width + 31) / 32) * 4
	maskOffset := pixelOffset + rowSize*height
	if pixelOffset > len(data) || maskOffset > len(data) || maskOffset+maskRowSize*height > len(data) {
		return nil, fmt.Errorf("truncated ICO bitmap")
	}

	hasAlpha := false
	if bits == 32 {
		for y := 0; y < height; y++ {
			srcRow := pixelOffset + y*rowSize
			for x := 0; x < width; x++ {
				if data[srcRow+x*4+3] != 0 {
					hasAlpha = true
					break
				}
			}
			if hasAlpha {
				break
			}
		}
	}

	icon := image.NewRGBA(image.Rect(0, 0, width, height))
	for y := 0; y < height; y++ {
		srcRow := pixelOffset + (height-1-y)*rowSize
		for x := 0; x < width; x++ {
			var r, g, b, a uint8 = 0, 0, 0, 255
			switch bits {
			case 32:
				pixel := srcRow + x*4
				b, g, r = data[pixel], data[pixel+1], data[pixel+2]
				if hasAlpha {
					a = data[pixel+3]
				}
			case 24:
				pixel := srcRow + x*3
				b, g, r = data[pixel], data[pixel+1], data[pixel+2]
			case 8:
				index := int(data[srcRow+x])
				if index >= colors {
					return nil, fmt.Errorf("invalid ICO palette index")
				}
				palette := paletteOffset + index*4
				b, g, r = data[palette], data[palette+1], data[palette+2]
			}

			mask := data[maskOffset+(height-1-y)*maskRowSize+x/8]
			if mask&(0x80>>uint(x&7)) != 0 {
				a = 0
			}
			icon.SetRGBA(x, y, color.RGBA{R: r, G: g, B: b, A: a})
		}
	}
	return icon, nil
}
