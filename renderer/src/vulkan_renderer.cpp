#include "wds/renderer/vulkan_renderer.hpp"
#include "wds/renderer/descriptor_pool_policy.hpp"
#include "wds/renderer/log.hpp"
#include "wds/renderer/upload_result.hpp"

#include <wds/common/crash_handler.hpp>
#include <wds/common/utf8_path.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wds::renderer {
namespace {

constexpr int kMaxFramesInFlight = 3;
// Fixed per-frame host-visible VB capacity (grows only if a frame exceeds this).
constexpr size_t kRingVertexCapacityBytes = 2 * 1024 * 1024;

const char* vk_result_name(VkResult result) noexcept {
  switch (result) {
    case VK_SUCCESS:
      return "VK_SUCCESS";
    case VK_NOT_READY:
      return "VK_NOT_READY";
    case VK_TIMEOUT:
      return "VK_TIMEOUT";
    case VK_EVENT_SET:
      return "VK_EVENT_SET";
    case VK_EVENT_RESET:
      return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
      return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
      return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
      return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
      return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
      return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
      return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
      return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
      return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
      return "VK_ERROR_FRAGMENTED_POOL";
#ifdef VK_ERROR_UNKNOWN
    case VK_ERROR_UNKNOWN:
      return "VK_ERROR_UNKNOWN";
#endif
    case VK_ERROR_OUT_OF_DATE_KHR:
      return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR:
      return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_SUBOPTIMAL_KHR:
      return "VK_SUBOPTIMAL_KHR";
    default:
      return "VkResult";
  }
}

const char* physical_device_type_name(VkPhysicalDeviceType type) noexcept {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
      return "other";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
      return "cpu";
    default:
      return "unknown";
  }
}

const char* present_mode_name(VkPresentModeKHR mode) noexcept {
  switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
      return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR:
      return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR:
      return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
      return "FIFO_RELAXED";
    default:
      return "OTHER";
  }
}

struct GpuTexture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDescriptorSet descriptor = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  int width = 0;
  int height = 0;
  bool alive = false;
  bool nearest = false;  // UI font atlases — avoid LINEAR soft-fringe emboldening.
  uint64_t retire_after_seq = 0;  // 0 = not retiring; GPU-free when frame_seq >= this
  uint64_t sampled_seq = 0;       // 0 = never bound in a submitted draw
};

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props,
                          VkMemoryPropertyFlags avoid_props = 0) {
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mem);
  const auto first_match = [&](bool apply_avoid) -> int {
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
      if ((type_bits & (1u << i)) == 0) {
        continue;
      }
      const VkMemoryPropertyFlags flags = mem.memoryTypes[i].propertyFlags;
      if ((flags & props) != props) {
        continue;
      }
      if (apply_avoid && avoid_props != 0 && (flags & avoid_props) != 0) {
        continue;
      }
      return static_cast<int>(i);
    }
    return -1;
  };
  if (avoid_props != 0) {
    const int idx = first_match(true);
    if (idx >= 0) {
      return static_cast<uint32_t>(idx);
    }
  }
  const int idx = first_match(false);
  if (idx >= 0) {
    return static_cast<uint32_t>(idx);
  }
  throw std::runtime_error("No suitable Vulkan memory type");
}

struct HostStaging {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
};

void destroy_host_staging(VkDevice device, HostStaging& staging) {
  if (device == VK_NULL_HANDLE) {
    staging = {};
    return;
  }
  if (staging.buffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(device, staging.buffer, nullptr);
  }
  if (staging.memory != VK_NULL_HANDLE) {
    vkFreeMemory(device, staging.memory, nullptr);
  }
  staging = {};
}

std::vector<uint32_t> host_visible_memory_types(VkPhysicalDevice phys, uint32_t type_bits) {
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mem);
  std::vector<uint32_t> ranked;
  auto consider = [&](bool want_coherent, bool avoid_device_local) {
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
      if ((type_bits & (1u << i)) == 0) {
        continue;
      }
      const VkMemoryPropertyFlags flags = mem.memoryTypes[i].propertyFlags;
      if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        continue;
      }
      const bool coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      const bool device_local = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
      if (want_coherent != coherent) {
        continue;
      }
      if (avoid_device_local && device_local) {
        continue;
      }
      if (std::find(ranked.begin(), ranked.end(), i) == ranked.end()) {
        ranked.push_back(i);
      }
    }
  };
  consider(true, true);
  consider(true, false);
  consider(false, true);
  consider(false, false);
  return ranked;
}

UploadResult create_filled_host_staging(VkDevice device, VkPhysicalDevice physical,
                                        VkDeviceSize bytes, const void* src, HostStaging* out) {
  if (out == nullptr || src == nullptr || bytes == 0) {
    return make_upload_result(VK_ERROR_INITIALIZATION_FAILED, UploadStage::Buffer);
  }
  HostStaging staging{};
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  const UploadResult created =
      make_upload_result(vkCreateBuffer(device, &info, nullptr, &staging.buffer),
                         UploadStage::Buffer);
  if (!created.ok()) {
    return created;
  }
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, staging.buffer, &req);
  const std::vector<uint32_t> types = host_visible_memory_types(physical, req.memoryTypeBits);
  if (types.empty()) {
    destroy_host_staging(device, staging);
    return upload_no_memory_type(UploadStage::Buffer);
  }

  VkPhysicalDeviceMemoryProperties mem_props{};
  vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
  UploadResult last_fail = upload_no_memory_type(UploadStage::Buffer);
  for (uint32_t type_index : types) {
    if (staging.buffer == VK_NULL_HANDLE) {
      const UploadResult recreated =
          make_upload_result(vkCreateBuffer(device, &info, nullptr, &staging.buffer),
                             UploadStage::Buffer);
      if (!recreated.ok()) {
        return recreated;
      }
      vkGetBufferMemoryRequirements(device, staging.buffer, &req);
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type_index;
    const UploadResult memory = make_upload_result(
        vkAllocateMemory(device, &alloc, nullptr, &staging.memory), UploadStage::Buffer);
    if (!memory.ok()) {
      last_fail = memory;
      staging.memory = VK_NULL_HANDLE;
      continue;
    }
    const UploadResult bound =
        make_upload_result(vkBindBufferMemory(device, staging.buffer, staging.memory, 0),
                           UploadStage::Bind);
    if (!bound.ok()) {
      last_fail = bound;
      vkFreeMemory(device, staging.memory, nullptr);
      staging.memory = VK_NULL_HANDLE;
      continue;
    }
    void* mapped = nullptr;
    const VkResult map_r = vkMapMemory(device, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    const UploadResult mapped_r = make_upload_map_result(map_r, mapped);
    if (!mapped_r.ok()) {
      last_fail = mapped_r;
      destroy_host_staging(device, staging);
      continue;
    }
    std::memcpy(mapped, src, static_cast<size_t>(bytes));
    const VkMemoryPropertyFlags flags = mem_props.memoryTypes[type_index].propertyFlags;
    if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = staging.memory;
      range.size = VK_WHOLE_SIZE;
      vkFlushMappedMemoryRanges(device, 1, &range);
    }
    vkUnmapMemory(device, staging.memory);
    *out = staging;
    return {};
  }
  destroy_host_staging(device, staging);
  return last_fail;
}

bool looks_like_shader_dir(const std::filesystem::path& dir) {
  using wds::common::is_regular_file_utf8;
  using wds::common::path_to_utf8;
  return is_regular_file_utf8(path_to_utf8(dir / "textured_quad.vert.spv")) &&
         is_regular_file_utf8(path_to_utf8(dir / "textured_quad.frag.spv"));
}

// Resolve SPIR-V directory for both in-tree builds and packaged installs.
// Packaged layout is identical on every platform: <root>/shaders
// (macOS .app: Contents/Resources/shaders; zip: next to the binary).
std::string resolve_shader_dir() {
  namespace fs = std::filesystem;
  std::error_code ec;

  if (const char* env = std::getenv("WDS_SHADER_DIR"); env != nullptr && env[0] != '\0') {
#if defined(_WIN32)
    // Process env vars from the CRT are ACP; convert via narrow path → wide → UTF-8.
    const fs::path env_path(env);
#else
    const fs::path env_path = wds::common::path_from_utf8(env);
#endif
    if (looks_like_shader_dir(env_path)) {
      return wds::common::path_to_utf8(env_path);
    }
  }

  const fs::path exe_dir = wds::common::executable_dir(nullptr);
  const fs::path candidates[] = {
      fs::current_path(ec) / "shaders",
      exe_dir / "shaders",
      exe_dir / ".." / "Resources" / "shaders",
      // In-tree: build-<target>[-debug]/ui/wds_editor → ../renderer/shaders
      exe_dir / ".." / "renderer" / "shaders",
      fs::current_path(ec) / "renderer" / "shaders",
      exe_dir / "renderer" / "shaders",
  };
  for (const auto& cand : candidates) {
    if (looks_like_shader_dir(cand)) {
      return wds::common::path_to_utf8(cand.lexically_normal());
    }
  }

#ifdef WDS_SHADER_DIR
  {
    const fs::path baked = wds::common::path_from_utf8(WDS_SHADER_DIR);
    if (looks_like_shader_dir(baked)) {
      return wds::common::path_to_utf8(baked);
    }
  }
#endif

  return "renderer/shaders";
}

std::vector<char> read_file(const std::string& path) {
  std::vector<char> buffer;
  if (!wds::common::read_file_bytes(path, buffer)) {
    return {};
  }
  return buffer;
}

VkShaderModule create_shader_module(VkDevice device, const std::vector<char>& code) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = code.size();
  info.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return module;
}

void ortho_rh(float l, float r, float b, float t, float n, float f, float* out16) {
  // Column-major ortho for Vulkan NDC z in [0, 1].
  // Maps world z so larger z is closer (smaller depth): depth = (f - z) / (f - n).
  // (OpenGL-style z_ndc = -z left positive-z sprites clipped under Vulkan.)
  std::memset(out16, 0, sizeof(float) * 16);
  out16[0] = 2.0f / (r - l);
  out16[5] = 2.0f / (t - b);
  out16[10] = -1.0f / (f - n);
  out16[12] = -(r + l) / (r - l);
  out16[13] = -(t + b) / (t - b);
  out16[14] = f / (f - n);
  out16[15] = 1.0f;
}

}  // namespace

struct VulkanRenderer::Impl {
  std::function<VkSurfaceKHR(VkInstance)> create_surface;
  std::function<VkSurfaceKHR()> acquire_surface;
  std::function<void(int* width, int* height)> framebuffer_size;
  bool owns_instance = true;
  bool owns_surface = true;
  WsiRecoverAction last_wsi_action = WsiRecoverAction::None;
  struct PendingUpload {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<HostStaging> staging;
    TextureId texture_id = kInvalidTextureId;
  };
  std::vector<PendingUpload> pending_uploads;
#if defined(_WIN32)
  std::function<HMONITOR()> win32_monitor;
#endif
  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphics_queue = VK_NULL_HANDLE;
  uint32_t graphics_family = 0;

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D swapchain_extent{};
  std::vector<VkImage> swapchain_images;
  std::vector<VkImageView> swapchain_views;
  std::vector<VkFramebuffer> framebuffers;
  // Per-swapchain-image fence currently using that image (VK_NULL_HANDLE if free).
  std::vector<VkFence> images_in_flight;
  // True while the surface reports 0×0 (minimize / fullscreen transition). Keep the
  // previous swapchain alive and skip acquire/present until a valid extent returns.
  bool swapchain_occluded = false;

  // Win VK_EXT_full_screen_exclusive (optional; acquired only while exclusive FS).
  bool fse_extension = false;
  bool exclusive_fullscreen_desired = false;
  bool fse_acquired = false;
  bool surface_caps2_extension = false;
#if defined(_WIN32)
  PFN_vkAcquireFullScreenExclusiveModeEXT acquire_fse = nullptr;
  PFN_vkReleaseFullScreenExclusiveModeEXT release_fse = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR get_surface_caps2 = nullptr;
#endif
  // Last draw_frame WSI / submit timing (µs).
  int64_t last_fence_wait_us = 0;
  int64_t last_acquire_wait_us = 0;
  int64_t last_present_us = 0;
  int64_t last_gpu_submit_us = 0;
  void release_fullscreen_exclusive_internal();
  void note_fullscreen_exclusive_lost(const char* where, VkResult result);

  VkImage depth_image = VK_NULL_HANDLE;
  VkDeviceMemory depth_memory = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

  // MSAA color target (resolved into swapchain); sample count from preferred_msaa.
  VkSampleCountFlagBits msaa_samples = VK_SAMPLE_COUNT_2_BIT;
  VkImage color_msaa_image = VK_NULL_HANDLE;
  VkDeviceMemory color_msaa_memory = VK_NULL_HANDLE;
  VkImageView color_msaa_view = VK_NULL_HANDLE;

  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipeline pipeline_additive = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkSampler sampler_nearest = VK_NULL_HANDLE;
  struct DescriptorPoolBlock {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    uint32_t capacity = 0;
    uint32_t live = 0;
  };
  std::vector<DescriptorPoolBlock> descriptor_blocks;

  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandPool upload_command_pool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kMaxFramesInFlight> command_buffers{};
  std::array<VkSemaphore, kMaxFramesInFlight> image_available{};
  std::array<VkSemaphore, kMaxFramesInFlight> render_finished{};
  std::array<VkFence, kMaxFramesInFlight> in_flight{};
  uint32_t frame_index = 0;
  int frames_in_flight = kMaxFramesInFlight;
  uint64_t frame_seq = 0;
  int preferred_msaa = 1;

  struct FrameVertexBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    size_t capacity_bytes = 0;
  };
  std::array<FrameVertexBuffer, kMaxFramesInFlight> frame_vertices{};

  // Reused per draw_frame to avoid heap churn on the present path.
  std::vector<uint32_t> bucket_first_vertex;
  std::vector<uint32_t> mid_first_vertex;
  std::vector<uint32_t> additive_first_vertex;
  std::vector<uint32_t> post_first_vertex;
  std::vector<uint32_t> post2_first_vertex;

  std::vector<GpuTexture> textures;  // index 0 unused
  std::string shader_dir;

  void cleanup_swapchain();
  // Destroy MSAA/depth/views/framebuffers but leave `swapchain` handle intact
  // (used when recreating via oldSwapchain).
  void cleanup_swapchain_resources_keep_handle();
  bool create_swapchain(int width, int height);
  bool create_color_msaa_resources();
  bool create_depth_resources();
  bool create_framebuffers();
  bool create_render_pass_and_pipelines();
  void destroy_render_pass_and_pipelines();
  bool ensure_frame_vertex_capacity(uint32_t frame, size_t bytes);
  void unmap_frame_vertices();
  bool remap_frame_vertices();
  void destroy_frame_vertices();
  TextureId alloc_texture_slot();
  void release_texture_descriptor(GpuTexture& tex);
  void destroy_gpu_texture_resources(GpuTexture& tex);
  void reap_retired_textures(uint64_t now_seq);
  UploadResult upload_texture_pixels(GpuTexture& tex, TextureId id, const unsigned char* pixels,
                                     int width, int height);
  bool create_descriptor_block(uint32_t capacity);
  UploadResult allocate_texture_descriptor(GpuTexture& tex);
  UploadResult create_texture_descriptor(GpuTexture& tex);
  UploadResult begin_one_time(VkCommandBuffer* out_cmd);
  UploadResult end_one_time(VkCommandBuffer cmd, bool* submitted, VkFence* out_fence);
  bool texture_has_pending_upload(TextureId id) const noexcept;
  bool reserve_pending_record();
  void rollback_pending_record();
  void commit_pending_record(PendingUpload pending);
  void release_pending_slot(PendingUpload& pending);
  UploadHealthDelta reap_pending_uploads();
  UploadHealthDelta wait_pending_uploads();
  void force_release_pending_uploads();
  VkSampleCountFlagBits pick_msaa_samples() const;
  VkResult device_wait_idle_result();

  bool* path_diag_enabled = nullptr;
  RendererPathSnapshot* path_diag = nullptr;
  bool in_apply_msaa = false;
  int64_t apply_msaa_idle_acc_us = 0;

  bool path_diag_on() const noexcept {
    return path_diag_enabled != nullptr && *path_diag_enabled && path_diag != nullptr;
  }
  void record_path(RendererPathSegment segment, int64_t us) {
    if (path_diag_on()) {
      record_path_sample(*path_diag, segment, us);
    }
  }
};

namespace {

using PathClock = std::chrono::steady_clock;

int64_t path_elapsed_us(PathClock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::microseconds>(PathClock::now() - t0).count();
}

struct PathTimer {
  RendererPathSnapshot* snap = nullptr;
  RendererPathSegment segment{};
  PathClock::time_point t0{};
  bool active = false;

  PathTimer(bool on, RendererPathSnapshot* s, RendererPathSegment seg) noexcept
      : snap(s), segment(seg), active(on && s != nullptr) {
    if (active) {
      t0 = PathClock::now();
    }
  }
  ~PathTimer() {
    if (active && snap != nullptr) {
      record_path_sample(*snap, segment, path_elapsed_us(t0));
    }
  }
  PathTimer(const PathTimer&) = delete;
  PathTimer& operator=(const PathTimer&) = delete;
};

}  // namespace

VulkanRenderer::VulkanRenderer() : impl_(std::make_unique<Impl>()) { bind_path_diag(); }

void VulkanRenderer::bind_path_diag() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->path_diag_enabled = &path_diag_enabled_;
  impl_->path_diag = &path_diag_;
}

void VulkanRenderer::set_path_diagnostics_enabled(bool enabled) noexcept {
  path_diag_enabled_ = enabled;
}

void VulkanRenderer::reset_path_diagnostics() noexcept { reset_path_snapshot(path_diag_); }

VulkanRenderer::~VulkanRenderer() { destroy(); }

void VulkanRenderer::set_preferred_msaa(int samples) noexcept {
  preferred_msaa_ = samples <= 1 ? 1 : (samples <= 2 ? 2 : 4);
}

int VulkanRenderer::active_msaa() const noexcept {
  if (!ready_ || !impl_) {
    return preferred_msaa_;
  }
  return static_cast<int>(impl_->msaa_samples);
}
bool VulkanRenderer::apply_msaa(int samples) {
  set_preferred_msaa(samples);
  if (health_ == RendererHealth::SurfaceLost) {
    if (!recover_surface_and_swapchain() || health_ != RendererHealth::Ready) {
      return false;
    }
  }
  if (!ready_ || impl_ == nullptr || impl_->device == VK_NULL_HANDLE) {
    return true;
  }
  impl_->preferred_msaa = preferred_msaa_;
  const VkSampleCountFlagBits next = impl_->pick_msaa_samples();
  if (next == impl_->msaa_samples) {
    return true;
  }

  int fb_w = 0, fb_h = 0;
  if (impl_->framebuffer_size) {
    impl_->framebuffer_size(&fb_w, &fb_h);
  }
  if (!apply_msaa_may_teardown(fb_w, fb_h)) {
    apply_zero_extent_now();
    return apply_msaa_zero_extent_ok();
  }

  WDS_LOG("apply_msaa %u -> %u\n", static_cast<unsigned>(impl_->msaa_samples),
          static_cast<unsigned>(next));
  PathTimer timed(path_diag_enabled_, &path_diag_, RendererPathSegment::ApplyMsaa);
  impl_->in_apply_msaa = true;
  impl_->apply_msaa_idle_acc_us = 0;
  auto finish_msaa = [&](bool ok) {
    impl_->in_apply_msaa = false;
    if (impl_->path_diag_on()) {
      impl_->record_path(RendererPathSegment::ApplyMsaaIdleWait, impl_->apply_msaa_idle_acc_us);
    }
    return ok;
  };
  // Keep vkDeviceWaitIdle: a graphics in-flight fence does not prove the
  // presentation engine has released swapchain images. Measured far under 8ms.
  if (!check_device_result(impl_->device_wait_idle_result())) {
    return finish_msaa(false);
  }
  impl_->cleanup_swapchain();
  impl_->destroy_render_pass_and_pipelines();
  impl_->msaa_samples = next;
  auto fail_after_teardown = [&]() {
    emit_health(apply_msaa_rebuild_failure_event(impl_->last_wsi_action));
    return finish_msaa(false);
  };
  if (!impl_->create_render_pass_and_pipelines()) {
    return fail_after_teardown();
  }
  if (!impl_->create_swapchain(fb_w, fb_h)) {
    return fail_after_teardown();
  }
  width_ = static_cast<int>(impl_->swapchain_extent.width);
  height_ = static_cast<int>(impl_->swapchain_extent.height);
  return finish_msaa(true);
}



void VulkanRenderer::Impl::cleanup_swapchain_resources_keep_handle() {
  if (device == VK_NULL_HANDLE) {
    return;
  }
  // Presentation may still own swapchain images; graphics fences are insufficient.
  (void)device_wait_idle_result();
  for (auto fb : framebuffers) {
    if (fb) vkDestroyFramebuffer(device, fb, nullptr);
  }
  framebuffers.clear();
  for (auto view : swapchain_views) {
    if (view) vkDestroyImageView(device, view, nullptr);
  }
  swapchain_views.clear();
  if (color_msaa_view) {
    vkDestroyImageView(device, color_msaa_view, nullptr);
    color_msaa_view = VK_NULL_HANDLE;
  }
  if (color_msaa_image) {
    vkDestroyImage(device, color_msaa_image, nullptr);
    color_msaa_image = VK_NULL_HANDLE;
  }
  if (color_msaa_memory) {
    vkFreeMemory(device, color_msaa_memory, nullptr);
    color_msaa_memory = VK_NULL_HANDLE;
  }
  if (depth_view) {
    vkDestroyImageView(device, depth_view, nullptr);
    depth_view = VK_NULL_HANDLE;
  }
  if (depth_image) {
    vkDestroyImage(device, depth_image, nullptr);
    depth_image = VK_NULL_HANDLE;
  }
  if (depth_memory) {
    vkFreeMemory(device, depth_memory, nullptr);
    depth_memory = VK_NULL_HANDLE;
  }
  swapchain_images.clear();
  images_in_flight.clear();
}

void VulkanRenderer::Impl::release_fullscreen_exclusive_internal() {
#if defined(_WIN32)
  if (!fse_acquired || device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE ||
      release_fse == nullptr) {
    fse_acquired = false;
    return;
  }
  // Present must be idle before vkReleaseFullScreenExclusiveModeEXT; otherwise Win
  // drivers can hang on the next swapchain recreate after leaving exclusive mode.
  (void)device_wait_idle_result();
  const VkResult r = release_fse(device, swapchain);
  if (r != VK_SUCCESS) {
    WDS_LOG("vkReleaseFullScreenExclusiveModeEXT failed result=%d\n", static_cast<int>(r));
  } else {
    WDS_LOG("released VK_EXT_full_screen_exclusive\n");
  }
  fse_acquired = false;
#else
  fse_acquired = false;
#endif
}

void VulkanRenderer::Impl::note_fullscreen_exclusive_lost(const char* where, VkResult result) {
#if defined(_WIN32)
  if (result != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
    return;
  }
  const char* at = where != nullptr ? where : "?";
  (void)at;  // referenced by WDS_LOG (no-op when logging is compiled out)
  WDS_LOG("VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT at %s (was_acquired=%d desired=%d)\n", at,
          fse_acquired ? 1 : 0, exclusive_fullscreen_desired ? 1 : 0);
  fse_acquired = false;
  // Force recreate on next resize/present path so we can re-acquire.
  if (exclusive_fullscreen_desired) {
    swapchain_occluded = true;
  }
#else
  (void)where;
  (void)result;
#endif
}

void VulkanRenderer::Impl::cleanup_swapchain() {
  release_fullscreen_exclusive_internal();
  cleanup_swapchain_resources_keep_handle();
  if (device != VK_NULL_HANDLE && swapchain) {
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
  }
}

UploadResult VulkanRenderer::Impl::begin_one_time(VkCommandBuffer* out_cmd) {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (out_cmd != nullptr) {
    *out_cmd = VK_NULL_HANDLE;
  }
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = upload_command_pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  const UploadResult allocated =
      make_upload_result(vkAllocateCommandBuffers(device, &alloc, &cmd),
                         UploadStage::CommandAllocate);
  if (!allocated.ok()) {
    return allocated;
  }
  if (out_cmd != nullptr) {
    *out_cmd = cmd;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  const UploadResult begun =
      make_upload_result(vkBeginCommandBuffer(cmd, &begin), UploadStage::CommandBegin);
  if (upload_begin_should_release_command(begun)) {
    vkFreeCommandBuffers(device, upload_command_pool, 1, &cmd);
    if (out_cmd != nullptr) {
      *out_cmd = VK_NULL_HANDLE;
    }
  }
  return begun;
}

bool VulkanRenderer::Impl::texture_has_pending_upload(TextureId id) const noexcept {
  if (id == kInvalidTextureId) {
    return false;
  }
  for (const auto& pending : pending_uploads) {
    if (pending.texture_id == id) {
      return true;
    }
  }
  return false;
}

bool VulkanRenderer::Impl::reserve_pending_record() {
  try {
    pending_uploads.reserve(pending_uploads.size() + 1);
    pending_uploads.emplace_back();
    return true;
  } catch (...) {
    return false;
  }
}

void VulkanRenderer::Impl::rollback_pending_record() {
  if (pending_uploads.empty()) {
    return;
  }
  release_pending_slot(pending_uploads.back());
  pending_uploads.pop_back();
}

void VulkanRenderer::Impl::commit_pending_record(PendingUpload pending) {
  if (pending_uploads.empty()) {
    pending_uploads.push_back(pending);
    return;
  }
  pending_uploads.back() = pending;
}

void VulkanRenderer::Impl::release_pending_slot(PendingUpload& pending) {
  if (pending.cmd != VK_NULL_HANDLE && upload_command_pool != VK_NULL_HANDLE &&
      device != VK_NULL_HANDLE) {
    vkFreeCommandBuffers(device, upload_command_pool, 1, &pending.cmd);
  }
  pending.cmd = VK_NULL_HANDLE;
  for (auto& block : pending.staging) {
    destroy_host_staging(device, block);
  }
  pending.staging.clear();
  if (pending.fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
    vkDestroyFence(device, pending.fence, nullptr);
  }
  pending.fence = VK_NULL_HANDLE;
  pending.texture_id = kInvalidTextureId;
}

UploadHealthDelta VulkanRenderer::Impl::reap_pending_uploads() {
  UploadHealthDelta worst{};
  if (device == VK_NULL_HANDLE || pending_uploads.empty()) {
    return worst;
  }
  const bool timed = path_diag_on();
  const auto t0 = timed ? PathClock::now() : PathClock::time_point{};
  for (size_t i = 0; i < pending_uploads.size();) {
    PendingUpload& pending = pending_uploads[i];
    if (pending.fence == VK_NULL_HANDLE) {
      release_pending_slot(pending);
      pending_uploads.erase(pending_uploads.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    const UploadFencePoll poll = classify_upload_fence_status(vkGetFenceStatus(device, pending.fence));
    if (upload_reap_may_release(poll, false)) {
      release_pending_slot(pending);
      pending_uploads.erase(pending_uploads.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    const UploadHealthDelta delta = upload_fence_poll_health(poll);
    if (delta.apply) {
      worst = delta;
    }
    ++i;
  }
  if (timed) {
    record_path(RendererPathSegment::UploadFenceReap, path_elapsed_us(t0));
  }
  return worst;
}

UploadHealthDelta VulkanRenderer::Impl::wait_pending_uploads() {
  UploadHealthDelta worst{};
  if (device == VK_NULL_HANDLE || pending_uploads.empty()) {
    return worst;
  }
  std::vector<VkFence> fences;
  fences.reserve(pending_uploads.size());
  for (const auto& pending : pending_uploads) {
    if (pending.fence != VK_NULL_HANDLE) {
      fences.push_back(pending.fence);
    }
  }
  if (!fences.empty()) {
    const bool timed = path_diag_on();
    const auto t0 = timed ? PathClock::now() : PathClock::time_point{};
    const VkResult wait = vkWaitForFences(device, static_cast<uint32_t>(fences.size()),
                                          fences.data(), VK_TRUE, UINT64_MAX);
    if (timed) {
      record_path(RendererPathSegment::UploadFenceWait, path_elapsed_us(t0));
    }
    const UploadHealthDelta wait_health = upload_health_delta(wait, UploadStage::Wait);
    if (wait_health.apply) {
      // DeviceLost / Fatal: do not reap staging while the GPU may still own it.
      return wait_health;
    }
  }
  const UploadHealthDelta reap = reap_pending_uploads();
  if (reap.apply) {
    worst = reap;
  }
  return worst;
}

void VulkanRenderer::Impl::force_release_pending_uploads() {
  for (auto& pending : pending_uploads) {
    release_pending_slot(pending);
  }
  pending_uploads.clear();
}

UploadResult VulkanRenderer::Impl::end_one_time(VkCommandBuffer cmd, bool* submitted,
                                               VkFence* out_fence) {
  if (submitted != nullptr) {
    *submitted = false;
  }
  if (out_fence != nullptr) {
    *out_fence = VK_NULL_HANDLE;
  }
  auto release_cmd = [&]() {
    if (cmd != VK_NULL_HANDLE && upload_command_pool != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device, upload_command_pool, 1, &cmd);
      cmd = VK_NULL_HANDLE;
    }
  };

  if (cmd == VK_NULL_HANDLE) {
    return make_upload_result(VK_ERROR_INITIALIZATION_FAILED, UploadStage::CommandEnd);
  }

  const UploadResult ended =
      make_upload_result(vkEndCommandBuffer(cmd), UploadStage::CommandEnd);
  if (!ended.ok()) {
    if (upload_may_release_command(ended, false, false)) {
      release_cmd();
    }
    return ended;
  }

  VkFence fence = VK_NULL_HANDLE;
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  const UploadResult fence_r =
      make_upload_result(vkCreateFence(device, &fence_info, nullptr, &fence),
                         UploadStage::FenceCreate);
  if (!fence_r.ok()) {
    if (upload_may_release_command(fence_r, false, false)) {
      release_cmd();
    }
    return fence_r;
  }

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  UploadResult submit_r;
  if (path_diag_on()) {
    const auto t0 = PathClock::now();
    submit_r = make_upload_result(vkQueueSubmit(graphics_queue, 1, &submit, fence),
                                  UploadStage::QueueSubmit);
    record_path(RendererPathSegment::UploadSubmit, path_elapsed_us(t0));
  } else {
    submit_r = make_upload_result(vkQueueSubmit(graphics_queue, 1, &submit, fence),
                                  UploadStage::QueueSubmit);
  }
  if (!submit_r.ok()) {
    if (upload_may_release_command(submit_r, false, false)) {
      release_cmd();
    }
    if (fence != VK_NULL_HANDLE) {
      vkDestroyFence(device, fence, nullptr);
    }
    return submit_r;
  }
  if (submitted != nullptr) {
    *submitted = true;
  }
  if (out_fence != nullptr) {
    *out_fence = fence;
  }
  return {};
}

bool VulkanRenderer::Impl::create_depth_resources() {
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {swapchain_extent.width, swapchain_extent.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.format = depth_format;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  image_info.samples = msaa_samples;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateImage(device, &image_info, nullptr, &depth_image) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device, depth_image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex =
      find_memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device, &alloc, nullptr, &depth_memory) != VK_SUCCESS) {
    return false;
  }
  vkBindImageMemory(device, depth_image, depth_memory, 0);

  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = depth_image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = depth_format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  return vkCreateImageView(device, &view_info, nullptr, &depth_view) == VK_SUCCESS;
}

bool VulkanRenderer::Impl::create_color_msaa_resources() {
  if (msaa_samples == VK_SAMPLE_COUNT_1_BIT) {
    // 1× path writes color directly to the swapchain image (no MSAA target).
    return true;
  }
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {swapchain_extent.width, swapchain_extent.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.format = swapchain_format;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  image_info.samples = msaa_samples;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateImage(device, &image_info, nullptr, &color_msaa_image) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device, color_msaa_image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex =
      find_memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device, &alloc, nullptr, &color_msaa_memory) != VK_SUCCESS) {
    return false;
  }
  vkBindImageMemory(device, color_msaa_image, color_msaa_memory, 0);

  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = color_msaa_image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = swapchain_format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  return vkCreateImageView(device, &view_info, nullptr, &color_msaa_view) == VK_SUCCESS;
}

bool VulkanRenderer::Impl::create_framebuffers() {
  framebuffers.resize(swapchain_views.size());
  for (size_t i = 0; i < swapchain_views.size(); ++i) {
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = render_pass;
    info.width = swapchain_extent.width;
    info.height = swapchain_extent.height;
    info.layers = 1;
    if (msaa_samples == VK_SAMPLE_COUNT_1_BIT) {
      std::array<VkImageView, 2> attachments = {swapchain_views[i], depth_view};
      info.attachmentCount = static_cast<uint32_t>(attachments.size());
      info.pAttachments = attachments.data();
      if (vkCreateFramebuffer(device, &info, nullptr, &framebuffers[i]) != VK_SUCCESS) {
        return false;
      }
    } else {
      std::array<VkImageView, 3> attachments = {color_msaa_view, depth_view, swapchain_views[i]};
      info.attachmentCount = static_cast<uint32_t>(attachments.size());
      info.pAttachments = attachments.data();
      if (vkCreateFramebuffer(device, &info, nullptr, &framebuffers[i]) != VK_SUCCESS) {
        return false;
      }
    }
  }
  return true;
}

VkSampleCountFlagBits VulkanRenderer::Impl::pick_msaa_samples() const {
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical, &props);
  const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                    props.limits.framebufferDepthSampleCounts;
  const int want = preferred_msaa <= 1 ? 1 : (preferred_msaa <= 2 ? 2 : 4);
  if (want >= 4 && (counts & VK_SAMPLE_COUNT_4_BIT)) {
    return VK_SAMPLE_COUNT_4_BIT;
  }
  if (want >= 2 && (counts & VK_SAMPLE_COUNT_2_BIT)) {
    return VK_SAMPLE_COUNT_2_BIT;
  }
  return VK_SAMPLE_COUNT_1_BIT;
}

VkResult VulkanRenderer::Impl::device_wait_idle_result() {
  if (device == VK_NULL_HANDLE) {
    return VK_SUCCESS;
  }
  // Retained: graphics fences do not prove presentation-engine completion.
  VkResult result = VK_SUCCESS;
  if (!path_diag_on() || !in_apply_msaa) {
    result = vkDeviceWaitIdle(device);
  } else {
    const auto t0 = PathClock::now();
    result = vkDeviceWaitIdle(device);
    apply_msaa_idle_acc_us += path_elapsed_us(t0);
  }
  if (result == VK_SUCCESS) {
    // Idle proves the upload queue is done; only then drop staging/cmd/fence.
    force_release_pending_uploads();
  }
  return result;
}

bool VulkanRenderer::Impl::create_swapchain(int width, int height) {
  PathTimer timed(path_diag_on(), path_diag, RendererPathSegment::SwapchainRecreate);
  last_wsi_action = WsiRecoverAction::None;
#if defined(_WIN32)
  // Drain GPU/presentation before FSE release + vkCreateSwapchainKHR. Rapid
  // exclusive-monitor toggles otherwise hang inside the Win32 WSI path with no
  // recoverable error.
  if (swapchain != VK_NULL_HANDLE) {
    (void)device_wait_idle_result();
  }
#endif
  VkSurfaceCapabilitiesKHR caps{};
  const VkResult caps_r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
  last_wsi_action = classify_wsi_result(caps_r);
  if (caps_r != VK_SUCCESS) {
    return false;
  }

  // Resolve the target extent BEFORE destroying the live swapchain. During
  // minimize / fullscreen enter-exit, Win32 and Cocoa often report 0×0 briefly;
  // tearing down first then failing leaves present broken and can TDR the GPU.
  VkExtent2D new_extent{};
  if (caps.currentExtent.width != UINT32_MAX) {
    new_extent = caps.currentExtent;
  } else {
    new_extent.width = std::clamp(static_cast<uint32_t>(width), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
    new_extent.height = std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
  }
  if (new_extent.width == 0 || new_extent.height == 0) {
    if (swapchain == VK_NULL_HANDLE) {
      return false;  // first create cannot succeed without a surface
    }
    swapchain_occluded = true;
    WDS_LOG("create_swapchain: occluded (0x0 surface), keeping previous swapchain\n");
    return true;
  }

  auto note_wsi = [&](VkResult result) -> bool {
    last_wsi_action = classify_wsi_result(result);
    return result == VK_SUCCESS;
  };

  uint32_t format_count = 0;
  if (!note_wsi(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr))) {
    return false;
  }
  if (format_count == 0) {
    last_wsi_action = WsiRecoverAction::Fatal;
    return false;
  }
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  if (!note_wsi(
          vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data()))) {
    return false;
  }
  VkSurfaceFormatKHR chosen = formats[0];
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB) {
      chosen = f;
      break;
    }
  }

  uint32_t present_count = 0;
  if (!note_wsi(
          vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &present_count, nullptr))) {
    return false;
  }
  if (present_count == 0) {
    last_wsi_action = WsiRecoverAction::Fatal;
    return false;
  }
  std::vector<VkPresentModeKHR> presents(present_count);
  if (!note_wsi(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &present_count,
                                                         presents.data()))) {
    return false;
  }
  // Prefer MAILBOX when available. FIFO makes vkQueuePresentKHR block each
  // viewport independently, so two visible surfaces serialize and produce
  // missed frames even when the GPU has spare time. Transport uses measured
  // elapsed time, therefore MAILBOX does not change playback speed.
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  if (std::find(presents.begin(), presents.end(), VK_PRESENT_MODE_MAILBOX_KHR) != presents.end())
    present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
  WDS_LOG("swapchain present=%s format=%u colorspace=%u extent=%ux%u modes=%u\n",
          present_mode_name(present_mode), static_cast<unsigned>(chosen.format),
          static_cast<unsigned>(chosen.colorSpace), new_extent.width, new_extent.height,
          present_count);

  // FIF+2 when the surface allows it (scan-out + queued + in-flight). Clamped
  // to maxImageCount — Mac often stays at 3; Win NVIDIA here can go to 5.
  const uint32_t image_count =
      preferred_swapchain_image_count(caps.minImageCount, caps.maxImageCount, kMaxFramesInFlight);

  // Release FSE on the live swapchain before we retire it.
  release_fullscreen_exclusive_internal();

  const VkSwapchainKHR old_swapchain = swapchain;
  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = surface;
  info.minImageCount = image_count;
  info.imageFormat = chosen.format;
  info.imageColorSpace = chosen.colorSpace;
  info.imageExtent = new_extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = old_swapchain;

#if defined(_WIN32)
  VkSurfaceFullScreenExclusiveInfoEXT fse_info{
      VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
  VkSurfaceFullScreenExclusiveWin32InfoEXT fse_win32{
      VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
  bool attach_fse = false;
  HMONITOR hmon = nullptr;
  VkBool32 fse_supported = VK_FALSE;
  bool fse_cap_queried = false;

  if (fse_extension && exclusive_fullscreen_desired) {
    if (win32_monitor) {
      hmon = win32_monitor();
    }
    if (hmon == nullptr) {
      WDS_LOG("FSE: win32_monitor returned null HMONITOR\n");
    }

    // Prefer an explicit capability query (needs VK_KHR_get_surface_capabilities2).
    if (get_surface_caps2 != nullptr) {
      VkSurfaceFullScreenExclusiveInfoEXT query_fse{
          VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
      query_fse.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
      VkSurfaceFullScreenExclusiveWin32InfoEXT query_win32{
          VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
      if (hmon != nullptr) {
        query_win32.hmonitor = hmon;
        query_fse.pNext = &query_win32;
      }
      VkPhysicalDeviceSurfaceInfo2KHR surface_info{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
      surface_info.surface = surface;
      surface_info.pNext = &query_fse;

      VkSurfaceCapabilitiesFullScreenExclusiveEXT fse_caps{
          VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT};
      VkSurfaceCapabilities2KHR caps2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
      caps2.pNext = &fse_caps;
      const VkResult cr = get_surface_caps2(physical, &surface_info, &caps2);
      if (cr == VK_SUCCESS) {
        fse_cap_queried = true;
        fse_supported = fse_caps.fullScreenExclusiveSupported;
        WDS_LOG("FSE capability query: supported=%d hmonitor=%p\n",
                fse_supported ? 1 : 0, static_cast<void*>(hmon));
      } else {
        WDS_LOG("FSE capability query failed result=%d hmonitor=%p\n", static_cast<int>(cr),
                static_cast<void*>(hmon));
      }
    } else {
      WDS_LOG("FSE: VK_KHR_get_surface_capabilities2 unavailable; trying acquire anyway\n");
    }

    // Attach FSE pNext when supported, or when we could not query (best-effort).
    if (!fse_cap_queried || fse_supported) {
      fse_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
      if (hmon != nullptr) {
        fse_win32.hmonitor = hmon;
        fse_info.pNext = &fse_win32;
      }
      info.pNext = &fse_info;
      attach_fse = true;
    } else {
      WDS_LOG("FSE not supported for this surface/monitor — swapchain without exclusive mode "
              "(DWM may still composite)\n");
    }
  }
#endif

  VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
  {
    const VkResult cr = vkCreateSwapchainKHR(device, &info, nullptr, &new_swapchain);
    if (!note_wsi(cr)) {
      WDS_LOG("create_swapchain: vkCreateSwapchainKHR failed result=%d (%s)\n",
              static_cast<int>(cr), vk_result_name(cr));
      return false;
    }
  }

  // Keep previous swapchain resources alive until the new chain is fully wired.
  const VkFormat prev_format = swapchain_format;
  const VkExtent2D prev_extent = swapchain_extent;
  std::vector<VkImageView> old_views = std::move(swapchain_views);
  std::vector<VkFramebuffer> old_framebuffers = std::move(framebuffers);
  std::vector<VkImage> old_images = std::move(swapchain_images);
  std::vector<VkFence> old_images_in_flight = std::move(images_in_flight);
  const VkImageView old_color_view = color_msaa_view;
  const VkImage old_color_image = color_msaa_image;
  const VkDeviceMemory old_color_memory = color_msaa_memory;
  const VkImageView old_depth_view = depth_view;
  const VkImage old_depth_image = depth_image;
  const VkDeviceMemory old_depth_memory = depth_memory;
  color_msaa_view = VK_NULL_HANDLE;
  color_msaa_image = VK_NULL_HANDLE;
  color_msaa_memory = VK_NULL_HANDLE;
  depth_view = VK_NULL_HANDLE;
  depth_image = VK_NULL_HANDLE;
  depth_memory = VK_NULL_HANDLE;

  auto restore_previous = [&]() {
    for (auto fb : framebuffers) {
      if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    framebuffers.clear();
    for (auto view : swapchain_views) {
      if (view) vkDestroyImageView(device, view, nullptr);
    }
    if (color_msaa_view) vkDestroyImageView(device, color_msaa_view, nullptr);
    if (color_msaa_image) vkDestroyImage(device, color_msaa_image, nullptr);
    if (color_msaa_memory) vkFreeMemory(device, color_msaa_memory, nullptr);
    if (depth_view) vkDestroyImageView(device, depth_view, nullptr);
    if (depth_image) vkDestroyImage(device, depth_image, nullptr);
    if (depth_memory) vkFreeMemory(device, depth_memory, nullptr);
    color_msaa_view = old_color_view;
    color_msaa_image = old_color_image;
    color_msaa_memory = old_color_memory;
    depth_view = old_depth_view;
    depth_image = old_depth_image;
    depth_memory = old_depth_memory;
    swapchain_views = std::move(old_views);
    framebuffers = std::move(old_framebuffers);
    swapchain_images = std::move(old_images);
    images_in_flight = std::move(old_images_in_flight);
    vkDestroySwapchainKHR(device, new_swapchain, nullptr);
    swapchain = old_swapchain;
    swapchain_format = prev_format;
    swapchain_extent = prev_extent;
  };

  swapchain = new_swapchain;
  swapchain_format = chosen.format;
  swapchain_extent = new_extent;
  swapchain_occluded = false;

#if defined(_WIN32)
  if (attach_fse && acquire_fse != nullptr) {
    const VkResult ar = acquire_fse(device, swapchain);
    if (ar == VK_SUCCESS) {
      fse_acquired = true;
      WDS_LOG("acquired VK_EXT_full_screen_exclusive (hmonitor=%p extent=%ux%u)\n",
              static_cast<void*>(hmon), swapchain_extent.width, swapchain_extent.height);
    } else {
      fse_acquired = false;
      WDS_LOG("vkAcquireFullScreenExclusiveModeEXT failed result=%d "
              "(supported=%d queried=%d hmonitor=%p) — continuing; DWM may still composite\n",
              static_cast<int>(ar), fse_supported ? 1 : 0, fse_cap_queried ? 1 : 0,
              static_cast<void*>(hmon));
    }
  } else if (exclusive_fullscreen_desired) {
    fse_acquired = false;
    WDS_LOG("FSE desired but not acquired (extension=%d attach=%d acquire_fn=%d)\n",
            fse_extension ? 1 : 0, attach_fse ? 1 : 0, acquire_fse != nullptr ? 1 : 0);
  } else {
    fse_acquired = false;
  }
#endif

  uint32_t actual = 0;
  if (!note_wsi(vkGetSwapchainImagesKHR(device, swapchain, &actual, nullptr))) {
    restore_previous();
    return false;
  }
  swapchain_images.resize(actual);
  if (!note_wsi(vkGetSwapchainImagesKHR(device, swapchain, &actual, swapchain_images.data()))) {
    restore_previous();
    return false;
  }
  images_in_flight.assign(actual, VK_NULL_HANDLE);
  frames_in_flight = frames_in_flight_for_swapchain(actual, kMaxFramesInFlight);
  if (frame_index >= static_cast<uint32_t>(frames_in_flight)) {
    frame_index = 0;
  }
  WDS_LOG("swapchain images=%u (requested=%u) frames_in_flight=%d min=%u max=%u\n", actual,
          image_count, frames_in_flight, caps.minImageCount, caps.maxImageCount);

  swapchain_views.resize(swapchain_images.size());
  bool ok = true;
  for (size_t i = 0; i < swapchain_images.size(); ++i) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = swapchain_images[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = swapchain_format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &view, nullptr, &swapchain_views[i]) != VK_SUCCESS) {
      ok = false;
      break;
    }
  }
  if (ok) ok = create_color_msaa_resources();
  if (ok) ok = create_depth_resources();
  if (ok) ok = create_framebuffers();
  if (!ok) {
    restore_previous();
    if (swapchain == VK_NULL_HANDLE) {
      last_wsi_action = WsiRecoverAction::Fatal;
    }
    WDS_LOG("create_swapchain: dependent setup failed; kept previous swapchain\n");
    return false;
  }

  // Success — wait for in-flight frames before retiring old swapchain resources.
  (void)device_wait_idle_result();
  for (auto fb : old_framebuffers) {
    if (fb) vkDestroyFramebuffer(device, fb, nullptr);
  }
  for (auto view : old_views) {
    if (view) vkDestroyImageView(device, view, nullptr);
  }
  if (old_color_view) vkDestroyImageView(device, old_color_view, nullptr);
  if (old_color_image) vkDestroyImage(device, old_color_image, nullptr);
  if (old_color_memory) vkFreeMemory(device, old_color_memory, nullptr);
  if (old_depth_view) vkDestroyImageView(device, old_depth_view, nullptr);
  if (old_depth_image) vkDestroyImage(device, old_depth_image, nullptr);
  if (old_depth_memory) vkFreeMemory(device, old_depth_memory, nullptr);
  if (old_swapchain) {
    vkDestroySwapchainKHR(device, old_swapchain, nullptr);
  }
  return true;
}

bool VulkanRenderer::Impl::ensure_frame_vertex_capacity(uint32_t frame, size_t bytes) {
  FrameVertexBuffer& slot = frame_vertices[frame];
  if (bytes <= slot.capacity_bytes && slot.buffer != VK_NULL_HANDLE) {
    // Uploads may leave rings unmapped; restore this slot before draw memcpy.
    if (slot.mapped == nullptr) {
      return remap_frame_vertices() && slot.mapped != nullptr;
    }
    return true;
  }

  if (slot.mapped) {
    vkUnmapMemory(device, slot.memory);
    slot.mapped = nullptr;
  }
  if (slot.buffer) {
    vkDestroyBuffer(device, slot.buffer, nullptr);
    slot.buffer = VK_NULL_HANDLE;
  }
  if (slot.memory) {
    vkFreeMemory(device, slot.memory, nullptr);
    slot.memory = VK_NULL_HANDLE;
  }

  size_t capacity = std::max(bytes, kRingVertexCapacityBytes);
  capacity = std::max(capacity, slot.capacity_bytes * 2);

  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = capacity;
  buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device, &buffer_info, nullptr, &slot.buffer) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, slot.buffer, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = find_memory_type(
      physical, req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (vkAllocateMemory(device, &alloc, nullptr, &slot.memory) != VK_SUCCESS) {
    return false;
  }
  vkBindBufferMemory(device, slot.buffer, slot.memory, 0);
  slot.mapped = nullptr;
  if (vkMapMemory(device, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped) != VK_SUCCESS ||
      slot.mapped == nullptr) {
    return false;
  }
  slot.capacity_bytes = capacity;
  return true;
}

void VulkanRenderer::Impl::unmap_frame_vertices() {
  for (auto& slot : frame_vertices) {
    if (slot.mapped != nullptr && slot.memory != VK_NULL_HANDLE) {
      vkUnmapMemory(device, slot.memory);
      slot.mapped = nullptr;
    }
  }
}

bool VulkanRenderer::Impl::remap_frame_vertices() {
  bool ok = true;
  for (auto& slot : frame_vertices) {
    if (slot.mapped != nullptr || slot.memory == VK_NULL_HANDLE || slot.buffer == VK_NULL_HANDLE) {
      continue;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device, slot.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
        mapped == nullptr) {
      ok = false;
      continue;
    }
    slot.mapped = mapped;
  }
  return ok;
}

void VulkanRenderer::Impl::destroy_frame_vertices() {
  for (auto& slot : frame_vertices) {
    if (slot.mapped) {
      vkUnmapMemory(device, slot.memory);
      slot.mapped = nullptr;
    }
    if (slot.buffer) {
      vkDestroyBuffer(device, slot.buffer, nullptr);
      slot.buffer = VK_NULL_HANDLE;
    }
    if (slot.memory) {
      vkFreeMemory(device, slot.memory, nullptr);
      slot.memory = VK_NULL_HANDLE;
    }
    slot.capacity_bytes = 0;
  }
}

TextureId VulkanRenderer::Impl::alloc_texture_slot() {
  for (size_t i = 1; i < textures.size(); ++i) {
    // Pending-destroy slots still hold live GPU images — do not reuse.
    if (!textures[i].alive && textures[i].image == VK_NULL_HANDLE) {
      textures[i] = GpuTexture{};
      textures[i].alive = true;
      return static_cast<TextureId>(i);
    }
  }
  textures.push_back(GpuTexture{});
  textures.back().alive = true;
  return static_cast<TextureId>(textures.size() - 1);
}

void VulkanRenderer::Impl::release_texture_descriptor(GpuTexture& tex) {
  if (tex.descriptor == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
    tex.descriptor = VK_NULL_HANDLE;
    tex.descriptor_pool = VK_NULL_HANDLE;
    return;
  }
  const VkDescriptorPool pool = tex.descriptor_pool;
  if (pool != VK_NULL_HANDLE) {
    vkFreeDescriptorSets(device, pool, 1, &tex.descriptor);
    for (auto& block : descriptor_blocks) {
      if (block.pool == pool) {
        block.live = descriptor_pool_live_after_free(block.live);
        break;
      }
    }
  }
  tex.descriptor = VK_NULL_HANDLE;
  tex.descriptor_pool = VK_NULL_HANDLE;
}

void VulkanRenderer::Impl::destroy_gpu_texture_resources(GpuTexture& tex) {
  release_texture_descriptor(tex);
  if (tex.view) vkDestroyImageView(device, tex.view, nullptr);
  if (tex.image) vkDestroyImage(device, tex.image, nullptr);
  if (tex.memory) vkFreeMemory(device, tex.memory, nullptr);
  tex = GpuTexture{};
}

void VulkanRenderer::Impl::reap_retired_textures(uint64_t now_seq) {
  for (size_t i = 1; i < textures.size(); ++i) {
    GpuTexture& tex = textures[i];
    if (tex.alive || tex.image == VK_NULL_HANDLE) {
      continue;
    }
    const bool retire_due = tex.retire_after_seq != 0 && now_seq >= tex.retire_after_seq;
    const bool never_sampled = tex.sampled_seq == 0;
    const bool upload_in_flight = texture_has_pending_upload(static_cast<TextureId>(i));
    if (upload_may_destroy_texture_image(upload_in_flight, retire_due || never_sampled, false)) {
      destroy_gpu_texture_resources(tex);
    }
  }
}

bool VulkanRenderer::Impl::create_descriptor_block(uint32_t capacity) {
  if (device == VK_NULL_HANDLE || capacity == 0) {
    return false;
  }
  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_size.descriptorCount = capacity;
  VkDescriptorPoolCreateInfo dp_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dp_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dp_info.maxSets = capacity;
  dp_info.poolSizeCount = 1;
  dp_info.pPoolSizes = &pool_size;
  DescriptorPoolBlock block;
  block.capacity = capacity;
  if (vkCreateDescriptorPool(device, &dp_info, nullptr, &block.pool) != VK_SUCCESS) {
    return false;
  }
  descriptor_blocks.push_back(block);
  return true;
}

UploadResult VulkanRenderer::Impl::allocate_texture_descriptor(GpuTexture& tex) {
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &descriptor_layout;
  UploadResult last = make_upload_result(VK_ERROR_OUT_OF_POOL_MEMORY, UploadStage::Descriptor);
  auto try_block = [&](DescriptorPoolBlock& block) -> UploadResult {
    alloc.descriptorPool = block.pool;
    tex.descriptor = VK_NULL_HANDLE;
    const UploadResult allocated =
        make_upload_result(vkAllocateDescriptorSets(device, &alloc, &tex.descriptor),
                           UploadStage::Descriptor);
    if (allocated.ok()) {
      tex.descriptor_pool = block.pool;
      block.live = descriptor_pool_live_after_alloc(block.live, true);
      return allocated;
    }
    tex.descriptor = VK_NULL_HANDLE;
    tex.descriptor_pool = VK_NULL_HANDLE;
    return allocated;
  };
  for (size_t i = 0; i < descriptor_blocks.size(); ++i) {
    auto& block = descriptor_blocks[i];
    const bool more = i + 1 < descriptor_blocks.size();
    const DescriptorAllocStep step =
        descriptor_alloc_step_for_block(block.live, block.capacity, more);
    if (step == DescriptorAllocStep::SkipFull) {
      continue;
    }
    if (step == DescriptorAllocStep::Grow) {
      break;
    }
    last = try_block(block);
    if (last.ok()) {
      return last;
    }
    if (!descriptor_alloc_try_next_block(last.result)) {
      return last;
    }
  }
  const uint32_t previous = descriptor_blocks.empty() ? 0u : descriptor_blocks.back().capacity;
  if (!create_descriptor_block(descriptor_pool_next_capacity(previous))) {
    return last;
  }
  last = try_block(descriptor_blocks.back());
  if (last.ok() || !descriptor_alloc_try_next_block(last.result)) {
    return last;
  }
  if (!create_descriptor_block(descriptor_pool_next_capacity(descriptor_blocks.back().capacity))) {
    return last;
  }
  return try_block(descriptor_blocks.back());
}

UploadResult VulkanRenderer::Impl::create_texture_descriptor(GpuTexture& tex) {
  UploadResult allocated;
  if (path_diag_on()) {
    const auto t0 = PathClock::now();
    allocated = allocate_texture_descriptor(tex);
    record_path(RendererPathSegment::DescriptorAllocate, path_elapsed_us(t0));
  } else {
    allocated = allocate_texture_descriptor(tex);
  }
  if (!allocated.ok()) {
    tex.descriptor = VK_NULL_HANDLE;
    tex.descriptor_pool = VK_NULL_HANDLE;
    return allocated;
  }
  VkDescriptorImageInfo image_info{};
  image_info.sampler = tex.nearest && sampler_nearest != VK_NULL_HANDLE ? sampler_nearest : sampler;
  image_info.imageView = tex.view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = tex.descriptor;
  write.dstBinding = 0;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.descriptorCount = 1;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  return {};
}

UploadResult VulkanRenderer::Impl::upload_texture_pixels(GpuTexture& tex, TextureId id,
                                                          const unsigned char* pixels, int width,
                                                          int height) {
  const VkDeviceSize image_bytes = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
  const VkDeviceSize row_bytes = static_cast<VkDeviceSize>(width) * 4;
  const VkDeviceSize chunk_bytes = staging_copy_chunk_bytes(row_bytes);
  const uint32_t rows_per_chunk =
      row_bytes == 0 ? 0u : static_cast<uint32_t>(chunk_bytes / row_bytes);

  std::vector<HostStaging> staging_blocks;
  auto cleanup_staging = [&]() {
    for (auto& block : staging_blocks) {
      destroy_host_staging(device, block);
    }
    staging_blocks.clear();
  };
  auto cleanup_tex = [&]() {
    release_texture_descriptor(tex);
    if (tex.view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, tex.view, nullptr);
      tex.view = VK_NULL_HANDLE;
    }
    if (tex.image != VK_NULL_HANDLE) {
      vkDestroyImage(device, tex.image, nullptr);
      tex.image = VK_NULL_HANDLE;
    }
    if (tex.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, tex.memory, nullptr);
      tex.memory = VK_NULL_HANDLE;
    }
  };
  auto fail_staging = [&](UploadResult failed) {
    cleanup_staging();
    return failed;
  };
  auto fail_all = [&](UploadResult failed) {
    cleanup_staging();
    cleanup_tex();
    return failed;
  };

  if (pixels == nullptr || width <= 0 || height <= 0 || rows_per_chunk == 0) {
    return make_upload_result(VK_ERROR_INITIALIZATION_FAILED, UploadStage::Buffer);
  }

  unmap_frame_vertices();
  // Leave rings unmapped. Consecutive create_texture_rgba calls then share the
  // ICD map budget and can QueueSubmit without a remap/unmap pair each time.
  // draw_frame / ensure_frame_vertex_capacity remap before writing vertices.

  std::vector<VkBufferImageCopy> copy_regions;
  copy_regions.reserve(staging_copy_chunk_count(image_bytes, chunk_bytes));
  int y = 0;
  while (y < height) {
    const int rows = std::min(static_cast<int>(rows_per_chunk), height - y);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rows) * row_bytes;
    HostStaging block{};
    const UploadResult filled = create_filled_host_staging(
        device, physical, bytes,
        pixels + static_cast<size_t>(y) * static_cast<size_t>(row_bytes), &block);
    if (!filled.ok()) {
      destroy_host_staging(device, block);
      return fail_staging(filled);
    }
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, y, 0};
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(rows), 1};
    copy_regions.push_back(region);
    staging_blocks.push_back(block);
    y += rows;
  }

  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  const UploadResult image = make_upload_result(
      vkCreateImage(device, &image_info, nullptr, &tex.image), UploadStage::Image);
  if (!image.ok()) {
    return fail_staging(image);
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device, tex.image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  try {
    alloc.memoryTypeIndex =
        find_memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  } catch (...) {
    return fail_all(upload_no_memory_type(UploadStage::Image));
  }
  const UploadResult image_memory =
      make_upload_result(vkAllocateMemory(device, &alloc, nullptr, &tex.memory), UploadStage::Image);
  if (!image_memory.ok()) {
    return fail_all(image_memory);
  }
  const UploadResult image_bound =
      make_upload_result(vkBindImageMemory(device, tex.image, tex.memory, 0), UploadStage::Bind);
  if (!image_bound.ok()) {
    return fail_all(image_bound);
  }

  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = tex.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  const UploadResult view =
      make_upload_result(vkCreateImageView(device, &view_info, nullptr, &tex.view),
                         UploadStage::View);
  if (!view.ok()) {
    return fail_all(view);
  }
  tex.width = width;
  tex.height = height;
  const UploadResult descriptor = create_texture_descriptor(tex);
  if (!descriptor.ok()) {
    return fail_all(descriptor);
  }

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  const UploadResult begun = begin_one_time(&cmd);
  if (!begun.ok()) {
    return fail_all(begun);
  }
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = tex.image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);

  for (size_t i = 0; i < staging_blocks.size(); ++i) {
    vkCmdCopyBufferToImage(cmd, staging_blocks[i].buffer, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_regions[i]);
  }

  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &barrier);
  if (!reserve_pending_record()) {
    if (cmd != VK_NULL_HANDLE && upload_command_pool != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device, upload_command_pool, 1, &cmd);
    }
    return fail_all(make_upload_result(VK_ERROR_OUT_OF_HOST_MEMORY, UploadStage::QueueSubmit));
  }
  bool submitted = false;
  VkFence fence = VK_NULL_HANDLE;
  const UploadResult ended = end_one_time(cmd, &submitted, &fence);
  if (!ended.ok()) {
    if (submitted && !upload_may_release_command(ended, true, false)) {
      PendingUpload pending;
      pending.cmd = cmd;
      pending.fence = fence;
      pending.staging = std::move(staging_blocks);
      pending.texture_id = id;
      commit_pending_record(pending);
      return ended;
    }
    rollback_pending_record();
    if (fence != VK_NULL_HANDLE) {
      vkDestroyFence(device, fence, nullptr);
    }
    return fail_all(ended);
  }

  PendingUpload pending;
  pending.cmd = cmd;
  pending.fence = fence;
  pending.staging = std::move(staging_blocks);
  pending.texture_id = id;
  commit_pending_record(pending);
  return {};
}

bool VulkanRenderer::Impl::create_render_pass_and_pipelines() {
  // Probe surface format (also refreshed by create_swapchain).
  {
    uint32_t format_count = 0;
    const VkResult count_r =
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
    last_wsi_action = classify_wsi_result(count_r);
    std::vector<VkSurfaceFormatKHR> formats;
    VkResult data_r = VK_ERROR_INITIALIZATION_FAILED;
    if (count_r == VK_SUCCESS && format_count > 0) {
      formats.resize(format_count);
      data_r = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
      last_wsi_action = classify_wsi_result(data_r);
    }
    if (!surface_formats_query_ok(count_r, format_count, data_r)) {
      if (last_wsi_action == WsiRecoverAction::None) {
        last_wsi_action = WsiRecoverAction::Fatal;
      }
      return false;
    }
    swapchain_format = formats[0].format;
    for (const auto& f : formats) {
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB) {
        swapchain_format = f.format;
        break;
      }
    }
  }

  VkAttachmentDescription color{};
  color.format = swapchain_format;
  color.samples = msaa_samples;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkAttachmentDescription depth{};
  depth.format = depth_format;
  depth.samples = msaa_samples;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;
  subpass.pDepthStencilAttachment = &depth_ref;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep.dstStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep.dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp_info.subpassCount = 1;
  rp_info.pSubpasses = &subpass;
  rp_info.dependencyCount = 1;
  rp_info.pDependencies = &dep;

  VkAttachmentDescription resolve{};
  VkAttachmentReference resolve_ref{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  std::array<VkAttachmentDescription, 3> attachments_msaa{};
  std::array<VkAttachmentDescription, 2> attachments_1x{};
  if (msaa_samples == VK_SAMPLE_COUNT_1_BIT) {
    // Direct-to-swapchain color + depth (no resolve attachment).
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments_1x = {color, depth};
    subpass.pResolveAttachments = nullptr;
    rp_info.attachmentCount = static_cast<uint32_t>(attachments_1x.size());
    rp_info.pAttachments = attachments_1x.data();
  } else {
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;  // resolved into swapchain
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    resolve.format = swapchain_format;
    resolve.samples = VK_SAMPLE_COUNT_1_BIT;
    resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    resolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments_msaa = {color, depth, resolve};
    subpass.pResolveAttachments = &resolve_ref;
    rp_info.attachmentCount = static_cast<uint32_t>(attachments_msaa.size());
    rp_info.pAttachments = attachments_msaa.data();
  }
  if (vkCreateRenderPass(device, &rp_info, nullptr, &render_pass) != VK_SUCCESS) {
    last_wsi_action = WsiRecoverAction::Fatal;
    return false;
  }

  const auto vert_code = read_file(shader_dir + "/textured_quad.vert.spv");
  const auto frag_code = read_file(shader_dir + "/textured_quad.frag.spv");
  if (vert_code.empty() || frag_code.empty()) {
    WDS_LOG("Failed to load SPIR-V from %s\n", shader_dir.c_str());
    last_wsi_action = WsiRecoverAction::Fatal;
    return false;
  }
  VkShaderModule vert = create_shader_module(device, vert_code);
  VkShaderModule frag = create_shader_module(device, frag_code);
  if (!vert || !frag) {
    last_wsi_action = WsiRecoverAction::Fatal;
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding_desc{};
  binding_desc.binding = 0;
  binding_desc.stride = sizeof(DrawVertex);
  binding_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  std::array<VkVertexInputAttributeDescription, 3> attrs{};
  attrs[0].location = 0;
  attrs[0].binding = 0;
  attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  attrs[0].offset = offsetof(DrawVertex, x);
  attrs[1].location = 1;
  attrs[1].binding = 0;
  attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;  // u*q, v*q, q
  attrs[1].offset = offsetof(DrawVertex, u);
  attrs[2].location = 2;
  attrs[2].binding = 0;
  attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
  attrs[2].offset = offsetof(DrawVertex, r);

  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding_desc;
  vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  vertex_input.pVertexAttributeDescriptions = attrs.data();

  VkPipelineInputAssemblyStateCreateInfo input_assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport_state{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = msaa_samples;

  VkPipelineDepthStencilStateCreateInfo depth_state{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_state.depthTestEnable = VK_TRUE;
  // Translucent sprites (arrows / ticks / holds) must not write depth or later
  // draws behind them fail the test and show clear-color black through the alpha.
  depth_state.depthWriteEnable = VK_FALSE;
  depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.blendEnable = VK_TRUE;
  // Straight alpha (matches textured_quad.frag / Sonolus sprite Draw).
  blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
  blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;

  std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT,
                                                  VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();

  VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = &depth_state;
  pipeline_info.pColorBlendState = &blend;
  pipeline_info.pDynamicState = &dynamic;
  pipeline_info.layout = pipeline_layout;
  pipeline_info.renderPass = render_pass;
  pipeline_info.subpass = 0;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                &pipeline) != VK_SUCCESS) {
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    last_wsi_action = WsiRecoverAction::Fatal;
    return false;
  }

  // Additive particles (hit VFX): SRC_ALPHA + ONE, no depth write so layers stack.
  VkPipelineDepthStencilStateCreateInfo depth_additive = depth_state;
  depth_additive.depthWriteEnable = VK_FALSE;
  VkPipelineColorBlendAttachmentState blend_add_attachment = blend_attachment;
  blend_add_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_add_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  VkPipelineColorBlendStateCreateInfo blend_add = blend;
  blend_add.pAttachments = &blend_add_attachment;
  pipeline_info.pDepthStencilState = &depth_additive;
  pipeline_info.pColorBlendState = &blend_add;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                &pipeline_additive) != VK_SUCCESS) {
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    last_wsi_action = WsiRecoverAction::Fatal;
    return false;
  }

  vkDestroyShaderModule(device, vert, nullptr);
  vkDestroyShaderModule(device, frag, nullptr);

  return true;
}

void log_physical_devices(VkInstance instance, VkSurfaceKHR surface, VkPhysicalDevice selected) {
  if (instance == VK_NULL_HANDLE) {
    WDS_LOG("vulkan: no instance to enumerate devices\n");
    return;
  }
  uint32_t device_count = 0;
  vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
  WDS_LOG("vulkan physical devices=%u\n", device_count);
  if (device_count == 0) {
    return;
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
  for (uint32_t i = 0; i < device_count; ++i) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(devices[i], &props);
    char name[48];
    std::snprintf(name, sizeof(name), "%s", props.deviceName);
    WDS_LOG("gpu[%u] '%s' type=%s vendor=0x%x id=0x%x api=%u.%u.%u drv=%u.%u.%u\n", i, name,
            physical_device_type_name(props.deviceType), props.vendorID, props.deviceID,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion), VK_VERSION_MAJOR(props.driverVersion),
            VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(devices[i], &mem);
    char heaps[120] = {};
    std::size_t used = 0;
    for (uint32_t h = 0; h < mem.memoryHeapCount && h < 4; ++h) {
      const unsigned long long mib = mem.memoryHeaps[h].size / (1024ull * 1024ull);
      const int n = std::snprintf(heaps + used, sizeof(heaps) - used, "%s%lluMiB", h ? "," : "",
                                  mib);
      if (n <= 0) {
        break;
      }
      used += static_cast<std::size_t>(n);
      if (used >= sizeof(heaps)) {
        break;
      }
    }
    WDS_LOG("gpu[%u] heaps=%u %s\n", i, mem.memoryHeapCount, heaps);

    if (surface == VK_NULL_HANDLE) {
      continue;
    }
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    if (family_count > 0) {
      vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families.data());
    }
    int gfx_present = -1;
    for (uint32_t q = 0; q < family_count; ++q) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], q, surface, &present);
      if ((families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
        gfx_present = static_cast<int>(q);
        break;
      }
    }
    const int is_sel = (selected != VK_NULL_HANDLE && devices[i] == selected) ? 1 : 0;
    WDS_LOG("gpu[%u] gfx_present_family=%d selected=%d\n", i, gfx_present, is_sel);
  }
}

void VulkanRenderer::Impl::destroy_render_pass_and_pipelines() {
  if (device == VK_NULL_HANDLE) {
    return;
  }
  if (pipeline) {
    vkDestroyPipeline(device, pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
  }
  if (pipeline_additive) {
    vkDestroyPipeline(device, pipeline_additive, nullptr);
    pipeline_additive = VK_NULL_HANDLE;
  }
  if (render_pass) {
    vkDestroyRenderPass(device, render_pass, nullptr);
    render_pass = VK_NULL_HANDLE;
  }
}


bool VulkanRenderer::create(const VulkanHostSurface& host) {
  destroy();
  if ((!host.create_surface && host.external_surface == VK_NULL_HANDLE) || !host.framebuffer_size) {
    WDS_LOG("VulkanHostSurface missing create_surface / framebuffer_size\n");
    return false;
  }
  bool committed = false;
  struct CreateGuard {
    VulkanRenderer* self = nullptr;
    bool* committed = nullptr;
    ~CreateGuard() {
      if (self != nullptr && committed != nullptr && !*committed) {
        self->destroy();
      }
    }
  } guard{this, &committed};
  impl_->create_surface = host.create_surface;
  impl_->acquire_surface = host.acquire_surface;
  impl_->framebuffer_size = host.framebuffer_size;
  impl_->owns_instance = host.external_instance == VK_NULL_HANDLE && host.renderer_owns_instance;
  impl_->owns_surface = host.external_surface == VK_NULL_HANDLE && host.renderer_owns_surface;
#if defined(_WIN32)
  impl_->win32_monitor = host.win32_monitor;
#endif
  impl_->preferred_msaa = preferred_msaa_;

  impl_->shader_dir = resolve_shader_dir();
  WDS_LOG("VulkanRenderer::create shader_dir=%s\n", impl_->shader_dir.c_str());

  if (host.external_instance != VK_NULL_HANDLE) {
    impl_->instance = host.external_instance;
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "WDS Preview";
  app.apiVersion = VK_API_VERSION_1_1;

  std::vector<const char*> extensions = host.instance_extensions;
  // MoltenVK is a portability ICD: without these, vkCreateInstance returns
  // VK_ERROR_INCOMPATIBLE_DRIVER / "Found no drivers!" on macOS.
  extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#if defined(_WIN32)
  if (impl_->instance == VK_NULL_HANDLE) {
    // Optional: required by VK_EXT_full_screen_exclusive on some ICDs.
    uint32_t inst_ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &inst_ext_count, nullptr);
    std::vector<VkExtensionProperties> inst_exts(inst_ext_count);
    if (inst_ext_count > 0) {
      vkEnumerateInstanceExtensionProperties(nullptr, &inst_ext_count, inst_exts.data());
    }
    for (const auto& e : inst_exts) {
      if (std::strcmp(e.extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0) {
        extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        impl_->surface_caps2_extension = true;
        WDS_LOG("enabling instance extension %s\n",
                VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        break;
      }
    }
  }
#endif
  WDS_LOG("instance extensions=%zu (incl. portability_enumeration)\n", extensions.size());
  for (const char* ext : extensions) {
    if (ext != nullptr) {
      WDS_LOG("instance extension %s\n", ext);
    }
  }

  {
    uint32_t loader_ver = VK_API_VERSION_1_0;
    const auto enumerate_ver = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerate_ver != nullptr) {
      enumerate_ver(&loader_ver);
    }
    WDS_LOG("vulkan loader api=%u.%u.%u request=%u.%u.%u\n", VK_VERSION_MAJOR(loader_ver),
            VK_VERSION_MINOR(loader_ver), VK_VERSION_PATCH(loader_ver),
            VK_VERSION_MAJOR(app.apiVersion), VK_VERSION_MINOR(app.apiVersion),
            VK_VERSION_PATCH(app.apiVersion));
  }

  if (host.external_instance == VK_NULL_HANDLE) {
    VkInstanceCreateInfo inst_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    inst_info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    inst_info.pApplicationInfo = &app;
    inst_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    inst_info.ppEnabledExtensionNames = extensions.data();
    const VkResult ir = vkCreateInstance(&inst_info, nullptr, &impl_->instance);
    if (ir != VK_SUCCESS) {
      WDS_LOG("vkCreateInstance failed result=%d (%s)\n", static_cast<int>(ir),
              vk_result_name(ir));
      return false;
    }
  } else {
    WDS_LOG("using host-owned Vulkan instance api=%u.%u.%u\n", VK_VERSION_MAJOR(app.apiVersion),
            VK_VERSION_MINOR(app.apiVersion), VK_VERSION_PATCH(app.apiVersion));
  }
#if defined(_WIN32)
  if (impl_->surface_caps2_extension) {
    impl_->get_surface_caps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
        vkGetInstanceProcAddr(impl_->instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    if (impl_->get_surface_caps2 == nullptr) {
      WDS_LOG("vkGetPhysicalDeviceSurfaceCapabilities2KHR missing; FSE capability query disabled\n");
      impl_->surface_caps2_extension = false;
    }
  }
#endif

  impl_->surface = host.external_surface != VK_NULL_HANDLE
                       ? host.external_surface
                       : host.create_surface(impl_->instance);
  if (impl_->surface == VK_NULL_HANDLE) {
    WDS_LOG("host.create_surface failed instance=%p\n",
            static_cast<void*>(impl_->instance));
    return false;
  }

  uint32_t device_count = 0;
  vkEnumeratePhysicalDevices(impl_->instance, &device_count, nullptr);
  if (device_count == 0) {
    WDS_LOG("no physical devices\n");
    return false;
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(impl_->instance, &device_count, devices.data());
  for (auto candidate : devices) {
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
    for (uint32_t i = 0; i < family_count; ++i) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, impl_->surface, &present);
      if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
        impl_->physical = candidate;
        impl_->graphics_family = i;
        break;
      }
    }
    if (impl_->physical != VK_NULL_HANDLE) {
      break;
    }
  }
  if (impl_->physical == VK_NULL_HANDLE) {
    WDS_LOG("no suitable graphics+present queue family\n");
    return false;
  }
  impl_->msaa_samples = impl_->pick_msaa_samples();
  log_physical_devices(impl_->instance, impl_->surface, impl_->physical);
  {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(impl_->physical, &props);
    std::strncpy(device_name_, props.deviceName, sizeof(device_name_) - 1);
    device_name_[sizeof(device_name_) - 1] = '\0';
    device_driver_version_ = props.driverVersion;
    device_api_version_ = props.apiVersion;
    WDS_LOG("selected GPU='%s' type=%s vendor=0x%x api=%u.%u.%u drv=%u.%u.%u "
            "queue_family=%u msaa=%u\n",
            props.deviceName, physical_device_type_name(props.deviceType), props.vendorID,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion), VK_VERSION_MAJOR(props.driverVersion),
            VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion),
            impl_->graphics_family, static_cast<unsigned>(impl_->msaa_samples));
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = impl_->graphics_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  std::vector<const char*> device_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  impl_->fse_extension = false;
#if defined(_WIN32)
  impl_->surface_caps2_extension =
      impl_->surface_caps2_extension && impl_->get_surface_caps2 != nullptr;
#endif
  {
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(impl_->physical, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> avail(ext_count);
    if (ext_count > 0) {
      vkEnumerateDeviceExtensionProperties(impl_->physical, nullptr, &ext_count, avail.data());
    }
    const char* portability_subset = "VK_KHR_portability_subset";
    for (const auto& e : avail) {
      if (std::strcmp(e.extensionName, portability_subset) == 0) {
        device_exts.push_back(portability_subset);
        WDS_LOG("enabling device extension %s\n", portability_subset);
      }
#if defined(_WIN32)
      if (std::strcmp(e.extensionName, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME) == 0) {
        device_exts.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
        impl_->fse_extension = true;
        WDS_LOG("enabling device extension %s\n", VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
      }
#endif
    }
  }

  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = static_cast<uint32_t>(device_exts.size());
  device_info.ppEnabledExtensionNames = device_exts.data();
  {
    const VkResult dr = vkCreateDevice(impl_->physical, &device_info, nullptr, &impl_->device);
    if (dr != VK_SUCCESS) {
      WDS_LOG("vkCreateDevice failed result=%d (%s)\n", static_cast<int>(dr), vk_result_name(dr));
      return false;
    }
  }
  vkGetDeviceQueue(impl_->device, impl_->graphics_family, 0, &impl_->graphics_queue);
#if defined(_WIN32)
  if (impl_->fse_extension) {
    impl_->acquire_fse = reinterpret_cast<PFN_vkAcquireFullScreenExclusiveModeEXT>(
        vkGetDeviceProcAddr(impl_->device, "vkAcquireFullScreenExclusiveModeEXT"));
    impl_->release_fse = reinterpret_cast<PFN_vkReleaseFullScreenExclusiveModeEXT>(
        vkGetDeviceProcAddr(impl_->device, "vkReleaseFullScreenExclusiveModeEXT"));
    if (impl_->acquire_fse == nullptr || impl_->release_fse == nullptr) {
      WDS_LOG("FSE entry points missing; disabling exclusive path\n");
      impl_->fse_extension = false;
      impl_->acquire_fse = nullptr;
      impl_->release_fse = nullptr;
    }
  }
#endif

  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = impl_->graphics_family;
  if (vkCreateCommandPool(impl_->device, &pool_info, nullptr, &impl_->command_pool) != VK_SUCCESS) {
    return false;
  }

  VkCommandPoolCreateInfo upload_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  upload_pool_info.flags = upload_command_pool_create_flags();
  upload_pool_info.queueFamilyIndex = impl_->graphics_family;
  if (vkCreateCommandPool(impl_->device, &upload_pool_info, nullptr,
                          &impl_->upload_command_pool) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(impl_->device, &layout_info, nullptr, &impl_->descriptor_layout) !=
      VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push.offset = 0;
  push.size = sizeof(float) * 16;
  VkPipelineLayoutCreateInfo pl_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl_info.setLayoutCount = 1;
  pl_info.pSetLayouts = &impl_->descriptor_layout;
  pl_info.pushConstantRangeCount = 1;
  pl_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(impl_->device, &pl_info, nullptr, &impl_->pipeline_layout) !=
      VK_SUCCESS) {
    return false;
  }

  if (!impl_->create_render_pass_and_pipelines()) {
    return false;
  }

  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(impl_->device, &sampler_info, nullptr, &impl_->sampler) != VK_SUCCESS) {
    return false;
  }
  VkSamplerCreateInfo nearest_info = sampler_info;
  nearest_info.magFilter = VK_FILTER_NEAREST;
  nearest_info.minFilter = VK_FILTER_NEAREST;
  if (vkCreateSampler(impl_->device, &nearest_info, nullptr, &impl_->sampler_nearest) !=
      VK_SUCCESS) {
    return false;
  }

  if (!impl_->create_descriptor_block(kDescriptorPoolInitialCapacity)) {
    return false;
  }

  int fb_w = 0, fb_h = 0;
  impl_->framebuffer_size(&fb_w, &fb_h);
  if (!impl_->create_swapchain(fb_w, fb_h)) {
    return false;
  }
  width_ = static_cast<int>(impl_->swapchain_extent.width);
  height_ = static_cast<int>(impl_->swapchain_extent.height);

  VkCommandBufferAllocateInfo cmd_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmd_alloc.commandPool = impl_->command_pool;
  cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_alloc.commandBufferCount = kMaxFramesInFlight;
  if (vkAllocateCommandBuffers(impl_->device, &cmd_alloc, impl_->command_buffers.data()) !=
      VK_SUCCESS) {
    return false;
  }

  VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (int i = 0; i < kMaxFramesInFlight; ++i) {
    if (!create_sync_object_ok(
            vkCreateSemaphore(impl_->device, &sem_info, nullptr, &impl_->image_available[i])) ||
        !create_sync_object_ok(
            vkCreateSemaphore(impl_->device, &sem_info, nullptr, &impl_->render_finished[i])) ||
        !create_sync_object_ok(
            vkCreateFence(impl_->device, &fence_info, nullptr, &impl_->in_flight[i]))) {
      return false;
    }
  }

  impl_->textures.resize(1);  // slot 0 unused
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (!impl_->ensure_frame_vertex_capacity(i, kRingVertexCapacityBytes)) {
      return false;
    }
  }

  emit_health(RendererHealthEvent::Created);
  committed = true;
  return true;
}

void VulkanRenderer::destroy() {
  if (!impl_) {
    emit_health(RendererHealthEvent::Destroyed);
    return;
  }
  emit_health(RendererHealthEvent::Destroyed);

  if (impl_->device != VK_NULL_HANDLE) {
    const VkResult idle = vkDeviceWaitIdle(impl_->device);
    const bool wait_ok = idle == VK_SUCCESS;
    const bool device_lost = idle == VK_ERROR_DEVICE_LOST;
    // Wait first; device-lost (or last-resort teardown) then releases in-flight
    // upload cmd/staging. Do not free those while the GPU may still use them.
    if (upload_destroy_may_release_pending(wait_ok, device_lost) || !wait_ok) {
      impl_->force_release_pending_uploads();
    }

    for (size_t i = 1; i < impl_->textures.size(); ++i) {
      destroy_texture(static_cast<TextureId>(i));
    }
    impl_->textures.clear();

    impl_->destroy_frame_vertices();

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
      if (impl_->image_available[i])
        vkDestroySemaphore(impl_->device, impl_->image_available[i], nullptr);
      if (impl_->render_finished[i])
        vkDestroySemaphore(impl_->device, impl_->render_finished[i], nullptr);
      if (impl_->in_flight[i]) vkDestroyFence(impl_->device, impl_->in_flight[i], nullptr);
    }

    impl_->cleanup_swapchain();
    if (impl_->pipeline) vkDestroyPipeline(impl_->device, impl_->pipeline, nullptr);
    if (impl_->pipeline_additive) vkDestroyPipeline(impl_->device, impl_->pipeline_additive, nullptr);
    if (impl_->pipeline_layout) vkDestroyPipelineLayout(impl_->device, impl_->pipeline_layout, nullptr);
    if (impl_->render_pass) vkDestroyRenderPass(impl_->device, impl_->render_pass, nullptr);
    if (impl_->sampler) vkDestroySampler(impl_->device, impl_->sampler, nullptr);
    if (impl_->sampler_nearest) vkDestroySampler(impl_->device, impl_->sampler_nearest, nullptr);
    for (auto& block : impl_->descriptor_blocks) {
      if (block.pool) vkDestroyDescriptorPool(impl_->device, block.pool, nullptr);
    }
    impl_->descriptor_blocks.clear();
    if (impl_->descriptor_layout)
      vkDestroyDescriptorSetLayout(impl_->device, impl_->descriptor_layout, nullptr);
    if (impl_->upload_command_pool)
      vkDestroyCommandPool(impl_->device, impl_->upload_command_pool, nullptr);
    if (impl_->command_pool) vkDestroyCommandPool(impl_->device, impl_->command_pool, nullptr);
    vkDestroyDevice(impl_->device, nullptr);
    impl_->device = VK_NULL_HANDLE;
  }

  // Half-init path (instance/surface without device) must still free WSI objects.
  if (impl_->surface && impl_->instance && impl_->owns_surface) {
    vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
    impl_->surface = VK_NULL_HANDLE;
  }
  if (impl_->instance && impl_->owns_instance) {
    vkDestroyInstance(impl_->instance, nullptr);
    impl_->instance = VK_NULL_HANDLE;
  }

  *impl_ = Impl{};
  bind_path_diag();
  emit_health(RendererHealthEvent::Destroyed);
}

void VulkanRenderer::device_wait_idle() {
  if (!impl_ || impl_->device == VK_NULL_HANDLE) {
    return;
  }
  (void)impl_->device_wait_idle_result();
}

void VulkanRenderer::set_exclusive_fullscreen_desired(bool desired) {
  if (!impl_) {
    return;
  }
#if !defined(_WIN32)
  (void)desired;
  impl_->exclusive_fullscreen_desired = false;
  return;
#else
  if (impl_->exclusive_fullscreen_desired == desired) {
    return;
  }
  impl_->exclusive_fullscreen_desired = desired;
  if (!ready_ || impl_->device == VK_NULL_HANDLE) {
    return;
  }
  // Leaving exclusive FS: drop FSE before the next swapchain recreate.
  if (!desired) {
    impl_->release_fullscreen_exclusive_internal();
  }
  // Force swapchain recreate so FSE pNext / acquire matches the new mode.
  impl_->swapchain_occluded = true;
  int fb_w = width_;
  int fb_h = height_;
  if (impl_->framebuffer_size) {
    impl_->framebuffer_size(&fb_w, &fb_h);
  }
  if (fb_w > 0 && fb_h > 0) {
    (void)resize(fb_w, fb_h);
  }
#endif
}

bool VulkanRenderer::exclusive_fullscreen_desired() const noexcept {
  return impl_ != nullptr && impl_->exclusive_fullscreen_desired;
}

bool VulkanRenderer::exclusive_fullscreen_extension() const noexcept {
  return impl_ != nullptr && impl_->fse_extension;
}

bool VulkanRenderer::exclusive_fullscreen_acquired() const noexcept {
  return impl_ != nullptr && impl_->fse_acquired;
}

int64_t VulkanRenderer::last_fence_wait_us() const noexcept {
  return impl_ != nullptr ? impl_->last_fence_wait_us : 0;
}

int64_t VulkanRenderer::last_acquire_wait_us() const noexcept {
  return impl_ != nullptr ? impl_->last_acquire_wait_us : 0;
}

int64_t VulkanRenderer::last_present_us() const noexcept {
  return impl_ != nullptr ? impl_->last_present_us : 0;
}

int64_t VulkanRenderer::last_gpu_submit_us() const noexcept {
  return impl_ != nullptr ? impl_->last_gpu_submit_us : 0;
}

VulkanRenderer::DescriptorPoolDiagnostics VulkanRenderer::descriptor_pool_diagnostics()
    const noexcept {
  DescriptorPoolDiagnostics out;
  if (impl_ == nullptr) {
    return out;
  }
  out.block_count = static_cast<uint32_t>(impl_->descriptor_blocks.size());
  for (const auto& block : impl_->descriptor_blocks) {
    out.live_sets += block.live;
  }
  return out;
}

void VulkanRenderer::release_fullscreen_exclusive() {
  if (!impl_) {
    return;
  }
  impl_->release_fullscreen_exclusive_internal();
  impl_->exclusive_fullscreen_desired = false;
}

void VulkanRenderer::emit_health(RendererHealthEvent event, VkResult result) noexcept {
  const bool was_unrecoverable = renderer_health_unrecoverable(health_);
  health_ = apply_renderer_health(health_, event);
  ready_ = renderer_health_ready(health_);
  if (event == RendererHealthEvent::Destroyed) {
    fatal_reported_ = false;
    return;
  }
  if (was_unrecoverable || !renderer_health_unrecoverable(health_) || fatal_reported_) {
    return;
  }
  fatal_reported_ = true;
  VkResult shown = result;
  if (shown == VK_SUCCESS) {
    shown = (event == RendererHealthEvent::DeviceLost) ? VK_ERROR_DEVICE_LOST : result;
  }
  char detail[512];
  if (shown != VK_SUCCESS) {
    std::snprintf(detail, sizeof(detail), "%s (%d), device=%s", vk_result_name(shown),
                  static_cast<int>(shown), device_name_[0] != '\0' ? device_name_ : "?");
  } else {
    std::snprintf(detail, sizeof(detail), "Fatal, device=%s",
                  device_name_[0] != '\0' ? device_name_ : "?");
  }
  wds::common::report_fatal("Vulkan device lost", detail);
}

bool VulkanRenderer::apply_zero_extent_now() noexcept {
  const bool live = impl_ != nullptr && impl_->swapchain != VK_NULL_HANDLE;
  health_ = apply_zero_extent(health_, live);
  ready_ = renderer_health_ready(health_);
  if (impl_ != nullptr) {
    impl_->swapchain_occluded = live && health_ == RendererHealth::Occluded;
  }
  return live && health_ == RendererHealth::Occluded;
}

bool VulkanRenderer::apply_swapchain_create_failure() {
  if (impl_ == nullptr) {
    return false;
  }
  const bool live = impl_->swapchain != VK_NULL_HANDLE;
  const WsiRecoverAction action = impl_->last_wsi_action;
  const RendererHealth next = apply_swapchain_create_failure_health(health_, action, live);
  (void)swapchain_create_failure_reports_occluded(action, live);
  switch (action) {
    case WsiRecoverAction::RecreateSurfaceAndSwapchain:
      emit_health(RendererHealthEvent::SurfaceLost);
      return false;
    case WsiRecoverAction::DeviceLost:
      emit_health(RendererHealthEvent::DeviceLost);
      return false;
    case WsiRecoverAction::Fatal:
      emit_health(RendererHealthEvent::Fatal);
      return false;
    case WsiRecoverAction::RecreateSwapchain:
      if (next == RendererHealth::Ready && health_ != RendererHealth::Ready) {
        emit_health(RendererHealthEvent::RecoveredExtent);
      }
      return false;
    case WsiRecoverAction::None:
      if (!live) {
        return apply_zero_extent_now();
      }
      break;
  }
  return false;
}

bool VulkanRenderer::check_device_result(VkResult result) noexcept {
  if (result == VK_SUCCESS) {
    return true;
  }
  emit_health(result == VK_ERROR_DEVICE_LOST ? RendererHealthEvent::DeviceLost
                                            : RendererHealthEvent::Fatal,
              result);
  return false;
}

bool VulkanRenderer::apply_wsi_action(WsiRecoverAction action) {
  switch (action) {
    case WsiRecoverAction::None:
      return true;
    case WsiRecoverAction::RecreateSwapchain:
      return recover_swapchain();
    case WsiRecoverAction::RecreateSurfaceAndSwapchain:
      return recover_surface_and_swapchain();
    case WsiRecoverAction::DeviceLost:
      emit_health(RendererHealthEvent::DeviceLost);
      return false;
    case WsiRecoverAction::Fatal:
      emit_health(RendererHealthEvent::Fatal);
      return false;
  }
  emit_health(RendererHealthEvent::Fatal);
  return false;
}

bool VulkanRenderer::recover_swapchain() {
  if (impl_ == nullptr || impl_->device == VK_NULL_HANDLE ||
      renderer_health_unrecoverable(health_)) {
    return false;
  }
  if (health_ == RendererHealth::SurfaceLost) {
    return recover_surface_and_swapchain();
  }
  int w = 0, h = 0;
  if (impl_->framebuffer_size) {
    impl_->framebuffer_size(&w, &h);
  }
  if (w <= 0 || h <= 0) {
    return apply_zero_extent_now();
  }
  if (!impl_->create_swapchain(w, h)) {
    return apply_swapchain_create_failure();
  }
  if (impl_->swapchain_occluded) {
    return apply_zero_extent_now();
  }
  width_ = static_cast<int>(impl_->swapchain_extent.width);
  height_ = static_cast<int>(impl_->swapchain_extent.height);
  emit_health(RendererHealthEvent::RecoveredExtent);
  return true;
}

bool VulkanRenderer::recover_surface_and_swapchain() {
  if (impl_ == nullptr || impl_->device == VK_NULL_HANDLE || impl_->instance == VK_NULL_HANDLE ||
      !impl_->create_surface || renderer_health_unrecoverable(health_)) {
    emit_health(RendererHealthEvent::SurfaceLost);
    return false;
  }

  if (!check_device_result(vkDeviceWaitIdle(impl_->device))) {
    return false;
  }
  // A host-owned surface must be recreated by the host (for example Qt after
  // a platform surface event); never destroy or reacquire it here.
  impl_->cleanup_swapchain();
  if (impl_->owns_surface && impl_->surface != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
    impl_->surface = VK_NULL_HANDLE;
  }

  if (!impl_->owns_surface) {
    if (!impl_->acquire_surface) {
      emit_health(RendererHealthEvent::SurfaceLost);
      return false;
    }
    impl_->surface = impl_->acquire_surface();
  } else {
    impl_->surface = impl_->create_surface(impl_->instance);
  }
  if (impl_->surface == VK_NULL_HANDLE) {
    WDS_LOG("recover_surface: create_surface callback failed\n");
    emit_health(RendererHealthEvent::SurfaceLost);
    return false;
  }

  VkBool32 present = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(impl_->physical, impl_->graphics_family, impl_->surface,
                                       &present);
  if (present != VK_TRUE) {
    WDS_LOG("recover_surface: graphics_family=%u cannot present\n", impl_->graphics_family);
    emit_health(RendererHealthEvent::SurfaceLost);
    return false;
  }

  int w = 0, h = 0;
  if (impl_->framebuffer_size) {
    impl_->framebuffer_size(&w, &h);
  }
  if (w <= 0 || h <= 0) {
    // Old swapchain is already gone; 0×0 is not Occluded.
    return apply_zero_extent_now();
  }
  if (!impl_->create_swapchain(w, h)) {
    if (impl_->last_wsi_action == WsiRecoverAction::DeviceLost ||
        impl_->last_wsi_action == WsiRecoverAction::Fatal) {
      return apply_swapchain_create_failure();
    }
    emit_health(RendererHealthEvent::SurfaceLost);
    return false;
  }
  if (impl_->swapchain_occluded || impl_->swapchain == VK_NULL_HANDLE) {
    return apply_zero_extent_now();
  }
  width_ = static_cast<int>(impl_->swapchain_extent.width);
  height_ = static_cast<int>(impl_->swapchain_extent.height);
  emit_health(RendererHealthEvent::SurfaceRecovered);
  return true;
}

bool VulkanRenderer::resize(int width, int height) {
  if (impl_ == nullptr || renderer_health_unrecoverable(health_)) {
    return false;
  }
  if (health_ == RendererHealth::SurfaceLost) {
    return recover_surface_and_swapchain();
  }
  if (!ready_ && health_ != RendererHealth::Occluded) {
    return false;
  }
  if (width <= 0 || height <= 0) {
    return apply_zero_extent_now();
  }
  if (resize_same_extent_short_circuits(width, height, width_, height_, impl_->swapchain_occluded,
                                        impl_->swapchain != VK_NULL_HANDLE)) {
    return true;
  }
  if (!impl_->create_swapchain(width, height)) {
    return apply_swapchain_create_failure();
  }
  if (impl_->swapchain_occluded) {
    return apply_zero_extent_now();
  }
  width_ = static_cast<int>(impl_->swapchain_extent.width);
  height_ = static_cast<int>(impl_->swapchain_extent.height);
  emit_health(RendererHealthEvent::RecoveredExtent);
  return true;
}

TextureInfo VulkanRenderer::create_texture_rgba(const unsigned char* pixels, int width, int height,
                                                bool nearest) {
  TextureInfo info;
  if (!ready_ || pixels == nullptr || width <= 0 || height <= 0) {
    return info;
  }
  const UploadHealthDelta reap = impl_->reap_pending_uploads();
  if (reap.apply) {
    emit_health(reap.event);
    if (!ready_) {
      return info;
    }
  }
  impl_->reap_retired_textures(impl_->frame_seq);
  PathTimer timed(path_diag_enabled_, &path_diag_, RendererPathSegment::CreateTextureRgba);
  const TextureId id = impl_->alloc_texture_slot();
  GpuTexture& tex = impl_->textures[id];
  tex.nearest = nearest;
  const UploadResult uploaded = impl_->upload_texture_pixels(tex, id, pixels, width, height);
  if (!uploaded.ok()) {
    const UploadHealthDelta delta = upload_health_delta(uploaded.result, uploaded.stage);
    if (delta.apply) {
      emit_health(delta.event);
    }
    if (impl_->texture_has_pending_upload(id)) {
      tex.alive = false;
      tex.retire_after_seq = impl_->frame_seq;
    } else {
      impl_->destroy_gpu_texture_resources(tex);
    }
    WDS_LOG("create_texture_rgba failed stage=%s result=%d (%s)\n",
            upload_stage_name(uploaded.stage), static_cast<int>(uploaded.result),
            vk_result_name(uploaded.result));
    return {};
  }
  info.id = id;
  info.width = width;
  info.height = height;
  info.u0 = 0.0f;
  info.v0 = 0.0f;
  info.u1 = 1.0f;
  info.v1 = 1.0f;
  return info;
}

void VulkanRenderer::destroy_texture(TextureId id) {
  if (!impl_ || id == kInvalidTextureId || id >= impl_->textures.size()) {
    return;
  }
  GpuTexture& tex = impl_->textures[id];
  if (!tex.alive && tex.image == VK_NULL_HANDLE) {
    return;
  }
  tex.alive = false;
  if (!ready_ || impl_->device == VK_NULL_HANDLE) {
    impl_->destroy_gpu_texture_resources(tex);
    return;
  }
  // Delay free until in-flight frames that sampled this image have completed.
  // Do not vkDeviceWaitIdle on the UI thread (CJK atlas / DPI hitch).
  tex.retire_after_seq =
      impl_->frame_seq + static_cast<uint64_t>(std::max(1, impl_->frames_in_flight));
}

bool VulkanRenderer::draw_frame(const DrawBatch& batch, const ScreenBounds& screen, float clear_r,
                                float clear_g, float clear_b, const DrawBatch* additive,
                                const DrawBatch* post_overlay, const DrawBatch* post_overlay2,
                                const ScissorRect* additive_scissor, const DrawBatch* mid_overlay) {
  if (impl_ == nullptr || renderer_health_unrecoverable(health_)) {
    return false;
  }
  auto reap_completed_keep_health = [&]() {
    if (!draw_frame_reaps_completed_uploads_before_zero_extent_return()) {
      return;
    }
    (void)impl_->reap_pending_uploads();
    if (!draw_frame_zero_extent_reap_applies_health()) {
      impl_->reap_retired_textures(impl_->frame_seq);
    }
  };
  if (health_ == RendererHealth::SurfaceLost) {
    if (!recover_surface_and_swapchain()) {
      return false;
    }
    if (health_ != RendererHealth::Ready) {
      reap_completed_keep_health();
      return health_ == RendererHealth::Occluded;
    }
  }
  const bool live_swapchain = impl_->swapchain != VK_NULL_HANDLE;
  if (draw_frame_blocks_before_recovery(health_, ready_, live_swapchain)) {
    return false;
  }

  // Recover from minimize / fullscreen 0×0 transitions before acquire.
  // ready && swapchain null must reach this block (RecreateSwapchain after teardown).
  if (impl_->swapchain_occluded || impl_->swapchain == VK_NULL_HANDLE) {
    int w = 0, h = 0;
    if (impl_->framebuffer_size) {
      impl_->framebuffer_size(&w, &h);
    }
    if (w <= 0 || h <= 0) {
      reap_completed_keep_health();
      return apply_zero_extent_now();
    }
    if (!resize(w, h) || impl_->swapchain_occluded || impl_->swapchain == VK_NULL_HANDLE) {
      reap_completed_keep_health();
      return health_ == RendererHealth::Occluded;
    }
  }

  {
    // Drain still-in-flight uploads before this frame samples them. Empty
    // pending is a no-op so the steady-state present path stays cheap.
    const UploadHealthDelta pending = impl_->wait_pending_uploads();
    if (pending.apply) {
      emit_health(pending.event);
      if (renderer_health_unrecoverable(health_)) {
        return false;
      }
    }
    impl_->reap_retired_textures(impl_->frame_seq);
  }

  using clock = std::chrono::steady_clock;
  const uint32_t frame = impl_->frame_index;
  const auto fence_t0 = clock::now();
  if (!check_device_result(vkWaitForFences(impl_->device, 1, &impl_->in_flight[frame], VK_TRUE,
                                          UINT64_MAX))) {
    return false;
  }
  impl_->last_fence_wait_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - fence_t0).count();
  ++impl_->frame_seq;
  impl_->reap_retired_textures(impl_->frame_seq);

  uint32_t image_index = 0;
  const auto acquire_t0 = clock::now();
  VkResult acquire = vkAcquireNextImageKHR(impl_->device, impl_->swapchain, UINT64_MAX,
                                           impl_->image_available[frame], VK_NULL_HANDLE,
                                           &image_index);
  impl_->last_acquire_wait_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - acquire_t0).count();
  impl_->note_fullscreen_exclusive_lost("vkAcquireNextImageKHR", acquire);
  const WsiRecoverAction acquire_action = classify_wsi_result(acquire);
  bool recreate_swapchain_after_present = false;
  if (acquire_action == WsiRecoverAction::RecreateSwapchain && acquire == VK_SUBOPTIMAL_KHR) {
    recreate_swapchain_after_present = true;
  } else if (acquire_action != WsiRecoverAction::None) {
    return apply_wsi_action(acquire_action);
  }

  // Do not reuse a swapchain image that is still referenced by an in-flight submit.
  if (image_index < impl_->images_in_flight.size() &&
      impl_->images_in_flight[image_index] != VK_NULL_HANDLE) {
    if (!check_device_result(vkWaitForFences(impl_->device, 1, &impl_->images_in_flight[image_index],
                                            VK_TRUE, UINT64_MAX))) {
      return false;
    }
  }

  const size_t mid_verts = mid_overlay ? mid_overlay->vertex_count() : 0;
  const size_t additive_verts = additive ? additive->vertex_count() : 0;
  const size_t post_verts = post_overlay ? post_overlay->vertex_count() : 0;
  const size_t post2_verts = post_overlay2 ? post_overlay2->vertex_count() : 0;
  const size_t total_verts =
      batch.vertex_count() + mid_verts + additive_verts + post_verts + post2_verts;
  const size_t bytes = total_verts * sizeof(DrawVertex);
  if (!impl_->remap_frame_vertices()) {
    return false;
  }
  if (!impl_->ensure_frame_vertex_capacity(frame, bytes)) {
    // Fence still signaled (not reset yet). Drain the acquire semaphore only.
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    drain.waitSemaphoreCount = 1;
    drain.pWaitSemaphores = &impl_->image_available[frame];
    drain.pWaitDstStageMask = &wait_stage;
    const VkResult drain_r = vkQueueSubmit(impl_->graphics_queue, 1, &drain, VK_NULL_HANDLE);
    if (!check_device_result(drain_r)) {
      return false;
    }
    vkQueueWaitIdle(impl_->graphics_queue);
    return false;
  }

  // Reset only once capacity is ensured so a failed path cannot leave the fence unsignaled.
  if (!check_device_result(vkResetFences(impl_->device, 1, &impl_->in_flight[frame]))) {
    return false;
  }

  auto& vb = impl_->frame_vertices[frame];
  auto& bucket_first_vertex = impl_->bucket_first_vertex;
  auto& mid_first_vertex = impl_->mid_first_vertex;
  auto& additive_first_vertex = impl_->additive_first_vertex;
  auto& post_first_vertex = impl_->post_first_vertex;
  auto& post2_first_vertex = impl_->post2_first_vertex;
  bucket_first_vertex.clear();
  mid_first_vertex.clear();
  additive_first_vertex.clear();
  post_first_vertex.clear();
  post2_first_vertex.clear();
  bucket_first_vertex.reserve(batch.buckets.size());
  if (mid_overlay) {
    mid_first_vertex.reserve(mid_overlay->buckets.size());
  }
  if (additive) {
    additive_first_vertex.reserve(additive->buckets.size());
  }
  if (post_overlay) {
    post_first_vertex.reserve(post_overlay->buckets.size());
  }
  if (post_overlay2) {
    post2_first_vertex.reserve(post_overlay2->buckets.size());
  }
  {
    auto* dst = static_cast<DrawVertex*>(vb.mapped);
    uint32_t cursor = 0;
    auto copy_buckets = [&](const DrawBatch& src, std::vector<uint32_t>& first_vertex) {
      for (const auto& bucket : src.buckets) {
        first_vertex.push_back(cursor);
        if (!bucket.vertices.empty()) {
          std::memcpy(dst + cursor, bucket.vertices.data(),
                      bucket.vertices.size() * sizeof(DrawVertex));
          cursor += static_cast<uint32_t>(bucket.vertices.size());
        }
      }
    };
    copy_buckets(batch, bucket_first_vertex);
    if (mid_overlay) {
      copy_buckets(*mid_overlay, mid_first_vertex);
    }
    if (additive) {
      copy_buckets(*additive, additive_first_vertex);
    }
    if (post_overlay) {
      copy_buckets(*post_overlay, post_first_vertex);
    }
    if (post_overlay2) {
      copy_buckets(*post_overlay2, post2_first_vertex);
    }
  }

  VkCommandBuffer cmd = impl_->command_buffers[frame];
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cmd, &begin);

  std::array<VkClearValue, 2> clears{};
  clears[0].color = {{clear_r, clear_g, clear_b, 1.0f}};
  clears[1].depthStencil = {1.0f, 0};

  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = impl_->render_pass;
  rp.framebuffer = impl_->framebuffers[image_index];
  rp.renderArea.extent = impl_->swapchain_extent;
  rp.clearValueCount = static_cast<uint32_t>(clears.size());
  rp.pClearValues = clears.data();
  vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

  // OpenGL-style world Y-up + Vulkan framebuffer Y-down → flip via negative viewport height
  // (VK_KHR_maintenance1 / Vulkan 1.1). Cull is disabled, so winding stays fine.
  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = static_cast<float>(impl_->swapchain_extent.height);
  viewport.width = static_cast<float>(impl_->swapchain_extent.width);
  viewport.height = -static_cast<float>(impl_->swapchain_extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = impl_->swapchain_extent;
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  float mvp[16];
  ortho_rh(screen.l, screen.r, screen.b, screen.t, -1.0f, 1.0f, mvp);
  vkCmdPushConstants(cmd, impl_->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), mvp);

  auto draw_buckets = [&](VkPipeline pipeline, const DrawBatch& src,
                          const std::vector<uint32_t>& first_vertex) {
    if (src.vertex_count() == 0) {
      return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buffer, &offset);
    for (size_t bi = 0; bi < src.buckets.size(); ++bi) {
      const auto& bucket = src.buckets[bi];
      if (bucket.vertices.empty() || bucket.texture == kInvalidTextureId ||
          bucket.texture >= impl_->textures.size() || !impl_->textures[bucket.texture].alive) {
        continue;
      }
      impl_->textures[bucket.texture].sampled_seq = impl_->frame_seq;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline_layout, 0, 1,
                              &impl_->textures[bucket.texture].descriptor, 0, nullptr);
      vkCmdDraw(cmd, static_cast<uint32_t>(bucket.vertices.size()), 1, first_vertex[bi], 0);
    }
  };

  draw_buckets(impl_->pipeline, batch, bucket_first_vertex);
  if (mid_overlay && mid_verts > 0) {
    draw_buckets(impl_->pipeline, *mid_overlay, mid_first_vertex);
  }
  if (additive && additive_verts > 0 && impl_->pipeline_additive) {
    VkRect2D add_scissor = scissor;
    if (additive_scissor != nullptr && additive_scissor->valid()) {
      const int fb_w = static_cast<int>(impl_->swapchain_extent.width);
      const int fb_h = static_cast<int>(impl_->swapchain_extent.height);
      const int x0 = std::clamp(additive_scissor->x, 0, fb_w);
      const int y0 = std::clamp(additive_scissor->y, 0, fb_h);
      const int x1 = std::clamp(additive_scissor->x + additive_scissor->w, 0, fb_w);
      const int y1 = std::clamp(additive_scissor->y + additive_scissor->h, 0, fb_h);
      add_scissor.offset.x = static_cast<int32_t>(x0);
      add_scissor.offset.y = static_cast<int32_t>(y0);
      add_scissor.extent.width = static_cast<uint32_t>(std::max(0, x1 - x0));
      add_scissor.extent.height = static_cast<uint32_t>(std::max(0, y1 - y0));
    }
    vkCmdSetScissor(cmd, 0, 1, &add_scissor);
    draw_buckets(impl_->pipeline_additive, *additive, additive_first_vertex);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
  }
  // Modal / top overlays: separate pass so sticky bucket indices in `batch` cannot bury them.
  if (post_overlay && post_verts > 0) {
    draw_buckets(impl_->pipeline, *post_overlay, post_first_vertex);
  }
  if (post_overlay2 && post2_verts > 0) {
    draw_buckets(impl_->pipeline, *post_overlay2, post2_first_vertex);
  }

  vkCmdEndRenderPass(cmd);
  vkEndCommandBuffer(cmd);

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &impl_->image_available[frame];
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &impl_->render_finished[frame];
  const auto submit_t0 = clock::now();
  const VkResult submit_r =
      vkQueueSubmit(impl_->graphics_queue, 1, &submit, impl_->in_flight[frame]);
  if (submit_r != VK_SUCCESS) {
    if (submit_r == VK_ERROR_DEVICE_LOST) {
      emit_health(RendererHealthEvent::DeviceLost, submit_r);
      return false;
    }
    emit_health(RendererHealthEvent::Fatal, submit_r);
    // Fence was reset but not submitted — signal it via drain so the next frame wait returns.
    VkPipelineStageFlags drain_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    drain.waitSemaphoreCount = 1;
    drain.pWaitSemaphores = &impl_->image_available[frame];
    drain.pWaitDstStageMask = &drain_stage;
    const VkResult drain_r =
        vkQueueSubmit(impl_->graphics_queue, 1, &drain, impl_->in_flight[frame]);
    if (drain_r == VK_ERROR_DEVICE_LOST) {
      emit_health(RendererHealthEvent::DeviceLost, drain_r);
      return false;
    }
    if (drain_r == VK_SUCCESS) {
      vkQueueWaitIdle(impl_->graphics_queue);
    }
    return false;
  }
  impl_->images_in_flight[image_index] = impl_->in_flight[frame];
  impl_->last_gpu_submit_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - submit_t0).count();

  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &impl_->render_finished[frame];
  present.swapchainCount = 1;
  present.pSwapchains = &impl_->swapchain;
  present.pImageIndices = &image_index;
  const auto present_t0 = clock::now();
  VkResult present_result = vkQueuePresentKHR(impl_->graphics_queue, &present);
  impl_->last_present_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - present_t0).count();
  impl_->note_fullscreen_exclusive_lost("vkQueuePresentKHR", present_result);
  const uint32_t fif = static_cast<uint32_t>(std::max(1, impl_->frames_in_flight));
  impl_->frame_index = (frame + 1) % fif;
  const WsiRecoverAction present_action = classify_wsi_result(present_result);
  if (present_action == WsiRecoverAction::None) {
    if (recreate_swapchain_after_present) {
      return recover_swapchain();
    }
    return true;
  }
  return apply_wsi_action(present_action);
}

}  // namespace wds::renderer
