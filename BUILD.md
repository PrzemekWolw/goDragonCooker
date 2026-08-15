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
- Go 1.26.5 or newer with cgo enabled
- C and C++ compilers for every selected target
- CMake 3.21 or newer
- nasm (Texconv JPEG support builds libjpeg-turbo with its SIMD extensions)
- Network access for first-time dependency and Linux Vulkan SDK downloads
- UPX, optionally

Windows Vulkan native builds require a Vulkan SDK with `VULKAN_SDK` set and
SPIR-V-capable DXC in `VULKAN_SDK/bin` or `VULKAN_SDK/Bin`. On Linux, the
build script downloads the latest LunarG Vulkan SDK automatically when
`VULKAN_SDK` is not set. The SDK is cached under `.build/vulkan-sdk` and is
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

## Building on Windows

Windows builds require:

- MinGW-w64 or LLVM C and C++ compilers on `PATH`
- Visual Studio C++ tools, ARM64 build tools, and the Windows SDK for Windows Texconv libraries
- `x86_64-linux-gnu-gcc/g++` and `aarch64-linux-gnu-gcc/g++` when building Linux targets

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
- x86_64 and aarch64 MinGW-w64 cross-compilers when building Windows targets
- `aarch64-linux-gnu-gcc/g++` for Linux ARM64 Texconv builds
- The X11 and OpenGL development packages required by GIU

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

After the matching files exist under `bin/<target>`, run:

```bash
go run ./app
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

Each Windows and Linux archive contains the executable, its matching Texconv
library, the Compressonator Vulkan library, decoder runtime libraries, and
BC4/BC5/BC6H/BC7/mipmap SPIR-V shaders in `bin/<target>`. MinGW builds also
package its required runtime libraries. Linux Vulkan libraries use a
transitive `$ORIGIN` RPATH so bundled dependencies also load from that
directory.

The native Vulkan build is performed for selected targets matching the build
host. Cross-platform release packaging uses already-built native libraries
from `bin/<target>`, matching the existing Texconv workflow. Use
`--skip-vulkan` when packaging those prebuilt files.

The Compressonator backend requires a Vulkan 1.1 compute-capable driver at
runtime. The driver supplies `vulkan-1.dll` on Windows or `libvulkan.so.1` on
Linux; these system loader libraries are intentionally not packaged. It has no
CPU or Texconv fallback.

macOS packages contain the executable and `libtexconv.dylib`; the
Compressonator Vulkan backend is not built for macOS.
