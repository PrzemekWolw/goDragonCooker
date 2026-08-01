# Building goDragonCooker

## Prerequisites

The release build is managed by `build_release.py`. Run it from the repository root.

All builds require:

- Python 3.10 or newer
- Go 1.26.5 or newer with cgo support
- C and C++ compilers for each selected target
- UPX for optional executable compression

UPX is optional. The script continues without compression when UPX is unavailable.

## Build a release

Set the release version in the `VERSION` file. The build script embeds it in the application window title.

Build and package Linux and Windows x64 and ARM64 targets:

```bash
python build_release.py
```

Build only one target:

```bash
python build_release.py --windows
python build_release.py --linux
python build_release.py --macos
python build_release.py --arm64
```

Clean previous binaries and release packages before building:

```bash
python build_release.py --clean
```

Other options:

```bash
python build_release.py --no-upx
python build_release.py --skip-texconv
```

- `--no-upx` disables UPX compression.
- `--skip-texconv` uses existing libraries from `bin` instead of building Texconv.
- `--macos` selects macOS targets. macOS builds must run on macOS.
- `--x64` and `--arm64` select an architecture.
- Target, cleanup, and compression options can be combined.

## Building on Windows

Windows builds require:

- MinGW-w64 or LLVM C and C++ compilers on `PATH` for the Windows Go binary
- Visual Studio C++ tools, ARM64 build tools, and the Windows SDK for Windows Texconv libraries
- `x86_64-linux-gnu-gcc/g++` and `aarch64-linux-gnu-gcc/g++` when building Linux targets

The script builds Texconv for all selected architectures on the host operating system. Therefore, a Windows host needs existing `bin/linux-x64/libtexconv.so` and `bin/linux-arm64/libtexconv.so` files to package Linux releases. All libraries use the target-specific `bin/<target>/` layout:

```powershell
python build_release.py --linux
```

## Building on Linux

Linux builds require:

- GCC and G++
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

The script builds Texconv for all selected architectures on the host operating system. Therefore, a Linux host needs existing `bin/windows-x64/texconv.dll` and `bin/windows-arm64/texconv.dll` files to package Windows releases. All libraries use the target-specific `bin/<target>/` layout:

```bash
python build_release.py --linux
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

Each archive contains the executable and its matching Texconv library in `bin/<target>`.

macOS releases use the same executable naming convention without `.exe`:

- `goDragonCooker.x64`
- `goDragonCooker.arm64`

The matching libraries are stored in `bin/macos-x64/libtexconv.dylib` or `bin/macos-arm64/libtexconv.dylib`.
