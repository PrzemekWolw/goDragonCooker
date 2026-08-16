# goDragonCooker

[![Build](https://github.com/PrzemekWolw/goDragonCooker/actions/workflows/build.yml/badge.svg)](https://github.com/PrzemekWolw/goDragonCooker/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/PrzemekWolw/goDragonCooker)](https://github.com/PrzemekWolw/goDragonCooker/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/PrzemekWolw/goDragonCooker/total)](https://github.com/PrzemekWolw/goDragonCooker/releases)
[![VirusTotal](https://img.shields.io/badge/VirusTotal-submitted-394eff?logo=virustotal&logoColor=white)](https://github.com/PrzemekWolw/goDragonCooker/actions/workflows/build.yml)

goDragonCooker is a desktop utility for preparing BeamNG texture and material files.

| Material Generator | Texture Cooker |
| :---: | :---: |
| <img width="100%" alt="Screenshot 2026-08-05 180738" src="https://github.com/user-attachments/assets/2856727d-9257-45f6-adc9-44f13b008cd3" /> | <img width="100%" alt="Screenshot 2026-08-05 180728" src="https://github.com/user-attachments/assets/46f32520-170d-429c-8b66-69e4a6c43864" /> |


It provides:

- Texture cooking to DDS using either Compressonator Vulkan or Texconv.
- Automatic handling of color, data, and normal texture suffixes.
- Parallel Vulkan GPU BC4, BC5, BC6H, and BC7 compression.
- Vulkan sRGB, separate-alpha, power-of-two resizing, and mip generation.
- Material JSON generation from groups of texture files.
- Optional detail-map and material-instance settings.

## Texture backends

`Compressonator (Vulkan GPU)` is the default. It loads the source, performs
power-of-two resizing and mip generation, compresses every texture on the
selected Vulkan device, and writes the complete DDS file.
Batch cooking overlaps image decoding and DDS writing with up to four Vulkan
compression jobs.

`Texconv` is the compatibility backend for systems without Vulkan support and
the only backend available on macOS. It uses DirectXTex for resizing, mip
generation, and DDS compression.

The Compressonator backend supports BC4_UNORM, BC5_UNORM, BC6H_UF16,
BC7_UNORM, and BC7_UNORM_SRGB targets. Its native loader accepts PNG, JPEG,
BMP, TGA, HDR, TIFF, and EXR sources.

## Platform support

- Windows x64 and ARM64: Compressonator Vulkan and Texconv.
- Linux x64 and ARM64: Compressonator Vulkan and Texconv.
- macOS x64 and ARM64: Texconv only, built on macOS with the Apple SDK.

GitHub Actions builds the Windows and Linux x64 and ARM64 release archives
(`.github/workflows/build.yml`): Linux ARM64 is compiled natively on an ARM64
runner, and Windows ARM64 is compiled natively on GitHub's hosted
`windows-11-arm` (Windows-on-ARM) runner — both ARM64 targets run on native
ARM64 hosts, since Go's cgo Windows linker needs a native aarch64 mingw-w64
that cannot be cross-compiled from an x64 host.

See [BUILD.md](BUILD.md) for build instructions.

## Running

The application loads native libraries from `bin/<platform>-<architecture>`
next to the executable:

- Windows: `texconv.dll`, `gdc_vulkan.dll`, runtime dependencies, and `shaders/*.spv`
- Linux: `libtexconv.so`, `libgdc_vulkan.so`, runtime dependencies, and `shaders/*.spv`
- macOS: `libtexconv.dylib`

The Compressonator backend requires a Vulkan 1.1 loader and a compute-capable
Vulkan driver. Use the GPU selector in the Texture Cooker to choose a physical
device or let the application select the best available device. Current GPU
drivers normally install the platform Vulkan loader (`vulkan-1.dll` on Windows
or `libvulkan.so.1` on Linux); all other native dependencies are included in
the release archive.

For development, first initialize the native backends with
`git submodule update --init --recursive`. On Linux, `build_release.py` also
downloads and caches the LunarG Vulkan SDK automatically when the native
Vulkan library needs rebuilding and `VULKAN_SDK` is unset. On Windows, set
`VULKAN_SDK` to an SDK containing DXC before building. Then run
`go run ./app` from the repository root.

The native backends are built from pinned sources under `third_party/` on
first run (network and nasm are required for the Texconv JPEG codec). See
[BUILD.md](BUILD.md) for the full prerequisite list and the pinned
dependency versions.

## License

goDragonCooker is distributed under the MIT License. See [LICENSE](LICENSE).
