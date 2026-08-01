# Cross compilation

Supported product targets:

| Target id | Platform | How to build |
|-----------|----------|--------------|
| `win-x86_64` | Windows x86_64 | Cross via MinGW on a **Linux host** (`cmake/toolchains/win-x86_64.cmake`) |
| `macos-arm` | macOS arm64 | **Native** on Apple Silicon (no cross toolchain file) |

**Linux is not a product target.** A Linux machine is only used as the host for Windows MinGW / vcpkg / msitools packaging.

## Machine-specific configuration

Do **not** commit absolute paths. Use:

```bash
cp scripts/env.example scripts/env.local
# edit: WDS_VCPKG_ROOT=…  (and optional MSI / MinGW overrides)
```

Or `export WDS_ENV_FILE=/path/to/your.env`.  
`build-target.sh` / `package-target.sh` load these via `scripts/lib/wds-env.sh` and derive `CMAKE_PREFIX_PATH` / `Vulkan_LIBRARY` when possible.

## Quick start

```bash
./scripts/build-target.sh --check

# Windows (after env.local has WDS_VCPKG_ROOT)
./scripts/build-target.sh win-x86_64

# macOS Apple Silicon
./scripts/build-target.sh macos-arm
./scripts/build-target.sh macos-arm --debug
```

Plain CMake equivalent (Windows toolchain):

```bash
# Prefer env-derived values; example shape only:
cmake -S . -B build-win-x86_64 --toolchain cmake/toolchains/win-x86_64.cmake \
  -DCMAKE_PREFIX_PATH="$WDS_CMAKE_PREFIX_PATH" \
  -DVulkan_LIBRARY="$WDS_VULKAN_LIBRARY"
cmake --build build-win-x86_64
```

| Target | Artifacts under `dist/` |
|--------|-------------------------|
| `win-x86_64` | `wds-win-x86_64.msi` + portable `wds-win-x86_64.zip` |
| `macos-arm` | `wds-macos-arm.zip` + `wds-macos-arm.dmg` |

Windows MSI uses [msitools](https://wiki.gnome.org/msitools) `wixl` on the Linux packaging host. Preferences live under `%LOCALAPPDATA%\WDS\config\config.yml` (not next to the exe).

## Defaults when cross-compiling

- `WDS_CORE_BUILD_TESTS=OFF`
- `WDS_CORE_BUILD_EXAMPLE=ON`

`wds_editor` needs **target** GLFW in the prefix (e.g. vcpkg `glfw3:x64-mingw-static`). Missing GLFW skips the editor with a status message.

## Installing toolchains (Linux host → Windows)

```bash
sudo apt update
sudo apt install mingw-w64 g++-mingw-w64-x86-64 cmake ninja-build zip curl
# optional MSI: msitools
```

vcpkg (static triplet):

```bash
export VCPKG_DEFAULT_TRIPLET=x64-mingw-static
export VCPKG_DEFAULT_HOST_TRIPLET=x64-mingw-static
./vcpkg install libpng zlib glfw3 vulkan-loader
```

Then in `scripts/env.local`:

```bash
WDS_VCPKG_ROOT="/path/to/vcpkg"
```

```bash
./scripts/build-target.sh win-x86_64
# core only:
./scripts/build-target.sh win-x86_64 -- -DWDS_BUILD_RENDERER=OFF
```

## Native macOS (arm64)

```bash
brew install cmake libpng glfw glslang molten-vk vulkan-headers vulkan-loader
./scripts/build-target.sh macos-arm
```

On Darwin arm64, CMake auto-sets `WDS_TARGET=macos-arm`.

## Renderer / BASS notes

`renderer` needs target `libpng` + Vulkan import library. **BASS** is vendored under `audio-player/third_party/bass/{macos-arm,win-x86_64}/`. Packaging copies the matching runtime into the artifact.
