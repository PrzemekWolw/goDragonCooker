#!/usr/bin/env python3
"""Build goDragonCooker for Linux, macOS, and Windows, package into release zips.

Builds with -ldflags="-s -w" to strip debug info, then UPX-compresses
for minimal binary size.

Usage:
    python build_release.py              # build Linux and Windows targets
    python build_release.py --linux      # Linux only
    python build_release.py --macos      # macOS only
    python build_release.py --windows    # Windows only
    python build_release.py --arm64      # ARM64 targets only
    python build_release.py --clean      # remove artifacts before building
    python build_release.py --no-upx     # skip UPX compression
    python build_release.py --skip-texconv  # use existing Texconv libraries
    python build_release.py --skip-vulkan   # use existing Vulkan libraries/shaders

Requires (Linux):
    - go 1.26.5+ with CGO support
    - mingw64-cross-gcc + mingw64-cross-gcc-c++ for Windows x64 cross-compile
    - aarch64-w64-mingw32-gcc + aarch64-w64-mingw32-g++ for Windows ARM64 cross-compile
    - aarch64-linux-gnu-gcc + aarch64-linux-gnu-g++ for Linux ARM64 cross-compile
    - CMake 3.21+; Linux downloads the Vulkan SDK/DXC automatically
      when VULKAN_SDK is not set or incomplete
    - curl is an optional fallback for LunarG SDK downloads
    - upx (optional, skipped gracefully if not found)

Requires (Windows):
    - go 1.26.5+ with CGO support
    - MinGW-w64 or LLVM C/C++ compilers on PATH
    - x86_64-linux-gnu-gcc/g++ for Linux x64 builds
    - aarch64-linux-gnu-gcc/g++ for Linux ARM64 builds
    - Visual Studio C++ tools and Windows SDK for Texconv builds
    - CMake 3.21+ and Vulkan SDK/DXC for Vulkan native builds
    - upx (optional, skipped gracefully if not found)
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
import os
import platform
import shutil
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
APP_VERSION = VERSION_FILE.read_text(encoding="utf-8").strip()
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
    vulkan_lib = "gdc_vulkan.dll" if goos == "windows" else "libgdc_vulkan.so"
    return {
        "name": name,
        "platform": platform_name,
        "goos": goos,
        "goarch": goarch,
        "arch_name": arch_name,
        "binary_name": f"goDragonCooker.{arch_name}{suffix}",
        "cflags": {},
        "ldflags_extra": "-H windowsgui" if goos == "windows" else "",
        "texconv_lib": texconv_lib,
        "vulkan_lib": vulkan_lib,
        "zip_name": f"goDragonCooker-{platform_name}-{arch_name}.zip",
    }


TARGETS = [
    make_target("linux", "linux", "amd64", "x64"),
    make_target("linux", "linux", "arm64", "arm64"),
    make_target("macos", "darwin", "amd64", "x64"),
    make_target("macos", "darwin", "arm64", "arm64"),
    make_target("windows", "windows", "amd64", "x64"),
    make_target("windows", "windows", "arm64", "arm64"),
]


def vulkan_runtime_libraries(directory: Path, platform_name: str) -> list[Path]:
    pattern = "*.dll" if platform_name == "windows" else "*.so*"
    libraries = list(directory.rglob(pattern))
    if platform_name == "linux":
        libraries = [
            library for library in libraries
            if (suffix := library.name.partition(".so")[2]) == ""
            or (suffix.startswith(".") and suffix[1:].isdigit())
        ]
    return libraries


def verify_vulkan_runtime(library: Path, directory: Path, platform_name: str):
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


def run(
    cmd: list[str],
    *,
    env: dict | None = None,
    check: bool = True,
    cwd: Path | None = None,
) -> subprocess.CompletedProcess:
    full_env = {**os.environ, **(env or {})}
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
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-20:]:
                log(f"  {line}", error=True)
        sys.exit(result.returncode)
    return result


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
    out = list(APP_SOURCE.glob("*.go"))
    out.extend(APP_SOURCE.glob("*.c"))
    out.extend(APP_SOURCE.glob("*.cpp"))
    out.extend(APP_SOURCE.glob("*.h"))
    if VULKAN_SOURCE.exists():
        out.extend(path for path in VULKAN_SOURCE.rglob("*") if path.is_file())
    out.append(VERSION_FILE)
    if (PROJECT / "go.mod").exists():
        out.append(PROJECT / "go.mod")
    if (PROJECT / "go.sum").exists():
        out.append(PROJECT / "go.sum")
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


def windows_compiler_env(target: dict) -> dict[str, str]:
    """Return a usable C/C++ compiler environment for a Windows build."""
    if os.environ.get("CC") and os.environ.get("CXX"):
        return compiler_env_with_runtime({"CC": os.environ["CC"], "CXX": os.environ["CXX"]})

    if platform.system() == "Windows":
        if target["goarch"] == "arm64":
            compiler_pairs = [
                ("aarch64-w64-mingw32-gcc", "aarch64-w64-mingw32-g++"),
            ]
        else:
            compiler_pairs = [("gcc", "g++"), ("clang", "clang++")]
    else:
        prefix = "aarch64" if target["goarch"] == "arm64" else "x86_64"
        compiler_pairs = [(f"{prefix}-w64-mingw32-gcc", f"{prefix}-w64-mingw32-g++")]

    for cc, cxx in compiler_pairs:
        if shutil.which(cc) and shutil.which(cxx):
            return compiler_env_with_runtime({"CC": cc, "CXX": cxx})

    log(
        f"No C/C++ compiler found for the Windows {target['arch_name']} build. "
        "Install MinGW-w64 or LLVM, or set CC and CXX.",
        error=True,
    )
    sys.exit(1)


def linux_compiler_env(target: dict) -> dict[str, str]:
    """Return a usable C/C++ compiler environment for a Linux build."""
    if platform.system() != "Windows" and target["goarch"] == host_goarch():
        return {}

    if os.environ.get("CC") and os.environ.get("CXX"):
        return compiler_env_with_runtime({"CC": os.environ["CC"], "CXX": os.environ["CXX"]})

    prefix = "aarch64" if target["goarch"] == "arm64" else "x86_64"
    compiler_pairs = [(f"{prefix}-linux-gnu-gcc", f"{prefix}-linux-gnu-g++")]
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
    if platform.system() != "Darwin":
        log(
            "macOS builds require a macOS host with the Apple SDK and clang.",
            error=True,
        )
        sys.exit(1)
    if not shutil.which("clang") or not shutil.which("clang++"):
        log("macOS builds require clang and clang++.", error=True)
        sys.exit(1)
    arch_flag = "x86_64" if target["goarch"] == "amd64" else target["goarch"]
    return {
        "CC": "clang",
        "CXX": "clang++",
        "CGO_CFLAGS": f"-arch {arch_flag}",
        "CGO_LDFLAGS": f"-arch {arch_flag}",
    }


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

    build_universal = (
        host_target == "macos"
        and {target["goarch"] for target in targets if target["platform"] == "macos"}
        == {"amd64", "arm64"}
    )

    build_targets = [host_targets[0]] if build_universal else host_targets
    for target in build_targets:
        build_texconv_target(target, build_universal)
    if build_universal:
        source = texconv_library(host_targets[0])
        for target in host_targets[1:]:
            destination = texconv_library(target)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(str(source), str(destination))
            log(f"Installed {destination}")


def build_texconv_target(target: dict, universal: bool):
    source_binary = TEXCONV_SOURCE / target["texconv_lib"]
    destination = texconv_library(target)
    texconv_source = TEXCONV_SOURCE / "TexconvDLL" / "texconv.cpp"
    if destination.exists() and not needs_build(destination, [texconv_source]):
        log(f"Skipping Texconv {target['name']} (up to date)")
        return

    architecture = "arm64" if target["goarch"] == "arm64" else "x64"
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
        ]
        if universal:
            command.append("--universal")
        else:
            command.extend(["--architecture", architecture])
        build_result = run(command, cwd=TEXCONV_SOURCE)
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
                ] + (
                    ["--universal"]
                    if universal
                    else ["--architecture", architecture]
                ),
                cwd=TEXCONV_SOURCE,
                env=(
                    linux_compiler_env(target)
                    if target["platform"] == "linux"
                    else darwin_compiler_env(target)
                ),
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
        if target["platform"] not in {"windows", "linux"} or target["platform"] != host_target:
            continue
        destination = vulkan_library(target)
        shader_destination = destination.parent / "shaders"
        shader_names = {
            shader.name for shader in shader_destination.glob("*.spv")
        } if shader_destination.is_dir() else set()
        if (
            destination.exists()
            and shader_names == required_shaders
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
            shutil.rmtree(install_dir)
        install_dir.mkdir(parents=True, exist_ok=True)
        env = (
            windows_compiler_env(target)
            if target["platform"] == "windows"
            else linux_compiler_env(target)
        )
        configure = [
            "cmake",
            "-S", str(VULKAN_SOURCE),
            "-B", str(build_dir),
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_INSTALL_PREFIX={install_dir}",
        ]
        if os.environ.get("VULKAN_SDK"):
            configure.append(f"-DCMAKE_PREFIX_PATH={os.environ['VULKAN_SDK']}")
        if os.environ.get("GDC_PRECOMPILED_SHADER_DIR"):
            configure.append(
                f"-DGDC_PRECOMPILED_SHADER_DIR={os.environ['GDC_PRECOMPILED_SHADER_DIR']}"
            )
        if env.get("CXX"):
            configure.append(f"-DCMAKE_CXX_COMPILER={env['CXX']}")
            compiler_name = Path(env["CXX"]).name.lower()
            if target["platform"] == "windows" and "clang" in compiler_name:
                configure[1:1] = ["-G", "Ninja"]
            elif "g++" in compiler_name:
                configure[1:1] = ["-G", "MinGW Makefiles"]
        run(configure, env=env)
        run(["cmake", "--build", str(build_dir), "--config", "Release", "--parallel", str(CPU_THREADS)], env=env)
        run([
            "cmake", "--install", str(build_dir), "--config", "Release",
            "--component", "gdc_vulkan_runtime",
        ], env=env)

        built = list(install_dir.rglob(target["vulkan_lib"]))
        if not built:
            log(f"Compressonator Vulkan build did not produce {target['vulkan_lib']}", error=True)
            sys.exit(1)
        VULKAN_BUILD_OUTPUTS[target["name"]] = built[0]
        verify_vulkan_runtime(built[0], install_dir, target["platform"])
        destination.parent.mkdir(parents=True, exist_ok=True)
        try:
            shutil.copy2(str(built[0]), str(destination))
        except PermissionError:
            log(f"Could not refresh locked runtime library {destination}; packaging fresh build output")
        runtime_pattern = "*.dll" if target["platform"] == "windows" else "*.so*"
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
        shutil.rmtree(legacy_artifacts)
        log("  removed legacy .build/")

    if DIST_DIR.exists():
        shutil.rmtree(DIST_DIR)
        log("  removed dist/")


def build_target(
    target: dict,
    upx: bool,
    upx_threads: bool,
    go_parallelism: int,
    sources: list[Path],
):
    name = target["name"]
    binary = binary_path(target)

    if not needs_build(binary, sources):
        log(f"Skipping {name} (up to date)")
        return

    log(f"Building {name}...")

    env = {
        "GOOS": target["goos"],
        "GOARCH": target["goarch"],
        "CGO_ENABLED": "1",
        "GOMAXPROCS": str(CPU_THREADS),
    }
    env.update(target.get("cflags", {}))
    if target["platform"] == "windows":
        env.update(windows_compiler_env(target))
    elif target["platform"] == "linux":
        env.update(linux_compiler_env(target))
    elif target["platform"] == "macos":
        env.update(darwin_compiler_env(target))

    if target["goarch"] == "arm64":
        for flag_name in ("CGO_CFLAGS", "CGO_CXXFLAGS"):
            env[flag_name] = f"{env.get(flag_name, '')} -fsigned-char".strip()

    binary.parent.mkdir(parents=True, exist_ok=True)
    tmp_binary = binary.parent / f".build_{name}{binary.suffix}"

    ld_extra = target.get("ldflags_extra", "")
    parts = ["-s", "-w", f"-X main.appVersion={APP_VERSION}"]
    if ld_extra:
        parts.append(ld_extra)
    ld_flags = " ".join(parts)

    # Build with stripped debug info
    run(
        [
            "go",
            "build",
            "-p",
            str(go_parallelism),
            "-trimpath",
            f"-ldflags={ld_flags}",
            "-o",
            str(tmp_binary),
            "./app",
        ],
        env=env,
    )

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
        log(f"Built {binary.name} ({after_mb:.1f} MB)")
    except PermissionError:
        BINARY_BUILD_OUTPUTS[name] = tmp_binary
        log(f"Could not replace locked {binary.name}; packaging fresh build output")


# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------

def package_target(target: dict):
    name = target["name"]
    binary_src = BINARY_BUILD_OUTPUTS.get(name, binary_path(target))
    lib_name = target["texconv_lib"]
    lib_src = texconv_library(target)
    vulkan_src = VULKAN_BUILD_OUTPUTS.get(target["name"], vulkan_library(target))
    shader_src = vulkan_src.parent / "shaders"

    if not binary_src.exists():
        log(f"Binary not found: {binary_src}", error=True)
        return
    if not lib_src.exists():
        log(f"Library not found: {lib_src}", error=True)
        return
    if target["platform"] in {"windows", "linux"} and (
        not vulkan_src.exists() or not shader_src.is_dir()
    ):
        log(f"Compressonator Vulkan backend not found: {vulkan_src}", error=True)
        return

    DIST_DIR.mkdir(exist_ok=True)
    stage = DIST_DIR / target["name"]
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir()
    stage_bin = stage / "bin" / target["name"]
    stage_bin.mkdir(parents=True)

    packaged_binary_name = binary_path(target).name
    shutil.copy2(str(binary_src), str(stage / packaged_binary_name))
    shutil.copy2(str(lib_src), str(stage_bin / lib_name))
    if target["platform"] in {"windows", "linux"}:
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

    zip_path = DIST_DIR / target["zip_name"]
    with zipfile.ZipFile(str(zip_path), "w", zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(str(stage)):
            for fname in sorted(files):
                fpath = Path(root) / fname
                arcname = str(fpath.relative_to(DIST_DIR))
                zf.write(str(fpath), arcname)

    zip_size = zip_path.stat().st_size / (1024 * 1024)
    log(f"Packed {target['zip_name']} ({zip_size:.1f} MB)")

    shutil.rmtree(stage)


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
    p.add_argument("--skip-texconv", action="store_true", help="Use existing Texconv libraries")
    p.add_argument("--skip-vulkan", action="store_true", help="Use existing Compressonator Vulkan libraries")
    args = p.parse_args()

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
    go_parallelism = max(1, CPU_THREADS // len(selected))
    with ThreadPoolExecutor(max_workers=len(selected)) as executor:
        futures = [
            executor.submit(
                build_target,
                target,
                upx_installed,
                upx_threads,
                go_parallelism,
                sources,
            )
            for target in selected
        ]
        for future in futures:
            future.result()

    for target in selected:
        package_target(target)

    zips = list(DIST_DIR.glob("*.zip"))
    if zips:
        log(f"\nDone. {len(zips)} release(s) in dist/")
        for z in sorted(zips):
            sz = f"{z.stat().st_size / (1024*1024):.1f} MB"
            log(f"  {z.name}  ({sz})", info=False)
    else:
        log("No releases produced.", error=True)


if __name__ == "__main__":
    main()
