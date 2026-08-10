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

仓库包含编译、打包与运行所需内容：源码、CMake/脚本、BASS、Vulkan 头、UI 字体/图标，以及运行时 `skins/`、`effects/` 等。  
本地路径请放在 `scripts/env.local`（已忽略），勿提交。

**已验证的主机 / 目标组合：**

| 构建主机            | 目标                           |
| ------------------- | ------------------------------ |
| Linux x86_64        | `win-x86_64`（MinGW 交叉编译） |
| macOS Apple Silicon | `macos-arm`（本机）            |

Windows本地编译尚未覆盖，需要自行探索。

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
   `libpng` `zlib` `glfw3` `vulkan-loader`
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
brew install cmake libpng glfw glslang molten-vk vulkan-headers vulkan-loader
```

**编译与打包**

```bash
./scripts/build-target.sh macos-arm              # Release → zip + dmg
./scripts/build-target.sh macos-arm --debug      # Debug，自动启动编辑器
./scripts/build-target.sh macos-arm --debug --no-run
```

产物：`dist/wds-macos-arm.zip`、`dist/wds-macos-arm.dmg`。

### C. Windows 本机编译（自行探索）

当前脚本与文档以 **Linux 交叉 MinGW** 为主路径。若在 Windows 上原生构建，需自行准备：

- Visual Studio 或 MinGW-w64、CMake
- vcpkg（如 `x64-windows` 或 `x64-mingw-static`）提供 GLFW / libpng / Vulkan
- 将 `CMAKE_PREFIX_PATH`、`Vulkan_LIBRARY` 指向本机前缀

欢迎把可复现步骤以 PR 形式补回文档。

### 构建模式速查

| 模式              | 目录                    | 打包         | 日志           |
| ----------------- | ----------------------- | ------------ | -------------- |
| Release（默认）   | `build-<target>/`       | 是 → `dist/` | 关闭           |
| Debug (`--debug`) | `build-<target>-debug/` | 否           | 详细 `WDS_LOG` |

模块开关：`WDS_BUILD_COMMON` / `CORE` / `AUDIO` / `RENDERER` / `INTERACTION` / `UI`（默认均 ON）。

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
├── ui/                     # 编辑器壳层 + wds_editor 可执行文件
├── cmake/                  # 平台默认值与交叉 toolchain
├── scripts/                # 构建 / 打包 / 图标
├── skins/ effects/ icons/  # 运行时资源
└── docs/                   # 官方 UI 对齐、编辑/预览一致性、复刻路线等
```

### 依赖方向（下层不得反向依赖 `ui`）

```text
ui → interaction (+ interaction_glfw) → draw → common
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
| [`ui/`](ui/README.md)                                       | `wds::ui` + `wds_editor`      | `wds::ui`                     | 四区编辑器壳                        |

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
| 着色器     | `renderer/shaders/`                                     |
| BASS       | `audio-player/third_party/bass/{macos-arm,win-x86_64}/` |
| 应用图标源 | `logo.png` → `scripts/generate-app-icons.sh`            |

### 预览每帧数据流

```text
interaction action
  → audio Transport request_*
  → TimelineSnapshot = transport.poll(delta)
  → ChartEditorEngine::apply_timeline(snap)
  → ChartPreviewPanel / PlaybackPreviewView::render(snapshot)
  → DrawBatch → Vulkan present
```
