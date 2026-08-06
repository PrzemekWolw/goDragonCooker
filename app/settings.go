package main

import (
	"encoding/json"
	"os"
	"path/filepath"
)

type appSettings struct {
	TextureBackend string `json:"textureBackend"`
	TexconvGPU     int32  `json:"texconvGPU"`
	VulkanGPU      int32  `json:"vulkanGPU"`
}

func settingsPath() string {
	directory, err := os.UserConfigDir()
	if err != nil {
		return ""
	}
	return filepath.Join(directory, "goDragonCooker", "settings.json")
}

func loadSettings() {
	path := settingsPath()
	data, err := os.ReadFile(path)
	if err != nil {
		return
	}
	var settings appSettings
	if json.Unmarshal(data, &settings) != nil {
		return
	}
	app.mu.Lock()
	defer app.mu.Unlock()
	if settings.TextureBackend == textureBackendTexconv ||
		settings.TextureBackend == textureBackendCompressonator {
		app.textureBackend = settings.TextureBackend
	}
	app.gpuAdapter = settings.TexconvGPU
	app.vulkanDevice = settings.VulkanGPU
}

func saveSettings() {
	app.mu.RLock()
	settings := appSettings{
		TextureBackend: app.textureBackend,
		TexconvGPU:     app.gpuAdapter,
		VulkanGPU:      app.vulkanDevice,
	}
	app.mu.RUnlock()
	data, err := json.MarshalIndent(settings, "", "    ")
	if err != nil {
		return
	}
	path := settingsPath()
	if path == "" || os.MkdirAll(filepath.Dir(path), 0o755) != nil {
		return
	}
	_ = os.WriteFile(path, data, 0o644)
}
