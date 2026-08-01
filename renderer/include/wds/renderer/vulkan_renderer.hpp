#pragma once

#include "draw_batch.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <windows.h>
#endif

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wds::renderer {

// Window-system binding supplied by the ui layer (GLFW today). Renderer never
// touches GLFWwindow — only the Vulkan surface and framebuffer size queries.
struct VulkanHostSurface {
  // Platform WSI instance extensions (e.g. glfwGetRequiredInstanceExtensions).
  // Renderer appends VK_KHR_portability_enumeration on its own for MoltenVK.
  std::vector<const char*> instance_extensions;

  // Create a VkSurfaceKHR for the given instance. Renderer owns and destroys it.
  // Return VK_NULL_HANDLE on failure.
  std::function<VkSurfaceKHR(VkInstance)> create_surface;

  // Current framebuffer size in pixels (swapchain extent source).
  std::function<void(int* width, int* height)> framebuffer_size;

#if defined(_WIN32)
  // Optional: HMONITOR for VK_EXT_full_screen_exclusive Win32 info. Used when
  // recreating the swapchain with APPLICATION_CONTROLLED exclusive mode.
  std::function<HMONITOR()> win32_monitor;
#endif
};

// Owns Vulkan instance/device/swapchain/pipeline and presents DrawBatch frames.
class VulkanRenderer {
 public:
  VulkanRenderer();
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  bool create(const VulkanHostSurface& host);
  void destroy();

  // Block until the GPU is idle. Call before freeing textures / tearing down outside
  // destroy() (e.g. toolbar icons) so in-flight frames cannot touch freed images.
  void device_wait_idle();

  bool ready() const noexcept { return ready_; }

  // Preferred MSAA sample count (1 / 2 / 4). App default is 2× across platforms.
  void set_preferred_msaa(int samples) noexcept;
  bool apply_msaa(int samples);
  int preferred_msaa() const noexcept { return preferred_msaa_; }
  int active_msaa() const noexcept;

  bool resize(int width, int height);

  // Win exclusive-fullscreen path: when true, recreate swapchain with
  // VK_EXT_full_screen_exclusive (APPLICATION_CONTROLLED) and acquire.
  // No-op on non-Win or when the extension is unavailable.
  void set_exclusive_fullscreen_desired(bool desired);
  bool exclusive_fullscreen_desired() const noexcept;
  // True when the device extension was enabled at create time.
  bool exclusive_fullscreen_extension() const noexcept;
  // True after a successful vkAcquireFullScreenExclusiveModeEXT (DWM bypass intent).
  bool exclusive_fullscreen_acquired() const noexcept;
  // Release FSE before leaving monitor / destroying the swapchain (close path).
  void release_fullscreen_exclusive();

  // Last draw_frame WSI timing (microseconds). Diagnostic — under FIFO the
  // in-flight fence wait typically carries the vsync pacing; acquire is often ~0.
  int64_t last_fence_wait_us() const noexcept;
  int64_t last_acquire_wait_us() const noexcept;
  int64_t last_present_us() const noexcept;
  int64_t last_gpu_submit_us() const noexcept;

  // Upload a standalone RGBA8 texture (full UV 0..1). Also used by atlas bake.
  TextureInfo create_texture_rgba(const unsigned char* pixels, int width, int height);
  void destroy_texture(TextureId id);

  // `batch` uses standard alpha blending; optional `additive` is drawn after with
  // SRC_ALPHA / ONE (hit particles, glows). Optional post overlays are drawn last
  // (above additive) — used for modal dialogs. `post_overlay2` draws after
  // `post_overlay` (e.g. modal footer above list sprites).
  // NOTE: DrawBatch::clear() keeps sticky bucket indices, so merging late into
  // `batch` does NOT guarantee later draw order across textures.
  bool draw_frame(const DrawBatch& batch, const ScreenBounds& screen, float clear_r = 0.05f,
                  float clear_g = 0.05f, float clear_b = 0.08f,
                  const DrawBatch* additive = nullptr,
                  const DrawBatch* post_overlay = nullptr,
                  const DrawBatch* post_overlay2 = nullptr);

  int framebuffer_width() const noexcept { return width_; }
  int framebuffer_height() const noexcept { return height_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool ready_ = false;
  int width_ = 1;
  int height_ = 1;
  int preferred_msaa_ = 1;
};

}  // namespace wds::renderer
