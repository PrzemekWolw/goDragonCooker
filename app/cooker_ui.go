package main

import (
	"strings"

	g "github.com/AllenDang/giu"
)

func cookTabUI() *g.TabItemWidget {
	return g.TabItem("Texture Cooker").Layout(
		g.Label("Textures directory:"),
		g.Row(
			g.Custom(func() {
				c := app.tabCook
				c.mu.Lock()
				defer c.mu.Unlock()
				g.InputText(&c.directory).Build()
			}),
			g.Button("Browse").OnClick(func() {
				c := app.tabCook
				c.mu.Lock()
				directory := c.directory
				c.mu.Unlock()
				ch := make(chan string)
				app.dirPicker.launch(ch, directory)
				go func() {
					if d := <-ch; d != "" {
						c.mu.Lock()
						c.directory = d
						c.mu.Unlock()
					}
				}()
			}),
		),

		g.Separator(),

		g.Custom(func() {
			c := app.tabCook
			c.mu.Lock()
			defer c.mu.Unlock()
			g.Checkbox("Assume .data textures are grayscale (BC4)", &c.autoDetectFmt).Build()
			g.Tooltip("Data textures will be cooked as grayscale to reduce memory usage regardless of input format mode.").Build()
		}),

		g.Separator(),

		g.Custom(func() {
			c := app.tabCook
			c.mu.Lock()
			directory := c.directory
			running := c.running
			c.mu.Unlock()
			label, click := "Run Texture Cooker", func() {
				if directory == "" {
					return
				}
				c.log("")
				go runCooker(directory, c)
			}
			if running {
				label = "Stop Cooker"
				click = func() {
					c.mu.Lock()
					close(c.stopCook)
					c.stopCook = make(chan struct{})
					c.mu.Unlock()
				}
			}
			g.Button(label).OnClick(click).Build()
		}),

		g.Custom(func() {
			c := app.tabCook
			c.mu.Lock()
			st := c.status
			c.mu.Unlock()
			if st == "" {
				st = "Waiting..."
			}
			g.Label("Status: " + st).Build()
		}),

		g.Separator(),
		g.Label("Output:"),

		g.Child().Size(g.Auto, 280).Border(true).Layout(
			g.Custom(func() {
				c := app.tabCook
				c.mu.Lock()
				logLines := append([]string(nil), c.logLines...)
				c.mu.Unlock()
				for _, line := range logLines {
					col := colText
					lower := strings.ToLower(line)
					if strings.HasPrefix(lower, "  ok:") {
						col = colCookOk
					} else if strings.HasPrefix(lower, "  fail:") || strings.HasPrefix(lower, "error") {
						col = colCookErr
					} else if strings.HasPrefix(lower, "  skip:") {
						col = colTextDim
					}
					g.PushStyleColor(g.StyleColorText, col)
					g.Labelf("%s", line).Build()
					g.PopStyleColor()
				}
			}),
		),

		g.Custom(gpuControlsUI),
	)
}

func gpuControlsUI() {
	app.mu.RLock()
	report := append([]string(nil), app.gpuReport...)
	configuredGPU := app.gpuAdapter
	checking := app.gpuChecking
	app.mu.RUnlock()
	g.Separator()

	if isWindows() {
		options := gpuAdapterOptions()
		comboIndex := int32(gpuAdapterOptionIndex(int(configuredGPU)))
		if comboIndex < 0 || comboIndex >= int32(len(options)) {
			comboIndex = 0
		}
		g.Label("Texconv GPU:").Build()
		g.SameLine()
		g.Combo("##texconv_gpu", options[comboIndex], options, &comboIndex).
			Size(320).
			OnChange(func() {
				app.mu.Lock()
				app.gpuAdapter = int32(gpuAdapterIndexForOption(int(comboIndex)))
				app.mu.Unlock()
				rerunGPUCheck()
			}).Build()
		if checking {
			g.SameLine()
			g.Label("Testing...").Build()
		}
	} else if checking {
		g.Label("Checking hardware...").Build()
	}

	if len(report) == 0 {
		return
	}
	g.PushStyleColor(g.StyleColorText, colTextDim)
	g.Label("System:").Build()
	for _, line := range report {
		g.Labelf("%s", line).Build()
	}
	g.PopStyleColor()
}
