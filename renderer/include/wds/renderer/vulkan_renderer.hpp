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

enum class RendererHealth : uint8_t {
  Uninitialized = 0,
  Ready,
  Occluded,
  SurfaceLost,
  DeviceLost,
  Fatal,
};

enum class RendererHealthEvent : uint8_t {
  Created = 0,
  Destroyed,
  OccludedZeroSize,
  RecoveredExtent,
  SurfaceLost,
  SurfaceRecovered,
  DeviceLost,
  Fatal,
};

enum class WsiRecoverAction : uint8_t {
  None = 0,
  RecreateSwapchain,
  RecreateSurfaceAndSwapchain,
  DeviceLost,
  Fatal,
};

inline constexpr bool renderer_health_ready(RendererHealth health) noexcept {
  switch (health) {
    case RendererHealth::Ready:
    case RendererHealth::Occluded:
    case RendererHealth::SurfaceLost:
      return true;
    case RendererHealth::Uninitialized:
    case RendererHealth::DeviceLost:
    case RendererHealth::Fatal:
      return false;
  }
  return false;
}

inline constexpr bool renderer_health_unrecoverable(RendererHealth health) noexcept {
  return health == RendererHealth::DeviceLost || health == RendererHealth::Fatal;
}

inline constexpr RendererHealth apply_renderer_health(RendererHealth current,
                                                      RendererHealthEvent event) noexcept {
  if (event == RendererHealthEvent::Destroyed) {
    return RendererHealth::Uninitialized;
  }
  if (renderer_health_unrecoverable(current)) {
    return current;
  }
  switch (event) {
    case RendererHealthEvent::Created:
      return RendererHealth::Ready;
    case RendererHealthEvent::OccludedZeroSize:
      return current == RendererHealth::SurfaceLost ? RendererHealth::SurfaceLost
                                                    : RendererHealth::Occluded;
    case RendererHealthEvent::RecoveredExtent:
      return current == RendererHealth::SurfaceLost ? RendererHealth::SurfaceLost
                                                    : RendererHealth::Ready;
    case RendererHealthEvent::SurfaceLost:
      return RendererHealth::SurfaceLost;
    case RendererHealthEvent::SurfaceRecovered:
      return RendererHealth::Ready;
    case RendererHealthEvent::DeviceLost:
      return RendererHealth::DeviceLost;
    case RendererHealthEvent::Fatal:
      return RendererHealth::Fatal;
    case RendererHealthEvent::Destroyed:
      break;
  }
  return current;
}

inline constexpr RendererHealth apply_zero_extent(RendererHealth current,
                                                 bool live_swapchain) noexcept {
  if (!live_swapchain) {
    return apply_renderer_health(current, RendererHealthEvent::SurfaceLost);
  }
  return apply_renderer_health(current, RendererHealthEvent::OccludedZeroSize);
}

inline constexpr bool renderer_health_occluded_valid(RendererHealth health,
                                                    bool live_swapchain) noexcept {
  return health != RendererHealth::Occluded || live_swapchain;
}

inline constexpr RendererHealthEvent apply_msaa_failure_event(RendererHealth current) noexcept {
  return current == RendererHealth::SurfaceLost ? RendererHealthEvent::SurfaceLost
                                                : RendererHealthEvent::Fatal;
}

// apply_msaa destroys RP before rebuild. recover_* only recreate the swapchain,
// so a post-teardown failure cannot be treated as SurfaceLost/Ready.
inline constexpr RendererHealthEvent apply_msaa_rebuild_failure_event(
    WsiRecoverAction action) noexcept {
  return action == WsiRecoverAction::DeviceLost ? RendererHealthEvent::DeviceLost
                                                : RendererHealthEvent::Fatal;
}

inline constexpr RendererHealth apply_msaa_rebuild_failure_health(
    RendererHealth current, WsiRecoverAction action) noexcept {
  return apply_renderer_health(current, apply_msaa_rebuild_failure_event(action));
}

inline constexpr bool apply_msaa_may_teardown(int framebuffer_w, int framebuffer_h) noexcept {
  return framebuffer_w > 0 && framebuffer_h > 0;
}

inline constexpr bool apply_msaa_zero_extent_ok() noexcept {
  return false;
}

inline constexpr RendererHealth apply_swapchain_create_failure_health(
    RendererHealth current, WsiRecoverAction action, bool live_swapchain) noexcept {
  switch (action) {
    case WsiRecoverAction::RecreateSurfaceAndSwapchain:
      return apply_renderer_health(current, RendererHealthEvent::SurfaceLost);
    case WsiRecoverAction::DeviceLost:
      return apply_renderer_health(current, RendererHealthEvent::DeviceLost);
    case WsiRecoverAction::Fatal:
      return apply_renderer_health(current, RendererHealthEvent::Fatal);
    case WsiRecoverAction::RecreateSwapchain:
      if (renderer_health_unrecoverable(current)) {
        return current;
      }
      if (current == RendererHealth::SurfaceLost) {
        return RendererHealth::SurfaceLost;
      }
      if (!live_swapchain) {
        return RendererHealth::Ready;
      }
      return current;
    case WsiRecoverAction::None:
      if (!live_swapchain) {
        return apply_zero_extent(current, false);
      }
      return current;
  }
  return current;
}

inline constexpr bool swapchain_create_failure_reports_occluded(WsiRecoverAction action,
                                                               bool live_swapchain) noexcept {
  (void)action;
  (void)live_swapchain;
  return false;
}

inline constexpr bool next_frame_recovers_swapchain(RendererHealth health,
                                                    bool live_swapchain) noexcept {
  return renderer_health_ready(health) && health != RendererHealth::SurfaceLost && !live_swapchain;
}

inline constexpr bool draw_frame_reaps_completed_uploads_before_zero_extent_return() noexcept {
  return true;
}

// Host drain before the graphics submit that samples newly published textures.
// create_texture_rgba stays asynchronous (no per-texture wait).
inline constexpr bool draw_frame_waits_pending_uploads_before_sample() noexcept {
  return true;
}

inline constexpr bool create_texture_rgba_waits_upload_fence() noexcept {
  return false;
}

inline constexpr bool wait_pending_uploads_is_noop(bool has_pending) noexcept {
  return !has_pending;
}

inline constexpr bool draw_frame_zero_extent_reap_applies_health() noexcept {
  return false;
}

inline constexpr bool draw_frame_blocks_before_recovery(RendererHealth health, bool ready,
                                                       bool live_swapchain) noexcept {
  if (renderer_health_unrecoverable(health) || health == RendererHealth::Uninitialized) {
    return true;
  }
  if (health == RendererHealth::SurfaceLost) {
    return false;
  }
  if (ready && !live_swapchain) {
    return false;
  }
  if (!ready && health != RendererHealth::Occluded) {
    return true;
  }
  return false;
}

inline constexpr bool surface_formats_query_ok(VkResult count_result, uint32_t format_count,
                                              VkResult data_result) noexcept {
  return count_result == VK_SUCCESS && format_count > 0 && data_result == VK_SUCCESS;
}

inline constexpr bool create_sync_object_ok(VkResult result) noexcept {
  return result == VK_SUCCESS;
}

inline constexpr WsiRecoverAction classify_wsi_result(VkResult result) noexcept {
  switch (result) {
    case VK_SUCCESS:
      return WsiRecoverAction::None;
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_SUBOPTIMAL_KHR:
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      return WsiRecoverAction::RecreateSwapchain;
    case VK_ERROR_SURFACE_LOST_KHR:
      return WsiRecoverAction::RecreateSurfaceAndSwapchain;
    case VK_ERROR_DEVICE_LOST:
      return WsiRecoverAction::DeviceLost;
    default:
      return WsiRecoverAction::Fatal;
  }
}

// Opt-in path timing. Disabled by default so Release pays no extra clock::now
// on upload / swapchain / MSAA unless a caller enables collection.
enum class RendererPathSegment : uint8_t {
  CreateTextureRgba = 0,
  UploadFenceWait,
  UploadSubmit,
  UploadFenceReap,
  DescriptorAllocate,
  SwapchainRecreate,
  ApplyMsaa,
  ApplyMsaaIdleWait,
};

struct RendererPathSample {
  int64_t create_texture_rgba_us = 0;
  int64_t upload_fence_wait_us = 0;
  int64_t upload_submit_us = 0;
  int64_t upload_fence_reap_us = 0;
  int64_t descriptor_allocate_us = 0;
  int64_t swapchain_recreate_us = 0;
  int64_t apply_msaa_us = 0;
  int64_t apply_msaa_idle_wait_us = 0;
};

struct RendererPathCounts {
  uint64_t create_texture_rgba = 0;
  uint64_t upload_fence_wait = 0;
  uint64_t upload_submit = 0;
  uint64_t upload_fence_reap = 0;
  uint64_t descriptor_allocate = 0;
  uint64_t swapchain_recreate = 0;
  uint64_t apply_msaa = 0;
  uint64_t apply_msaa_idle_wait = 0;
};

struct RendererPathSnapshot {
  RendererPathSample last{};
  RendererPathSample total{};
  RendererPathCounts counts{};
};

inline int64_t* path_sample_us(RendererPathSample& sample, RendererPathSegment segment) noexcept {
  switch (segment) {
    case RendererPathSegment::CreateTextureRgba:
      return &sample.create_texture_rgba_us;
    case RendererPathSegment::UploadFenceWait:
      return &sample.upload_fence_wait_us;
    case RendererPathSegment::UploadSubmit:
      return &sample.upload_submit_us;
    case RendererPathSegment::UploadFenceReap:
      return &sample.upload_fence_reap_us;
    case RendererPathSegment::DescriptorAllocate:
      return &sample.descriptor_allocate_us;
    case RendererPathSegment::SwapchainRecreate:
      return &sample.swapchain_recreate_us;
    case RendererPathSegment::ApplyMsaa:
      return &sample.apply_msaa_us;
    case RendererPathSegment::ApplyMsaaIdleWait:
      return &sample.apply_msaa_idle_wait_us;
  }
  return nullptr;
}

inline uint64_t* path_count_field(RendererPathCounts& counts, RendererPathSegment segment) noexcept {
  switch (segment) {
    case RendererPathSegment::CreateTextureRgba:
      return &counts.create_texture_rgba;
    case RendererPathSegment::UploadFenceWait:
      return &counts.upload_fence_wait;
    case RendererPathSegment::UploadSubmit:
      return &counts.upload_submit;
    case RendererPathSegment::UploadFenceReap:
      return &counts.upload_fence_reap;
    case RendererPathSegment::DescriptorAllocate:
      return &counts.descriptor_allocate;
    case RendererPathSegment::SwapchainRecreate:
      return &counts.swapchain_recreate;
    case RendererPathSegment::ApplyMsaa:
      return &counts.apply_msaa;
    case RendererPathSegment::ApplyMsaaIdleWait:
      return &counts.apply_msaa_idle_wait;
  }
  return nullptr;
}

inline void record_path_sample(RendererPathSnapshot& snapshot, RendererPathSegment segment,
                               int64_t us) noexcept {
  if (us < 0) {
    us = 0;
  }
  if (int64_t* last = path_sample_us(snapshot.last, segment)) {
    *last = us;
  }
  if (int64_t* total = path_sample_us(snapshot.total, segment)) {
    *total += us;
  }
  if (uint64_t* count = path_count_field(snapshot.counts, segment)) {
    *count += 1;
  }
}

inline void reset_path_snapshot(RendererPathSnapshot& snapshot) noexcept {
  snapshot = {};
}

// One resize() call either short-circuits or performs a single swapchain recreate.
// No cross-call delay / coalesce window — same-extent skip is the only coalesce.
inline constexpr bool resize_same_extent_short_circuits(int width, int height, int current_w,
                                                        int current_h, bool occluded,
                                                        bool live_swapchain) noexcept {
  return !occluded && live_swapchain && width > 0 && height > 0 && width == current_w &&
         height == current_h;
}

inline constexpr int resize_recreates_for_call(bool short_circuits) noexcept {
  return short_circuits ? 0 : 1;
}

inline constexpr bool resize_introduces_delay_semantics() noexcept {
  return false;
}

// Graphics in-flight fences only prove that queue's submits finished. They do
// not prove the presentation engine has released swapchain images. Measured
// Apple M4/MoltenVK: resize P95=0.121ms, MSAA apply=0.728ms, idle=0.108ms —
// far under the 8ms gate. Keep vkDeviceWaitIdle on swapchain/MSAA teardown.
inline constexpr bool graphics_fence_proves_presentation_complete() noexcept {
  return false;
}

inline constexpr bool retain_device_wait_idle_on_msaa_and_resize() noexcept {
  return true;
}

// FIFO keeps one image on the scan-out and typically one queued for the next
// vsync. In-flight submits may still reference `frames_in_flight` images.
// Request FIF+2 when the surface allows it. If image count equals FIF, FIFO
// has no spare and the CPU bursts then blocks on the image fence (0/33ms).
// Mac maxImageCount is often 3 and clamps back.
inline constexpr uint32_t preferred_swapchain_image_count(uint32_t min_images, uint32_t max_images,
                                                          int frames_in_flight) noexcept {
  const uint32_t fif = frames_in_flight > 0 ? static_cast<uint32_t>(frames_in_flight) : 1u;
  uint32_t want = min_images + 1u;
  if (fif + 2u > want) {
    want = fif + 2u;
  }
  if (max_images > 0 && want > max_images) {
    want = max_images;
  }
  if (want < min_images) {
    want = min_images;
  }
  return want;
}

// Never occupy every swapchain image with in-flight submits — leave at least
// one for the presentation engine. 3 images → 2 FIF; 5 images → 3 FIF.
inline constexpr int frames_in_flight_for_swapchain(uint32_t image_count,
                                                    int max_frames_in_flight) noexcept {
  if (max_frames_in_flight < 1) {
    return 1;
  }
  if (image_count <= 1) {
    return 1;
  }
  const int spare = static_cast<int>(image_count - 1u);
  return spare < max_frames_in_flight ? spare : max_frames_in_flight;
}

// Window-system binding supplied by the ui layer. Renderer never touches a
// native window object — only the Vulkan instance/surface and size callbacks.
struct VulkanHostSurface {
  // Optional Qt (or other host) owned handles. When supplied, the renderer
  // adopts them without creating or destroying the instance/surface.
  VkInstance external_instance = VK_NULL_HANDLE;
  VkSurfaceKHR external_surface = VK_NULL_HANDLE;
  bool renderer_owns_instance = true;
  bool renderer_owns_surface = true;
  // Host-provided WSI instance when the UI owns the Vulkan instance.
  // Renderer appends VK_KHR_portability_enumeration on its own for MoltenVK.
  std::vector<const char*> instance_extensions;

  // Create a VkSurfaceKHR for the given instance. Renderer owns and destroys it.
  // Return VK_NULL_HANDLE on failure.
  std::function<VkSurfaceKHR(VkInstance)> create_surface;
  // Reacquire a host-owned surface after platform surface loss.
  std::function<VkSurfaceKHR()> acquire_surface;

  // Current framebuffer size in pixels (swapchain extent source).
  std::function<void(int* width, int* height)> framebuffer_size;
  // Optional display refresh rate used for one-frame visual lead.
  std::function<int()> display_refresh_hz;

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

  // Block until the GPU is idle. Call before tearing down outside destroy().
  // Texture frees are fence-deferred; this is for swapchain / device teardown.
  // Kept (not replaced by a graphics fence): presentation may still own images.
  void device_wait_idle();

  bool ready() const noexcept { return ready_; }
  RendererHealth health() const noexcept { return health_; }

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

  // Opt-in segmented path timings (µs). Off by default — no extra clocks.
  void set_path_diagnostics_enabled(bool enabled) noexcept;
  bool path_diagnostics_enabled() const noexcept { return path_diag_enabled_; }
  void reset_path_diagnostics() noexcept;
  RendererPathSnapshot path_diagnostics() const noexcept { return path_diag_; }
  RendererPathSample last_path_sample() const noexcept { return path_diag_.last; }
  // Last selected physical device name (empty until create()).
  const char* device_name() const noexcept { return device_name_; }

  struct DescriptorPoolDiagnostics {
    uint32_t block_count = 0;
    uint32_t live_sets = 0;
  };
  DescriptorPoolDiagnostics descriptor_pool_diagnostics() const noexcept;

  // Upload a standalone RGBA8 texture (full UV 0..1). Also used by atlas bake.
  // Submit is asynchronous: a successful queue submit publishes TextureInfo
  // immediately (no per-texture host wait). Staging is filled in ≤16MiB host maps
  // after unmapping the persistent vertex rings (some Windows ICDs refuse a
  // further vkMapMemory with VK_ERROR_MEMORY_MAP_FAILED; 32MiB maps also fail).
  // Consecutive uploads leave the rings unmapped so later submits overlap GPU
  // copies; draw_frame remaps and then blocking-drains pending upload fences.
  // `nearest`: UI font atlases — NEAREST avoids LINEAR fringe that reads as bold text.
  TextureInfo create_texture_rgba(const unsigned char* pixels, int width, int height,
                                  bool nearest = false);
  void destroy_texture(TextureId id);

  // Framebuffer-pixel scissor (top-left origin), matching UI/panel rects.
  struct ScissorRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool valid() const noexcept { return w > 0 && h > 0; }
  };

  // Draw order (depth write off → later passes win):
  //   batch (stage / SplitEffect) → mid_overlay (notes / edit skins) →
  //   additive (hit VFX) → post_overlay → post_overlay2 (modals).
  // `additive` uses SRC_ALPHA / ONE. When `additive_scissor` is set, only that
  // pass is clipped (UI in `batch` / `mid_overlay` stays unclipped).
  // NOTE: DrawBatch::clear() keeps sticky bucket indices, so merging late into
  // `batch` does NOT guarantee later draw order across textures — use a later
  // pass instead.
  bool draw_frame(const DrawBatch& batch, const ScreenBounds& screen, float clear_r = 0.05f,
                  float clear_g = 0.05f, float clear_b = 0.08f,
                  const DrawBatch* additive = nullptr,
                  const DrawBatch* post_overlay = nullptr,
                  const DrawBatch* post_overlay2 = nullptr,
                  const ScissorRect* additive_scissor = nullptr,
                  const DrawBatch* mid_overlay = nullptr);

  int framebuffer_width() const noexcept { return width_; }
  int framebuffer_height() const noexcept { return height_; }

 private:
  void emit_health(RendererHealthEvent event) noexcept;
  bool check_device_result(VkResult result) noexcept;
  bool apply_wsi_action(WsiRecoverAction action);
  bool apply_zero_extent_now() noexcept;
  bool apply_swapchain_create_failure();
  bool recover_swapchain();
  bool recover_surface_and_swapchain();
  void bind_path_diag() noexcept;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool ready_ = false;
  RendererHealth health_ = RendererHealth::Uninitialized;
  int width_ = 1;
  int height_ = 1;
  int preferred_msaa_ = 1;
  bool path_diag_enabled_ = false;
  RendererPathSnapshot path_diag_{};
  char device_name_[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
};

}  // namespace wds::renderer
