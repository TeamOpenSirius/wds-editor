# interaction-interface — `wds_interaction` / `wds::interaction`

输入事件、控件树、快捷键命名空间、Material Design 深色主题。  
绘制依赖 `wds::draw`（`DrawBatch`），**不**创建 Vulkan 设备、不做谱面算法。

## 职责

- 指针 / 键盘 / 文本 / 修饰键事件队列
- 绝对布局控件树（命中测试、焦点、捕获）
- 快捷键：namespace → chord → action
- UI 文本：`FontAtlas` + `UiPainter`
- GLFW 适配（可选目标 `wds_interaction_glfw`）

## 结构

```text
interaction-interface/
├── CMakeLists.txt
├── include/wds/interaction/
│   ├── events.hpp / types.hpp / platform.hpp / gesture.hpp
│   ├── widget.hpp / widget_root.hpp
│   ├── shortcuts.hpp / theme.hpp / ui_painter.hpp / font_atlas.hpp
│   ├── glfw_input_adapter.hpp
│   └── widgets/
│       ├── button.hpp / icon_button.hpp / checkbox.hpp
│       ├── text_field.hpp / slider.hpp / stepper.hpp
│       ├── dropdown.hpp / combo_box.hpp / modal.hpp
├── src/
├── tests/
└── third_party/          # 字体等
```

| CMake 目标 | 角色 |
|------------|------|
| `wds_interaction` | 控件与绘制；PUBLIC 链 `wds::draw` |
| `wds_interaction_glfw` | `GlfwInputAdapter`（ui 链接） |

## 主要接口

### 事件与适配

```cpp
#include <wds/interaction/glfw_input_adapter.hpp>

wds::interaction::GlfwInputAdapter adapter(window);
// 每帧取出 events → WidgetRoot::process_frame
```

事件类型见 `events.hpp`（Pointer / Click / Scroll / Key / Text / Modifiers）。

### `WidgetRoot`

```cpp
root.process_frame(delta_seconds, events, &shortcuts);
root.paint(painter);
root.set_focus(widget);
Widget* hit = root.widget_at(point);
```

### `ShortcutManager`

按 namespace 绑定快捷操作（如 `preview`、`edit`）：

```cpp
shortcuts.bind("preview", chord, "toggle_play");
```

平台主键：Windows/Linux = Ctrl，macOS = Cmd（见 `platform.hpp`）。

### 控件

`Button`、`IconButton`、`TextField`、`Slider`、`Dropdown`、`ComboBox`、`Stepper`、`Checkbox`、`Modal` 等，均继承 `Widget`，经 `UiPainter` 画到 `DrawBatch`。

### `Theme`

MD 深色 token（颜色、圆角、字号）；面板应读 theme 而非硬编码色值。

### 滚轮偏好（进程内）

`invert_scroll_wheel` / `invert_visible_range_scroll` / `scroll_wheel_speed` 见 `<wds/interaction/editor_input.hpp>`。适配器在入队前按「反转时间轴滚轮」取反 delta。速度夹在 0.25–3，NaN / Inf 回 1。宽度槽每位 1–12。手势语义在 [`ui/README.md`](../ui/README.md)。

## 依赖

- `wds::common`
- `wds::draw`（无 Vulkan）

## 测试

交叉编译时不编 `wds_interaction_tests`。macOS 门禁见根 README。不要对 `build-win-x86_64` 跑 `ctest`。

```bash
ctest --test-dir build-macos-arm -R wds_interaction_tests --output-on-failure
```

覆盖：快捷键 namespace、按钮、Slider、滚轮速度夹取、手势与平台修饰键。
