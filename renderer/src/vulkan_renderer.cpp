#include "wds/renderer/vulkan_renderer.hpp"
#include "wds/renderer/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wds::renderer {
namespace {

constexpr int kMaxFramesInFlight = 3;
// Prefer triple-buffer FIFO: with 2 images + exclusive fullscreen, a slightly late
// frame drops to a 0ms/33ms hitch cadence; a third image absorbs one missed vsync.
constexpr uint32_t kPreferredSwapchainImages = 3;
// Fixed per-frame host-visible VB capacity (grows only if a frame exceeds this).
constexpr size_t kRingVertexCapacityBytes = 2 * 1024 * 1024;

std::filesystem::path executable_dir() {
  namespace fs = std::filesystem;
  std::error_code ec;
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size > 0 ? size : 1, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) == 0) {
    buf.resize(std::strlen(buf.c_str()));
    return fs::weakly_canonical(fs::path(buf), ec).parent_path();
  }
#elif defined(_WIN32)
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    return fs::weakly_canonical(fs::path(buf), ec).parent_path();
  }
#elif defined(__linux__)
  return fs::weakly_canonical(fs::path("/proc/self/exe"), ec).parent_path();
#endif
  return fs::current_path(ec);
}

struct GpuTexture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDescriptorSet descriptor = VK_NULL_HANDLE;
  int width = 0;
  int height = 0;
  bool alive = false;
};

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mem);
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
      return i;
    }
  }
  throw std::runtime_error("No suitable Vulkan memory type");
}

bool looks_like_shader_dir(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::is_regular_file(dir / "textured_quad.vert.spv", ec) && !ec &&
         std::filesystem::is_regular_file(dir / "textured_quad.frag.spv", ec) && !ec;
}

// Resolve SPIR-V directory for both in-tree builds and packaged installs.
// Packaged layout is identical on every platform: <root>/shaders
// (macOS .app: Contents/Resources/shaders; zip: next to the binary).
std::string resolve_shader_dir() {
  namespace fs = std::filesystem;
  std::error_code ec;

  if (const char* env = std::getenv("WDS_SHADER_DIR"); env != nullptr && env[0] != '\0') {
    if (looks_like_shader_dir(env)) {
      return env;
    }
  }

  const fs::path exe_dir = executable_dir();
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
      return cand.lexically_normal().string();
    }
  }

#ifdef WDS_SHADER_DIR
  if (looks_like_shader_dir(WDS_SHADER_DIR)) {
    return WDS_SHADER_DIR;
  }
#endif

  return "renderer/shaders";
}

std::vector<char> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file) {
    return {};
  }
  const size_t size = static_cast<size_t>(file.tellg());
  std::vector<char> buffer(size);
  file.seekg(0);
  file.read(buffer.data(), static_cast<std::streamsize>(size));
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
  std::function<void(int* width, int* height)> framebuffer_size;
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
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

  VkCommandPool command_pool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kMaxFramesInFlight> command_buffers{};
  std::array<VkSemaphore, kMaxFramesInFlight> image_available{};
  std::array<VkSemaphore, kMaxFramesInFlight> render_finished{};
  std::array<VkFence, kMaxFramesInFlight> in_flight{};
  VkFence upload_fence = VK_NULL_HANDLE;
  uint32_t frame_index = 0;
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
  void destroy_frame_vertices();
  TextureId alloc_texture_slot();
  bool upload_texture_pixels(GpuTexture& tex, const unsigned char* pixels, int width, int height);
  bool create_texture_descriptor(GpuTexture& tex);
  VkCommandBuffer begin_one_time();
  void end_one_time(VkCommandBuffer cmd);
  VkSampleCountFlagBits pick_msaa_samples() const;
};

VulkanRenderer::VulkanRenderer() : impl_(std::make_unique<Impl>()) {}

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
  if (!ready_ || impl_ == nullptr || impl_->device == VK_NULL_HANDLE) {
    return true;
  }
  impl_->preferred_msaa = preferred_msaa_;
  const VkSampleCountFlagBits next = impl_->pick_msaa_samples();
  if (next == impl_->msaa_samples) {
    return true;
  }
  WDS_LOG("apply_msaa %u -> %u\n", static_cast<unsigned>(impl_->msaa_samples),
          static_cast<unsigned>(next));
  vkDeviceWaitIdle(impl_->device);
  impl_->cleanup_swapchain();
  impl_->destroy_render_pass_and_pipelines();
  impl_->msaa_samples = next;
  if (!impl_->create_render_pass_and_pipelines()) {
    ready_ = false;
    return false;
  }
  if (!impl_->create_swapchain(width_, height_)) {
    ready_ = false;
    return false;
  }
  width_ = static_cast<int>(impl_->swapchain_extent.width);
  height_ = static_cast<int>(impl_->swapchain_extent.height);
  return true;
}



void VulkanRenderer::Impl::cleanup_swapchain_resources_keep_handle() {
  if (device == VK_NULL_HANDLE) {
    return;
  }
  vkDeviceWaitIdle(device);
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

VkCommandBuffer VulkanRenderer::Impl::begin_one_time() {
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = command_pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device, &alloc, &cmd);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  return cmd;
}

void VulkanRenderer::Impl::end_one_time(VkCommandBuffer cmd) {
  vkEndCommandBuffer(cmd);
  // Fence waits only for this upload CB — avoids stalling the whole graphics queue idle.
  if (upload_fence == VK_NULL_HANDLE) {
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device, &fence_info, nullptr, &upload_fence);
  } else {
    vkResetFences(device, 1, &upload_fence);
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(graphics_queue, 1, &submit, upload_fence);
  vkWaitForFences(device, 1, &upload_fence, VK_TRUE, UINT64_MAX);
  vkFreeCommandBuffers(device, command_pool, 1, &cmd);
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

bool VulkanRenderer::Impl::create_swapchain(int width, int height) {
  VkSurfaceCapabilitiesKHR caps{};
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) != VK_SUCCESS) {
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

  uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
  if (format_count == 0) {
    return false;
  }
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
  VkSurfaceFormatKHR chosen = formats[0];
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB) {
      chosen = f;
      break;
    }
  }

  uint32_t present_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &present_count, nullptr);
  std::vector<VkPresentModeKHR> presents(present_count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &present_count, presents.data());
  // FIFO caps the main loop to the display refresh everywhere. MAILBOX lets the
  // CPU race ahead of scan-out; combined with a per-tick display lead that made
  // preview time run hot then get pulled back by Transport (speed wobble).
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  (void)presents;

  // Request triple buffering when the surface allows it (minImageCount is often 2).
  uint32_t image_count = std::max(caps.minImageCount + 1, kPreferredSwapchainImages);
  if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
    image_count = caps.maxImageCount;
  }

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
  if (vkCreateSwapchainKHR(device, &info, nullptr, &new_swapchain) != VK_SUCCESS) {
    // Keep the previous swapchain if recreation failed.
    WDS_LOG("create_swapchain: vkCreateSwapchainKHR failed\n");
    return false;
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
  vkGetSwapchainImagesKHR(device, swapchain, &actual, nullptr);
  swapchain_images.resize(actual);
  vkGetSwapchainImagesKHR(device, swapchain, &actual, swapchain_images.data());
  images_in_flight.assign(actual, VK_NULL_HANDLE);
  WDS_LOG("swapchain images=%u (requested=%u) frames_in_flight=%d min=%u max=%u\n", actual,
          image_count, kMaxFramesInFlight, caps.minImageCount, caps.maxImageCount);

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
    WDS_LOG("create_swapchain: dependent setup failed; kept previous swapchain\n");
    return false;
  }

  // Success — retire previous dependent resources and swapchain handle.
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
  vkMapMemory(device, slot.memory, 0, capacity, 0, &slot.mapped);
  slot.capacity_bytes = capacity;
  return true;
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
    if (!textures[i].alive) {
      textures[i] = GpuTexture{};
      textures[i].alive = true;
      return static_cast<TextureId>(i);
    }
  }
  textures.push_back(GpuTexture{});
  textures.back().alive = true;
  return static_cast<TextureId>(textures.size() - 1);
}

bool VulkanRenderer::Impl::create_texture_descriptor(GpuTexture& tex) {
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = descriptor_pool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &descriptor_layout;
  if (vkAllocateDescriptorSets(device, &alloc, &tex.descriptor) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorImageInfo image_info{};
  image_info.sampler = sampler;
  image_info.imageView = tex.view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = tex.descriptor;
  write.dstBinding = 0;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.descriptorCount = 1;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  return true;
}

bool VulkanRenderer::Impl::upload_texture_pixels(GpuTexture& tex, const unsigned char* pixels,
                                                 int width, int height) {
  const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &info, nullptr, &staging);
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, staging, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        physical, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &alloc, nullptr, &staging_mem);
    vkBindBufferMemory(device, staging, staging_mem, 0);
    void* mapped = nullptr;
    vkMapMemory(device, staging_mem, 0, size, 0, &mapped);
    std::memcpy(mapped, pixels, static_cast<size_t>(size));
    vkUnmapMemory(device, staging_mem);
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
  if (vkCreateImage(device, &image_info, nullptr, &tex.image) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device, tex.image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex =
      find_memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device, &alloc, nullptr, &tex.memory) != VK_SUCCESS) {
    return false;
  }
  vkBindImageMemory(device, tex.image, tex.memory, 0);

  VkCommandBuffer cmd = begin_one_time();
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

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &barrier);
  end_one_time(cmd);

  vkDestroyBuffer(device, staging, nullptr);
  vkFreeMemory(device, staging_mem, nullptr);

  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = tex.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device, &view_info, nullptr, &tex.view) != VK_SUCCESS) {
    return false;
  }
  tex.width = width;
  tex.height = height;
  return create_texture_descriptor(tex);
}

bool VulkanRenderer::Impl::create_render_pass_and_pipelines() {
  // Probe surface format (also refreshed by create_swapchain).
  {
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
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
    return false;
  }

  const auto vert_code = read_file(shader_dir + "/textured_quad.vert.spv");
  const auto frag_code = read_file(shader_dir + "/textured_quad.frag.spv");
  if (vert_code.empty() || frag_code.empty()) {
    std::fprintf(stderr, "Failed to load SPIR-V from %s\n", shader_dir.c_str());
    return false;
  }
  VkShaderModule vert = create_shader_module(device, vert_code);
  VkShaderModule frag = create_shader_module(device, frag_code);
  if (!vert || !frag) {
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
    return false;
  }

  vkDestroyShaderModule(device, vert, nullptr);
  vkDestroyShaderModule(device, frag, nullptr);

  return true;
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
  if (!host.create_surface || !host.framebuffer_size) {
    std::fprintf(stderr, "VulkanHostSurface missing create_surface / framebuffer_size\n");
    return false;
  }
  impl_->framebuffer_size = host.framebuffer_size;
#if defined(_WIN32)
  impl_->win32_monitor = host.win32_monitor;
#endif
  impl_->preferred_msaa = preferred_msaa_;

  impl_->shader_dir = resolve_shader_dir();
  WDS_LOG("VulkanRenderer::create shader_dir=%s\n", impl_->shader_dir.c_str());

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "WDS Preview";
  app.apiVersion = VK_API_VERSION_1_1;

  std::vector<const char*> extensions = host.instance_extensions;
  // MoltenVK is a portability ICD: without these, vkCreateInstance returns
  // VK_ERROR_INCOMPATIBLE_DRIVER / "Found no drivers!" on macOS.
  extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#if defined(_WIN32)
  {
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

  VkInstanceCreateInfo inst_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  inst_info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  inst_info.pApplicationInfo = &app;
  inst_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  inst_info.ppEnabledExtensionNames = extensions.data();
  {
    const VkResult ir = vkCreateInstance(&inst_info, nullptr, &impl_->instance);
    if (ir != VK_SUCCESS) {
      WDS_LOG("vkCreateInstance failed result=%d\n", static_cast<int>(ir));
      return false;
    }
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

  impl_->surface = host.create_surface(impl_->instance);
  if (impl_->surface == VK_NULL_HANDLE) {
    WDS_LOG("host.create_surface failed\n");
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
  WDS_LOG("physical devices=%u\n", device_count);
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
  {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(impl_->physical, &props);
    WDS_LOG("selected GPU='%s' api=%u.%u.%u queue_family=%u msaa=%u\n", props.deviceName,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion), impl_->graphics_family,
            static_cast<unsigned>(impl_->msaa_samples));
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
  if (vkCreateDevice(impl_->physical, &device_info, nullptr, &impl_->device) != VK_SUCCESS) {
    WDS_LOG("vkCreateDevice failed\n");
    return false;
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

  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_size.descriptorCount = 256;
  VkDescriptorPoolCreateInfo dp_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dp_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dp_info.maxSets = 256;
  dp_info.poolSizeCount = 1;
  dp_info.pPoolSizes = &pool_size;
  if (vkCreateDescriptorPool(impl_->device, &dp_info, nullptr, &impl_->descriptor_pool) !=
      VK_SUCCESS) {
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
    vkCreateSemaphore(impl_->device, &sem_info, nullptr, &impl_->image_available[i]);
    vkCreateSemaphore(impl_->device, &sem_info, nullptr, &impl_->render_finished[i]);
    vkCreateFence(impl_->device, &fence_info, nullptr, &impl_->in_flight[i]);
  }

  impl_->textures.resize(1);  // slot 0 unused
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (!impl_->ensure_frame_vertex_capacity(i, kRingVertexCapacityBytes)) {
      return false;
    }
  }

  ready_ = true;
  return true;
}

void VulkanRenderer::destroy() {
  if (!impl_) {
    ready_ = false;
    return;
  }
  ready_ = false;

  if (impl_->device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(impl_->device);

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
    if (impl_->upload_fence) {
      vkDestroyFence(impl_->device, impl_->upload_fence, nullptr);
      impl_->upload_fence = VK_NULL_HANDLE;
    }

    impl_->cleanup_swapchain();
    if (impl_->pipeline) vkDestroyPipeline(impl_->device, impl_->pipeline, nullptr);
    if (impl_->pipeline_additive) vkDestroyPipeline(impl_->device, impl_->pipeline_additive, nullptr);
    if (impl_->pipeline_layout) vkDestroyPipelineLayout(impl_->device, impl_->pipeline_layout, nullptr);
    if (impl_->render_pass) vkDestroyRenderPass(impl_->device, impl_->render_pass, nullptr);
    if (impl_->sampler) vkDestroySampler(impl_->device, impl_->sampler, nullptr);
    if (impl_->descriptor_pool) vkDestroyDescriptorPool(impl_->device, impl_->descriptor_pool, nullptr);
    if (impl_->descriptor_layout)
      vkDestroyDescriptorSetLayout(impl_->device, impl_->descriptor_layout, nullptr);
    if (impl_->command_pool) vkDestroyCommandPool(impl_->device, impl_->command_pool, nullptr);
    vkDestroyDevice(impl_->device, nullptr);
    impl_->device = VK_NULL_HANDLE;
  }

  // Half-init path (instance/surface without device) must still free WSI objects.
  if (impl_->surface && impl_->instance) {
    vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
    impl_->surface = VK_NULL_HANDLE;
  }
  if (impl_->instance) {
    vkDestroyInstance(impl_->instance, nullptr);
    impl_->instance = VK_NULL_HANDLE;
  }

  *impl_ = Impl{};
  ready_ = false;
}

void VulkanRenderer::device_wait_idle() {
  if (!impl_ || impl_->device == VK_NULL_HANDLE) {
    return;
  }
  vkDeviceWaitIdle(impl_->device);
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

void VulkanRenderer::release_fullscreen_exclusive() {
  if (!impl_) {
    return;
  }
  impl_->release_fullscreen_exclusive_internal();
  impl_->exclusive_fullscreen_desired = false;
}

bool VulkanRenderer::resize(int width, int height) {
  if (!ready_ || impl_ == nullptr) {
    return false;
  }
  if (width <= 0 || height <= 0) {
    // Minimize / fullscreen transition: keep the last good swapchain.
    impl_->swapchain_occluded = true;
    return true;
  }
  if (!impl_->swapchain_occluded && width == width_ && height == height_ &&
      impl_->swapchain != VK_NULL_HANDLE) {
    return true;
  }
  if (!impl_->create_swapchain(width, height)) {
    return false;
  }
  if (impl_->swapchain_occluded) {
    // Surface still 0×0; previous swapchain retained.
    return true;
  }
  width_ = static_cast<int>(impl_->swapchain_extent.width);
  height_ = static_cast<int>(impl_->swapchain_extent.height);
  return true;
}

TextureInfo VulkanRenderer::create_texture_rgba(const unsigned char* pixels, int width, int height) {
  TextureInfo info;
  if (!ready_ || pixels == nullptr || width <= 0 || height <= 0) {
    return info;
  }
  const TextureId id = impl_->alloc_texture_slot();
  GpuTexture& tex = impl_->textures[id];
  if (!impl_->upload_texture_pixels(tex, pixels, width, height)) {
    tex = GpuTexture{};
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
  if (!tex.alive) {
    return;
  }
  // Ensure in-flight descriptor/image use is complete before free.
  if (ready_ && impl_->device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(impl_->device);
  }
  if (tex.descriptor) {
    vkFreeDescriptorSets(impl_->device, impl_->descriptor_pool, 1, &tex.descriptor);
  }
  if (tex.view) vkDestroyImageView(impl_->device, tex.view, nullptr);
  if (tex.image) vkDestroyImage(impl_->device, tex.image, nullptr);
  if (tex.memory) vkFreeMemory(impl_->device, tex.memory, nullptr);
  tex = GpuTexture{};
}

bool VulkanRenderer::draw_frame(const DrawBatch& batch, const ScreenBounds& screen, float clear_r,
                                float clear_g, float clear_b, const DrawBatch* additive,
                                const DrawBatch* post_overlay, const DrawBatch* post_overlay2) {
  if (!ready_ || impl_ == nullptr || impl_->swapchain == VK_NULL_HANDLE) {
    return false;
  }

  // Recover from minimize / fullscreen 0×0 transitions before acquire.
  if (impl_->swapchain_occluded) {
    int w = 0, h = 0;
    if (impl_->framebuffer_size) {
      impl_->framebuffer_size(&w, &h);
    }
    if (w <= 0 || h <= 0) {
      return true;  // still occluded — skip present
    }
    if (!resize(w, h) || impl_->swapchain_occluded || impl_->swapchain == VK_NULL_HANDLE) {
      return true;
    }
  }

  using clock = std::chrono::steady_clock;
  const uint32_t frame = impl_->frame_index;
  const auto fence_t0 = clock::now();
  vkWaitForFences(impl_->device, 1, &impl_->in_flight[frame], VK_TRUE, UINT64_MAX);
  impl_->last_fence_wait_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - fence_t0).count();

  uint32_t image_index = 0;
  const auto acquire_t0 = clock::now();
  VkResult acquire = vkAcquireNextImageKHR(impl_->device, impl_->swapchain, UINT64_MAX,
                                           impl_->image_available[frame], VK_NULL_HANDLE,
                                           &image_index);
  impl_->last_acquire_wait_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - acquire_t0).count();
  if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
    int w = 0, h = 0;
    if (impl_->framebuffer_size) {
      impl_->framebuffer_size(&w, &h);
    }
    (void)resize(w, h);
    return true;  // retry next frame
  }
  impl_->note_fullscreen_exclusive_lost("vkAcquireNextImageKHR", acquire);
  if (acquire == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
    int w = 0, h = 0;
    if (impl_->framebuffer_size) {
      impl_->framebuffer_size(&w, &h);
    }
    (void)resize(w, h);
    return true;
  }
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
    return false;
  }

  // Do not reuse a swapchain image that is still referenced by an in-flight submit.
  if (image_index < impl_->images_in_flight.size() &&
      impl_->images_in_flight[image_index] != VK_NULL_HANDLE) {
    vkWaitForFences(impl_->device, 1, &impl_->images_in_flight[image_index], VK_TRUE,
                    UINT64_MAX);
  }

  vkResetFences(impl_->device, 1, &impl_->in_flight[frame]);

  const size_t additive_verts = additive ? additive->vertex_count() : 0;
  const size_t post_verts = post_overlay ? post_overlay->vertex_count() : 0;
  const size_t post2_verts = post_overlay2 ? post_overlay2->vertex_count() : 0;
  const size_t total_verts = batch.vertex_count() + additive_verts + post_verts + post2_verts;
  const size_t bytes = total_verts * sizeof(DrawVertex);
  if (!impl_->ensure_frame_vertex_capacity(frame, bytes)) {
    // Acquire already signaled image_available[frame]; drain it before returning.
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    drain.waitSemaphoreCount = 1;
    drain.pWaitSemaphores = &impl_->image_available[frame];
    drain.pWaitDstStageMask = &wait_stage;
    vkQueueSubmit(impl_->graphics_queue, 1, &drain, VK_NULL_HANDLE);
    vkQueueWaitIdle(impl_->graphics_queue);
    return false;
  }

  auto& vb = impl_->frame_vertices[frame];
  auto& bucket_first_vertex = impl_->bucket_first_vertex;
  auto& additive_first_vertex = impl_->additive_first_vertex;
  auto& post_first_vertex = impl_->post_first_vertex;
  auto& post2_first_vertex = impl_->post2_first_vertex;
  bucket_first_vertex.clear();
  additive_first_vertex.clear();
  post_first_vertex.clear();
  post2_first_vertex.clear();
  bucket_first_vertex.reserve(batch.buckets.size());
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
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline_layout, 0, 1,
                              &impl_->textures[bucket.texture].descriptor, 0, nullptr);
      vkCmdDraw(cmd, static_cast<uint32_t>(bucket.vertices.size()), 1, first_vertex[bi], 0);
    }
  };

  draw_buckets(impl_->pipeline, batch, bucket_first_vertex);
  if (additive && additive_verts > 0 && impl_->pipeline_additive) {
    draw_buckets(impl_->pipeline_additive, *additive, additive_first_vertex);
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
  if (vkQueueSubmit(impl_->graphics_queue, 1, &submit, impl_->in_flight[frame]) != VK_SUCCESS) {
    // Submit failed after acquire signaled image_available; drain so the slot is reusable.
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    drain.waitSemaphoreCount = 1;
    drain.pWaitSemaphores = &impl_->image_available[frame];
    drain.pWaitDstStageMask = &wait_stage;
    vkQueueSubmit(impl_->graphics_queue, 1, &drain, VK_NULL_HANDLE);
    vkQueueWaitIdle(impl_->graphics_queue);
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
  if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR ||
      present_result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
    int w = 0, h = 0;
    if (impl_->framebuffer_size) {
      impl_->framebuffer_size(&w, &h);
    }
    (void)resize(w, h);
  } else if (present_result == VK_ERROR_SURFACE_LOST_KHR) {
    impl_->swapchain_occluded = true;
  }

  impl_->frame_index = (frame + 1) % kMaxFramesInFlight;
  return true;
}

}  // namespace wds::renderer
