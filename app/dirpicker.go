package main

import (
	"os"
	"path/filepath"
	"strings"

	g "github.com/AllenDang/giu"
)

// dirPickerState holds the in-app directory browser state.
type dirPickerState struct {
	open    bool
	pathStr string
	result  chan<- string
}

func newDirPicker() *dirPickerState {
	return &dirPickerState{}
}

// launch opens the directory picker. The caller passes a channel to receive the result.
func (d *dirPickerState) launch(result chan string, lastPath string) {
	d.open = true
	d.result = result
	if lastPath != "" {
		d.pathStr = lastPath
	} else if d.pathStr == "" {
		d.pathStr, _ = os.Getwd()
	}
}

func (d *dirPickerState) closePicker(chosen string) {
	d.open = false
	if d.result != nil {
		d.result <- chosen
		close(d.result)
		d.result = nil
	}
}

// renderDraw must be called every frame from the giu loop.
func (d *dirPickerState) renderDraw() {
	if !d.open {
		return
	}

	window := g.Window("Pick Directory").Size(650, 500).IsOpen(&d.open)
	window.BringToFront()
	window.Layout(
		g.Row(
			g.InputText(&d.pathStr),
			g.Custom(func() {
				dir := filepath.Dir(d.pathStr)
				g.Button("Up").OnClick(func() {
					if dir != d.pathStr {
						d.pathStr = dir
					}
				}).Build()
			}),
		),
		g.Separator(),

		g.Label("Folders"),
		g.Child().Size(g.Auto, -70).Border(true).Layout(
			g.Custom(func() {
				d.renderListing()
			}),
		),

		g.Row(
			g.Button("Select").OnClick(func() {
				d.closePicker(d.pathStr)
			}),
			g.Button("Cancel").OnClick(func() {
				d.closePicker("")
			}),
		),
	)
}

func (d *dirPickerState) renderListing() {
	path := d.pathStr
	info, err := os.Stat(path)
	if err != nil || !info.IsDir() {
		g.Labelf("Cannot read: %s", path).Build()
		return
	}

	entries, err := os.ReadDir(path)
	if err != nil {
		return
	}

	for _, e := range entries {
		if !e.IsDir() || strings.HasPrefix(e.Name(), ".") {
			continue
		}
		full := filepath.Join(path, e.Name())
		name := e.Name() + "/"
		g.Selectable(name).Selected(false).OnClick(func() {
			d.pathStr = full
		}).Build()
	}

}
