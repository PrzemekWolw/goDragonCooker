#!/usr/bin/env python3
"""Build goDragonCooker for Linux, macOS, and Windows, package into release zips.

The app is C++ (CMake, Dear ImGui, GLFW). Release builds are stripped, then
UPX-compressed when UPX is installed.

Usage:
    python build_release.py              # build Linux and Windows targets
    python build_release.py --linux      # Linux only
    python build_release.py --macos      # macOS ARM64 only
    python build_release.py --windows    # Windows only
    python build_release.py --arm64      # ARM64 targets only
    python build_release.py --clean      # remove artifacts before building
    python build_release.py --no-upx     # skip UPX compression
    python build_release.py --tests      # run i18n, model, and texconv cook self-tests
    python build_release.py --skip-texconv  # use existing Texconv libraries
    python build_release.py --skip-vulkan   # use existing Vulkan libraries/shaders

Requires:
    - CMake 3.21+ and a C++17 compiler. On Windows the app builds with the
      installed Visual Studio (2022 or 2026, -A x64 or -A ARM64), so ARM64
      cross-compiles from an x64 host. Linux and macOS use the system compiler;
      macOS builds are ARM64 and must run on macOS (11 or newer).
    - MinGW-w64 for the Windows x64 Compressonator backend, and the Visual
      Studio ARM64 tools for the Windows ARM64 backend
    - aarch64 cross-compilers when compiling Linux ARM64 off an x64 host
    - Vulkan SDK/DXC for the Compressonator backend. On Linux the SDK is
      downloaded when VULKAN_SDK is unset; curl is the fallback downloader
    - upx (optional, skipped gracefully if not found)
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

PROJECT = Path(__file__).resolve().parent
APP_SOURCE = PROJECT / "app"
BIN_DIR = PROJECT / "bin"
DIST_DIR = PROJECT / "dist"
TEXCONV_SOURCE = PROJECT / "third_party" / "Texconv-Custom-DLL"
VULKAN_SOURCE = APP_SOURCE / "native" / "vulkan"
VERSION_FILE = PROJECT / "VERSION"
CPU_THREADS = max(1, os.cpu_count() or 1)
VULKAN_BUILD_OUTPUTS: dict[str, Path] = {}
BINARY_BUILD_OUTPUTS: dict[str, Path] = {}

def make_target(platform_name: str, goos: str, goarch: str, arch_name: str) -> dict:
    name = f"{platform_name}-{arch_name}"
    suffix = ".exe" if goos == "windows" else ""
    texconv_lib = {
        "linux": "libtexconv.so",
        "darwin": "libtexconv.dylib",
        "windows": "texconv.dll",
    }[goos]
    vulkan_lib = {
        "linux": "libgdc_vulkan.so",
        "darwin": "libgdc_vulkan.dylib",
        "windows": "gdc_vulkan.dll",
    }[goos]
    return {
        "name": name,
        "platform": platform_name,
        "goos": goos,
        "goarch": goarch,
        "arch_name": arch_name,
        "binary_name": f"goDragonCooker.{arch_name}{suffix}",
        "texconv_lib": texconv_lib,
        "vulkan_lib": vulkan_lib,
        "zip_name": f"goDragonCooker-{platform_name}-{arch_name}.zip",
    }


TARGETS = [
    make_target("linux", "linux", "amd64", "x64"),
    make_target("linux", "linux", "arm64", "arm64"),
    make_target("macos", "darwin", "arm64", "arm64"),
    make_target("windows", "windows", "amd64", "x64"),
    make_target("windows", "windows", "arm64", "arm64"),
]


def vulkan_runtime_libraries(directory: Path, platform_name: str) -> list[Path]:
    if platform_name == "macos":
        return [
            path for path in list(directory.glob("*.dylib")) + list(directory.glob("*.json"))
            if path.is_file()
        ]
    pattern = "*.dll" if platform_name == "windows" else "*.so*"
    libraries = list(directory.rglob(pattern))
    if platform_name == "linux":
        libraries = [
            library for library in libraries
            if (suffix := library.name.partition(".so")[2]) == ""
            or (suffix.startswith(".") and suffix[1:].isdigit())
        ]
    return libraries


def library_arch_matches_host(library: Path, readelf: str) -> bool:
    """True when *library* targets the host architecture (safe to resolve with ldd)."""
    host = platform.machine().lower()
    host_is_arm = host in {"aarch64", "arm64"}
    result = run([readelf, "-h", str(library)], check=False)
    for line in result.stdout.splitlines():
        if "Machine:" not in line:
            continue
        machine = line.split(":", 1)[1].strip().lower()
        if "aarch64" in machine:
            return True == host_is_arm
        if "x86-64" in machine or "advanced micro" in machine:
            return False == host_is_arm
        return True
    return True


def write_vulkan_icd(directory: Path, filename: str, library_name: str):
    payload = {
        "file_format_version": "1.0.0",
        "ICD": {
            "library_path": library_name,
            "api_version": "1.1.0",
        },
    }
    (directory / filename).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def copy_macos_dylib(source: Path, destination: Path) -> str:
    shutil.copy2(source, destination / source.name)
    resolved = source.resolve()
    if resolved.name != source.name:
        shutil.copy2(resolved, destination / resolved.name)
    return source.name


def install_macos_vulkan_drivers(install_dir: Path):
    sdk = os.environ.get("VULKAN_SDK")
    if not sdk:
        log("VULKAN_SDK is not set; cannot bundle the macOS Vulkan loader", error=True)
        sys.exit(1)
    sdk_path = Path(sdk)
    dylibs = [path for path in sdk_path.rglob("*.dylib") if path.is_file()]

    def named(fragment: str) -> list[Path]:
        return [path for path in dylibs if fragment in path.name.lower()]

    loaders = named("libvulkan")
    molten = named("libmoltenvk")
    kosmic = named("kosmickrisp")
    if not loaders or not molten:
        log(f"Vulkan SDK at {sdk_path} has no libvulkan or libMoltenVK", error=True)
        sys.exit(1)

    def prefer(paths: list[Path]) -> Path:
        return sorted(paths, key=lambda path: (path.name.count("."), len(str(path))))[0]

    ordered = sorted(
        loaders + molten + kosmic,
        key=lambda path: (path.name.count("."), len(str(path))),
        reverse=True,
    )
    for path in ordered:
        copy_macos_dylib(path, install_dir)
    write_vulkan_icd(install_dir, "MoltenVK_icd.json", prefer(molten).name)
    if kosmic:
        write_vulkan_icd(install_dir, "kosmickrisp_icd.json", prefer(kosmic).name)
        log("Bundled MoltenVK and KosmicKrisp")
    else:
        log("Bundled MoltenVK; this Vulkan SDK has no KosmicKrisp")


def rewrite_macos_loader_paths(library: Path, install_dir: Path):
    listing = subprocess.run(
        ["otool", "-L", str(library)],
        capture_output=True,
        text=True,
    )
    if listing.returncode != 0:
        log(listing.stderr.strip() or "otool failed", error=True)
        sys.exit(1)
    for line in listing.stdout.splitlines()[1:]:
        dep = line.strip().split(" (", 1)[0]
        name = Path(dep).name
        if not (install_dir / name).exists():
            continue
        updated = f"@loader_path/{name}"
        if dep == updated:
            continue
        subprocess.run(
            ["install_name_tool", "-change", dep, updated, str(library)],
            check=True,
        )


def verify_vulkan_runtime(library: Path, directory: Path, platform_name: str):
    if platform_name == "macos":
        missing = []
        if not (directory / "MoltenVK_icd.json").is_file():
            missing.append("MoltenVK_icd.json")
        if not any(directory.glob("libMoltenVK*.dylib")):
            missing.append("libMoltenVK*.dylib")
        if not any(directory.glob("libvulkan*.dylib")):
            missing.append("libvulkan*.dylib")
        if missing:
            log(f"macOS Vulkan package is missing {', '.join(missing)}", error=True)
            sys.exit(1)
        return

    if platform_name == "windows":
        try:
            with os.add_dll_directory(str(directory)):
                ctypes.WinDLL(str(library))
        except OSError as error:
            log(f"Packaged Vulkan library cannot be loaded: {error}", error=True)
            sys.exit(1)
        return

    readelf = shutil.which("readelf")
    if not readelf:
        log("readelf is required to verify the Linux Vulkan package", error=True)
        sys.exit(1)
    dynamic = run([readelf, "-d", str(library)])
    if "(RPATH)" not in dynamic.stdout or "$ORIGIN" not in dynamic.stdout:
        log(
            f"{library.name} must use transitive RPATH=$ORIGIN for bundled dependencies",
            error=True,
        )
        sys.exit(1)

    # ldd can only resolve dependencies for the host's own architecture, so skip it when the
    # library was cross-compiled for a different architecture (e.g. aarch64 on an x86_64 host).
    if library_arch_matches_host(library, readelf):
        dependencies = run(["ldd", str(library)])
        if "not found" in dependencies.stdout:
            log(f"Unresolved Vulkan runtime dependency:\n{dependencies.stdout}", error=True)
            sys.exit(1)
        bundled = {path.name for path in vulkan_runtime_libraries(directory, "linux")}
        for line in dependencies.stdout.splitlines():
            if "=>" not in line:
                continue
            name, resolved = (part.strip() for part in line.split("=>", 1))
            resolved_path = resolved.split(" ", 1)[0]
            if name in bundled and Path(resolved_path).resolve().parent != directory.resolve():
                log(f"{name} resolved outside the package: {resolved_path}", error=True)
                sys.exit(1)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def ansi_supported() -> bool:
    if os.environ.get("NO_COLOR") or not sys.stdout.isatty():
        return False
    if platform.system() != "Windows":
        return True

    try:
        import msvcrt

        handle = msvcrt.get_osfhandle(sys.stdout.fileno())
        mode = ctypes.c_ulong()
        kernel32 = ctypes.windll.kernel32
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))
    except (AttributeError, OSError, ValueError):
        return False


def log(msg: str, *, info: bool = True, error: bool = False):
    if ansi_supported():
        tag = "\033[91mERROR\033[0m" if error else ("\033[94mINFO\033[0m" if info else "")
    else:
        tag = "ERROR" if error else ("INFO" if info else "")
    prefix = f"[{tag}] " if tag else ""
    print(f"{prefix}{msg}")


def remove_tree(path: Path):
    """Delete a tree, including the read-only files Git leaves in fetched checkouts.

    CMake FetchContent clones (Imath under the Texconv build, the app's own
    dependencies) mark pack files read-only. Windows then returns access denied
    from os.unlink, and shutil.rmtree stops.
    """

    def onexc(func, name, exc):
        if not isinstance(exc, PermissionError):
            raise exc
        os.chmod(name, stat.S_IWRITE)
        func(name)

    shutil.rmtree(path, onexc=onexc)


# A MinGW OpenEXR/TIFF on CMAKE_PREFIX_PATH makes the MSVC ARM64 build include
# C:\msys64 headers (vadefs.h rejects that target). These stay out of VS configures.
MSVC_CONFIG_ENV_DROP = (
    "CMAKE_PREFIX_PATH",
    "CMAKE_MODULE_PATH",
    "OpenEXR_DIR",
    "OpenEXR_ROOT",
    "Imath_DIR",
    "Imath_ROOT",
    "TIFF_DIR",
    "TIFF_ROOT",
    "CPATH",
    "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH",
    "LIBRARY_PATH",
)


def run(
    cmd: list[str],
    *,
    env: dict | None = None,
    check: bool = True,
    cwd: Path | None = None,
    drop_env: tuple[str, ...] = (),
) -> subprocess.CompletedProcess:
    full_env = {**os.environ, **(env or {})}
    for key in drop_env:
        full_env.pop(key, None)
    log(f"  $ {' '.join(cmd)}", info=False)
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        env=full_env,
        cwd=str(cwd or PROJECT),
    )
    if check and result.returncode != 0:
        log(f"Command failed (exit {result.returncode}):", error=True)
        # Build tools (ninja, gmake, cmake) usually write the real error to stdout, so
        # surface both streams' tails rather than stderr alone.
        lines = (result.stderr or "").splitlines()[-20:] + (result.stdout or "").splitlines()[-30:]
        for line in lines:
            if line.strip():
                log(f"  {line}", error=True)
        sys.exit(result.returncode)
    return result


X86_64_V3 = True
ARCH_STAMP_DIR = PROJECT / ".build" / "arch"


def cpu_flags(target: dict) -> str:
    """CPU baseline flags for code built outside the app's CMake project (Texconv)."""
    msvc = target["platform"] == "windows" and platform.system() == "Windows"
    if target["goarch"] == "amd64" and X86_64_V3:
        return "/arch:AVX2" if msvc else "-march=x86-64-v3 -ffp-contract=off"
    if target["goarch"] == "arm64" and msvc:
        return "/arch:armv8.2"
    return ""


def cpu_flags_env(target: dict) -> dict[str, str]:
    flags = cpu_flags(target)
    if not flags:
        return {}
    if target["platform"] == "windows" and platform.system() == "Windows":
        return {"CL": " ".join(filter(None, [os.environ.get("CL", ""), flags]))}
    return {
        "CFLAGS": " ".join(filter(None, [os.environ.get("CFLAGS", ""), flags])),
        "CXXFLAGS": " ".join(filter(None, [os.environ.get("CXXFLAGS", ""), flags])),
    }


def arch_stamp(kind: str, target: dict) -> Path:
    return ARCH_STAMP_DIR / f"{kind}-{target['name']}.txt"


def arch_stamp_matches(kind: str, target: dict, value: str) -> bool:
    stamp = arch_stamp(kind, target)
    return stamp.is_file() and stamp.read_text(encoding="utf-8") == value


def write_arch_stamp(kind: str, target: dict, value: str):
    stamp = arch_stamp(kind, target)
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(value, encoding="utf-8")


def needs_build(binary: Path, sources: list[Path]) -> bool:
    if not binary.exists():
        return True
    for src in sources:
        if not src.exists():
            continue
        if src.stat().st_mtime > binary.stat().st_mtime:
            return True
    return False


def binary_path(target: dict) -> Path:
    return PROJECT / target["binary_name"]


def get_sources() -> list[Path]:
    out = [PROJECT / "CMakeLists.txt", VERSION_FILE]
    out.extend(APP_SOURCE.rglob("*.cpp"))
    out.extend(APP_SOURCE.rglob("*.h"))
    out.append(APP_SOURCE / "dragonpbr.ico")
    if VULKAN_SOURCE.exists():
        out.extend(path for path in VULKAN_SOURCE.rglob("*") if path.is_file())
    return out


def upx_available() -> bool:
    return shutil.which("upx") is not None


def upx_supports_threads() -> bool:
    if not upx_available():
        return False
    result = subprocess.run(
        ["upx", "--help"],
        capture_output=True,
        text=True,
    )
    return "--threads" in result.stdout or "--threads" in result.stderr


def host_goarch() -> str:
    machine = platform.machine().lower()
    if machine in {"amd64", "x86_64", "x64"}:
        return "amd64"
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    return ""


def compiler_env_with_runtime(env: dict[str, str]) -> dict[str, str]:
    result = dict(env)
    runtime_paths = []
    for compiler in (env.get("CC"), env.get("CXX")):
        if not compiler:
            continue
        compiler_path = Path(shutil.which(compiler) or compiler)
        if not compiler_path.is_absolute():
            continue
        msys_runtime = compiler_path.parent.parent.parent / "usr" / "bin"
        if msys_runtime.is_dir():
            runtime_paths.append(str(msys_runtime))
        runtime_paths.append(str(compiler_path.parent))
    if runtime_paths:
        result["PATH"] = os.pathsep.join(dict.fromkeys(runtime_paths + [os.environ.get("PATH", "")]))
    return result


def _cc_dumpmachine(cc: str) -> str:
    try:
        return run([cc, "-dumpmachine"], check=False).stdout.strip()
    except Exception:
        return ""


def _is_clang(compiler: str) -> bool:
    return bool(compiler) and "clang" in Path(compiler).name.lower()


def windows_compiler_env(target: dict) -> dict[str, str]:
    """Return a usable C/C++ compiler environment for a Windows build."""
    if os.environ.get("CC") and os.environ.get("CXX"):
        chosen = {"CC": os.environ["CC"], "CXX": os.environ["CXX"]}
    else:
        if platform.system() == "Windows":
            if target["goarch"] == "arm64":
                # An ARM64 target needs a compiler that actually targets aarch64. The
                # default gcc on a Windows-on-ARM runner is the x86_64 MinGW, so prefer a
                # native aarch64 toolchain (clang or an aarch64 MinGW) over the x64 one.
                compiler_pairs = [
                    ("clang", "clang++"),
                    ("aarch64-w64-mingw32-gcc", "aarch64-w64-mingw32-g++"),
                    ("arm64-w64-mingw32-gcc", "arm64-w64-mingw32-g++"),
                    ("gcc", "g++"),
                ]
            else:
                compiler_pairs = [("gcc", "g++"), ("clang", "clang++")]
        else:
            prefix = "aarch64" if target["goarch"] == "arm64" else "x86_64"
            compiler_pairs = [(f"{prefix}-w64-mingw32-gcc", f"{prefix}-w64-mingw32-g++")]

        chosen = None
        for cc, cxx in compiler_pairs:
            if not (shutil.which(cc) and shutil.which(cxx)):
                continue
            machine = _cc_dumpmachine(cc)
            low = machine.lower()
            if target["goarch"] == "arm64":
                log(f"  candidate {cc} -> {machine or 'unknown'}", info=False)
                # A MinGW compiler only works if it already targets aarch64. The x86_64
                # MinGW cannot consume aarch64 libraries. clang is accepted because a
                # native ARM64 clang defaults to aarch64.
                if not _is_clang(cc) and "aarch64" not in low and "arm64" not in low:
                    continue
            chosen = {"CC": cc, "CXX": cxx}
            break

        if chosen is None:
            log(
                f"No usable C/C++ compiler found for the Windows {target['arch_name']} build "
                f"(tried: {', '.join(cc for cc, _ in compiler_pairs)}). "
                + (
                    "Windows ARM64 needs a native aarch64 compiler (clang or aarch64 MinGW-w64) "
                    "-- install an ARM64 LLVM toolchain or point CC/CXX at one."
                    if target["goarch"] == "arm64"
                    else "Install MinGW-w64 or LLVM, or set CC and CXX."
                ),
                error=True,
            )
            sys.exit(1)

    if _cc_dumpmachine(chosen["CC"]):
        log(
            f"  Windows {target['arch_name']} C/C++ compiler: {chosen['CC']} -> {_cc_dumpmachine(chosen['CC'])}",
            info=False,
        )

    return compiler_env_with_runtime(chosen)


def linux_compiler_env(target: dict) -> dict[str, str]:
    """Return a usable C/C++ compiler environment for a Linux build."""
    if platform.system() != "Windows" and target["goarch"] == host_goarch():
        return {}

    if os.environ.get("CC") and os.environ.get("CXX"):
        return compiler_env_with_runtime({"CC": os.environ["CC"], "CXX": os.environ["CXX"]})

    prefix = "aarch64" if target["goarch"] == "arm64" else "x86_64"
    compiler_pairs = [
        (f"{prefix}-linux-gnu-gcc", f"{prefix}-linux-gnu-g++"),
        (f"{prefix}-suse-linux-gcc", f"{prefix}-suse-linux-g++"),
    ]
    for cc, cxx in compiler_pairs:
        if shutil.which(cc) and shutil.which(cxx):
            return compiler_env_with_runtime({"CC": cc, "CXX": cxx})

    log(
        f"No C/C++ compiler found for the Linux {target['arch_name']} build. "
        "Install the matching Linux cross-compiler, or set CC and CXX "
        "to a Linux-targeting toolchain.",
        error=True,
    )
    sys.exit(1)


def ensure_linux_vulkan_sdk() -> None:
    if platform.system() != "Linux":
        return
    configured_sdk = os.environ.get("VULKAN_SDK")
    if configured_sdk:
        dxc_paths = [
            Path(configured_sdk) / "bin" / "dxc",
            Path(configured_sdk) / "Bin" / "dxc",
        ]
        if any(path.is_file() for path in dxc_paths) or shutil.which("dxc"):
            return
        log(
            f"VULKAN_SDK is set to {configured_sdk}, but dxc was not found; "
            "installing a complete SDK"
        )

    sdk_cache = PROJECT / ".build" / "vulkan-sdk"
    sdk_cache.mkdir(parents=True, exist_ok=True)
    version_url = "https://vulkan.lunarg.com/sdk/latest/linux.txt"
    try:
        request = urllib.request.Request(
            version_url,
            headers={"User-Agent": "goDragonCooker build"},
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            version = response.read().decode("utf-8").strip()
    except Exception as exception:
        log(f"Could not query the latest Vulkan SDK: {exception}", error=True)
        sys.exit(1)
    if not version or any(character not in "0123456789." for character in version):
        log(f"Invalid Vulkan SDK version returned by LunarG: {version!r}", error=True)
        sys.exit(1)

    sdk_root = sdk_cache / version
    sdk_path = sdk_root / "x86_64"
    if not sdk_path.is_dir() or not (sdk_path / "bin" / "dxc").is_file():
        archive = sdk_cache / f"vulkan_sdk-{version}.tar.xz"
        download_url = (
            f"https://sdk.lunarg.com/sdk/download/{version}/linux/vulkan_sdk.tar.xz"
        )
        log(f"Downloading Vulkan SDK {version}...")
        try:
            request = urllib.request.Request(
                download_url,
                headers={"User-Agent": "goDragonCooker build"},
            )
            with urllib.request.urlopen(request, timeout=300) as response:
                with archive.open("wb") as output:
                    shutil.copyfileobj(response, output)
        except Exception as exception:
            if not shutil.which("curl"):
                archive.unlink(missing_ok=True)
                log(f"Could not download Vulkan SDK: {exception}", error=True)
                sys.exit(1)
            log("Retrying Vulkan SDK download with curl...")
            result = subprocess.run(
                [
                    "curl",
                    "--fail",
                    "--location",
                    "--retry",
                    "3",
                    "--user-agent",
                    "goDragonCooker build",
                    "--output",
                    str(archive),
                    download_url,
                ],
                capture_output=True,
                text=True,
            )
            if result.returncode != 0:
                detail = result.stderr.strip() or str(exception)
                archive.unlink(missing_ok=True)
                log(f"Could not download Vulkan SDK: {detail}", error=True)
                sys.exit(1)
        try:
            with tarfile.open(archive, "r:xz") as package:
                package.extractall(sdk_cache)
        except Exception as exception:
            log(f"Could not install Vulkan SDK: {exception}", error=True)
            sys.exit(1)
        finally:
            archive.unlink(missing_ok=True)
        if not sdk_path.is_dir():
            log(f"Vulkan SDK extracted without x86_64 files: {sdk_root}", error=True)
            sys.exit(1)

    os.environ["VULKAN_SDK"] = str(sdk_path)
    os.environ["PATH"] = os.pathsep.join(
        [str(sdk_path / "bin"), os.environ.get("PATH", "")]
    )
    os.environ["CMAKE_PREFIX_PATH"] = os.pathsep.join(
        [str(sdk_path), str(sdk_path / "lib" / "VulkanLoader")]
    )
    log(f"Using Vulkan SDK {version} from {sdk_path}")


def darwin_compiler_env(target: dict) -> dict[str, str]:
    """Return a usable C/C++ compiler environment for a macOS build."""
    del target
    if platform.system() != "Darwin":
        log("macOS builds require a macOS host with the Apple SDK and clang.", error=True)
        sys.exit(1)
    if not shutil.which("clang") or not shutil.which("clang++"):
        log("macOS builds require clang and clang++.", error=True)
        sys.exit(1)
    return {"CC": "clang", "CXX": "clang++"}


def texconv_library(target: dict) -> Path:
    return BIN_DIR / target["name"] / target["texconv_lib"]


def vulkan_library(target: dict) -> Path:
    return BIN_DIR / target["name"] / target["vulkan_lib"]


def texconv_host_target() -> str:
    if platform.system() == "Windows":
        return "windows"
    if platform.system() == "Linux":
        return "linux"
    if platform.system() == "Darwin":
        return "macos"
    return ""


def build_texconv(targets: list[dict], skip: bool):
    if skip:
        return
    if not TEXCONV_SOURCE.exists():
        log("Texconv source not found, using existing libraries in bin/")
        return

    host_target = texconv_host_target()
    if not host_target:
        log("Texconv build skipped: unsupported host platform")
        return

    host_targets = [target for target in targets if target["platform"] == host_target]
    if not host_targets:
        return

    for target in host_targets:
        build_texconv_target(target)


def build_texconv_target(target: dict):
    source_binary = TEXCONV_SOURCE / target["texconv_lib"]
    destination = texconv_library(target)
    texconv_source = TEXCONV_SOURCE / "TexconvDLL" / "texconv.cpp"
    flags = cpu_flags(target)
    if (
        destination.exists()
        and not needs_build(destination, [texconv_source])
        and arch_stamp_matches("texconv", target, flags)
    ):
        log(f"Skipping Texconv {target['name']} (up to date)")
        return

    architecture = "arm64" if target["goarch"] == "arm64" else "x64"
    # The flags come from the environment, which neither MSBuild nor a configured CMake cache
    # tracks, so a baseline change needs a fresh build directory.
    texconv_build_dir = TEXCONV_SOURCE / f"build_{architecture}"
    if texconv_build_dir.exists() and not arch_stamp_matches("texconv", target, flags):
        log(f"  Removing {texconv_build_dir.name} built with other CPU flags")
        remove_tree(texconv_build_dir)
    if flags:
        log(f"  Texconv CPU flags: {flags}")
    gpu_mode = "with GPU codec" if target["platform"] == "windows" else "without GPU codec"
    log(f"Building Texconv {target['name']} {gpu_mode}...")
    if platform.system() == "Windows":
        command = [
            "cmd.exe",
            "/d",
            "/c",
            "build.cmd",
            "--use-optional-formats",
            "--no-texassemble",
            "--generator",
            visual_studio_generator(),
        ]
        command.extend(["--architecture", architecture])
        build_result = run(command, cwd=TEXCONV_SOURCE, env=cpu_flags_env(target))
    else:
        build_script = TEXCONV_SOURCE / "scripts" / "build.sh"
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            suffix=".sh",
            dir=build_script.parent,
            delete=False,
        ) as normalized:
            normalized.write(build_script.read_text(encoding="utf-8").replace("\r\n", "\n"))
            normalized_script = Path(normalized.name)
        try:
            build_result = run(
                [
                    "bash",
                    str(normalized_script),
                    "--use-optional-formats",
                    "--no-texassemble",
                    "--architecture",
                    architecture,
                ],
                cwd=TEXCONV_SOURCE,
                env={
                    **(
                        linux_compiler_env(target)
                        if target["platform"] == "linux"
                        else darwin_compiler_env(target)
                    ),
                    **cpu_flags_env(target),
                },
            )
        finally:
            normalized_script.unlink(missing_ok=True)

    if not source_binary.exists():
        built_libraries = list(TEXCONV_SOURCE.glob(f"build*/**/{source_binary.name}"))
        if built_libraries:
            source_binary = max(built_libraries, key=lambda path: path.stat().st_mtime)
            log(f"Found Texconv output at {source_binary}")

    if not source_binary.exists():
        output = (build_result.stdout or build_result.stderr).strip()
        if output:
            log("Texconv build output:", error=True)
            for line in output.splitlines()[-20:]:
                log(f"  {line}", error=True)
        log(f"Texconv build did not produce {source_binary}", error=True)
        sys.exit(1)

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(str(source_binary), str(destination))
    write_arch_stamp("texconv", target, flags)
    log(f"Installed {destination}")


def build_vulkan(targets: list[dict], skip: bool):
    if skip:
        return
    if not VULKAN_SOURCE.exists():
        log("Compressonator Vulkan source not found", error=True)
        sys.exit(1)

    host_target = texconv_host_target()
    native_sources = [path for path in VULKAN_SOURCE.rglob("*") if path.is_file()]
    required_shaders = {"bc4.spv", "bc5.spv", "bc6.spv", "bc7.spv", "mipmap.spv"}
    pending_targets = []
    for target in targets:
        if target["platform"] not in {"windows", "linux", "macos"} or target["platform"] != host_target:
            continue
        destination = vulkan_library(target)
        shader_destination = destination.parent / "shaders"
        shader_names = {
            shader.name for shader in shader_destination.glob("*.spv")
        } if shader_destination.is_dir() else set()
        drivers_ready = True
        if target["platform"] == "macos":
            parent = destination.parent
            drivers_ready = (
                (parent / "MoltenVK_icd.json").is_file()
                and any(parent.glob("libMoltenVK*.dylib"))
                and any(parent.glob("libvulkan*.dylib"))
            )
        if (
            destination.exists()
            and shader_names == required_shaders
            and drivers_ready
            and not needs_build(destination, native_sources)
        ):
            log(f"Skipping Compressonator Vulkan {target['name']} (up to date)")
            continue
        pending_targets.append(target)

    if not pending_targets:
        return
    if not shutil.which("cmake"):
        log("CMake is required for the Compressonator Vulkan backend", error=True)
        sys.exit(1)
    if not os.environ.get("VULKAN_SDK"):
        if platform.system() == "Linux":
            ensure_linux_vulkan_sdk()
        else:
            log(
                "VULKAN_SDK is not set; using existing Compressonator Vulkan "
                "libraries in bin/"
            )
            return

    for target in pending_targets:
        destination = vulkan_library(target)
        shader_destination = destination.parent / "shaders"
        build_dir = PROJECT / ".build" / f"vulkan-{target['name']}"
        install_dir = PROJECT / ".build" / f"vulkan-install-{target['name']}"
        build_dir.mkdir(parents=True, exist_ok=True)
        if install_dir.exists():
            remove_tree(install_dir)
        install_dir.mkdir(parents=True, exist_ok=True)
        msvc_arm64 = (
            target["platform"] == "windows" and target["goarch"] == "arm64"
        )
        if msvc_arm64:
            # OpenEXR's internal_zip.c selects <arm64_neon.h> under _MSC_VER, whose
            # NEON intrinsics resolve to MSVC-only neon_* helper symbols that clang
            # cannot satisfy ("undefined symbol: neon_zip1_q8"). Build this
            # self-contained aarch64 DLL with real MSVC -- the same toolchain Texconv
            # already uses for arm64 -- so OpenEXR links. The Visual Studio generator
            # finds cl.exe via vswhere.
            env = {}
            configure = [
                "cmake",
                "-S", str(VULKAN_SOURCE),
                "-B", str(build_dir),
                "-G", visual_studio_generator(),
                "-A", "ARM64",
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_INSTALL_PREFIX={install_dir}",
                "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
            ]
        else:
            if target["platform"] == "windows":
                env = windows_compiler_env(target)
            elif target["platform"] == "macos":
                env = darwin_compiler_env(target)
            else:
                env = linux_compiler_env(target)
            configure = [
                "cmake",
                "-S", str(VULKAN_SOURCE),
                "-B", str(build_dir),
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_INSTALL_PREFIX={install_dir}",
            ]
            if env.get("CC"):
                # Pin the C compiler explicitly. CMake's auto-detect on a Windows
                # runner can resolve to the x86_64 MinGW (C:\mingw64).
                configure.append(f"-DCMAKE_C_COMPILER={env['CC']}")
            if target["platform"] == "macos":
                configure += [
                    "-DCMAKE_OSX_ARCHITECTURES=arm64",
                    "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0",
                ]
            if env.get("CXX"):
                configure.append(f"-DCMAKE_CXX_COMPILER={env['CXX']}")
                compiler_name = Path(env["CXX"]).name.lower()
                if target["platform"] == "windows" and "clang" in compiler_name:
                    configure[1:1] = ["-G", "Ninja"]
                elif target["platform"] == "windows" and "g++" in compiler_name:
                    configure[1:1] = ["-G", "MinGW Makefiles"]
        if os.environ.get("VULKAN_SDK"):
            configure.append(f"-DCMAKE_PREFIX_PATH={os.environ['VULKAN_SDK']}")
        if os.environ.get("GDC_PRECOMPILED_SHADER_DIR"):
            configure.append(
                f"-DGDC_PRECOMPILED_SHADER_DIR={os.environ['GDC_PRECOMPILED_SHADER_DIR']}"
            )
        cache = build_dir / "CMakeCache.txt"
        if cache.is_file() and "msys64" in cache.read_text(encoding="utf-8", errors="replace").lower():
            log(f"  Removing {build_dir.name} configured against MinGW libraries")
            remove_tree(build_dir)
        reset_stale_cmake_generator(build_dir, configure)
        run(configure, env=env, drop_env=MSVC_CONFIG_ENV_DROP if msvc_arm64 else ())
        run(["cmake", "--build", str(build_dir), "--config", "Release", "--parallel", str(CPU_THREADS)], env=env)
        run([
            "cmake", "--install", str(build_dir), "--config", "Release",
            "--component", "gdc_vulkan_runtime",
        ], env=env)

        built = list(install_dir.rglob(target["vulkan_lib"]))
        if not built:
            log(f"Compressonator Vulkan build did not produce {target['vulkan_lib']}", error=True)
            sys.exit(1)
        if target["platform"] == "macos":
            install_macos_vulkan_drivers(install_dir)
            rewrite_macos_loader_paths(built[0], install_dir)
        VULKAN_BUILD_OUTPUTS[target["name"]] = built[0]
        verify_vulkan_runtime(built[0], install_dir, target["platform"])
        destination.parent.mkdir(parents=True, exist_ok=True)
        try:
            shutil.copy2(str(built[0]), str(destination))
        except PermissionError:
            log(f"Could not refresh locked runtime library {destination}; packaging fresh build output")
        if target["platform"] == "windows":
            runtime_pattern = "*.dll"
        elif target["platform"] == "macos":
            runtime_pattern = "*.dylib"
        else:
            runtime_pattern = "*.so*"
        backend_libraries = {
            target["vulkan_lib"].lower(),
            target["texconv_lib"].lower(),
        }
        for old_runtime in destination.parent.glob(runtime_pattern):
            if old_runtime.name.lower() not in backend_libraries:
                try:
                    old_runtime.unlink()
                except PermissionError:
                    log(f"Could not remove locked runtime library {old_runtime.name}")
        for runtime_library in vulkan_runtime_libraries(install_dir, target["platform"]):
            if runtime_library.name.lower() not in backend_libraries:
                try:
                    shutil.copy2(
                        str(runtime_library),
                        str(destination.parent / runtime_library.name),
                    )
                except PermissionError:
                    log(f"Could not refresh locked runtime library {runtime_library.name}")
        built_shaders = list(install_dir.rglob("*.spv"))
        if {shader.name for shader in built_shaders} != required_shaders:
            log("Compressonator Vulkan build did not produce all required shaders", error=True)
            sys.exit(1)
        shader_destination.mkdir(parents=True, exist_ok=True)
        for shader in built_shaders:
            shutil.copy2(str(shader), str(shader_destination / shader.name))
        log(f"Installed Compressonator Vulkan backend in {destination.parent}")


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def clean():
    log("Cleaning built artifacts...")
    for target in TARGETS:
        binary = binary_path(target)
        if binary.exists():
            binary.unlink()
            log(f"  removed {binary.name}")

    legacy_artifacts = PROJECT / ".build"
    if legacy_artifacts.exists():
        remove_tree(legacy_artifacts)
        log("  removed legacy .build/")

    if DIST_DIR.exists():
        remove_tree(DIST_DIR)
        log("  removed dist/")


def built_named_binary(build_dir: Path, target: dict, stem: str) -> Path:
    name = stem + (".exe" if target["goos"] == "windows" else "")
    for candidate in (build_dir / "Release" / name, build_dir / name):
        if candidate.is_file():
            return candidate
    matches = [
        path for path in build_dir.rglob(name)
        if path.is_file() and "_deps" not in path.parts
    ]
    if not matches:
        log(f"C++ build did not produce {name}", error=True)
        sys.exit(1)
    return max(matches, key=lambda path: path.stat().st_mtime)


def built_app_binary(build_dir: Path, target: dict) -> Path:
    return built_named_binary(build_dir, target, "goDragonCooker")


def reset_stale_cmake_generator(build_dir: Path, command: list[str]):
    if "-G" not in command or not (build_dir / "CMakeCache.txt").is_file():
        return
    wanted = command[command.index("-G") + 1]
    cached = ""
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_GENERATOR:INTERNAL="):
            cached = line.split("=", 1)[1]
            break
    if cached and cached != wanted:
        log(f"  Removing {build_dir.name} configured with {cached}")
        remove_tree(build_dir)


def visual_studio_generator() -> str:
    """Newest Visual Studio CMake generator installed on this machine.

    GitHub's windows-2025 and windows-11-arm images ship Visual Studio 2026.
    CMake's generator for that release is "Visual Studio 18 2026" and it does
    not accept the 2022 generator when 2022 is not installed.
    """
    program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    major = 17
    if vswhere.is_file():
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*", "-property", "installationVersion"],
            capture_output=True,
            text=True,
        )
        version = result.stdout.strip().splitlines()
        if version and version[0].split(".")[0].isdigit():
            major = int(version[0].split(".")[0])
    if major >= 18:
        return "Visual Studio 18 2026"
    return "Visual Studio 17 2022"


def app_cmake_command(target: dict) -> tuple[list[str], dict[str, str], Path]:
    """CMake configure command for one app target.

    Windows builds use the Visual Studio generator, so ARM64 cross-compiles from
    an x64 host. Other platforms use the matching C++ compiler directly.
    """
    build_dir = PROJECT / ".build" / f"app-{target['name']}"
    command = ["cmake", "-S", str(PROJECT), "-B", str(build_dir)]
    env: dict[str, str] = {}
    host = platform.system()

    if target["platform"] == "windows" and host == "Windows":
        arch = "ARM64" if target["goarch"] == "arm64" else "x64"
        command += ["-G", visual_studio_generator(), "-A", arch]
    elif target["platform"] == "macos":
        compiler = darwin_compiler_env(target)
        command += [
            f"-DCMAKE_C_COMPILER={compiler['CC']}",
            f"-DCMAKE_CXX_COMPILER={compiler['CXX']}",
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
        ]
    else:
        if target["platform"] == "windows":
            env = windows_compiler_env(target)
            command.append("-DCMAKE_SYSTEM_NAME=Windows")
        else:
            env = linux_compiler_env(target)
            if host != "Linux" or target["goarch"] != host_goarch():
                processor = "aarch64" if target["goarch"] == "arm64" else "x86_64"
                command += ["-DCMAKE_SYSTEM_NAME=Linux", f"-DCMAKE_SYSTEM_PROCESSOR={processor}"]
        if env.get("CC"):
            command += [f"-DCMAKE_C_COMPILER={env['CC']}", f"-DCMAKE_CXX_COMPILER={env['CXX']}"]

    command.append("-DCMAKE_BUILD_TYPE=Release")
    command.append(f"-DGDC_X86_64_V3={'ON' if X86_64_V3 else 'OFF'}")
    command.append("-DGDC_BUILD_TESTS=ON")
    return command, env, build_dir


def build_target(target: dict, upx: bool, upx_threads: bool, sources: list[Path]):
    name = target["name"]
    binary = binary_path(target)

    baseline = f"x86-64-v3={X86_64_V3}"
    if not needs_build(binary, sources) and arch_stamp_matches("app", target, baseline):
        log(f"Skipping {name} (up to date)")
        return
    if not shutil.which("cmake"):
        log("CMake is required to build the application", error=True)
        sys.exit(1)

    log(f"Building {name}...")
    command, env, build_dir = app_cmake_command(target)
    reset_stale_cmake_generator(build_dir, command)
    drop_env = MSVC_CONFIG_ENV_DROP if any(
        part.startswith("Visual Studio") for part in command
    ) else ()
    run(command, env=env, drop_env=drop_env)
    run(
        ["cmake", "--build", str(build_dir), "--config", "Release", "--parallel", str(CPU_THREADS)],
        env=env,
        drop_env=drop_env,
    )

    binary.parent.mkdir(parents=True, exist_ok=True)
    tmp_binary = binary.parent / f".build_{name}{binary.suffix}"
    shutil.copy2(str(built_app_binary(build_dir, target)), str(tmp_binary))

    before_mb = tmp_binary.stat().st_size / (1024 * 1024)
    log(f"  Stripped build: {before_mb:.1f} MB")

    # UPX compress if available and requested
    if upx:
        upx_cmd = ["upx", "--best"]
        if upx_threads:
            upx_cmd.append(f"--threads={CPU_THREADS}")
        upx_cmd.append(str(tmp_binary))
        result = run(upx_cmd, check=False)
        if result.returncode == 0:
            after_mb = tmp_binary.stat().st_size / (1024 * 1024)
            log(f"  UPX compressed: {after_mb:.1f} MB ({before_mb - after_mb:.1f} MB saved)")
        else:
            after_mb = before_mb
            log("  UPX compression failed, keeping stripped binary", error=True)
    else:
        after_mb = before_mb

    # Replace final binary
    try:
        if binary.exists():
            binary.unlink()
        shutil.move(str(tmp_binary), str(binary))
        binary.chmod(0o755)
        BINARY_BUILD_OUTPUTS[name] = binary
        write_arch_stamp("app", target, baseline)
        log(f"Built {binary.name} ({after_mb:.1f} MB)")
    except PermissionError:
        BINARY_BUILD_OUTPUTS[name] = tmp_binary
        log(f"Could not replace locked {binary.name}; packaging fresh build output")


# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------

def host_can_run(target: dict) -> bool:
    host = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}.get(platform.system(), "")
    return host == target["platform"] and host_goarch() == target["goarch"]


def build_self_test_binaries(target: dict) -> dict[str, Path]:
    command, env, build_dir = app_cmake_command(target)
    drop_env = MSVC_CONFIG_ENV_DROP if any(
        part.startswith("Visual Studio") for part in command
    ) else ()
    reset_stale_cmake_generator(build_dir, command)
    run(command, env=env, drop_env=drop_env)
    run(
        [
            "cmake", "--build", str(build_dir), "--config", "Release",
            "--parallel", str(CPU_THREADS),
            "--target", "gdc_i18n_test",
            "--target", "gdc_model_test",
            "--target", "gdc_cook_test",
        ],
        env=env,
        drop_env=drop_env,
    )
    return {
        name: built_named_binary(build_dir, target, name)
        for name in ("gdc_i18n_test", "gdc_model_test", "gdc_cook_test")
    }


def run_report(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    result = run(cmd, **kwargs)
    for line in ((result.stdout or "") + (result.stderr or "")).splitlines():
        if line.strip():
            log(f"  {line}", info=False)
    return result


def run_self_tests(target: dict, stage: Path):
    """Run the harnesses against the staged package. Cook uses the texconv just copied there."""
    if not host_can_run(target):
        log(f"Self-test skipped for {target['name']}: this host cannot run that build")
        return
    data = APP_SOURCE / "tests" / "data"
    image = data / "color.png"
    model = data / "triangle.obj"
    if not image.is_file() or not model.is_file():
        log(f"Self-test fixtures missing in {data}", error=True)
        sys.exit(1)

    binaries = build_self_test_binaries(target)
    log(f"Self-test {target['name']}")
    run_report([str(binaries["gdc_i18n_test"])])

    converted = PROJECT / ".build" / f"selftest-model-{target['name']}"
    if converted.exists():
        remove_tree(converted)
    run_report([
        str(binaries["gdc_model_test"]),
        "--convert", "glb,glb-draco,dae,cdae,dts",
        "--out", str(converted),
        "--back",
        str(model),
    ])
    hierarchy = data / "hierarchy.dae"
    hier_out = PROJECT / ".build" / f"selftest-hierarchy-{target['name']}"
    if hier_out.exists():
        remove_tree(hier_out)
    run_report([
        str(binaries["gdc_model_test"]),
        "--convert", "glb,dae,fbx,obj,x,cdae,dts",
        "--out", str(hier_out),
        "--back",
        str(hierarchy),
    ])
    run_report([str(binaries["gdc_model_test"]), "--corrupt-structure", "--out", str(hier_out), str(hierarchy)])
    caps_out = hier_out / "caps"
    caps_out.mkdir(parents=True, exist_ok=True)
    run_report([str(binaries["gdc_model_test"]), "--caps", "--out", str(caps_out), str(hierarchy)])

    cook = str(binaries["gdc_cook_test"])
    cooked = PROJECT / ".build" / f"selftest-cook-{target['name']}"
    if cooked.exists():
        remove_tree(cooked)
    run_report([
        cook, "--work", str(cooked),
        f"color:{image}", f"normal:{image}", f"dataR:{image}",
    ], cwd=stage)
    corrupted = PROJECT / ".build" / f"selftest-cook-corrupt-{target['name']}"
    if corrupted.exists():
        remove_tree(corrupted)
    run_report([cook, "--work", str(corrupted), "--corrupt", f"color:{image}"], cwd=stage)
    log(f"Self-test {target['name']} passed")


def package_target(target: dict, run_tests: bool = False):
    name = target["name"]
    binary_src = BINARY_BUILD_OUTPUTS.get(name, binary_path(target))
    lib_name = target["texconv_lib"]
    lib_src = texconv_library(target)
    vulkan_src = VULKAN_BUILD_OUTPUTS.get(target["name"], vulkan_library(target))
    shader_src = vulkan_src.parent / "shaders"

    if not binary_src.exists():
        log(f"Binary not found: {binary_src}", error=True)
        sys.exit(1)
    if not lib_src.exists():
        log(f"Library not found: {lib_src}", error=True)
        sys.exit(1)
    ships_vulkan = target["platform"] in {"windows", "linux", "macos"}
    if ships_vulkan and (
        not vulkan_src.exists() or not shader_src.is_dir()
    ):
        log(f"Compressonator Vulkan backend not found: {vulkan_src}", error=True)
        sys.exit(1)

    DIST_DIR.mkdir(exist_ok=True)
    stage = DIST_DIR / target["name"]
    if stage.exists():
        remove_tree(stage)
    stage.mkdir()
    stage_bin = stage / "bin" / target["name"]
    stage_bin.mkdir(parents=True)

    packaged_binary_name = binary_path(target).name
    shutil.copy2(str(binary_src), str(stage / packaged_binary_name))
    font_dir = PROJECT / "app" / "fonts"
    if font_dir.is_dir():
        shutil.copytree(str(font_dir), str(stage / "fonts"))
    shutil.copy2(str(lib_src), str(stage_bin / lib_name))
    if ships_vulkan:
        shutil.copy2(str(vulkan_src), str(stage_bin / target["vulkan_lib"]))
        shutil.copytree(str(shader_src), str(stage_bin / "shaders"))
        backend_libraries = {target["vulkan_lib"], target["texconv_lib"]}
        for runtime_library in vulkan_runtime_libraries(
            vulkan_src.parent,
            target["platform"],
        ):
            if runtime_library.name not in backend_libraries:
                shutil.copy2(
                    str(runtime_library),
                    str(stage_bin / runtime_library.name),
                )

    if run_tests:
        run_self_tests(target, stage)

    zip_path = DIST_DIR / target["zip_name"]
    with zipfile.ZipFile(str(zip_path), "w", zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(str(stage)):
            for fname in sorted(files):
                fpath = Path(root) / fname
                arcname = str(fpath.relative_to(DIST_DIR))
                zf.write(str(fpath), arcname)

    zip_size = zip_path.stat().st_size / (1024 * 1024)
    log(f"Packed {target['zip_name']} ({zip_size:.1f} MB)")

    remove_tree(stage)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description="Build and package goDragonCooker")
    p.add_argument("--linux", action="store_true", help="Build Linux target only")
    p.add_argument("--macos", action="store_true", help="Build macOS target only")
    p.add_argument("--windows", action="store_true", help="Build Windows target only")
    p.add_argument("--x64", action="store_true", help="Build x64 targets only")
    p.add_argument("--arm64", action="store_true", help="Build ARM64 targets only")
    p.add_argument("--clean", action="store_true", help="Remove artifacts before building")
    p.add_argument("--no-upx", action="store_true", help="Skip UPX compression")
    p.add_argument(
        "--tests",
        action="store_true",
        help="Run the i18n, model, and texconv cook self-tests against the staged package",
    )
    p.add_argument("--skip-texconv", action="store_true", help="Use existing Texconv libraries")
    p.add_argument("--skip-vulkan", action="store_true", help="Use existing Compressonator Vulkan libraries")
    p.add_argument(
        "--no-x86-64-v3",
        action="store_true",
        help="Build x64 targets for the baseline x86-64 CPU instead of x86-64-v3 (AVX2)",
    )
    args = p.parse_args()

    global X86_64_V3
    X86_64_V3 = not args.no_x86_64_v3

    if args.clean:
        clean()

    platform_args = {
        name for name, enabled in (
            ("linux", args.linux),
            ("macos", args.macos),
            ("windows", args.windows),
        ) if enabled
    }
    arch_args = {
        name for name, enabled in (
            ("x64", args.x64),
            ("arm64", args.arm64),
        ) if enabled
    }

    selected_platforms = platform_args or {"linux", "windows"}
    selected = [
        target for target in TARGETS
        if target["platform"] in selected_platforms
        and (not arch_args or target["arch_name"] in arch_args)
    ]

    if not selected:
        p.error("no build targets selected")

    upx_enabled = not args.no_upx
    build_texconv(selected, args.skip_texconv)
    build_vulkan(selected, args.skip_vulkan)

    sources = get_sources()
    upx_installed = upx_enabled and upx_available()
    upx_threads = upx_installed and upx_supports_threads()
    if upx_enabled and not upx_installed:
        log("UPX not found, skipping compression")
    with ThreadPoolExecutor(max_workers=len(selected)) as executor:
        futures = [
            executor.submit(build_target, target, upx_installed, upx_threads, sources)
            for target in selected
        ]
        for future in futures:
            future.result()

    for target in selected:
        package_target(target, args.tests)

    zips = list(DIST_DIR.glob("*.zip"))
    if zips:
        log(f"\nDone. {len(zips)} release(s) in dist/")
        for z in sorted(zips):
            sz = f"{z.stat().st_size / (1024*1024):.1f} MB"
            log(f"  {z.name}  ({sz})", info=False)
    else:
        log("No releases produced.", error=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
