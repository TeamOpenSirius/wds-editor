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

`wds_editor` needs target Qt and Vulkan runtime support in the prefix. The Qt editor host no longer depends on GLFW.

## Tests: native CTest vs cross-compile

The product test gate is **macOS native CTest**. CI `macos-arm` order is `--no-package` → `ctest` → `--package-only`:

```bash
./scripts/build-target.sh macos-arm --no-package
ctest --test-dir build-macos-arm --output-on-failure
./scripts/build-target.sh --package-only macos-arm
```

| Host / target | CTest | What happens |
|---------------|-------|----------------|
| **macOS arm64 native** (`macos-arm`) | Run on the host | After `./scripts/build-target.sh macos-arm --no-package`, use `ctest --test-dir build-macos-arm --output-on-failure`, then `./scripts/build-target.sh --package-only macos-arm`. Binaries are host-native. |
| **Linux → Windows** (`win-x86_64`) | Do **not** run | Cross-compile (and package) only. The Linux host cannot execute those PE binaries. Do not run `ctest` against `build-win-x86_64`. |

When `CMAKE_CROSSCOMPILING`:

- `common` / `audio-player` / `interaction-interface` skip **building** their test targets.
- `core` / `renderer` / `chart-render` / `ui` may still **build** test executables, but `add_test` is skipped (host `ctest` cannot launch PE). Cross builds also default `WDS_CORE_BUILD_TESTS=OFF`.

Linux host, common+core unit tests only (not a product configure, not a Windows runtime check):

```bash
./scripts/run-host-core-tests.sh
```

Opt-in benches (`WDS_CORE_BUILD_BENCHMARKS`, `WDS_RENDERER_BUILD_BENCHMARKS`) are **not** CTest and are not run on the Linux packaging host. Renderer performance is measured through the Qt editor host diagnostics. This document does not claim Windows-native test results.

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
./vcpkg install libpng zlib vulkan-loader
```

Then in `scripts/env.local`:

```bash
WDS_VCPKG_ROOT="/path/to/vcpkg"
```

```bash
./scripts/build-target.sh win-x86_64
# core-only: renderer + interaction + ui must all be OFF (renderer-only OFF is FATAL).
./scripts/build-target.sh win-x86_64 -- \
  -DWDS_BUILD_RENDERER=OFF -DWDS_BUILD_INTERACTION=OFF -DWDS_BUILD_UI=OFF
# core without audio:
./scripts/build-target.sh win-x86_64 -- \
  -DWDS_BUILD_RENDERER=OFF -DWDS_BUILD_INTERACTION=OFF -DWDS_BUILD_UI=OFF \
  -DWDS_BUILD_AUDIO=OFF
```

## Native macOS (arm64)

```bash
brew install cmake libpng glslang molten-vk vulkan-headers vulkan-loader qtbase qtsvg
./scripts/build-target.sh macos-arm
```

On Darwin arm64, CMake auto-sets `WDS_TARGET=macos-arm`.

## Renderer / BASS notes

`renderer` needs target `libpng` + Vulkan import library. **BASS** is vendored under `audio-player/third_party/bass/{macos-arm,win-x86_64}/`. Packaging copies the matching runtime into the artifact.
