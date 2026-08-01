package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

type colorType int

const (
	ctUnknown colorType = iota
	ctGrayscale
	ctRGB
	ctPalette
	ctGrayAlpha
	ctRGBA
)

type colorSpace int

const (
	csUnknown colorSpace = iota
	csLinear
	csSRGB
)

type imgInfo struct {
	format     string
	bits       int
	colorType  colorType
	colorSpace colorSpace
}

var supportedImageExtensions = map[string]string{
	".bmp":  "bmp",
	".exr":  "exr",
	".hdr":  "hdr",
	".jpeg": "jpeg",
	".jpg":  "jpeg",
	".png":  "png",
	".tga":  "tga",
	".tif":  "tiff",
	".tiff": "tiff",
}

func imageFormat(path string) string {
	return supportedImageExtensions[strings.ToLower(filepath.Ext(path))]
}

// readImageHeader reads only the source header and metadata needed for format
// and color-space selection.
func readImageHeader(path string) (imgInfo, error) {
	format := imageFormat(path)
	if format == "" {
		return imgInfo{}, fmt.Errorf("unsupported image format: %s", filepath.Ext(path))
	}

	switch format {
	case "png":
		return readPNGHeader(path)
	case "jpeg":
		return readJPEGHeader(path)
	case "exr", "hdr":
		return imgInfo{format: format, bits: 32, colorType: ctRGB, colorSpace: csLinear}, nil
	default:
		return imgInfo{format: format, colorType: ctUnknown, colorSpace: csUnknown}, nil
	}
}

func readJPEGHeader(path string) (imgInfo, error) {
	f, err := os.Open(path)
	if err != nil {
		return imgInfo{}, err
	}
	defer f.Close()

	var soi [2]byte
	if _, err := io.ReadFull(f, soi[:]); err != nil || soi != [2]byte{0xff, 0xd8} {
		return imgInfo{}, fmt.Errorf("invalid JPEG header")
	}

	for {
		var marker [2]byte
		if _, err := io.ReadFull(f, marker[:]); err != nil {
			return imgInfo{}, fmt.Errorf("failed to read JPEG header: %w", err)
		}
		if marker[0] != 0xff {
			continue
		}
		for marker[1] == 0xff {
			if _, err := io.ReadFull(f, marker[1:]); err != nil {
				return imgInfo{}, fmt.Errorf("failed to read JPEG marker: %w", err)
			}
		}
		if marker[1] == 0xd8 || marker[1] == 0xd9 {
			continue
		}
		var lengthBuf [2]byte
		if _, err := io.ReadFull(f, lengthBuf[:]); err != nil {
			return imgInfo{}, fmt.Errorf("failed to read JPEG segment: %w", err)
		}
		length := int(binary.BigEndian.Uint16(lengthBuf[:]))
		if length < 2 {
			return imgInfo{}, fmt.Errorf("invalid JPEG segment length")
		}
		if isJPEGStartOfFrame(marker[1]) {
			header := make([]byte, 6)
			if _, err := io.ReadFull(f, header); err != nil {
				return imgInfo{}, fmt.Errorf("failed to read JPEG frame: %w", err)
			}
			ct := ctRGB
			if header[5] == 1 {
				ct = ctGrayscale
			}
			return imgInfo{format: "jpeg", bits: int(header[0]), colorType: ct, colorSpace: csSRGB}, nil
		}
		if _, err := io.CopyN(io.Discard, f, int64(length-2)); err != nil {
			return imgInfo{}, fmt.Errorf("failed to skip JPEG segment: %w", err)
		}
	}
}

func isJPEGStartOfFrame(marker byte) bool {
	return marker >= 0xc0 && marker <= 0xcf &&
		marker != 0xc4 && marker != 0xc8 && marker != 0xcc
}

func (p imgInfo) bit8() bool { return p.bits == 8 }

func readPNGHeader(path string) (imgInfo, error) {
	f, err := os.Open(path)
	if err != nil {
		return imgInfo{}, err
	}
	defer f.Close()

	var sig [8]byte
	if _, err := io.ReadFull(f, sig[:]); err != nil {
		return imgInfo{}, fmt.Errorf("bad PNG signature: %w", err)
	}
	if string(sig[:]) != "\x89PNG\r\n\x1a\n" {
		return imgInfo{}, fmt.Errorf("not a PNG file")
	}

	info := imgInfo{format: "png", colorSpace: csUnknown}
	for {
		var lenBuf [4]byte
		if _, err := io.ReadFull(f, lenBuf[:]); err != nil {
			return imgInfo{}, err
		}
		chunkLen := int(binary.BigEndian.Uint32(lenBuf[:]))

		var typeBuf [4]byte
		if _, err := io.ReadFull(f, typeBuf[:]); err != nil {
			return imgInfo{}, err
		}
		chunkType := string(typeBuf[:])

		if chunkType == "IHDR" {
			if chunkLen != 13 {
				return imgInfo{}, fmt.Errorf("IHDR too short")
			}
			var data [13]byte
			if _, err := io.ReadFull(f, data[:]); err != nil {
				return imgInfo{}, err
			}
			bits := int(data[8])
			ct, ok := pngColorType(data[9])
			if !ok {
				return imgInfo{}, fmt.Errorf("unsupported PNG color type: %d", data[9])
			}
			info.bits = bits
			info.colorType = ct
		} else if chunkType == "sRGB" && chunkLen == 1 {
			var data [1]byte
			if _, err := io.ReadFull(f, data[:]); err != nil {
				return imgInfo{}, err
			}
			info.colorSpace = csSRGB
		} else if chunkType == "gAMA" && chunkLen == 4 {
			var data [4]byte
			if _, err := io.ReadFull(f, data[:]); err != nil {
				return imgInfo{}, err
			}
			gamma := binary.BigEndian.Uint32(data[:])
			switch gamma {
			case 100000:
				info.colorSpace = csLinear
			case 45455:
				info.colorSpace = csSRGB
			}
		} else {
			if _, err := io.CopyN(io.Discard, f, int64(chunkLen)); err != nil {
				return imgInfo{}, err
			}
		}

		var crc [4]byte
		if _, err := io.ReadFull(f, crc[:]); err != nil {
			return imgInfo{}, err
		}

		if chunkType == "IDAT" || chunkType == "IEND" {
			break
		}
	}
	if info.bits != 0 {
		return info, nil
	}
	return imgInfo{}, fmt.Errorf("IHDR not found")
}

func pngColorType(value byte) (colorType, bool) {
	switch value {
	case 0:
		return ctGrayscale, true
	case 2:
		return ctRGB, true
	case 3:
		return ctPalette, true
	case 4:
		return ctGrayAlpha, true
	case 6:
		return ctRGBA, true
	default:
		return ctUnknown, false
	}
}
