//go:build windows

package main

import (
	"image/color"
	"unsafe"

	"golang.org/x/sys/windows"
)

const (
	dwmwaCaptionColor = 35
	dwmwaTextColor    = 36
)

var (
	user32 = windows.NewLazySystemDLL("user32.dll")
	dwmapi = windows.NewLazySystemDLL("dwmapi.dll")
)

func setTitleBarColor(title string, background, text color.RGBA) {
	findWindow := user32.NewProc("FindWindowW")
	setWindowAttribute := dwmapi.NewProc("DwmSetWindowAttribute")
	windowTitle, err := windows.UTF16PtrFromString(title)
	if err != nil {
		return
	}

	hwnd, _, _ := findWindow.Call(0, uintptr(unsafe.Pointer(windowTitle)))
	if hwnd == 0 {
		return
	}

	setDWMColor(setWindowAttribute, hwnd, dwmwaCaptionColor, background)
	setDWMColor(setWindowAttribute, hwnd, dwmwaTextColor, text)
}

func setDWMColor(proc *windows.LazyProc, hwnd uintptr, attribute uint32, value color.RGBA) {
	colorRef := uint32(value.R) | uint32(value.G)<<8 | uint32(value.B)<<16
	proc.Call(
		hwnd,
		uintptr(attribute),
		uintptr(unsafe.Pointer(&colorRef)),
		unsafe.Sizeof(colorRef),
	)
}
