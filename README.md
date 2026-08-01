# goDragonCooker

goDragonCooker is a desktop utility for preparing BeamNG texture and material files.

| Material Generator | Texture Cooker |
| :---: | :---: |
| <img width="100%" alt="Screenshot 2026-08-05 180738" src="https://github.com/user-attachments/assets/2856727d-9257-45f6-adc9-44f13b008cd3" /> | <img width="100%" alt="Screenshot 2026-08-05 180728" src="https://github.com/user-attachments/assets/46f32520-170d-429c-8b66-69e4a6c43864" /> |


It provides:

- Texture cooking to DDS using Texconv.
- Automatic handling of color, data, and normal texture suffixes.
- GPU BC6H/BC7 compression with CPU fallback.
- Material JSON generation from groups of texture files.
- Optional detail-map and material-instance settings.

## Platform support

- Windows x64 and arm64: supported when built with matching C/C++ and Texconv toolchains.
- Linux x64 and arm64: supported when built with matching C/C++ and Texconv toolchains.
- macOS x64 and arm64: supported when built on macOS with the Apple SDK.

See [BUILD.md](BUILD.md) for build instructions.

## Running

The platform-specific Texconv library must be in the target-specific `bin` subdirectory next to the application:

- Windows x64: `bin/windows-x64/texconv.dll`
- Windows ARM64: `bin/windows-arm64/texconv.dll`
- Linux x64: `bin/linux-x64/libtexconv.so`
- Linux ARM64: `bin/linux-arm64/libtexconv.so`
- macOS x64: `bin/macos-x64/libtexconv.dylib`
- macOS ARM64: `bin/macos-arm64/libtexconv.dylib`

For development, run `go run ./app` from the repository root. The application selects the matching target directory automatically.

## License

goDragonCooker is distributed under the MIT License. See [LICENSE](LICENSE).
