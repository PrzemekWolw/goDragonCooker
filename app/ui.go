package main

import (
	g "github.com/AllenDang/giu"
)

// loop is the render callback - called every frame.
func loop() {
	if !startupGPUCheckStarted {
		startupGPUCheckStarted = true
		go runStartupGPUCheck()
	}

	app.mu.RLock()
	warnShown := app.warnShown
	texconvWarn := app.texconvWarn
	app.mu.RUnlock()
	if warnShown {
		g.OpenPopup("texconv_warning")
	}
	g.PopupModal("texconv_warning").Layout(
		g.Label("Warning: "+texconvWarn),
		g.Dummy(0, 10),
		g.Button("OK").OnClick(func() {
			app.mu.Lock()
			app.warnShown = false
			app.mu.Unlock()
			g.CloseCurrentPopup()
		}),
	).Build()

	g.SingleWindow().Layout(
		g.Separator(),
		g.TabBar().TabItems(
			cookTabUI(),
			matGenTabUI(),
		),
	)

	app.dirPicker.renderDraw()
}
