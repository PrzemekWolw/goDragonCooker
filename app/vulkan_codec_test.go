package main

import "testing"

func TestVulkanFormats(t *testing.T) {
	formats := map[string]uint32{
		"BC4_UNORM":      1,
		"BC5_UNORM":      2,
		"BC6H_UF16":      3,
		"BC7_UNORM":      4,
		"BC7_UNORM_SRGB": 5,
	}
	for name, expected := range formats {
		actual, ok := vulkanFormat(name)
		if !ok || actual != expected {
			t.Fatalf("vulkanFormat(%q) = %d, %v; want %d, true", name, actual, ok, expected)
		}
	}
	if _, ok := vulkanFormat("R8G8B8A8_UNORM"); ok {
		t.Fatal("unsupported Vulkan target format was accepted")
	}
}

func TestCompressonatorIsDefaultBackend(t *testing.T) {
	if app.textureBackend != textureBackendCompressonator {
		t.Fatalf("default backend = %q; want %q", app.textureBackend, textureBackendCompressonator)
	}
}

func TestVulkanPipelineOverlapsCPUWork(t *testing.T) {
	if vulkanPipelineWorkerCount <= vulkanWorkerCount {
		t.Fatalf(
			"pipeline workers = %d; want more than %d GPU workers",
			vulkanPipelineWorkerCount,
			vulkanWorkerCount,
		)
	}
}
