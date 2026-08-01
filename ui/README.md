# ui — `wds_ui` + `wds_editor`

编辑器壳层：窗口、四区排版、`UiManager`、工程会话与原生文件对话框。  
组装 `core` / `audio` / `interaction` / `chart-render` / `renderer`，**不要**把谱面算法或 Vulkan 设备细节复制进 panel。

## 职责

- GLFW 窗口 + Vulkan 表面注入（`VulkanHostSurface`）
- 工具栏 / 预览 / 预览设置 / 编辑区布局
- `.wdsproject` 打开保存、官方 CSV 只读导入、撤销重做
- 资源路径解析（`skins/`、`effects/`、shaders、fonts、icons）

## 结构

```text
ui/
├── CMakeLists.txt
├── apps/                   # wds_editor / Uninstall 等入口
├── include/wds/ui/
│   ├── ui_manager.hpp / window.hpp
│   ├── editor_session.hpp / editor_ui_config.hpp
│   ├── resource_paths.hpp / startup_deps.hpp
│   ├── native_file_dialog.hpp / macos_*.hpp
│   ├── layout/editor_layout.hpp
│   └── regions/
│       ├── preview/        # ChartPreviewPanel, PlaybackPreview
│       ├── settings/       # PreviewSettingsPanel, 对话框
│       ├── toolbar/        # EditorToolbar
│       └── edit/           # ChartEditPanel, viewport, gutters
├── src/
├── assets/                 # 字体、应用图标
└── tests/                  # wds_ui_logic_tests（无 GPU）
```

| CMake 目标 | 角色 |
|------------|------|
| `wds_ui` | 静态库：regions / session / dialogs |
| `wds_editor` | 可执行文件 |
| `wds_ui_logic_tests` | 视口 / 会话逻辑 |

## 布局

```text
┌──────────────────────┬──────────────────────┐
│     Preview          │                      │
│                      │                      │
├──────────────────────┤        Edit          │
│     Settings         │                      │
├──────────┬───────────┤                      │
│ Toolbar  │ Convert   │                      │
└──────────┴───────────┴──────────────────────┘
```

- `EditorLayouter`：计算四区矩形与舞台 content bounds  
- `ChartPreviewPanel` / `PlaybackPreviewView`：Vulkan 预览  
- `PreviewSettingsPanel`：速度、进度、音量  
- `EditorToolbar`：打开/保存/导入导出/音乐/撤销/网格/转换  
- `ChartEditPanel`：铺平 tick×lane 编辑  
- `EditorSession`：工程生命周期  

窗口固定 16:9。

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

### `EditorSession`

- 新建 / 打开 / 保存 `.wdsproject`
- 官方 CSV 只读导入
- dirty 提示与未保存对话框

### `resource_paths`

发行包与 Debug 树通过 `WDS_REPO_ROOT` / 可执行文件旁资源定位 `skins/`、`effects/` 等。交叉编译的 Win Debug 需经 `package-target.sh --stage-win-debug` 拷贝资源。

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

Windows 产物见仓库根 README 的安装与交叉编译章节。

## 依赖

`wds::interaction`（+ glfw）、`wds::audio_player`、`wds::core`、`wds::chart_render`、`wds::renderer`。
