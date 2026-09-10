# ui — `wds_ui` + `wds_editor`

编辑器壳层：Qt 窗口与 Dock 工作区、`UiManager`、工程会话与原生文件对话框。
组装 `core` / `audio` / `interaction` / `chart-render` / `renderer`，**不要**把谱面算法或 Vulkan 设备细节复制进 panel。

## 职责

- Qt Widgets 菜单、工具栏、属性/播放/音频/编辑工具箱 Dock
- `QWindow`/`QVulkanInstance` 实时预览 + Vulkan 表面注入（`VulkanHostSurface`）
- `QPainter` 谱面编辑画布（编辑交互仍由 `ChartEditPanel` 提供）
- `.wdsproject` 打开保存、官方 CSV 只读导入、撤销重做
- 资源路径解析（`skins/`、`effects/`、shaders、fonts、icons）

## 结构

```text
ui/
├── CMakeLists.txt
├── apps/                   # wds_editor / Uninstall 等入口
├── include/wds/ui/
│   ├── ui_manager.hpp / qt/
│   ├── editor_session.hpp / editor_ui_config.hpp
│   ├── resource_paths.hpp / startup_deps.hpp
│   ├── native_file_dialog.hpp
│   ├── layout/editor_layout.hpp
│   └── regions/
│       ├── preview/        # ChartPreviewPanel, PlaybackPreview
│       ├── settings/       # PreviewSettingsPanel, 对话框
│       ├── toolbar/        # EditorToolbar
│       └── edit/           # ChartEditPanel, viewport, gutters
├── src/
├── assets/                 # 字体、应用图标、OBS/Yami 主题资源
├── wds_resources.qrc       # 必须内嵌的 Qt 资源（当前为应用图标）
└── tests/                  # UI 逻辑与 Qt shell 回归测试
```

| CMake 目标 | 角色 |
|------------|------|
| `wds_ui` | 静态库：regions / session / dialogs |
| `wds_editor` | 可执行文件 |
| `wds_ui_logic_tests` | 视口 / 会话逻辑 |
| `wds_qt_shell_tests` | Qt 主题、Dock 尺寸和 resize suspension |

## Qt Dock 布局

```text
┌─────────────────┬─────────────────┬──────────┐
│ Vulkan Preview  │ QPainter Edit   │   属性   │
│                 │                 │          │
├─────────────────┴─────────────────┴──────────┤
│         播放       │  音频  │   编辑工具箱   │
└────────────────────┴────────┴────────────────┘
```

- 顶行 Preview/Edit 是唯一的纵向弹性区域，自动吸收主窗口高度变化。
- 播放、音频、编辑工具箱停靠在底部时默认约 200px 高；窗口整体 resize 不会自动改写该高度，但用户仍可拖动分隔线手动调整。横向宽度由 Qt 按当前比例自适应，移到侧边或浮动后恢复自由尺寸。
- 所有 Dock 可移动、浮动、关闭；“视图 → 重置布局”恢复默认比例。
- Preview 内容保持舞台宽高比，Dock 本身可自由缩放；编辑画布自适应可用区域。
- `ChartEditPanel` 保留 tick×lane 编辑算法，`ChartEditWidget` 负责 Qt 绘制和事件桥接。

### 谱面画布命中与层级

- Hold ribbon 先统一绘制，Tap、Hold cap、星标等可选 note sprite 始终在其上层。
- 第一次点击用于选中；只有已选 note 才显示并响应宽度/时间 resize handle，避免窄 note 的热区吞掉选择。
- ScratchHold 共享关节按时间方向拆分命中：线下半选择前段尾，线上半选择后段 tap 头，两者可独立调整。
- 按住 Alt 点击拼接的 ScratchHold，可直接选择当前段（及其配对头），不进入整条链编辑。
- 宽度/时间 resize handle 最多占 note 每侧 20%，窄 note 与极短 hold 始终保留中央选择/移动区。

### 工程加载

`.wdsproject` 和其引用谱面的文件读取、反序列化在 Qt worker 线程完成；只有把完整结果提交到 `EditorSession`、切换 Transport/Vulkan 状态的短步骤留在 GUI 线程。整曲波形解码与双声道 FFT 也在独立后台任务中执行，主线程只接收最新一代结果并上传频谱纹理，因此启动页和主窗口在加载期间都能继续刷新。

### 预览 / 编辑滚轮

预览区命中控件把滚轮转给编辑区同一套时间轴逻辑（与 BPM 无关）：

| 手势 | 行为 |
|------|------|
| 滚轮 | scrub 播放头。可见范围 20 hectom、速度 1× 时约 100ms/格；步长随可见范围与「时间轴滚轮速度」正比 |
| Option+滚轮（Windows/Linux 为 Alt+滚轮） | 调可见范围（1–1000 hectom）。默认上滚缩小窗口 |

设置（「输入」页，写入 OS 配置目录的 `config.yml`）：

- **反转时间轴滚轮方向**：只翻 scrub；适配器先按此项取反 delta
- **反转滚轮调节可见范围大小方向**：只翻 Option+滚轮。两项各自生效、互不抵消
- **时间轴滚轮速度**：0.25–3；非有限值回 1。只影响 scrub，不影响可见范围

### 帧诊断

仅当环境变量 **精确** 为 `WDS_FRAME_DIAG=1` 时开启（`1` 以外、大小写变体均关），与编译期 `WDS_ENABLE_LOGGING` 无关。默认不打诊断时钟、不建日志文件。

macOS 已验证路径：`~/Library/Application Support/WDS/logs/frame-diag.log`。约每秒一行：fps、playing 帧占比、wall / hitch（原始帧间隔 >25ms）、poll / update / tick / batch / render，以及 fence / acquire / submit / present。打开失败不致命。不要把 Debug 控制台当诊断输出（避免 I/O 伪卡顿）。

```bash
WDS_FRAME_DIAG=1 ./build-macos-arm-debug/ui/wds_editor
```

## 主要接口

### `UiManager`

```cpp
#include <wds/ui/ui_manager.hpp>

UiManager ui;
ui.chart_preview();
ui.root();          // WidgetRoot
ui.shortcuts();
ui.session();
```

每帧：input → `transport.poll` → `session/engine.apply_timeline` → regions paint → present。
墙钟 delta 夹在 0–80ms，低于 Transport 的 100ms hard snap，避免单帧 hitch 触发硬对齐。

### `EditorSession`

- 新建 / 打开 / 保存 `.wdsproject`
- 官方 CSV 只读导入
- dirty 提示与未保存对话框

### `resource_paths` 与 Qt Resources

应用图标属于不可替换的 Qt chrome，编入 `wds_resources.qrc`。其余资源有意保留为发行包内的目录：

- `skins/`、`effects/` 由 Vulkan/BASS 原生路径 API 使用，不能直接换成 `:/` URL；
- `theme/` 与字体保留外置，便于主题枚举、用户替换及许可证随包分发；
- CMake/打包脚本负责把这些目录放到可执行文件旁（macOS 为 App Resources），业务代码统一通过 `resource_paths` 解析。

这样安装目录仍是自包含的，但不会把可定制资源和原生加载资源硬塞进可执行文件。交叉编译的 Win Debug 需经 `package-target.sh --stage-win-debug` 拷贝资源。

## 快捷键（节选）

| 和弦 | 动作 |
|------|------|
| Space | 播放 / 暂停 |
| Primary+O / S | 打开 / 保存 |
| Primary+Z / Y | 撤销 / 重做 |
| Primary+C / V | 复制 / 粘贴 |
| Arrows | 微移选区 |
| Q/W/E/A/S/D | 默认宽度 1/2/3/4/6/12 |

Primary = Ctrl（Windows）/ Cmd（macOS）。

## 运行

```bash
./scripts/build-target.sh macos-arm --debug --no-run
./build-macos-arm-debug/ui/wds_editor
./build-macos-arm-debug/ui/wds_editor path/to/project.wdsproject
```

Windows 产物见仓库根 README 的安装与交叉编译章节。交叉构建仍可能编译 `wds_ui_logic_tests` / `wds_note_draw_order_tests`，但 **不** 向 CTest 注册。macOS：

```bash
ctest --test-dir build-macos-arm -R 'wds_ui_logic_tests|wds_note_draw_order_tests' --output-on-failure
```

## 依赖

`wds::interaction`、`wds::audio_player`、`wds::core`、`wds::chart_render`、`wds::renderer`。
