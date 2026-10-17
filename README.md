# WDS Editor

_World Dai Star_ 的跨平台制谱器。

---

## 安装

从 GitHub **Releases** 下载对应安装包，无需自行编译：

| 平台                  | 文件                 | 安装方式                                       |
| --------------------- | -------------------- | ---------------------------------------------- |
| Windows               | `wds-win-x86_64.msi` | 双击安装（可选桌面 / 开始菜单快捷方式）        |
| macOS (Apple Silicon) | `wds-macos-arm.dmg`  | 打开 DMG，将 `WDS Editor.app` 拖入「应用程序」 |

### 运行前需要准备的外部依赖

| 平台        | 依赖                           | 说明                                                                           |
| ----------- | ------------------------------ | ------------------------------------------------------------------------------ |
| **两端**    | 支持 Vulkan 的 GPU 驱动        | NVIDIA / AMD / Intel 等最新驱动；无独立显卡时需系统提供可用 Vulkan ICD         |
| **Windows** | Vulkan Runtime（多数机器已有） | 若启动报 Vulkan 相关错误，从 [LunarG](https://vulkan.lunarg.com/) 安装 Runtime |
| **macOS**   | 无需单独装 Vulkan SDK          | 发行包已捆绑 MoltenVK；需要 **Apple Silicon + Metal**                          |

### macOS：首次打开被拦截时

从网络下载的 `.app` 可能带隔离属性，Gatekeeper 会阻止打开。在「应用程序」安装完成后执行一次：

```bash
xattr -cr "/Applications/WDS Editor.app"
```

## 从源码编译

仓库包含编译、打包与运行所需内容：源码、CMake/脚本、BASS、Vulkan 头、UI 字体/图标，以及运行时 `skins/`、`effects/`、`resources/` 等。  
本地路径请放在 `scripts/env.local`（已忽略），勿提交。

**已验证的主机 / 目标组合：**

| 构建主机            | 目标                           |
| ------------------- | ------------------------------ |
| Linux x86_64        | `win-x86_64`（MinGW 交叉编译） |
| macOS Apple Silicon | `macos-arm`（本机）            |
| Linux Mint 22.3 / Ubuntu 24.04 x86_64 | `linux-x86_64`（本机，Qt 6.8.3） |

Windows 本地可使用 Qt 6 + Visual Studio/CMake 构建；发行门禁仍以 CI 配置为准。

机器相关路径请写入本地环境文件，**不要写进示例文件**：

```bash
cp scripts/env.example scripts/env.local
# 编辑 env.local
```

也可用 `export WDS_ENV_FILE=/path/to/your.env`。变量说明见 [`scripts/env.example`](scripts/env.example)；交叉细节见 [`cmake/CROSS_COMPILE.md`](cmake/CROSS_COMPILE.md)。

### A. Linux 主机 → Windows（`win-x86_64`）

**环境准备**

1. 工具链：`mingw-w64`、`cmake`、`ninja` 或 Make、`zip`、`curl`
2. MSI 打包（可选但推荐）：`msitools`（`wixl` / `wixl-heat` / `msiinfo`）
3. [vcpkg](https://github.com/microsoft/vcpkg)，triplet `x64-mingw-static`，安装例如：
   `libpng` `zlib` `vulkan-loader`
4. 在 `scripts/env.local` 中设置：
   ```bash
   WDS_VCPKG_ROOT="$HOME/path/to/vcpkg"
   # 可选：WDS_MSITOOLS_PREFIX=...  WDS_PRODUCT_VERSION=1.0.0
   ```

**编译与打包**

```bash
./scripts/build-target.sh --check          # 探测 MinGW toolchain
./scripts/build-target.sh win-x86_64       # Release + MSI/zip → dist/
./scripts/build-target.sh win-x86_64 --debug --no-run   # Debug 树，不打包
```

产物：`dist/wds-win-x86_64.msi`、`dist/wds-win-x86_64.zip`。

### B. macOS Apple Silicon → `macos-arm`

**环境准备**

```bash
xcode-select --install   # 若尚未安装
brew install cmake libpng glslang molten-vk vulkan-headers vulkan-loader qtbase qtsvg
```

**编译与打包**

```bash
./scripts/build-target.sh macos-arm              # Release → zip + dmg
./scripts/build-target.sh macos-arm --debug      # Debug，自动启动编辑器
./scripts/build-target.sh macos-arm --debug --no-run
```

产物：`dist/wds-macos-arm.zip`、`dist/wds-macos-arm.dmg`。

### C. Windows 本机编译

当前跨平台脚本仍以 **Linux 交叉 MinGW** 为主路径。Windows 原生开发需准备：

- Visual Studio 或 MinGW-w64、CMake
- vcpkg（如 `x64-windows` 或 `x64-mingw-static`）提供 libpng / Vulkan
- 将 `CMAKE_PREFIX_PATH`、`Vulkan_LIBRARY` 指向本机前缀

配置后可用标准 CMake workflow 构建 `wds_editor`；Qt 版 Release 打包入口为 `scripts/package-qt-release.ps1`。

### D. Linux 本机编译（`linux-x86_64`）

支持 Linux x86_64 原生开发构建；已在 Linux Mint 22.3（Ubuntu 24.04 基础）、X11、NVIDIA Vulkan 上验证。
依赖 C++17 编译器、CMake、Ninja/Make、libpng/zlib、Vulkan loader/头文件、glslangValidator，
以及 Qt **6.5 以上**的 Core/Gui/Widgets/Concurrent/Network/Svg。
[Ubuntu 24.04 的 Qt 包](https://packages.ubuntu.com/noble/qt6-base-dev) 为 6.4 系列，需要另装较新 SDK；
以下示例使用与现有构建一致的 Qt 6.8.3。

Ubuntu 24.04 / Mint 22 的依赖准备：

```bash
sudo apt install build-essential ninja-build python3-venv libpng-dev zlib1g-dev \
  libvulkan-dev glslang-tools libgl-dev libglx-dev libopengl-dev libegl-dev \
  libxkbcommon-dev libxkbcommon-x11-0 libxcb-cursor0 libasound2t64 libpulse0

cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/wds-editor"
python3 -m venv "$cache_dir/tools"
"$cache_dir/tools/bin/pip" install aqtinstall cmake
"$cache_dir/tools/bin/aqt" install-qt linux desktop 6.8.3 linux_gcc_64 \
  -O "$cache_dir/qt" --archives qtbase qtsvg icu
```

从 [BASS 官方下载页](https://www.un4seen.com/bass.html) 分别下载 **BASS** 和 **BASSmix** 的 Linux 版本。
将压缩包 `libs/x86_64/` 下的 `libbass.so`、`libbassmix.so` 放进
`$cache_dir/deps/bass-linux/x86_64/`，保留随包许可文件。
Linux 库由使用者提供，构建脚本不会自动下载或安装系统依赖；图形驱动还需提供可用的 Vulkan ICD。

```bash
./scripts/build-linux.sh
"$cache_dir/tools/bin/ctest" --test-dir "$cache_dir/build-linux" --output-on-failure
./scripts/run-linux.sh
./scripts/run-linux.sh --open /absolute/path/song.wdsproject
```

构建产物默认位于用户缓存中。首次构建默认 `RelWithDebInfo`、4 个并行任务；后续构建保留已有
生成器、构建类型及依赖配置。额外的 CMake `-D` / `-U` 参数可在 `--` 后传入，例如：

```bash
./scripts/build-linux.sh -- -DCMAKE_BUILD_TYPE=Debug
```

| 环境变量 | 用途 |
| --- | --- |
| `WDS_LINUX_CACHE` | 缓存根目录，默认 `${XDG_CACHE_HOME:-$HOME/.cache}/wds-editor` |
| `WDS_LINUX_BUILD_DIR` | 覆盖构建目录；构建和启动时使用相同值，不同 checkout 各用一个目录 |
| `WDS_QT_LINUX_ROOT` | 自定义 Qt SDK 前缀，例如 `/opt/Qt/6.8.3/gcc_64` |
| `WDS_BASS_LINUX_DIR` | 自定义 BASS/BASSmix `.so` 目录 |
| `WDS_CMAKE` | 自定义 CMake 可执行文件 |
| `CMAKE_BUILD_PARALLEL_LEVEL` | 编译并行数，默认 4 |
| `CMAKE_GENERATOR` | 新构建的生成器；未指定时优先 Ninja |
| `CMAKE_PREFIX_PATH` | 标准 CMake 搜索前缀；Linux 环境变量用冒号分隔，`-D` 参数用分号分隔 |

脚本也识别缓存中的 Qt 6.8.3 SDK、`tools/bin`、`deps/sysroot/usr` 和可选的 `deps/runtime/lib`。
显式设置的 SDK/工具优先；切换 Qt SDK 或编译器时请使用新的构建目录。
脚本可从任意工作目录调用；带空格的路径请加引号。发现构建缓存属于另一 checkout 时会报错，避免启动旧版本。

在有图形会话的机器上，可验证播放和截图：

```bash
./scripts/run-linux.sh --smoke-test --quit-after 6 --autoplay \
  --open /absolute/path/song.wdsproject \
  --screenshot /tmp/wds-editor-playing.png@4000
```

`--open` 直接打开工程；`--smoke-test` 跳过欢迎页和更新检查，默认 1.2 秒退出，
显式 `--quit-after` 优先。工程应包含其引用的 `.wdschart` 与可解码音频。
原生窗口运行已验证 X11；Wayland 尚未验证，可通过 `QT_QPA_PLATFORM=xcb` 使用 XWayland。

也可用 CMake 安装到独立目录（Qt 6.8.3 的运行库部署已验证）：

```bash
"$cache_dir/tools/bin/cmake" --install "$cache_dir/build-linux" \
  --prefix "$HOME/.local/opt/wds-editor"
"$HOME/.local/opt/wds-editor/bin/wds_editor"
```

安装目录包含 `bin/`、`lib/`、Qt 插件及编辑器资源，可从其他工作目录启动；仍依赖宿主的图形/音频驱动。
上面的系统依赖也需安装。若用缓存 `deps/runtime/lib` 临时代替系统 `libxcb-cursor0`，
直接启动安装版时同样需设置相应的 `LD_LIBRARY_PATH`；`run-linux.sh` 会自动处理该开发环境兜底。
本入口不生成 AppImage、deb 或 rpm，也不改变 Windows/macOS 的发布流程。
脚本回归测试可单独运行：`python3 -B scripts/tests/linux_scripts_tests.py`，也会在找到 Python 3 的原生 Linux 构建中注册到 CTest。

### 构建模式速查

以下为 `scripts/build-target.sh` 的发行构建模式；Linux 开发入口的默认值见上一节。

| 模式              | 目录                    | 打包         | 日志           |
| ----------------- | ----------------------- | ------------ | -------------- |
| Release（默认）   | `build-<target>/`       | 是 → `dist/` | 关闭           |
| Debug (`--debug`) | `build-<target>-debug/` | 否           | 详细 `WDS_LOG` |

模块开关：`WDS_BUILD_COMMON` / `CORE` / `AUDIO` / `RENDERER` / `INTERACTION` / `UI`（默认均 ON）。
只关 `RENDERER` 会 FATAL（`INTERACTION` / `UI` 仍依赖它）。core-only 需同时 `-DWDS_BUILD_RENDERER=OFF -DWDS_BUILD_INTERACTION=OFF -DWDS_BUILD_UI=OFF`；只要 core、不要 audio 时再加 `-DWDS_BUILD_AUDIO=OFF`。
基准默认关、不进 CTest：`WDS_CORE_BUILD_BENCHMARKS`、`WDS_RENDERER_BUILD_BENCHMARKS`。

---

## 测试

当前产品门禁是 **macOS 原生 CTest**。CI `macos-arm` job 顺序为 `--no-package` → `ctest` → `--package-only`：

```bash
./scripts/build-target.sh macos-arm --no-package
ctest --test-dir build-macos-arm --output-on-failure
./scripts/build-target.sh --package-only macos-arm
```

默认全开模块时会注册单元测试（各模块 `*/tests/`）以及系统测试（`tests/system/`：分割线颜色/几何、分割线粒子播放、OpenWDS 绑定 / hydrate / 资源 / 控制器生命周期）。

**Linux 宿主交叉出的 Windows PE 不能在该宿主上运行。** 不要对 `build-win-x86_64` 跑 `ctest`（部分测试目标仍可能被编译，但宿主执行的是 PE）。交叉策略见 [`cmake/CROSS_COMPILE.md`](cmake/CROSS_COMPILE.md)。

Linux 上若只需跑 common + core 单测（**不是**产品构建），可用 [`scripts/run-host-core-tests.sh`](scripts/run-host-core-tests.sh)。这不能代替 macOS 门禁，也不等于 Windows 运行时验证。

Windows 本机跑测试尚未覆盖，本文不写未实测结论。

---

## 项目结构

```text
wds-editor/
├── common/                 # 时间轴、日志等跨层原语
├── core/                   # 谱面数据、编辑、序列化、预览快照
├── renderer/               # DrawBatch + Vulkan 呈现 / 纹理
├── chart-render/           # 舞台几何、皮肤图集、音符条带
├── audio-player/           # BASS 混音与 Transport 时钟
├── interaction-interface/  # 输入、控件、快捷键、主题
├── ui/                     # Qt Dock 壳层、QPainter 编辑器 + Vulkan 预览
├── cmake/                  # 平台默认值与交叉 toolchain
├── scripts/                # 构建 / 打包 / 图标
├── tests/system/           # 跨模块系统测试
├── resources/              # 分割线粒子运行时数据（objects.tsv / objects.jsonl），随包发布
└── skins/ effects/ icons/  # 运行时资源
```

### 依赖方向（下层不得反向依赖 `ui`）

```text
ui → interaction → draw → common
ui → chart-render → renderer → draw → common
ui → audio-player → common
ui → core → common
```

| 模块                                                        | CMake 目标                    | 命名空间                      | 说明                                |
| ----------------------------------------------------------- | ----------------------------- | ----------------------------- | ----------------------------------- |
| [`common/`](common/README.md)                               | `wds::common`                 | `wds::common`                 | `Timeline`、时间单位、`WDS_LOG`     |
| [`core/`](core/README.md)                                   | `wds::core`                   | `wds::chart_editor`           | 谱面内核（无 Vulkan / BASS / 窗口） |
| [`renderer/`](renderer/README.md)                           | `wds::draw` / `wds::renderer` | `wds::draw` / `wds::renderer` | CPU 绘制批 + Vulkan                 |
| [`chart-render/`](chart-render/README.md)                   | `wds::chart_render`           | `wds::renderer`               | 谱面视觉辅助（无设备创建）          |
| [`audio-player/`](audio-player/README.md)                   | `wds::audio_player`           | `wds::audio`                  | BASS + Transport                    |
| [`interaction-interface/`](interaction-interface/README.md) | `wds::interaction`            | `wds::interaction`            | UI 控件与输入                       |
| [`ui/`](ui/README.md)                                       | `wds::ui` + `wds_editor`      | `wds::ui`                     | Qt Dock 编辑器壳                    |

各模块 README 含职责、目录结构与主要接口。

### Include 约定

```cpp
#include <wds/common/timeline.hpp>
#include <wds/core/chart_editor_engine.hpp>
#include <wds/audio/audio.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/ui/regions/preview/chart_preview_panel.hpp>
#include <wds/renderer/draw_batch.hpp>
```

### 资源归属

| 资源       | 路径                                                    |
| ---------- | ------------------------------------------------------- |
| 皮肤 PNG   | `skins/`                                                |
| 打击音效   | `effects/`                                              |
| 分割线粒子 | `resources/`                                            |
| 着色器     | `renderer/shaders/`                                     |
| BASS       | `audio-player/third_party/bass/{macos-arm,win-x86_64}/` |
| 应用图标源 | `logo.png` → `scripts/generate-app-icons.sh`            |

Qt 应用图标通过 `ui/wds_resources.qrc` 内嵌；可替换主题、字体以及 Vulkan/BASS 需要真实文件路径的 `skins/`、`effects/`、`resources/` 由打包脚本放在发行目录中。详见 [`ui/README.md`](ui/README.md)。

**`resources/` 的查找方式**：程序按以下顺序寻找分割线粒子数据，可执行文件中不编入任何绝对路径。

- 发行包（Windows zip / MSI）：与 `wds_editor.exe` 同级的 `resources/`；
- macOS：`WDS Editor.app/Contents/Resources/resources`（可执行文件位于 `Contents/MacOS`）；
- 构建树：从可执行文件所在目录逐级向上搜索仓库的 `resources/`。

因此编译后的构建树（如 `build-macos-arm/ui/wds_editor`）无需打包即可直接运行。`package-target.sh` / `package-qt-release.ps1` 缺少 `resources/` 时会直接报错，不会产出缺少分割线粒子的包。

### 预览每帧数据流

```text
interaction action
  → audio Transport request_*
  → TimelineSnapshot = transport.poll(delta)
  → ChartEditorEngine::apply_timeline(snap)
  → ChartPreviewPanel / PlaybackPreviewView::render(snapshot)
  → DrawBatch → Vulkan present
```

预览区滚轮与编辑区共用同一套时间轴手势（scrub / Option+滚轮调可见范围），见 [`ui/README.md`](ui/README.md)。

开发诊断（默认关闭，不依赖 `WDS_ENABLE_LOGGING`）：环境变量 **精确** `WDS_FRAME_DIAG=1` 写帧耗时日志，见 [`ui/README.md`](ui/README.md)。异步上传 / descriptor 块链见 [`renderer/README.md`](renderer/README.md)；音频恢复退避与 pending SFX sync（10s / 4096）见 [`audio-player/README.md`](audio-player/README.md)。
