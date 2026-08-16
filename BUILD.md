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

The `giu`/`cimgui-go` GUI toolkit ships prebuilt `cimgui.a`/`libglfw3.a` static
libraries only for Linux x64, macOS, and Windows x64. For the unshipped **Linux
ARM64** and **Windows ARM64** targets, `build_release.py` first runs
`go mod download` (so the module is in the cache), resolves the module directory
via `go list -m`, then builds those two static libraries from the `cimgui-go`
module's C++ sources before the Go build. That needs CMake and a C/C++ compiler
for the target, and — for Linux — the X11/OpenGL development headers, which the
build symlinks from the host when cross-compiling.

**Windows ARM64 cannot be cross-compiled from an x64 host.** A Go cgo build for
`GOOS=windows GOARCH=arm64` requires an aarch64 MSVC-ABI linker, which on the
`windows-11-arm` runner comes from **clang/clang++** (the runner's default `gcc` is
the x86_64 MinGW, and MSVC `cl.exe` cannot be Go's cgo linker). MSYS2 does not package
an aarch64 MinGW-w64 cross-compiler (its `mingw-w64-ucrt-aarch64-gcc` does not exist).
So Windows ARM64 must be built **
natively on a Windows-on-ARM machine** (GitHub's hosted `windows-11-arm` runner) —
the same code path the Linux ARM64 CI job uses. Linux ARM64, by contrast, is built
natively on an `ubuntu-24.04-arm` runner (a pure x86_64 cross toolchain without the
aarch64 `libGL`/`libX11` runtime libraries cannot complete the final link).

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

## Release builds in CI

`.github/workflows/build.yml` produces the release archives on push to `master`,
for version tags, and on pull requests. The `shaders` job compiles the SPIR-V
shaders once (architecture-independent), then:

| Job | Target(s) | Runner | Notes |
| --- | --- | --- | --- |
| `build` (matrix) | Linux x64 | `ubuntu-24.04` | native |
| `build` (matrix) | Linux ARM64 | `ubuntu-24.04-arm` | native ARM64 |
| `build` (matrix) | Windows x64 | `windows-2025` | native |
| `build` (matrix) | Windows ARM64 | `windows-11-arm` | **native** Windows-on-ARM hosted runner |
| `build` (matrix) | macOS ARM64 | `macos-15` | Texconv only |

Every target is a row in the single `build` matrix job (each row pins its own
`runs-on` runner). The Windows ARM64 row runs natively on GitHub's hosted
`windows-11-arm` (Windows-on-ARM) runner, so it needs no self-hosting and is not a
cross-compile. All five rows feed the `release` job. The cimgui-go ARM64 static
libraries are built automatically for every ARM64 target that the module does not
ship (see Common prerequisites).

## Building on Windows

Windows x64 builds require:

- MinGW-w64 or LLVM C and C++ compilers on `PATH` (Windows x64)
- Visual Studio C++ tools and the Windows SDK for the Windows Texconv libraries
- `x86_64-linux-gnu-gcc/g++` when building Linux targets

Windows **ARM64** builds must run natively on a Windows-on-ARM machine (GitHub's
hosted `windows-11-arm` runner, or your own ARM64 Windows box). The runner image's
default `gcc` is the **x86_64 MinGW** (`C:\mingw64`) — **not** aarch64 — so the build
does not rely on it. Two toolchains are used, both producing aarch64 MSVC-ABI:

- **clang/clang++** (LLVM) for the cimgui-go and GLFW static libraries and for the
go/cgo step — Go on `windows/arm64` requires the MSVC ABI, which clang supplies (MSVC
`cl.exe` cannot be Go's cgo linker, and MSYS2 has no aarch64 MinGW). If that clang
defaults to a non-aarch64 triple, the build pins `-target aarch64-pc-windows-msvc`
so the output is aarch64 either way.
- **MSVC `cl.exe`** (`Visual Studio 2022` + `-A ARM64`) for the Compressonator
Vulkan backend, because OpenEXR's `internal_zip.c` selects MSVC's `<arm64_neon.h>`
under `_MSC_VER`, whose NEON intrinsics need MSVC's own `neon_*` helper symbols that
clang cannot provide (`undefined symbol: neon_zip1_q8`). `gdc_vulkan.dll` is a
self-contained C-ABI DLL loaded at runtime, so its MSVC ABI is independent of the
clang-built executable.

Cross-compiling from x64 is not possible — see Common prerequisites.

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
- `x86_64` MinGW-w64 cross-compilers when building **Windows x64** targets
  (Windows ARM64 must be built on a native ARM64 host — see Building on Windows)
- For Linux ARM64, either a native ARM64 host (simplest; used by CI) or the
  `aarch64-linux-gnu-gcc/g++` cross compiler **plus** the aarch64 OpenGL and X11
  runtime libraries — the Go binary links against `libGL`/`libX11`, so a pure
  cross toolchain without those aarch64 libs cannot complete the final link
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
