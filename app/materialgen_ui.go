package main

import (
	"slices"
	"strconv"
	"strings"

	g "github.com/AllenDang/giu"
)

var defaultPatterns = map[string]patternEditBuf{
	"baseColorMap":              {suffix: "_b", extIdx: 0},
	"normalMap":                 {suffix: "_nm", extIdx: 1},
	"roughnessMap":              {suffix: "_r", extIdx: 2},
	"metallicMap":               {suffix: "_m", extIdx: 2},
	"ambientOcclusionMap":       {suffix: "_ao", extIdx: 2},
	"emissiveMap":               {suffix: "_e", extIdx: 0},
	"opacityMap":                {suffix: "_o", extIdx: 2},
	"detailMap":                 {suffix: "_dd", extIdx: 0},
	"detailNormalMap":           {suffix: "_dn", extIdx: 1},
	"opacityDetailMap":          {suffix: "_do", extIdx: 2},
	"metallicDetailMap":         {suffix: "_dm", extIdx: 2},
	"roughnessDetailMap":        {suffix: "_dr", extIdx: 2},
	"ambientOcclusionDetailMap": {suffix: "_dao", extIdx: 2},
}

var allStageKeys = []string{
	"baseColorMap", "normalMap", "roughnessMap", "metallicMap",
	"ambientOcclusionMap", "emissiveMap", "opacityMap",
	"detailMap", "detailNormalMap", "opacityDetailMap",
	"metallicDetailMap", "roughnessDetailMap", "ambientOcclusionDetailMap",
}

var allExtOpts = []string{
	".color.png", ".normal.png", ".data.png",
	".color.jpg", ".normal.jpg", ".data.jpg",
}

func matGenTabUI() *g.TabItemWidget {
	return g.TabItem("Material Generator").Layout(
		g.Label("Textures directory:"),
		g.Row(
			g.Custom(func() {
				m := app.tabMatGen
				m.mu.Lock()
				defer m.mu.Unlock()
				g.InputText(&m.directory).Build()
			}),
			g.Button("Browse").OnClick(func() {
				m := app.tabMatGen
				m.mu.Lock()
				directory := m.directory
				m.mu.Unlock()
				ch := make(chan string)
				app.dirPicker.launch(ch, directory)
				go func() {
					if d := <-ch; d != "" {
						m.mu.Lock()
						m.directory = d
						m.mu.Unlock()
					}
				}()
			}),
		),

		g.Separator(),

		g.Custom(func() {
			g.TreeNode("Basic Settings").
				Flags(g.TreeNodeFlagsDefaultOpen).
				Layout(
					g.Custom(func() {
						m := app.tabMatGen
						m.mu.Lock()
						defer m.mu.Unlock()
						g.Checkbox("Instance opacity", &m.settings.instanceOpacity).Build()
						g.Tooltip("Set instanceOpacity=true on stages that have an opacity map.").Build()
					}),
					g.Custom(func() {
						m := app.tabMatGen
						m.mu.Lock()
						defer m.mu.Unlock()
						g.Checkbox("Instance emissive", &m.settings.instanceEmissive).Build()
						g.Tooltip("Set instanceEmissive=true on stages that have an emissive map.").Build()
					}),
				).Build()
		}),

		g.Custom(func() {
			g.TreeNode("Detail Maps").
				Layout(
					g.Custom(func() {
						m := app.tabMatGen
						m.mu.Lock()
						defer m.mu.Unlock()
						g.Checkbox("Enable detail maps (wire up _dd, _dn, etc.)", &m.settings.enableDetailMaps).Build()
						g.Tooltip("When both base and detail textures exist in a directory, auto-wire them into the output.").Build()
					}),
					g.Custom(func() {
						m := app.tabMatGen
						m.mu.Lock()
						enabled := m.settings.enableDetailMaps
						x := float32(m.settings.detailScaleX)
						y := float32(m.settings.detailScaleY)
						m.mu.Unlock()
						if !enabled {
							return
						}
						g.Label("Detail scale:").Build()
						g.SameLine()
						g.InputFloat(&x).StepSize(0.01).Format("%.2f").OnChange(func() {
							m.mu.Lock()
							m.settings.detailScaleX = float64(x)
							m.mu.Unlock()
						}).Build()
						g.Tooltip("UV tiling X for detail maps.").Build()
						g.SameLine()
						g.Label("x ").Build()
						g.SameLine()
						g.InputFloat(&y).StepSize(0.01).Format("%.2f").OnChange(func() {
							m.mu.Lock()
							m.settings.detailScaleY = float64(y)
							m.mu.Unlock()
						}).Build()
						g.Tooltip("UV tiling Y for detail maps.").Build()
					}),
				).Build()
		}),

		g.Custom(func() {
			g.TreeNode("Texture Patterns").Layout(
				g.Custom(func() {
					g.PushStyleColor(g.StyleColorText, colTextDim)
					g.Label("Built-in patterns are used automatically. Add a custom pattern for a different filename convention.").Build()
					g.PopStyleColor()
				}),
				g.Custom(patternsRowUI),
			).Build()
		}),

		g.Separator(),

		g.Custom(func() {
			m := app.tabMatGen
			m.mu.Lock()
			directory := m.directory
			running := m.running
			status := m.status
			m.mu.Unlock()
			label, click := "Generate Materials", func() {
				if directory == "" {
					return
				}
				m.log("")
				go runMatGen(directory, m)
			}
			if running {
				label = "Stop Generator"
				click = func() {
					m.mu.Lock()
					close(m.stopGenerate)
					m.stopGenerate = make(chan struct{})
					m.mu.Unlock()
				}
			}
			g.Button(label).OnClick(click).Build()
			g.SameLine()
			if status == "" {
				status = "Waiting..."
			}
			g.Label("Status: " + status).Build()
		}),

		g.Separator(),
		g.Label("Output:"),

		g.Child().Size(g.Auto, g.Auto).Border(true).Layout(
			g.Custom(func() {
				m := app.tabMatGen
				m.mu.Lock()
				logLines := append([]string(nil), m.logLines...)
				m.mu.Unlock()
				for _, line := range logLines {
					col := colText
					lower := strings.ToLower(line)
					if strings.Contains(lower, "error") || strings.Contains(lower, "fail") {
						col = colCookErr
					}
					g.PushStyleColor(g.StyleColorText, col)
					g.Labelf("%s", line).Build()
					g.PopStyleColor()
				}
			}),
		),
	)
}

var patternNames = map[string]string{
	"baseColorMap":              "Base color",
	"normalMap":                 "Normal",
	"roughnessMap":              "Roughness",
	"metallicMap":               "Metallic",
	"ambientOcclusionMap":       "Ambient occlusion",
	"emissiveMap":               "Emissive",
	"opacityMap":                "Opacity",
	"detailMap":                 "Detail color",
	"detailNormalMap":           "Detail normal",
	"opacityDetailMap":          "Detail opacity",
	"metallicDetailMap":         "Detail metallic",
	"roughnessDetailMap":        "Detail roughness",
	"ambientOcclusionDetailMap": "Detail ambient occlusion",
}

var patternDescriptions = map[string]string{
	"baseColorMap":              "Main surface color",
	"normalMap":                 "Surface normal detail",
	"roughnessMap":              "Surface roughness",
	"metallicMap":               "Metal surface mask",
	"ambientOcclusionMap":       "Ambient light occlusion",
	"emissiveMap":               "Self-lit surface",
	"opacityMap":                "Transparency mask",
	"detailMap":                 "Fine color detail",
	"detailNormalMap":           "Fine normal detail",
	"opacityDetailMap":          "Fine transparency detail",
	"metallicDetailMap":         "Fine metallic detail",
	"roughnessDetailMap":        "Fine roughness detail",
	"ambientOcclusionDetailMap": "Fine occlusion detail",
}

func patternsRowUI() {
	m := app.tabMatGen
	m.mu.Lock()
	for _, stage := range allStageKeys {
		if m.patternBufs[stage] == nil {
			def := defaultPatterns[stage]
			m.patternBufs[stage] = &def
		}
	}
	m.mu.Unlock()

	g.Child().Size(g.Auto, 390).Border(true).Layout(
		g.Custom(func() {
			for _, stage := range allStageKeys {
				renderPatternRow(stage)
			}
		}),
	).Build()
}

func renderPatternRow(stage string) {
	m := app.tabMatGen
	m.mu.Lock()
	customIndexes := make([]int, 0)
	for i, entry := range m.settings.customSuffixes {
		if entry.stageKey == stage {
			customIndexes = append(customIndexes, i)
		}
	}
	draft := m.patternBufs[stage]
	m.mu.Unlock()

	name := patternNames[stage]
	description := patternDescriptions[stage]

	g.Separator()
	g.PushStyleColor(g.StyleColorText, colTextDim)
	g.Label(name).Build()
	g.PopStyleColor()

	for _, index := range customIndexes {
		g.Row(customPatternWidgets(stage, index)...).Build()
	}

	markDirty := func() { draft.dirty = true }
	g.Row(
		g.InputText(&draft.suffix).Size(75).OnChange(markDirty),
		g.Combo("##pat_"+stage+"_ext", allExtOpts[draft.extIdx], allExtOpts, &draft.extIdx).Size(110).OnChange(markDirty),
		g.Button("Add pattern##add_"+stage).OnClick(func() {
			patternAdd(stage)
		}),
	).Build()
	g.Tooltip(description).Build()
}

func customPatternWidgets(stage string, index int) []g.Widget {
	key := patternBufferKey(stage, index)
	m := app.tabMatGen
	m.mu.Lock()
	if index < 0 || index >= len(m.settings.customSuffixes) || m.settings.customSuffixes[index].stageKey != stage {
		m.mu.Unlock()
		return nil
	}
	entry := m.settings.customSuffixes[index]
	buf := m.patternBufs[key]
	if buf == nil {
		buf = &patternEditBuf{suffix: entry.suffix}
		for i, option := range allExtOpts {
			if option == entry.ext {
				buf.extIdx = int32(i)
				break
			}
		}
		m.patternBufs[key] = buf
	} else if !buf.dirty {
		buf.suffix = entry.suffix
		for i, option := range allExtOpts {
			if option == entry.ext {
				buf.extIdx = int32(i)
				break
			}
		}
	}
	m.mu.Unlock()

	markDirty := func() { buf.dirty = true }
	return []g.Widget{
		g.InputText(&buf.suffix).Size(75).OnChange(markDirty),
		g.Combo("##pat_"+stage+"_"+strconv.Itoa(index), allExtOpts[buf.extIdx], allExtOpts, &buf.extIdx).Size(110).OnChange(markDirty),
		g.Row(
			g.Button("Save##upd_"+stage+"_"+strconv.Itoa(index)).OnClick(func() {
				patternSave(stage, index)
			}),
			g.Button("Remove##rem_"+stage+"_"+strconv.Itoa(index)).OnClick(func() {
				patternRemove(stage, index)
			}),
		),
	}
}

func patternBufferKey(stage string, index int) string {
	return stage + "#" + strconv.Itoa(index)
}

func patternSave(stage string, index int) {
	m := app.tabMatGen
	m.mu.Lock()
	defer m.mu.Unlock()
	buf := m.patternBufs[patternBufferKey(stage, index)]
	if buf == nil {
		return
	}
	if index < 0 || index >= len(m.settings.customSuffixes) || m.settings.customSuffixes[index].stageKey != stage {
		return
	}
	trimSuf := strings.TrimSpace(buf.suffix)
	if trimSuf == "" {
		return
	}
	m.settings.customSuffixes[index] = texEntry{
		suffix:   trimSuf,
		ext:      allExtOpts[buf.extIdx],
		stageKey: stage,
	}
	buf.dirty = false
	g.Update()
}

func patternRemove(stage string, index int) {
	m := app.tabMatGen
	m.mu.Lock()
	defer m.mu.Unlock()
	if index < 0 || index >= len(m.settings.customSuffixes) || m.settings.customSuffixes[index].stageKey != stage {
		return
	}
	m.settings.customSuffixes = slices.Delete(m.settings.customSuffixes, index, index+1)
	prefix := stage + "#"
	for key := range m.patternBufs {
		if strings.HasPrefix(key, prefix) {
			delete(m.patternBufs, key)
		}
	}
	g.Update()
}

func patternAdd(stage string) {
	m := app.tabMatGen
	m.mu.Lock()
	defer m.mu.Unlock()
	buf := m.patternBufs[stage]
	if buf == nil {
		return
	}
	trimSuf := strings.TrimSpace(buf.suffix)
	if trimSuf == "" {
		return
	}
	m.settings.customSuffixes = append(m.settings.customSuffixes, texEntry{
		suffix:   trimSuf,
		ext:      allExtOpts[buf.extIdx],
		stageKey: stage,
	})
	buf.dirty = false
	g.Update()
}
