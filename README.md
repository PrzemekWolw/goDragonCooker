# goDragonCooker

[![Build](https://github.com/PrzemekWolw/goDragonCooker/actions/workflows/build.yml/badge.svg)](https://github.com/PrzemekWolw/goDragonCooker/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/PrzemekWolw/goDragonCooker)](https://github.com/PrzemekWolw/goDragonCooker/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/PrzemekWolw/goDragonCooker/total)](https://github.com/PrzemekWolw/goDragonCooker/releases)
[![VirusTotal](https://img.shields.io/badge/VirusTotal-submitted-394eff?logo=virustotal&logoColor=white)](https://github.com/PrzemekWolw/goDragonCooker/actions/workflows/build.yml)

goDragonCooker is a desktop utility for preparing BeamNG textures, materials, and models.

| Material Generator | Texture Cooker |
| :---: | :---: |
| <img width="100%" alt="Screenshot 2026-08-05 180738" src="https://github.com/user-attachments/assets/2856727d-9257-45f6-adc9-44f13b008cd3" /> | <img width="100%" alt="Screenshot 2026-08-05 180728" src="https://github.com/user-attachments/assets/46f32520-170d-429c-8b66-69e4a6c43864" /> |

## Texture Cooker

Cooks textures to DDS with Compressonator Vulkan or Texconv.

- Color, data, dataR, and normal suffixes pick the compression format.
- Vulkan compresses BC4, BC5, BC6H, and BC7 on the GPU, several textures at once, with sRGB, separate alpha, power-of-two resizing, and mip generation.
- Optional quality check after cooking: the DDS is decoded and compared with the source (PSNR, maximum error, SSIM, and a per-block check). A file below its target can be cooked again with Texconv, and the retry is kept when it is clearly better.

## Material Generator

Writes `.materials.json` from groups of texture files. Detail maps and per-stage instance opacity and emissive flags are optional. Generation looks for a `levels/` directory in the chosen path.

## Model Converter

Converts between BeamNG shapes and the formats Assimp reads and writes.

- Targets are CDAE v31, DTS v24 through v29 (v27 and v28 are labelled unverified), and every Assimp exporter, including glTF 2.0 with optional Draco mesh compression, FBX, Collada, OBJ, STL, and PLY.
- Scan a directory, tick the source extensions to read, and pick one target from the dropdown. That format's settings appear beside it.
- Files are written beside the source, or into a folder that mirrors the source tree. An output that already exists is skipped until overwrite is enabled.
- Conversion uses every available CPU core.
- Optional validation re-reads each written file and compares geometry, normals, UVs, colors, tangents, materials, skin weights, and animation with the source. The log shows a pass, warning, or failure per file, and the run writes `model_validation.json`.
- Collada files the game still loads are repaired on import when the XML declaration is broken or the scene reference points nowhere. BeamNG `.ter` terrain and Blender `.blend` files are not offered as sources.

## Languages

English, Polish, Spanish, German, and Italian. The first run follows the system language, and the choice in the top-right corner is saved. The texts live in `app/locales/<locale>/main.translation.json`, in the same flat `id` to text layout as BeamNG's locale files, and are built into the executable.

## Texture backends

`Compressonator (Vulkan GPU)` is the default. It loads the source, performs
power-of-two resizing and mip generation, compresses every texture on the
selected Vulkan device, and writes the complete DDS file.
Batch cooking overlaps image decoding and DDS writing with up to four Vulkan
compression jobs.

`Texconv` is the compatibility backend for systems without Vulkan support. It
uses DirectXTex for resizing, mip generation, and DDS compression.

The Compressonator backend supports BC4_UNORM, BC5_UNORM, BC6H_UF16,
BC7_UNORM, and BC7_UNORM_SRGB targets. Its native loader accepts PNG, JPEG,
BMP, TGA, HDR, TIFF, and EXR sources.

## Platform support

- Windows x64 and ARM64: Compressonator Vulkan and Texconv.
- Linux x64 and ARM64: Compressonator Vulkan and Texconv.
- macOS ARM64: Compressonator Vulkan via MoltenVK or KosmicKrisp and Texconv.

See [BUILD.md](BUILD.md) for build instructions.

## Running

Settings are saved per user: `%APPDATA%/goDragonCooker/settings.json` on
Windows, `~/Library/Application Support/goDragonCooker/settings.json` on
macOS, and `$XDG_CONFIG_HOME` or `~/.config/goDragonCooker/settings.json`
on Linux.

The application loads native libraries from `bin/<platform>-<architecture>`
next to the executable:

- Windows: `texconv.dll`, `gdc_vulkan.dll`, runtime dependencies, and `shaders/*.spv`
- Linux: `libtexconv.so`, `libgdc_vulkan.so`, runtime dependencies, and `shaders/*.spv`
- macOS: `libtexconv.dylib`, `libgdc_vulkan.dylib`, the Vulkan loader, MoltenVK, KosmicKrisp when shipped, their ICD manifests, and `shaders/*.spv`

The Compressonator backend requires a Vulkan 1.1 loader and a compute-capable
Vulkan driver. Use the GPU selector in the Texture Cooker to choose a physical
device or let the application select the best available device.

For development, first initialize the submodules (the texture backends and
Assimp) with `git submodule update --init --recursive`. On Linux, `build_release.py` also
downloads and caches the LunarG Vulkan SDK automatically when the native
Vulkan library needs rebuilding and `VULKAN_SDK` is unset. On Windows, set
`VULKAN_SDK` to an SDK containing DXC before building. Then build and run from
the repository root:

```bash
cmake -S . -B .build/app -DCMAKE_BUILD_TYPE=Release
cmake --build .build/app --config Release
```

The native backends are built from pinned sources under `third_party/` on
first run (network and nasm are required for the Texconv JPEG codec). See
[BUILD.md](BUILD.md) for the full prerequisite list and the pinned
dependency versions.

## License

goDragonCooker is distributed under the MIT License. See [LICENSE](LICENSE).
