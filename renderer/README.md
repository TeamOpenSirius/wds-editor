# renderer — `wds_draw` + `wds_renderer`

GPU 呈现与 CPU 侧绘制批。谱面舞台 / 皮肤逻辑在 [`chart-render/`](../chart-render/README.md)，本模块不创建窗口、不依赖 GLFW。

## 职责

| 目标 | 角色 |
|------|------|
| `wds_draw` / `wds::draw` | Header-oriented：`DrawBatch`、`draw_types`、`TextureInfo`（无 Vulkan 链接） |
| `wds_renderer` / `wds::renderer` | `VulkanRenderer`、`TextureCache`、PNG/SVG 上传与 present |

## 结构

```text
renderer/
├── CMakeLists.txt
├── include/wds/renderer/
│   ├── draw_batch.hpp / draw_types.hpp
│   ├── vulkan_renderer.hpp   # VulkanHostSurface + VulkanRenderer
│   ├── texture.hpp
│   └── …（部分类型转发到 chart-render）
├── src/
├── shaders/                  # 编译进包内的 GLSL/SPIR-V 资源
└── third_party/              # Vulkan headers、libpng 辅助等
```

## 主要接口

### `VulkanHostSurface`（由 ui 注入）

```cpp
struct VulkanHostSurface {
  std::vector<const char*> instance_extensions;           // 如 GLFW WSI
  std::function<VkSurfaceKHR(VkInstance)> create_surface;
  std::function<void(int*, int*)> framebuffer_size;
  // Win32: optional win32_monitor for fullscreen exclusive
};
```

Renderer **不**持有 `GLFWwindow`，只通过上述回调拿 surface 与尺寸。

### `VulkanRenderer`

- 创建 instance / device / swapchain / pipeline  
- 每帧接收 `DrawBatch`，present（倾向 FIFO vsync，`minImageCount` ≥ 3）  
- 依赖目标平台的 Vulkan loader / import lib（交叉 Win 时由 `WDS_VULKAN_LIBRARY` 指定）

### `DrawBatch`（`<wds/renderer/draw_batch.hpp>`）

按纹理分桶的 textured quad 列表；`interaction` / `ui` / `chart-render` 均可填充，最终交给 `VulkanRenderer`。

### `TextureCache`

从 PNG/SVG 路径上传并缓存 `VkImage`；chart-render 的 `SkinCatalog` 使用它。

## 依赖

- `wds::common`
- 目标平台：Vulkan loader、libpng；着色器工具链（glslangValidator）用于构建期
- **不**链接 GLFW（WSI 在 ui）

## 构建

```bash
./scripts/build-target.sh macos-arm --no-package
# 或 Linux→Win：配置好 scripts/env.local 后
./scripts/build-target.sh win-x86_64
```

关闭本层：`-DWDS_BUILD_RENDERER=OFF`（同时不会编 `chart-render` / 依赖它的上层）。
