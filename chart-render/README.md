# chart-render — `wds_chart_render` / `wds::chart_render`

Sirius / WDS 谱面视觉辅助：舞台梯形、车道、皮肤图集、音符条带。  
类型仍落在命名空间 `wds::renderer`（历史兼容）；**不**创建 Vulkan 设备、不做谱面编辑算法。

## 职责

- 将 framebuffer / 内容矩形映射为舞台与判定线几何
- 解析 `skins/` 目录中的 PNG 图集映射
- 为预览 / 编辑画布提供统一的 note strip、split-line 色板与视觉常量

## 结构

```text
chart-render/
├── CMakeLists.txt
├── include/wds/chart_render/
│   ├── stage_geometry.hpp
│   ├── preview_visual_config.hpp
│   ├── skin_catalog.hpp
│   ├── split_line_skins.hpp
│   └── note_strips.hpp
└── src/
```

CMake：`wds_chart_render`（别名 `wds::chart_render`）。  
依赖：`wds::draw`、`wds::renderer`（`TextureCache`）。

## 主要接口

### `StageGeometry`

```cpp
StageGeometry stage;
stage.configure(config);
stage.resize(fb_w, fb_h);
stage.set_content_rect(x, y, w, h);   // UI 预览区内嵌舞台
Vec2 p = stage.lane_position(lane, percent);  // lane 0-based
const JudgelineQuad& jl = stage.judgeline();
```

| API | 说明 |
|-----|------|
| `configure` / `resize` | 视觉常量与 framebuffer |
| `set_content_rect` / `clear_content_rect` | 限制舞台到面板矩形 |
| `lane_position` | 车道 × 纵向 percent → 屏幕/NDC 坐标 |
| `screen` / `content` / `stage` | 各层 bounds |

### `PreviewVisualConfig`

音符速度、舞台常量、资源目录等；供 preview / edit 共用。

### `SkinCatalog`

扫描并映射仓库根（或打包根）下 `skins/` PNG，配合 `TextureCache` 上传。

### `note_strips` / `split_line_skins`

左/中/右音符条带绘制辅助；分割线色来自官方 `LineColor` 表（`official_split_line_color`），不是 PNG 采样。

## 与 renderer 的边界

| 放本模块 | 放 renderer |
|----------|-------------|
| 谱面舞台几何、皮肤语义 | Vulkan 设备、swapchain、通用 DrawBatch |
| note strip 布局 | Texture 上传底层 |

## 构建与测试

随 `WDS_BUILD_RENDERER=ON` 一起由根 `CMakeLists.txt` 加入；见仓库根 README。交叉时仍编译 `wds_note_visual_policy_tests` / `wds_split_line_official_colors_tests`，但 **不** 向 CTest 注册。macOS：

```bash
ctest --test-dir build-macos-arm -R 'wds_note_visual_policy_tests|wds_split_line_official_colors_tests' --output-on-failure
```
