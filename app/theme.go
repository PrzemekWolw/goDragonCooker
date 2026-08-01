package main

import (
	"image/color"
	"os"

	g "github.com/AllenDang/giu"
)

// Theme colors - deep dark palette

var (
	colBg       = color.RGBA{10, 10, 14, 255}   // near-black window bg
	colPanel    = color.RGBA{14, 14, 20, 255}   // child windows / log area
	colFrame    = color.RGBA{20, 20, 28, 255}   // input/button frames
	colFrameHov = color.RGBA{32, 32, 44, 255}   // hovered frame
	colAccent   = color.RGBA{70, 110, 220, 255} // deeper blue accent
	colAccentHi = color.RGBA{90, 140, 240, 255} // active/hover highlight
	colText     = color.RGBA{200, 200, 215, 255}
	colTextDim  = color.RGBA{90, 90, 108, 255} // muted dim text
	colBorder   = color.RGBA{40, 40, 56, 255}
	colBar      = color.RGBA{12, 12, 18, 255}  // title bar bg
	colSide     = color.RGBA{60, 95, 185, 255} // side accent strip
	colCookOk   = color.RGBA{60, 180, 100, 255}
	colCookErr  = color.RGBA{200, 70, 70, 255}
)

func applyTheme() *g.StyleSetter {
	s := g.DefaultTheme()
	s.SetColor(g.StyleColorText, colText)
	s.SetColor(g.StyleColorTextDisabled, colTextDim)
	s.SetColor(g.StyleColorWindowBg, colBg)
	s.SetColor(g.StyleColorChildBg, colPanel)
	s.SetColor(g.StyleColorPopupBg, colPanel)
	s.SetColor(g.StyleColorBorder, colBorder)
	s.SetColor(g.StyleColorFrameBg, colFrame)
	s.SetColor(g.StyleColorFrameBgHovered, colFrameHov)
	s.SetColor(g.StyleColorFrameBgActive, colAccent)
	s.SetColor(g.StyleColorTitleBg, colBar)
	s.SetColor(g.StyleColorButton, colFrame)
	s.SetColor(g.StyleColorButtonHovered, colAccent)
	s.SetColor(g.StyleColorButtonActive, colAccentHi)
	s.SetColor(g.StyleColorTab, colFrame)
	s.SetColor(g.StyleColorTabHovered, colFrameHov)
	s.SetColor(g.StyleColorTabActive, colAccent)
	s.SetColor(g.StyleColorSeparator, colBorder)
	s.SetColor(g.StyleColorScrollbarBg, colPanel)
	s.SetColor(g.StyleColorScrollbarGrab, colBorder)
	s.SetColor(g.StyleColorScrollbarGrabHovered, colAccent)
	s.SetColor(g.StyleColorCheckMark, colAccentHi)
	s.SetColor(g.StyleColorSliderGrab, colAccent)
	s.SetColor(g.StyleColorSliderGrabActive, colAccentHi)

	s.SetStyleFloat(g.StyleVarWindowRounding, 0) // sharp corners for keygen look
	s.SetStyleFloat(g.StyleVarChildRounding, 0)
	s.SetStyleFloat(g.StyleVarFrameRounding, 0)
	s.SetStyleFloat(g.StyleVarGrabRounding, 0)
	s.SetStyleFloat(g.StyleVarTabRounding, 2)
	s.SetStyleFloat(g.StyleVarWindowBorderSize, 1)
	s.SetStyleFloat(g.StyleVarChildBorderSize, 1)
	s.SetStyleFloat(g.StyleVarFrameBorderSize, 1)

	return s
}

// loadFont picks a monospace font. Tries several paths per platform.
func loadFont() {
	candidates := []string{
		"/usr/share/fonts/truetype/Hack-Regular.ttf",
		"/usr/share/fonts/truetype/LiberationMono-Regular.ttf",
		"C:/Windows/Fonts/consola.ttf",
		"C:/Windows/Fonts/consolas.ttf",
		"/System/Library/Fonts/Courier New.ttf",
		"/System/Library/Fonts/Monaco.ttf",
	}
	for _, p := range candidates {
		if _, err := os.Stat(p); err == nil {
			g.Context.FontAtlas.SetDefaultFont(p)
			return
		}
	}
}
