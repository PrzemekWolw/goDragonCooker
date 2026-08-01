//go:build !windows

package main

import "image/color"

func setTitleBarColor(_ string, _, _ color.RGBA) {}
