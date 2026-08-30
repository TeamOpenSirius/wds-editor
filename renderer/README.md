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

### 异步上传与 descriptor 块链

`create_texture_rgba` 在 queue submit 成功后立刻发布 `TextureInfo`；staging / command / fence 用非阻塞 `vkGetFenceStatus` 回收。失败时 Debug 的 `WDS_LOG` 带 `upload_stage_name`（buffer / map / descriptor / wait 等）与 `VkResult`。`DEVICE_LOST` 与 wait 失败会抬升 renderer health；其余上传错误不改 health。

Descriptor 池按块增长：**256 → 512 → 之后每块 1024**。池满或碎片化时试下一块。`descriptor_pool_diagnostics()` 给出 `block_count` / `live_sets`（path bench 的 300 纹理压测会用到）。

路径分段计时默认关：`set_path_diagnostics_enabled(true)` 才打额外时钟。编辑器帧诊断走 `WDS_FRAME_DIAG=1`，见 [`ui/README.md`](../ui/README.md)。

## 依赖

- `wds::common`
- 目标平台：Vulkan loader、libpng；着色器工具链（glslangValidator）用于构建期
- **不**链接 GLFW（WSI 在 ui）

## 构建与测试

```bash
./scripts/build-target.sh macos-arm --no-package
# 或 Linux→Win：配置好 scripts/env.local 后
./scripts/build-target.sh win-x86_64
```

关闭本层须同时关掉上层：`-DWDS_BUILD_RENDERER=OFF -DWDS_BUILD_INTERACTION=OFF -DWDS_BUILD_UI=OFF`（只关 `RENDERER` 会 FATAL，因为 `INTERACTION` / `UI` 仍依赖它）。关 renderer 后不会编 `chart-render`。只要 core、不要 audio 时再加 `-DWDS_BUILD_AUDIO=OFF`。

`wds_renderer_tests`（逻辑 + TextureCache）交叉时仍可能编译，但 **不** 向 CTest 注册。macOS：

```bash
ctest --test-dir build-macos-arm -R wds_renderer_tests --output-on-failure
```

## 路径基准（默认关）

`WDS_RENDERER_BUILD_BENCHMARKS=ON` 且找到 GLFW 时编 `wds_renderer_path_bench`。隐藏窗口、无手动点击；面向宿主 Vulkan（macOS 上会优先探测 Homebrew MoltenVK ICD）。**不**进 CTest，也不是 CI 门禁。

无 GLFW / 无 Vulkan / `create` 失败时打印 `UNMEASURED` 并以 0 退出。测得到时打印 font 2048²、skin 4096² 上传（含 async submit / fence wait / reap / descriptor）、swapchain resize、MSAA，以及 descriptor 300 创建 + 100 回收。主线程 upload 或 wait 的 P95 **信息门**为 8ms（超出只打印 `EXCEEDS`，不当作产品 CTest）。首帧以及 atlas/图标 rebake 后的下一帧可能出现 `upload_fence_wait`（`draw_frame` 在采样前 drain pending）；稳态 pending 为空时该 wait 为 no-op。

```bash
./scripts/build-target.sh macos-arm --no-package -- -DWDS_RENDERER_BUILD_BENCHMARKS=ON
./build-macos-arm/renderer/wds_renderer_path_bench
```

本文不写未在 Windows 本机测过的路径数字。
