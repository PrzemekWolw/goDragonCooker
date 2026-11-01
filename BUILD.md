# Building goDragonCooker

Release builds are managed by `build_release.py`. Run all commands from the
repository root.

## Checkout

Both native backends are Git submodules. `Texconv-Custom-DLL` has nested
submodules of its own (DirectXTex, DirectX-Headers, DirectXMath,
safestringlib), so the `--recursive` flag is required - without it the
Texconv CMake configuration fails on the empty `DirectXTex/` directory:

```bash
git submodule update --init --recursive
```

## Common prerequisites

- Python 3.10 or newer
- A C++17 compiler. Windows builds use the installed Visual Studio (2022 or
  2026; `build_release.py` picks the matching CMake generator), including the
  ARM64 toolset for Windows ARM64, which cross-compiles from an x64 host.
  Linux uses GCC or Clang. macOS builds run on macOS 11 or newer with Apple clang.
- CMake 3.21 or newer. The first configure downloads GLFW, Dear ImGui, and
  nlohmann/json.
- nasm (Texconv JPEG support builds libjpeg-turbo with its SIMD extensions)
- Network access for first-time dependency and Linux Vulkan SDK downloads
- UPX, optionally
- Linux X11 and OpenGL development packages, used by GLFW

Windows ARM64 and macOS ARM64 are ordinary CMake targets. There is no separate
GUI toolkit to build per architecture. Linux ARM64 is built natively in CI; a
cross build also needs the aarch64 X11 and OpenGL libraries GLFW links.

Windows and macOS Vulkan native builds require a Vulkan SDK with `VULKAN_SDK`
set. Windows also needs SPIR-V-capable DXC in `VULKAN_SDK/bin` or
`VULKAN_SDK/Bin`. On macOS the script copies `libvulkan`, MoltenVK, and
KosmicKrisp when the SDK includes it, and writes ICD manifests beside
`libgdc_vulkan.dylib`. On Linux, the build script downloads the latest LunarG
Vulkan SDK automatically when `VULKAN_SDK` is not set. The SDK is cached under `.build/vulkan-sdk` and is
reused on later builds. The build uses installed libtiff and OpenEXR CMake
packages when available. Otherwise CMake downloads the pinned fallback
versions, so a clean build may require network access. Runtime libraries found
by CMake are copied into the release package. The build verifies that the
native backend can load from that package and that Linux dependencies resolve
through the bundled library directory.

UPX is optional. The build continues without executable compression when it is
not installed.

## Pinned third-party dependencies

Both backends build their codec dependencies from source with pinned versions:

| Dependency | Version | Used by |
| --- | --- | --- |
| OpenEXR | v3.4.14 | Texconv, Compressonator Vulkan |
| libjpeg-turbo | 3.1.3 | Texconv (JPEG) |
| libpng | v1.6.54 | Texconv (PNG) |
| zlib | v1.3.1.2 | Texconv (via libpng) |
| libtiff | v4.7.1 | Compressonator Vulkan (fallback when no system package) |

Keep the two OpenEXR pins in sync: `third_party/Texconv-Custom-DLL/CMakeLists.txt`
and `app/native/vulkan/CMakeLists.txt`. The pin must be v3.4.9 or newer so
`internal_thread.h` works on modern glibc (conflicting `once_flag`/`call_once`,
upstream OpenEXR PR #2262), and new enough that the vendored deflate no longer
uses `evex512` target attributes, which GCC 16 rejects (v3.4.2 does not
compile; v3.4.14 does).

## Build a release

Set the release version in the `VERSION` file. The build script embeds it in the application window title.

Build and package Linux and Windows x64 and ARM64 targets:

```bash
python build_release.py
```

Build only one target:

```bash
python build_release.py --windows --x64
python build_release.py --windows --arm64
python build_release.py --linux --x64
python build_release.py --linux --arm64
python build_release.py --macos --x64
python build_release.py --macos --arm64
```

Clean previous binaries and release packages before building:

```bash
python build_release.py --clean
```

Other options:

```bash
python build_release.py --no-upx
python build_release.py --skip-texconv
python build_release.py --skip-vulkan
```

- `--no-upx` disables UPX compression.
- `--skip-texconv` uses existing libraries from `bin` instead of building Texconv.
- `--skip-vulkan` uses existing Compressonator Vulkan libraries and shaders from `bin`.
- On Linux, omit `--skip-vulkan` to automatically download and cache the LunarG
  Vulkan SDK when `VULKAN_SDK` is unset. Set `VULKAN_SDK` to use a specific
  existing SDK instead; an incomplete value such as `/usr` is rejected when
  `dxc` is missing.
- If LunarG rejects the Python download, install `curl`; the build retries the
  download through curl with redirects and retries enabled.
- `--macos` selects macOS targets. macOS builds must run on macOS.
- `--x64` and `--arm64` select an architecture.
- Target, cleanup, and compression options can be combined.

## Release builds in CI

`.github/workflows/build.yml` produces the release archives on push to `master`,
for version tags, and on pull requests. The `shaders` job compiles the SPIR-V
shaders once (architecture-independent), then:

| Job | Target(s) | Runner | Notes |
| --- | --- | --- | --- |
| `build` (matrix) | Linux x64 | `ubuntu-24.04` | native |
| `build` (matrix) | Linux ARM64 | `ubuntu-24.04-arm` | native ARM64 |
| `build` (matrix) | Windows x64 | `windows-2025` | native, Visual Studio 2026 |
| `build` (matrix) | Windows ARM64 | `windows-11-arm` | **native** Windows-on-ARM, Visual Studio 2026 |
| `build` (matrix) | macOS ARM64 | `macos-15` | MoltenVK, macOS 11+ |

Every target is a row in the single `build` matrix job (each row pins its own
`runs-on` runner). The Windows ARM64 row runs on GitHub's hosted
`windows-11-arm` runner and builds with the Visual Studio ARM64 tools. The same
target cross-compiles from an x64 Windows host that has those tools installed.
`build_release.py` selects the Visual Studio 18 2026 CMake generator when that
Visual Studio is the newest one installed, and Visual Studio 17 2022 otherwise.
All five rows feed the `release` job.

## Building on Windows

Windows builds require:

- Visual Studio 2022 or 2026 with the C++ toolset for the target architecture.
  The app and the Windows ARM64 Compressonator backend both use it (`-A x64` or
  `-A ARM64`), so Windows ARM64 cross-compiles from an x64 host that has the
  ARM64 toolset. The GitHub `windows-2025` and `windows-11-arm` images provide
  Visual Studio 2026.
- MinGW-w64 for the Windows x64 Compressonator backend
- `x86_64-linux-gnu-gcc/g++` when building Linux targets

The Windows ARM64 Compressonator backend stays on MSVC because OpenEXR's
`internal_zip.c` selects MSVC's `<arm64_neon.h>` under `_MSC_VER`, and those
NEON helpers are not provided by clang (`undefined symbol: neon_zip1_q8`).
That build also needs a Vulkan SDK whose import library matches ARM64.
`C:\VulkanSDK\...\Lib\vulkan-1.lib` from an x64 SDK does not. GitHub Actions
installs an ARM64 SDK on `windows-11-arm`.

Native libraries are built only for targets matching the host operating
system. To package Linux releases on Windows, first place Linux Texconv and
Vulkan artifacts in:

```text
bin/linux-x64/
bin/linux-arm64/
```

## Building on Linux

Linux builds require:

- GCC and G++
- `curl` if the automatic Vulkan SDK download needs its fallback downloader
- `x86_64` MinGW-w64 cross-compilers when building Windows x64 targets
- For Linux ARM64, either a native ARM64 host (used by CI) or
  `aarch64-linux-gnu-gcc/g++` plus the aarch64 OpenGL and X11 libraries GLFW links
- The X11 and OpenGL development packages required by GLFW

Debian or Ubuntu:

```bash
sudo apt install gcc g++ libx11-dev libxcursor-dev libxrandr-dev \
    libxinerama-dev libxi-dev libglx-dev libgl1-mesa-dev libxxf86vm-dev
```

Fedora:

```bash
sudo dnf install gcc gcc-c++ libX11-devel libXcursor-devel libXrandr-devel \
    libXinerama-devel libXi-devel libGL-devel libXxf86vm-devel
```

To package Windows releases on Linux, first place Windows Texconv and Vulkan
artifacts in:

```text
bin/windows-x64/
bin/windows-arm64/
```

Use `--skip-texconv --skip-vulkan` when packaging prebuilt native artifacts.

## Development build

Build the native libraries for the host target:

```bash
python build_release.py --linux --x64 --no-upx
```

On Linux, the command automatically downloads the Vulkan SDK if needed. Use
the matching host command on Windows:

```bash
python build_release.py --windows --x64 --no-upx
```

After the matching files exist under `bin/<target>`, the built executable is in
the repository root (`goDragonCooker.x64.exe` on Windows x64). A manual build
without the release script is:

```bash
cmake -S . -B .build/app -DCMAKE_BUILD_TYPE=Release
cmake --build .build/app --config Release
```

## Output

The script creates stripped binaries in the repository root. Each executable
uses its matching library under `bin/<target>/`:

- `goDragonCooker.x64.exe`
- `goDragonCooker.arm64.exe`
- `goDragonCooker.x64`
- `goDragonCooker.arm64`

Release archives are written to `dist`:

- `dist/goDragonCooker-windows-x64.zip`
- `dist/goDragonCooker-windows-arm64.zip`
- `dist/goDragonCooker-linux-x64.zip`
- `dist/goDragonCooker-linux-arm64.zip`

Each Windows, Linux, and macOS archive contains the executable, its matching
Texconv library, the Compressonator Vulkan library, decoder runtime libraries,
and BC4/BC5/BC6H/BC7/mipmap SPIR-V shaders in `bin/<target>`. MinGW builds also
package its required runtime libraries. Linux Vulkan libraries use a
transitive `$ORIGIN` RPATH so bundled dependencies also load from that
directory. macOS uses `@loader_path` and also ships `libvulkan`, MoltenVK, and
KosmicKrisp (when the Vulkan SDK includes it) with ICD manifests beside
`libgdc_vulkan.dylib`.

The native Vulkan build is performed for selected targets matching the build
host. Cross-platform release packaging uses already-built native libraries
from `bin/<target>`, matching the existing Texconv workflow. Use
`--skip-vulkan` when packaging those prebuilt files.

The Compressonator backend requires a Vulkan 1.1 compute-capable driver at
runtime. On Windows and Linux the driver supplies `vulkan-1.dll` or
`libvulkan.so.1`; those system loaders are intentionally not packaged. macOS
has no system Vulkan loader, so the archive includes one. Apple Silicon on
macOS 26 or newer loads KosmicKrisp when `kosmickrisp_icd.json` is present;
other Macs load MoltenVK. There is no CPU fallback. The macOS 15 CI runner
builds the backend and ships both drivers, and can only execute MoltenVK.
