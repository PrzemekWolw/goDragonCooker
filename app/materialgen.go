package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"
	"sync"

	g "github.com/AllenDang/giu"
)

// ---------------------------------------------------------------------------
// Material Generator
// ---------------------------------------------------------------------------

type matGenSettings struct {
	suffixes         []texEntry
	customSuffixes   []texEntry // user-defined extra patterns merged on top of defaults
	detailScaleX     float64
	detailScaleY     float64
	instanceOpacity  bool
	instanceEmissive bool
	enableDetailMaps bool
}

type matGenTab struct {
	mu           sync.Mutex
	directory    string
	status       string
	logLines     []string
	running      bool
	stopGenerate chan struct{}
	settings     matGenSettings
	// patternBufs holds per-stage-key temporary strings for the UI input widgets.
	patternBufs map[string]*patternEditBuf
}

type patternEditBuf struct {
	suffix string
	extIdx int32
	dirty  bool
}

func (t *matGenTab) log(msg string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.logLines = append(t.logLines, msg)
	if len(t.logLines) > 500 {
		t.logLines = t.logLines[len(t.logLines)-500:]
	}
}

type texEntry struct {
	suffix   string // e.g. "_nm", "_b"
	ext      string // e.g. ".normal.png", ".color.jpg"
	stageKey string // JSON key: "normalMap", "baseColorMap"
}

// defaultTextureEntries is the built-in set of texture patterns.
var defaultTextureEntries = []texEntry{
	{suffix: "_nm", ext: ".normal.png", stageKey: "normalMap"},
	{suffix: "_b", ext: ".color.png", stageKey: "baseColorMap"},
	{suffix: "_d", ext: ".color.png", stageKey: "baseColorMap"},
	{suffix: "_n", ext: ".normal.png", stageKey: "normalMap"},
	{suffix: "_r", ext: ".data.png", stageKey: "roughnessMap"},
	{suffix: "_m", ext: ".data.png", stageKey: "metallicMap"},
	{suffix: "_ao", ext: ".data.png", stageKey: "ambientOcclusionMap"},
	{suffix: "_e", ext: ".color.png", stageKey: "emissiveMap"},
	{suffix: "_o", ext: ".data.png", stageKey: "opacityMap"},
}

// detailPatterns maps a base stage key -> the detail texture suffix to search for.
// e.g. if baseColorMap exists, also look for _dd.color.png -> detailMap.
var detailPatterns = map[string]struct {
	suffix   string // file suffix to search for
	ext      string // extension
	stageKey string // JSON key in output
}{
	"baseColorMap":        {"_dd", ".color.png", "detailMap"},
	"normalMap":           {"_dn", ".normal.png", "detailNormalMap"},
	"opacityMap":          {"_do", ".data.png", "opacityDetailMap"},
	"metallicMap":         {"_dm", ".data.png", "metallicDetailMap"},
	"roughnessMap":        {"_dr", ".data.png", "roughnessDetailMap"},
	"ambientOcclusionMap": {"_dao", ".data.png", "ambientOcclusionDetailMap"},
}

func matchTex(name string, entries []texEntry) (base, key string, ok bool) {
	lower := strings.ToLower(name)
	for _, e := range entries {
		suff := strings.ToLower(e.suffix + e.ext)
		if strings.HasSuffix(lower, suff) {
			return name[:len(name)-len(suff)], e.stageKey, true
		}
	}
	return "", "", false
}

type matDef struct {
	Name               string     `json:"name"`
	MapTo              string     `json:"mapTo"`
	Class              string     `json:"class"`
	MaterialTag0       string     `json:"materialTag0"`
	TranslucentBlendOp string     `json:"translucentBlendOp"`
	Version            float64    `json:"version"`
	AlphaTest          *bool      `json:"alphaTest,omitempty"`
	AlphaRef           *int       `json:"alphaRef,omitempty"`
	Stages             []matStage `json:"Stages"`
}

type matStage struct {
	AmbientOcclusionMap       *string   `json:"ambientOcclusionMap,omitempty"`
	BaseColorMap              *string   `json:"baseColorMap,omitempty"`
	NormalMap                 *string   `json:"normalMap,omitempty"`
	RoughnessMap              *string   `json:"roughnessMap,omitempty"`
	MetallicFactor            *float64  `json:"metallicFactor,omitempty"`
	MetallicMap               *string   `json:"metallicMap,omitempty"`
	OpacityMap                *string   `json:"opacityMap,omitempty"`
	EmissiveFactor            []float64 `json:"emissiveFactor,omitempty"`
	EmissiveIntensityNits     *int      `json:"emissiveIntensityNits,omitempty"`
	EmissiveMap               *string   `json:"emissiveMap,omitempty"`
	DetailMap                 *string   `json:"detailMap,omitempty"`
	DetailNormalMap           *string   `json:"detailNormalMap,omitempty"`
	OpacityDetailMap          *string   `json:"opacityDetailMap,omitempty"`
	MetallicDetailMap         *string   `json:"metallicDetailMap,omitempty"`
	RoughnessDetailMap        *string   `json:"roughnessDetailMap,omitempty"`
	AmbientOcclusionDetailMap *string   `json:"ambientOcclusionDetailMap,omitempty"`
	DetailScale               []float64 `json:"detailScale,omitempty"`
	InstanceOpacity           *bool     `json:"instanceOpacity,omitempty"`
	InstanceEmissive          *bool     `json:"instanceEmissive,omitempty"`
}

// strPtr is a convenience for taking the address of a string literal.
func strPtr(s string) *string { return &s }

func buildMat(baseName, texPath string, tex map[string]string, settings *matGenSettings) matDef {
	stage := matStage{}
	one := 1.0
	twoK := 2000

	if v, ok := tex["ambientOcclusionMap"]; ok {
		stage.AmbientOcclusionMap = strPtr(buildTexPath(texPath, v))
	}
	if v, ok := tex["baseColorMap"]; ok {
		stage.BaseColorMap = strPtr(buildTexPath(texPath, v))
	}
	if v, ok := tex["normalMap"]; ok {
		stage.NormalMap = strPtr(buildTexPath(texPath, v))
	}
	if v, ok := tex["roughnessMap"]; ok {
		stage.RoughnessMap = strPtr(buildTexPath(texPath, v))
	}
	if v, ok := tex["metallicMap"]; ok {
		stage.MetallicFactor = &one
		stage.MetallicMap = strPtr(buildTexPath(texPath, v))
	}
	if v, ok := tex["opacityMap"]; ok {
		stage.OpacityMap = strPtr(buildTexPath(texPath, v))
	}
	if v, ok := tex["emissiveMap"]; ok {
		stage.EmissiveFactor = []float64{1.0, 1.0, 1.0}
		stage.EmissiveIntensityNits = &twoK
		stage.EmissiveMap = strPtr(buildTexPath(texPath, v))
	}

	// Wire detail maps from tex map if they were detected by runMatGen.
	if settings.enableDetailMaps {
		hasAnyDetail := false
		for _, dpKey := range []string{"detailMap", "detailNormalMap", "opacityDetailMap", "metallicDetailMap", "roughnessDetailMap", "ambientOcclusionDetailMap"} {
			if v, ok := tex[dpKey]; ok {
				hasAnyDetail = true
				switch dpKey {
				case "detailMap":
					stage.DetailMap = strPtr(buildTexPath(texPath, v))
				case "detailNormalMap":
					stage.DetailNormalMap = strPtr(buildTexPath(texPath, v))
				case "opacityDetailMap":
					stage.OpacityDetailMap = strPtr(buildTexPath(texPath, v))
				case "metallicDetailMap":
					stage.MetallicDetailMap = strPtr(buildTexPath(texPath, v))
				case "roughnessDetailMap":
					stage.RoughnessDetailMap = strPtr(buildTexPath(texPath, v))
				case "ambientOcclusionDetailMap":
					stage.AmbientOcclusionDetailMap = strPtr(buildTexPath(texPath, v))
				}
			}
		}
		if hasAnyDetail {
			stage.DetailScale = []float64{settings.detailScaleX, settings.detailScaleY}
		}
	}

	// Instance flags.
	if settings.instanceOpacity {
		stage.InstanceOpacity = &trueVal
	}
	if settings.instanceEmissive {
		stage.InstanceEmissive = &trueVal
	}

	def := matDef{
		Name:               baseName,
		MapTo:              baseName,
		Class:              "Material",
		MaterialTag0:       "dragon2ng",
		TranslucentBlendOp: "None",
		Version:            1.5,
		Stages:             []matStage{stage},
	}
	if _, ok := tex["opacityMap"]; ok {
		def.AlphaTest = &trueVal
		ref := 64
		def.AlphaRef = &ref
	}
	return def
}

var trueVal = true

func buildTexPath(texPath, origFile string) string {
	return fmt.Sprintf("%s%s", texPath, origFile)
}

func computeTexPath(root string) (string, error) {
	parts := strings.Split(filepath.ToSlash(filepath.Clean(root)), "/")
	if index := levelsPartIndex(parts); index >= 0 {
		return "/" + strings.Join(parts[index:], "/") + "/", nil
	}
	return "", fmt.Errorf("no 'levels' directory found in: %s", root)
}

func findLevelsRoot(root string) (string, error) {
	parts := strings.Split(filepath.ToSlash(filepath.Clean(root)), "/")
	if index := levelsPartIndex(parts); index >= 0 {
		return filepath.FromSlash(strings.Join(parts[:index+1], "/")), nil
	}
	return "", fmt.Errorf("no 'levels' directory found in: %s", root)
}

func levelsPartIndex(parts []string) int {
	for i, p := range parts {
		if strings.EqualFold(p, "levels") {
			return i
		}
	}
	return -1
}

func computeTexPathForDirectory(levelsRoot, directory string) string {
	relative, err := filepath.Rel(filepath.Dir(levelsRoot), directory)
	if err != nil {
		return ""
	}
	return "/" + filepath.ToSlash(relative) + "/"
}

func mergeMaterials(path string, generated map[string]matDef) (int, error) {
	merged := make(map[string]json.RawMessage, len(generated))
	if data, err := os.ReadFile(path); err == nil {
		if err := json.Unmarshal(data, &merged); err != nil {
			return 0, fmt.Errorf("invalid existing materials file: %w", err)
		}
	} else if !os.IsNotExist(err) {
		return 0, err
	}
	if merged == nil {
		merged = make(map[string]json.RawMessage, len(generated))
	}

	existingNames := make(map[string]struct{}, len(merged))
	for name, raw := range merged {
		existingNames[name] = struct{}{}
		var material struct {
			Name string `json:"name"`
		}
		if err := json.Unmarshal(raw, &material); err == nil && material.Name != "" {
			existingNames[material.Name] = struct{}{}
		}
	}
	added := 0
	for name, material := range generated {
		if _, exists := existingNames[name]; exists {
			continue
		}
		data, err := json.Marshal(material)
		if err != nil {
			return 0, err
		}
		merged[name] = data
		existingNames[name] = struct{}{}
		added++
	}
	if added == 0 {
		return 0, nil
	}

	data, err := json.MarshalIndent(merged, "", "  ")
	if err != nil {
		return 0, err
	}
	data = append(data, '\n')
	if err := os.WriteFile(path, data, 0644); err != nil {
		return 0, err
	}
	return added, nil
}

func runMatGen(directory string, tab *matGenTab) {
	tab.mu.Lock()
	tab.running = true
	tab.status = "Scanning..."
	stopGenerate := tab.stopGenerate
	settings := tab.settings
	settings.suffixes = slices.Clone(settings.suffixes)
	settings.customSuffixes = slices.Clone(settings.customSuffixes)
	tab.mu.Unlock()
	g.Update()

	texPath, err := computeTexPath(directory)
	if err != nil {
		tab.log(fmt.Sprintf("ERROR: %v", err))
		tab.mu.Lock()
		tab.running = false
		tab.status = "Error"
		tab.mu.Unlock()
		return
	}

	tab.log(fmt.Sprintf("Textures root: %s", directory))
	tab.log(fmt.Sprintf("Tex path:     %s", texPath))
	levelsRoot, err := findLevelsRoot(directory)
	if err != nil {
		tab.log(fmt.Sprintf("ERROR: %v", err))
		tab.mu.Lock()
		tab.running = false
		tab.status = "Error"
		tab.mu.Unlock()
		return
	}

	// Merge default and custom suffixes for scanning.
	allEntries := make([]texEntry, 0, len(settings.suffixes)+len(settings.customSuffixes))
	allEntries = append(allEntries, settings.suffixes...)
	allEntries = append(allEntries, settings.customSuffixes...)
	if len(settings.customSuffixes) > 0 {
		tab.log(fmt.Sprintf("Custom patterns: %d entries", len(settings.customSuffixes)))
	}

	if settings.enableDetailMaps {
		tab.log(fmt.Sprintf("Detail maps:  enabled (scale %.2f x %.2f)", settings.detailScaleX, settings.detailScaleY))
	}
	if settings.instanceOpacity {
		tab.log("Instance opacity: true")
	}
	if settings.instanceEmissive {
		tab.log("Instance emissive: true")
	}
	tab.log("---")

	totalGroups := 0

	err = filepath.Walk(directory, func(path string, info os.FileInfo, err error) error {
		if err != nil || !info.IsDir() {
			return err
		}

		select {
		case <-stopGenerate:
			tab.log("Generation cancelled.")
			finishMatGen(tab, "Cancelled")
			return filepath.SkipAll
		default:
		}

		entries, rerr := os.ReadDir(path)
		if rerr != nil {
			return nil
		}

		// Collect all file names for detail map lookups.
		fileNames := make(map[string]string)
		for _, e := range entries {
			if !e.IsDir() {
				lower := strings.ToLower(e.Name())
				if _, exists := fileNames[lower]; !exists {
					fileNames[lower] = e.Name()
				}
			}
		}

		// Build base texture map per material name.
		mat := make(map[string]map[string]string)
		for _, e := range entries {
			if e.IsDir() {
				continue
			}
			base, key, ok := matchTex(e.Name(), allEntries)
			if !ok || base == "" {
				continue
			}
			if mat[base] == nil {
				mat[base] = make(map[string]string)
			}
			if _, dup := mat[base][key]; !dup {
				mat[base][key] = e.Name()
			}
		}

		if len(mat) == 0 {
			return nil
		}

		// Wire up detail maps if enabled.
		if settings.enableDetailMaps {
			for baseKey, textures := range mat {
				for baseStageKey, dp := range detailPatterns {
					_, hasBase := textures[baseStageKey]
					if !hasBase {
						continue
					}
					detailName := strings.ToLower(baseKey + dp.suffix + dp.ext)
					if actualName, exists := fileNames[detailName]; exists {
						textures[dp.stageKey] = actualName
					}
				}
			}
		}

		output := make(map[string]matDef)
		keys := make([]string, 0, len(mat))
		for k := range mat {
			keys = append(keys, k)
		}
		sort.Strings(keys)

		directoryTexPath := computeTexPathForDirectory(levelsRoot, path)
		for _, base := range keys {
			output[base] = buildMat(base, directoryTexPath, mat[base], &settings)
		}

		outPath := filepath.Join(path, filepath.Base(path)+".materials.json")
		added, werr := mergeMaterials(outPath, output)
		if werr != nil {
			tab.log(fmt.Sprintf("  WRITE ERROR in %s: %v", filepath.Base(path), werr))
			return nil
		}

		totalGroups += added
		if added == 0 {
			tab.log(fmt.Sprintf("  %s/ -> unchanged (%d existing materials)", filepath.Base(path), len(output)))
		} else {
			tab.log(fmt.Sprintf("  %s/ -> %s.materials.json (%d new materials)", filepath.Base(path), filepath.Base(path), added))
		}

		tab.mu.Lock()
		tab.status = fmt.Sprintf("Generated %d material groups", totalGroups)
		tab.mu.Unlock()
		g.Update()
		return nil
	})

	if err != nil && err != filepath.SkipAll {
		tab.log(fmt.Sprintf("Walk error: %v", err))
	} else if err == nil {
		tab.log(fmt.Sprintf("\nDone. %d material groups generated.", totalGroups))
	}

	finishMatGen(tab, fmt.Sprintf("Done: %d groups", totalGroups))
}

func finishMatGen(tab *matGenTab, status string) {
	tab.mu.Lock()
	tab.running = false
	tab.status = status
	tab.mu.Unlock()
	g.Update()
}

// ---------------------------------------------------------------------------
// Default settings initialization
// ---------------------------------------------------------------------------

func defaultMatGenSettings() matGenSettings {
	return matGenSettings{
		suffixes:     slices.Clone(defaultTextureEntries),
		detailScaleX: 0.1,
		detailScaleY: 0.1,
	}
}
