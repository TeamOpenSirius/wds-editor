#include "wds/renderer/vulkan_renderer.hpp"
#include "wds/renderer/descriptor_pool_policy.hpp"
#include "wds/renderer/log.hpp"
#include "wds/renderer/upload_result.hpp"

#include <wds/common/crash_handler.hpp>
#include <wds/common/perf_stages.hpp>
#include <wds/common/utf8_path.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
// 10660 late HDR trails are ~1.1M verts × 48 B ≈ 53 MB. A 2 MB ring grew on
// the first heavy frame (destroy/recreate mapped VB). Size the ring for that
// batch so upload is memcpy-only after startup.
constexpr size_t kRingVertexCapacityBytes = VulkanRenderer::kPersistentVertexRingBytes;

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

const char* vk_format_name(VkFormat format) noexcept {
  switch (format) {
    case VK_FORMAT_UNDEFINED:
      return "UNDEFINED";
    case VK_FORMAT_B8G8R8A8_UNORM:
      return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB:
      return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM:
      return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB:
      return "R8G8B8A8_SRGB";
    default:
      return "OTHER";
  }
}

const char* vk_colorspace_name(VkColorSpaceKHR space) noexcept {
  switch (space) {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
      return "SRGB_NONLINEAR";
    default:
      return "OTHER";
  }
}

const char* composite_alpha_name(VkCompositeAlphaFlagBitsKHR bit) noexcept {
  switch (bit) {
    case VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR:
      return "OPAQUE";
    case VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR:
      return "PRE_MULTIPLIED";
    case VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR:
      return "POST_MULTIPLIED";
    case VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR:
      return "INHERIT";
    default:
      return "OTHER";
  }
}

const char* surface_transform_name(VkSurfaceTransformFlagBitsKHR transform) noexcept {
  switch (transform) {
    case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
      return "IDENTITY";
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
      return "ROTATE_90";
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
      return "ROTATE_180";
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
      return "ROTATE_270";
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR:
      return "H_MIRROR";
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR:
      return "H_MIRROR_90";
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR:
      return "H_MIRROR_180";
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR:
      return "H_MIRROR_270";
    case VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR:
      return "INHERIT";
    default:
      return "OTHER";
  }
}

void append_token(char* buf, size_t cap, const char* token, bool* first) noexcept {
  if (buf == nullptr || cap == 0 || token == nullptr || first == nullptr) {
    return;
  }
  const size_t len = std::strlen(buf);
  if (len + 1 >= cap) {
    return;
  }
  if (!*first) {
    buf[len] = ',';
    buf[len + 1] = '\0';
  }
  *first = false;
  std::strncat(buf, token, cap - std::strlen(buf) - 1);
}

void format_present_modes(char* buf, size_t cap, const std::vector<VkPresentModeKHR>& modes) noexcept {
  if (buf == nullptr || cap == 0) {
    return;
  }
  buf[0] = '\0';
  bool first = true;
  for (const VkPresentModeKHR mode : modes) {
    append_token(buf, cap, present_mode_name(mode), &first);
  }
  if (first) {
    std::strncat(buf, "none", cap - 1);
  }
}

void format_alpha_mask(char* buf, size_t cap, VkCompositeAlphaFlagsKHR flags) noexcept {
  if (buf == nullptr || cap == 0) {
    return;
  }
  buf[0] = '\0';
  bool first = true;
  const VkCompositeAlphaFlagBitsKHR bits[] = {
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
  for (const VkCompositeAlphaFlagBitsKHR bit : bits) {
    if ((flags & bit) != 0) {
      append_token(buf, cap, composite_alpha_name(bit), &first);
    }
  }
  if (first) {
    std::strncat(buf, "none", cap - 1);
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
  bool retiring = false;               // destroy_texture ran; waiting for the GPU
  uint64_t retire_after_submit = 0;    // GPU-free when completed_submit_seq >= this
  uint64_t sampled_seq = 0;       // 0 = never bound in a recorded draw
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

struct HostVisiblePick {
  uint32_t type_index = 0;
  VkMemoryPropertyFlags flags = 0;
};

// Persistent mapped VERTEX buffers: ReBAR/SAM first, then host-cached
// coherent, then non-coherent (caller flushes). No vendor names.
int host_visible_vertex_rank(VkMemoryPropertyFlags flags) noexcept {
  const bool host = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
  if (!host) {
    return 0;
  }
  const bool local = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
  const bool coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  if (local && coherent) {
    return 4;
  }
  if (local) {
    return 3;
  }
  if (coherent) {
    return 2;
  }
  return 1;
}

std::vector<HostVisiblePick> rank_host_visible_vertex_types(VkPhysicalDevice phys,
                                                            uint32_t type_bits) {
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mem);
  std::vector<HostVisiblePick> ranked;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) == 0) {
      continue;
    }
    const VkMemoryPropertyFlags flags = mem.memoryTypes[i].propertyFlags;
    if (host_visible_vertex_rank(flags) == 0) {
      continue;
    }
    ranked.push_back(HostVisiblePick{i, flags});
  }
  std::sort(ranked.begin(), ranked.end(), [](const HostVisiblePick& a, const HostVisiblePick& b) {
    const int ra = host_visible_vertex_rank(a.flags);
    const int rb = host_visible_vertex_rank(b.flags);
    if (ra != rb) {
      return ra > rb;
    }
    return a.type_index < b.type_index;
  });
  return ranked;
}

size_t clamp_persistent_ring_bytes(VkPhysicalDevice phys, int frames_in_flight,
                                   size_t want) noexcept {
  if (phys == VK_NULL_HANDLE) {
    return want;
  }
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(phys, &props);
  const bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  VkDeviceSize max_alloc = 0;
  if (props.apiVersion >= VK_API_VERSION_1_1) {
    VkPhysicalDeviceMaintenance3Properties m3{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &m3;
    vkGetPhysicalDeviceProperties2(phys, &p2);
    max_alloc = m3.maxMemoryAllocationSize;
  }
  if (max_alloc > 0 && max_alloc < static_cast<VkDeviceSize>(want)) {
    want = static_cast<size_t>(max_alloc);
  }

  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mem);
  VkDeviceSize host_heap = 0;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if ((mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
      continue;
    }
    const uint32_t heap = mem.memoryTypes[i].heapIndex;
    if (heap < mem.memoryHeapCount) {
      host_heap = std::max(host_heap, mem.memoryHeaps[heap].size);
    }
  }
  // Discrete host-visible is often a small BAR shared with staging; unified
  // heaps can keep one spare ring.
  const int rings = std::max(frames_in_flight, 1) + (discrete ? 2 : 1);
  if (host_heap > 0) {
    const VkDeviceSize per = host_heap / static_cast<VkDeviceSize>(rings);
    if (per < static_cast<VkDeviceSize>(want)) {
      want = static_cast<size_t>(per);
    }
  }
  constexpr size_t kMiB = 1024 * 1024;
  want = (want / kMiB) * kMiB;
  if (want < 4 * kMiB) {
    want = 4 * kMiB;
  }
  return want;
}

size_t choose_persistent_ring_bytes(VkPhysicalDevice phys, int frames_in_flight) noexcept {
  return clamp_persistent_ring_bytes(phys, frames_in_flight,
                                     VulkanRenderer::kPersistentVertexRingBytes);
}

void flush_mapped_vertex_range(VkDevice device, VkDeviceMemory memory, bool coherent,
                               VkDeviceSize atom, VkDeviceSize offset, VkDeviceSize bytes,
                               VkDeviceSize alloc_size) {
  if (device == VK_NULL_HANDLE || memory == VK_NULL_HANDLE || coherent || bytes == 0) {
    return;
  }
  const MappedFlushRange r = mapped_flush_range(offset, bytes, atom, alloc_size);
  if (r.size == 0) {
    return;
  }
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = memory;
  range.offset = r.offset;
  range.size = r.size;
  vkFlushMappedMemoryRanges(device, 1, &range);
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

std::vector<uint32_t> host_visible_memory_types(const VkPhysicalDeviceMemoryProperties& mem,
                                                uint32_t type_bits) {
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

// `mem_props` / `atom` come from Impl's cached physical-device query (per-chunk
// vkGetPhysicalDevice* calls were pure overhead).
UploadResult create_filled_host_staging(VkDevice device,
                                        const VkPhysicalDeviceMemoryProperties& mem_props,
                                        VkDeviceSize atom, VkDeviceSize bytes, const void* src,
                                        HostStaging* out) {
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
  const std::vector<uint32_t> types = host_visible_memory_types(mem_props, req.memoryTypeBits);
  if (types.empty()) {
    destroy_host_staging(device, staging);
    return upload_no_memory_type(UploadStage::Buffer);
  }

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
    flush_mapped_vertex_range(device, staging.memory,
                              (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0, atom, 0, bytes,
                              req.size);
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
#if defined(__linux__)
      exe_dir / ".." / "shaders",  // native install: <prefix>/bin/wds_editor
#endif
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

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
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

struct GpuDrawBucket {
  TextureId texture = kInvalidTextureId;
  bool disable_alpha_clip = false;
  uint32_t first_vertex = 0;
  uint32_t vert_count = 0;
  uint32_t first_index = 0;
  uint32_t index_count = 0;
  uint8_t overlay_pipe = 0;
};

struct TexKeyFix {
  TextureId texture = kInvalidTextureId;
  bool disable_alpha_clip = false;
};

// Per-frame planning vectors kept on Impl so clear() retains capacity. Only
// valid inside one draw_frame call: nothing may keep a reference past it.
struct DrawPlanScratch {
  std::vector<GpuDrawBucket> scene;
  std::vector<GpuDrawBucket> early;
  std::vector<GpuDrawBucket> mid;
  std::vector<GpuDrawBucket> additive;
  std::vector<GpuDrawBucket> carve;
  std::vector<GpuDrawBucket> post;
  std::vector<GpuDrawBucket> post2;
  std::vector<GpuDrawBucket> head_packed;
  std::vector<TexKeyFix> order;
};

}  // namespace

struct VulkanRenderer::Impl {
  DrawPlanScratch plan_scratch;
  // Physical-device data is immutable for a device; queried once per physical.
  VkPhysicalDevice cached_props_for = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties cached_mem_props{};
  VkDeviceSize cached_non_coherent_atom = 1;
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
  uint32_t last_presented_image = UINT32_MAX;
  bool last_present_ok = false;

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
  int64_t last_draw_record_us = 0;
  size_t last_vb_used_bytes = 0;
  size_t last_vb_cap_bytes = 0;
  size_t last_ib_used_bytes = 0;
  size_t last_ib_cap_bytes = 0;
  size_t last_total_verts = 0;
  size_t last_early_verts = 0;
  size_t last_carve_verts = 0;
  int last_vb_staging = 0;
  int last_bloom_pass = 0;
  int last_carve_pass = 0;
  int64_t last_vk_draws = 0;
  int64_t last_pipe_binds = 0;
  int64_t last_desc_binds = 0;
  int64_t last_push_consts = 0;
  int64_t last_render_passes = 0;
  int64_t last_vb_binds = 0;
  // Debug-session diagnostics for a black preview. Edge-triggered so a stuck
  // acquire/present result or a repeated early-out does not fill the log.
  const char* draw_stop_reason = nullptr;
  bool logged_first_present = false;
  bool have_logged_acquire = false;
  bool have_logged_present = false;
  VkResult logged_acquire = VK_SUCCESS;
  VkResult logged_present = VK_SUCCESS;
  void release_fullscreen_exclusive_internal();
  void note_fullscreen_exclusive_lost(const char* where, VkResult result);
  void note_wsi_result(const char* where, VkResult result, bool* have, VkResult* previous);
  void note_draw_stopped(const char* why);

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
  VkPipeline pipeline_alpha_soft = VK_NULL_HANDLE;
  VkPipeline pipeline_additive = VK_NULL_HANDLE;
  VkPipeline pipeline_additive_no_clip = VK_NULL_HANDLE;
  VkPipeline pipeline_additive_clip01 = VK_NULL_HANDLE;
  VkPipeline pipeline_syuriken_add = VK_NULL_HANDLE;
  VkPipeline pipeline_syuriken_extract = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_syuriken = VK_NULL_HANDLE;
  TextureId syuriken_plate_tex = kInvalidTextureId;
  float syuriken_time = 0.0f;
  float syuriken_sprite = 2.56f;

  VkPipeline pipeline_bloom = VK_NULL_HANDLE;
  VkPipeline pipeline_bloom_extract = VK_NULL_HANDLE;
  VkPipeline pipeline_bloom_extract_no_clip = VK_NULL_HANDLE;
  VkPipeline pipeline_bloom_extract_clip01 = VK_NULL_HANDLE;
  VkPipeline pipeline_bloom_extract_alpha = VK_NULL_HANDLE;
  VkPipeline pipeline_hdr_composite = VK_NULL_HANDLE;
  VkPipeline pipeline_hdr_replace = VK_NULL_HANDLE;
  VkPipeline pipeline_alpha_1x = VK_NULL_HANDLE;
  VkRenderPass bloom_pass = VK_NULL_HANDLE;
  VkRenderPass bloom_extract_pass = VK_NULL_HANDLE;
  VkRenderPass bloom_extract_load_pass = VK_NULL_HANDLE;
  VkImage bloom_src_image = VK_NULL_HANDLE;
  VkDeviceMemory bloom_src_memory = VK_NULL_HANDLE;
  VkImageView bloom_src_view = VK_NULL_HANDLE;
  VkFramebuffer bloom_extract_fb = VK_NULL_HANDLE;
  VkDescriptorSet bloom_src_descriptor = VK_NULL_HANDLE;
  VkDescriptorPool bloom_src_pool = VK_NULL_HANDLE;
  VkImage hdr_late_image = VK_NULL_HANDLE;
  VkDeviceMemory hdr_late_memory = VK_NULL_HANDLE;
  VkImageView hdr_late_view = VK_NULL_HANDLE;
  VkFramebuffer hdr_late_fb = VK_NULL_HANDLE;
  VkDescriptorSet hdr_late_descriptor = VK_NULL_HANDLE;
  VkDescriptorPool hdr_late_pool = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> bloom_framebuffers;
  bool bloom_ready = false;
  bool hdr_late_ready = false;
  bool swapchain_can_blit = false;
  bool hdr_can_scene_carve = false;
  VkExtent2D bloom_extent{};
  VkFormat bloom_src_format = VK_FORMAT_R16G16B16A16_SFLOAT;
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
  // One per swapchain image (not per frame slot): present waits on the
  // semaphore of the image it presents, so a slot re-submit cannot re-signal a
  // semaphore whose present wait is still pending.
  std::vector<VkSemaphore> render_finished;
  std::array<VkFence, kMaxFramesInFlight> in_flight{};
  uint32_t frame_index = 0;
  int frames_in_flight = kMaxFramesInFlight;
  uint64_t frame_seq = 0;  // draw_frame call counter; only a "sampled" marker
  // GPU-completion bookkeeping for texture retirement (see advance_completed_submit).
  uint64_t submit_seq = 0;            // successful real-frame vkQueueSubmit count
  uint64_t completed_submit_seq = 0;  // all submits <= this are known complete
  std::array<uint64_t, kMaxFramesInFlight> slot_submit_seq{};
  int preferred_msaa = 1;

  struct FrameVertexBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    size_t capacity_bytes = 0;
    VkDeviceSize mapped_alloc_bytes = 0;
    bool coherent = true;
    bool uses_staging = false;
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  };
  std::array<FrameVertexBuffer, kMaxFramesInFlight> frame_vertices{};
  std::array<FrameVertexBuffer, kMaxFramesInFlight> frame_indices{};
  bool persistent_vb_locked = false;
  size_t persistent_ring_bytes = VulkanRenderer::kPersistentVertexRingBytes;
  size_t persistent_ring_max_bytes = VulkanRenderer::kPersistentVertexRingMaxWantBytes;
  size_t persistent_index_ring_bytes = VulkanRenderer::kPersistentIndexRingBytes;
  size_t persistent_index_ring_max_bytes = VulkanRenderer::kPersistentIndexRingMaxWantBytes;
  int ring_high_water_frames = 0;
  size_t ring_grow_pending_bytes = 0;
  int ib_high_water_frames = 0;
  size_t ib_grow_pending_bytes = 0;
  VkDeviceSize non_coherent_atom = 256;

  // Reused per draw_frame to avoid heap churn on the present path.
  std::vector<uint32_t> bucket_first_vertex;
  std::vector<uint32_t> early_first_vertex;
  std::vector<uint32_t> mid_first_vertex;
  std::vector<uint32_t> additive_first_vertex;
  std::vector<uint32_t> carve_first_vertex;
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
  bool create_bloom_pipeline();
  void destroy_bloom_pipeline();
  bool create_bloom_resources();
  void destroy_bloom_resources();
  void record_glow_extract(VkCommandBuffer cmd, const DrawBatch& early,
                           const std::vector<uint32_t>& first_vertex, VkBuffer vb,
                           const float* mvp, VkRect2D scissor_full, VkFramebuffer extract_fb,
                           bool load_existing = false);
  void record_glow_extract_gpu(VkCommandBuffer cmd, const GpuDrawBucket* buckets, size_t bucket_n,
                              uint32_t vert_count, VkBuffer vb, const float* mvp,
                              VkRect2D scissor_full, VkFramebuffer extract_fb,
                              bool load_existing = false, VkBuffer ib = VK_NULL_HANDLE);
  void push_syuriken_pc(VkCommandBuffer cmd, const float* mvp) const;
  bool bucket_is_syuriken(const DrawBucket& bucket) const noexcept;
  void record_bloom(VkCommandBuffer cmd, uint32_t image_index);
  bool ensure_ring_slot(FrameVertexBuffer& slot, size_t bytes, size_t ring, size_t ring_max,
                        VkBufferUsageFlags usage);
  bool ensure_frame_vertex_capacity(uint32_t frame, size_t bytes);
  bool ensure_frame_index_capacity(uint32_t frame, size_t bytes);
  size_t next_ring_target(size_t min_bytes) const noexcept;
  size_t next_ib_ring_target(size_t min_bytes) const noexcept;
  bool commit_ring_grow(size_t min_bytes);
  bool commit_ib_ring_grow(size_t min_bytes);
  void note_vb_high_water(size_t used_bytes);
  void note_ib_high_water(size_t used_bytes);
  void unmap_frame_vertices();
  bool remap_frame_vertices();
  void destroy_frame_vertices();
  TextureId alloc_texture_slot();
  void release_texture_descriptor(GpuTexture& tex);
  void destroy_gpu_texture_resources(GpuTexture& tex);
  void reap_retired_textures();
  void note_slot_fence_signalled(uint32_t frame) noexcept;
  void destroy_render_finished(std::vector<VkSemaphore>& sems);
  bool create_render_finished(size_t count);
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

  // Per-slot TIMESTAMP query pools. Created only while perf::enabled().
  static constexpr uint32_t kGpuTimestampQueries = 32;
  static constexpr uint32_t kGpuTimestampUsed = 6;
  std::array<VkQueryPool, kMaxFramesInFlight> gpu_timestamp_pools{};
  std::array<uint32_t, kMaxFramesInFlight> gpu_timestamp_written{};
  std::array<bool, kMaxFramesInFlight> gpu_timestamp_pending{};
  std::vector<std::pair<const char*, int64_t>> last_gpu_segments;
  float gpu_timestamp_period = 0.0f;
  uint32_t gpu_timestamp_valid_bits = 0;
  uint64_t gpu_timestamp_mask = 0;
  bool gpu_timestamps_probed = false;
  bool gpu_timestamps_supported = false;
  bool gpu_timestamps_logged = false;
  bool gpu_timestamps_sample_logged = false;
  bool gpu_timestamps_recording = false;
  uint32_t gpu_timestamp_cursor = 0;
  uint32_t gpu_timestamp_frame = 0;

  void destroy_gpu_timestamp_pools();
  void probe_gpu_timestamps();
  bool ensure_gpu_timestamp_pool(uint32_t frame);
  void reap_gpu_timestamps(uint32_t frame);
  bool begin_gpu_timestamps(uint32_t frame, VkCommandBuffer cmd);
  void write_gpu_timestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage);
  void commit_gpu_timestamps(uint32_t frame);
  int64_t gpu_timestamp_delta_ns(uint64_t t0, uint64_t t1) const;
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
  destroy_gpu_timestamp_pools();
  destroy_bloom_resources();
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
  // Device is idle above, so no present wait is pending on these.
  destroy_render_finished(render_finished);
  last_present_ok = false;
  last_presented_image = UINT32_MAX;
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

void VulkanRenderer::Impl::note_wsi_result(const char* where, VkResult result, bool* have,
                                           VkResult* previous) {
#if WDS_ENABLE_LOGGING
  if (have == nullptr || previous == nullptr) {
    return;
  }
  if (!*have) {
    *have = true;
    *previous = result;
    if (result != VK_SUCCESS) {
      WDS_LOG("wsi %s %s (%d)\n", where != nullptr ? where : "?", vk_result_name(result),
              static_cast<int>(result));
    }
    return;
  }
  if (*previous == result) {
    return;
  }
  WDS_LOG("wsi %s %s -> %s (%d)\n", where != nullptr ? where : "?", vk_result_name(*previous),
          vk_result_name(result), static_cast<int>(result));
  *previous = result;
#else
  (void)where;
  (void)result;
  (void)have;
  (void)previous;
#endif
}

void VulkanRenderer::Impl::note_draw_stopped(const char* why) {
#if WDS_ENABLE_LOGGING
  if (why == nullptr || draw_stop_reason == why) {
    return;
  }
  draw_stop_reason = why;
  WDS_LOG("draw_frame stopped: %s\n", why);
#else
  (void)why;
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

void VulkanRenderer::Impl::destroy_gpu_timestamp_pools() {
  if (device != VK_NULL_HANDLE) {
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
      if (gpu_timestamp_pools[static_cast<size_t>(i)] != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, gpu_timestamp_pools[static_cast<size_t>(i)], nullptr);
        gpu_timestamp_pools[static_cast<size_t>(i)] = VK_NULL_HANDLE;
      }
      gpu_timestamp_written[static_cast<size_t>(i)] = 0;
      gpu_timestamp_pending[static_cast<size_t>(i)] = false;
    }
  }
  gpu_timestamps_recording = false;
  gpu_timestamp_cursor = 0;
}

void VulkanRenderer::Impl::probe_gpu_timestamps() {
  if (gpu_timestamps_probed) {
    return;
  }
  gpu_timestamps_probed = true;
  gpu_timestamps_supported = false;
  if (physical == VK_NULL_HANDLE) {
    return;
  }
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical, &props);
  gpu_timestamp_period = props.limits.timestampPeriod;

  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  if (family_count > 0) {
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
  }
  gpu_timestamp_valid_bits =
      graphics_family < family_count ? families[graphics_family].timestampValidBits : 0;

  if (!(gpu_timestamp_period > 0.0f) || gpu_timestamp_valid_bits == 0) {
    if (!gpu_timestamps_logged) {
      gpu_timestamps_logged = true;
      WDS_LOG("gpu timestamps unsupported timestampPeriod=%g timestampValidBits=%u\n",
              static_cast<double>(gpu_timestamp_period), gpu_timestamp_valid_bits);
    }
    return;
  }
  gpu_timestamp_mask = gpu_timestamp_valid_bits >= 64
                           ? ~uint64_t{0}
                           : ((uint64_t{1} << gpu_timestamp_valid_bits) - 1ull);
  gpu_timestamps_supported = true;
  wds::common::perf::stage_id("gpu.glow_hdr");
  wds::common::perf::stage_id("gpu.late_hdr");
  wds::common::perf::stage_id("gpu.opaque");
  wds::common::perf::stage_id("gpu.carve");
  wds::common::perf::stage_id("gpu.composite");
  wds::common::perf::stage_id("gpu.total");
  if (!gpu_timestamps_logged) {
    gpu_timestamps_logged = true;
    WDS_LOG("gpu timestamps enabled timestampPeriod=%g timestampValidBits=%u\n",
            static_cast<double>(gpu_timestamp_period), gpu_timestamp_valid_bits);
  }
}

bool VulkanRenderer::Impl::ensure_gpu_timestamp_pool(uint32_t frame) {
  if (frame >= static_cast<uint32_t>(kMaxFramesInFlight) || device == VK_NULL_HANDLE) {
    return false;
  }
  probe_gpu_timestamps();
  if (!gpu_timestamps_supported) {
    return false;
  }
  VkQueryPool& pool = gpu_timestamp_pools[frame];
  if (pool != VK_NULL_HANDLE) {
    return true;
  }
  VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = kGpuTimestampQueries;
  const VkResult cr = vkCreateQueryPool(device, &info, nullptr, &pool);
  if (cr != VK_SUCCESS) {
    pool = VK_NULL_HANDLE;
    gpu_timestamps_supported = false;
    WDS_LOG("gpu timestamps vkCreateQueryPool failed result=%d (%s)\n", static_cast<int>(cr),
            vk_result_name(cr));
    return false;
  }
  gpu_timestamp_written[frame] = 0;
  gpu_timestamp_pending[frame] = false;
  return true;
}

int64_t VulkanRenderer::Impl::gpu_timestamp_delta_ns(uint64_t t0, uint64_t t1) const {
  t0 &= gpu_timestamp_mask;
  t1 &= gpu_timestamp_mask;
  if (t1 < t0) {
    return 0;
  }
  const uint64_t ticks = t1 - t0;
  const double ns = static_cast<double>(ticks) * static_cast<double>(gpu_timestamp_period);
  if (!(ns > 0.0) || ns >= static_cast<double>(INT64_MAX)) {
    return 0;
  }
  return static_cast<int64_t>(ns + 0.5);
}

void VulkanRenderer::Impl::reap_gpu_timestamps(uint32_t frame) {
  if (frame >= static_cast<uint32_t>(kMaxFramesInFlight) || device == VK_NULL_HANDLE) {
    return;
  }
  if (!gpu_timestamp_pending[frame] || gpu_timestamp_pools[frame] == VK_NULL_HANDLE) {
    return;
  }
  gpu_timestamp_pending[frame] = false;
  const uint32_t n = gpu_timestamp_written[frame];
  if (n < kGpuTimestampUsed || n > kGpuTimestampQueries) {
    return;
  }
  struct StampAvail {
    uint64_t ts = 0;
    uint64_t avail = 0;
  };
  StampAvail stamps[kGpuTimestampQueries]{};
  const VkResult qr = vkGetQueryPoolResults(
      device, gpu_timestamp_pools[frame], 0, n, n * sizeof(StampAvail), stamps, sizeof(StampAvail),
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if (qr != VK_SUCCESS) {
    return;
  }

  auto delta = [&](uint32_t a, uint32_t b) -> int64_t {
    if (a >= n || b >= n || stamps[a].avail == 0 || stamps[b].avail == 0) {
      return 0;
    }
    return gpu_timestamp_delta_ns(stamps[a].ts, stamps[b].ts);
  };
  const int64_t glow = delta(0, 1);
  const int64_t late = delta(1, 2);
  const int64_t opaque = delta(2, 3);
  const int64_t carve = delta(3, 4);
  const int64_t composite = delta(4, 5);
  const int64_t total = delta(0, n - 1);

  last_gpu_segments.clear();
  last_gpu_segments.reserve(6);
  const std::pair<const char*, int64_t> segs[] = {
      {"gpu.glow_hdr", glow}, {"gpu.late_hdr", late},     {"gpu.opaque", opaque},
      {"gpu.carve", carve},   {"gpu.composite", composite}, {"gpu.total", total},
  };
  for (const auto& seg : segs) {
    last_gpu_segments.push_back(seg);
    wds::common::perf::add_ns(wds::common::perf::stage_id(seg.first), seg.second);
  }
  if (!gpu_timestamps_sample_logged) {
    gpu_timestamps_sample_logged = true;
    WDS_LOG("gpu timestamps sample glow=%lld late=%lld opaque=%lld carve=%lld composite=%lld "
            "total=%lld ns\n",
            static_cast<long long>(glow), static_cast<long long>(late),
            static_cast<long long>(opaque), static_cast<long long>(carve),
            static_cast<long long>(composite), static_cast<long long>(total));
  }
}

bool VulkanRenderer::Impl::begin_gpu_timestamps(uint32_t frame, VkCommandBuffer cmd) {
  gpu_timestamps_recording = false;
  gpu_timestamp_cursor = 0;
  if (!wds::common::perf::enabled() || cmd == VK_NULL_HANDLE) {
    return false;
  }
  if (!ensure_gpu_timestamp_pool(frame)) {
    return false;
  }
  vkCmdResetQueryPool(cmd, gpu_timestamp_pools[frame], 0, kGpuTimestampQueries);
  gpu_timestamps_recording = true;
  gpu_timestamp_cursor = 0;
  gpu_timestamp_frame = frame;
  return true;
}

void VulkanRenderer::Impl::write_gpu_timestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage) {
  if (!gpu_timestamps_recording || cmd == VK_NULL_HANDLE) {
    return;
  }
  if (gpu_timestamp_cursor >= kGpuTimestampQueries) {
    return;
  }
  if (gpu_timestamp_frame >= static_cast<uint32_t>(kMaxFramesInFlight) ||
      gpu_timestamp_pools[gpu_timestamp_frame] == VK_NULL_HANDLE) {
    return;
  }
  vkCmdWriteTimestamp(cmd, stage, gpu_timestamp_pools[gpu_timestamp_frame], gpu_timestamp_cursor);
  ++gpu_timestamp_cursor;
}

void VulkanRenderer::Impl::commit_gpu_timestamps(uint32_t frame) {
  if (!gpu_timestamps_recording || frame >= static_cast<uint32_t>(kMaxFramesInFlight)) {
    gpu_timestamps_recording = false;
    return;
  }
  gpu_timestamp_written[frame] = gpu_timestamp_cursor;
  gpu_timestamp_pending[frame] = gpu_timestamp_cursor >= 2;
  gpu_timestamps_recording = false;
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
    // Idle proves every submitted frame is done.
    completed_submit_seq = advance_completed_submit(completed_submit_seq, submit_seq);
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
  // Unity project is Gamma (m_ActiveColorSpace=0). Shaders write
  // display-referred values. Prefer UNORM so an _SRGB attachment does
  // not encode gamma a second time.
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ColorSpace.html
  VkSurfaceFormatKHR chosen = formats[0];
  bool found_unorm = false;
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
      chosen = f;
      found_unorm = true;
      break;
    }
  }
  if (!found_unorm) {
    for (const auto& f : formats) {
      if (f.format == VK_FORMAT_B8G8R8A8_SRGB) {
        chosen = f;
        break;
      }
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
  char mode_list[96];
  format_present_modes(mode_list, sizeof(mode_list), presents);
  char alpha_list[64];
  format_alpha_mask(alpha_list, sizeof(alpha_list), caps.supportedCompositeAlpha);
  char current_extent[32];
  if (caps.currentExtent.width == UINT32_MAX) {
    std::snprintf(current_extent, sizeof(current_extent), "unspecified");
  } else {
    std::snprintf(current_extent, sizeof(current_extent), "%ux%u", caps.currentExtent.width,
                  caps.currentExtent.height);
  }
  // Opaque alpha on a child HWND, a non-identity transform, or an unexpected
  // format can present successfully and still show black. Log the contract
  // the driver actually offered, not only the values we picked.
  WDS_LOG("swapchain present=%s modes=%s format=%s/%u colorspace=%s/%u extent=%ux%u "
          "source=%s current=%s requested=%dx%d transform=%s alpha=%s supported=%s\n",
          present_mode_name(present_mode), mode_list, vk_format_name(chosen.format),
          static_cast<unsigned>(chosen.format), vk_colorspace_name(chosen.colorSpace),
          static_cast<unsigned>(chosen.colorSpace), new_extent.width, new_extent.height,
          caps.currentExtent.width != UINT32_MAX ? "surface" : "clamped", current_extent, width,
          height, surface_transform_name(caps.currentTransform),
          composite_alpha_name(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR), alpha_list);
  // Release builds compile WDS_LOG to a ring; capture needs this on stderr.
  std::fprintf(stderr, "swapchain format=%s/%u colorspace=%s/%u\n",
               vk_format_name(chosen.format), static_cast<unsigned>(chosen.format),
               vk_colorspace_name(chosen.colorSpace),
               static_cast<unsigned>(chosen.colorSpace));

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
  if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
    info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
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
  const bool prev_can_blit = swapchain_can_blit;
  std::vector<VkImageView> old_views = std::move(swapchain_views);
  std::vector<VkFramebuffer> old_framebuffers = std::move(framebuffers);
  std::vector<VkImage> old_images = std::move(swapchain_images);
  std::vector<VkFence> old_images_in_flight = std::move(images_in_flight);
  std::vector<VkSemaphore> old_render_finished = std::move(render_finished);
  render_finished.clear();  // moved-from state is unspecified; start empty
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
    destroy_render_finished(render_finished);  // semaphores of the failed new chain
    render_finished = std::move(old_render_finished);
    vkDestroySwapchainKHR(device, new_swapchain, nullptr);
    swapchain = old_swapchain;
    swapchain_format = prev_format;
    swapchain_extent = prev_extent;
    swapchain_can_blit = prev_can_blit;
  };

  swapchain = new_swapchain;
  swapchain_format = chosen.format;
  swapchain_extent = new_extent;
  swapchain_can_blit = (info.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
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
  if (!create_render_finished(actual)) {
    restore_previous();
    return false;
  }
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

  // Success — wait for in-flight frames before retiring old swapchain / bloom.
  (void)device_wait_idle_result();
  destroy_render_finished(old_render_finished);
  destroy_gpu_timestamp_pools();
  if (!create_bloom_resources()) {
    WDS_LOG("bloom off (extent=%ux%u blit=%d)\n", swapchain_extent.width, swapchain_extent.height,
            swapchain_can_blit ? 1 : 0);
  } else {
    WDS_LOG("bloom ready extent=%ux%u\n", swapchain_extent.width, swapchain_extent.height);
  }
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

bool VulkanRenderer::Impl::ensure_ring_slot(FrameVertexBuffer& slot, size_t bytes, size_t ring,
                                           size_t ring_max, VkBufferUsageFlags usage) {
  if (bytes <= slot.capacity_bytes && slot.buffer != VK_NULL_HANDLE) {
    if (slot.mapped == nullptr) {
      return remap_frame_vertices() && slot.mapped != nullptr;
    }
    return true;
  }

  auto destroy_slot = [&]() {
    const VkDeviceMemory mapped_mem = slot.uses_staging ? slot.staging_memory : slot.memory;
    if (slot.mapped != nullptr && mapped_mem != VK_NULL_HANDLE) {
      vkUnmapMemory(device, mapped_mem);
    }
    slot.mapped = nullptr;
    if (slot.staging_buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, slot.staging_buffer, nullptr);
      slot.staging_buffer = VK_NULL_HANDLE;
    }
    if (slot.staging_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, slot.staging_memory, nullptr);
      slot.staging_memory = VK_NULL_HANDLE;
    }
    if (slot.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, slot.buffer, nullptr);
      slot.buffer = VK_NULL_HANDLE;
    }
    if (slot.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, slot.memory, nullptr);
      slot.memory = VK_NULL_HANDLE;
    }
    slot.coherent = true;
    slot.uses_staging = false;
    slot.mapped_alloc_bytes = 0;
  };
  destroy_slot();

  const size_t ring_bytes = ring > 0 ? ring : kRingVertexCapacityBytes;
  const size_t capacity = ring_slot_capacity(bytes, ring_bytes, ring_max);
  slot.capacity_bytes = 0;

  auto make_buffer = [&](VkBufferUsageFlags make_usage, VkBuffer* out) -> bool {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = capacity;
    info.usage = make_usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    return vkCreateBuffer(device, &info, nullptr, out) == VK_SUCCESS && *out != VK_NULL_HANDLE;
  };

  if (!make_buffer(usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &slot.buffer)) {
    return false;
  }
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, slot.buffer, &req);
  const std::vector<HostVisiblePick> types =
      rank_host_visible_vertex_types(physical, req.memoryTypeBits);
  for (const HostVisiblePick& pick : types) {
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = pick.type_index;
    slot.memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &alloc, nullptr, &slot.memory) != VK_SUCCESS ||
        slot.memory == VK_NULL_HANDLE) {
      slot.memory = VK_NULL_HANDLE;
      continue;
    }
    if (vkBindBufferMemory(device, slot.buffer, slot.memory, 0) != VK_SUCCESS) {
      vkFreeMemory(device, slot.memory, nullptr);
      slot.memory = VK_NULL_HANDLE;
      continue;
    }
    slot.mapped = nullptr;
    if (vkMapMemory(device, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped) != VK_SUCCESS ||
        slot.mapped == nullptr) {
      vkFreeMemory(device, slot.memory, nullptr);
      slot.memory = VK_NULL_HANDLE;
      continue;
    }
    slot.coherent = (pick.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    slot.uses_staging = false;
    slot.capacity_bytes = capacity;
    slot.mapped_alloc_bytes = req.size;
    return true;
  }

  // No host-visible type, or every map/alloc failed: DEVICE_LOCAL + staging.
  destroy_slot();
  if (!make_buffer(usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &slot.buffer)) {
    return false;
  }
  vkGetBufferMemoryRequirements(device, slot.buffer, &req);
  bool local_ok = false;
  try {
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        find_memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &slot.memory) == VK_SUCCESS &&
        slot.memory != VK_NULL_HANDLE &&
        vkBindBufferMemory(device, slot.buffer, slot.memory, 0) == VK_SUCCESS) {
      local_ok = true;
    }
  } catch (...) {
    local_ok = false;
  }
  if (!local_ok) {
    destroy_slot();
    return false;
  }
  if (!make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &slot.staging_buffer)) {
    destroy_slot();
    return false;
  }
  VkMemoryRequirements sreq{};
  vkGetBufferMemoryRequirements(device, slot.staging_buffer, &sreq);
  const std::vector<HostVisiblePick> staging_types =
      rank_host_visible_vertex_types(physical, sreq.memoryTypeBits);
  for (const HostVisiblePick& pick : staging_types) {
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = sreq.size;
    alloc.memoryTypeIndex = pick.type_index;
    slot.staging_memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &alloc, nullptr, &slot.staging_memory) != VK_SUCCESS ||
        slot.staging_memory == VK_NULL_HANDLE) {
      slot.staging_memory = VK_NULL_HANDLE;
      continue;
    }
    if (vkBindBufferMemory(device, slot.staging_buffer, slot.staging_memory, 0) != VK_SUCCESS) {
      vkFreeMemory(device, slot.staging_memory, nullptr);
      slot.staging_memory = VK_NULL_HANDLE;
      continue;
    }
    slot.mapped = nullptr;
    if (vkMapMemory(device, slot.staging_memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped) !=
            VK_SUCCESS ||
        slot.mapped == nullptr) {
      vkFreeMemory(device, slot.staging_memory, nullptr);
      slot.staging_memory = VK_NULL_HANDLE;
      continue;
    }
    slot.coherent = (pick.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    slot.uses_staging = true;
    slot.capacity_bytes = capacity;
    slot.mapped_alloc_bytes = sreq.size;
    return true;
  }
  destroy_slot();
  return false;
}

bool VulkanRenderer::Impl::ensure_frame_vertex_capacity(uint32_t frame, size_t bytes) {
  const size_t ring =
      persistent_ring_bytes > 0 ? persistent_ring_bytes : kRingVertexCapacityBytes;
  return ensure_ring_slot(frame_vertices[frame], bytes, ring, persistent_ring_max_bytes,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

bool VulkanRenderer::Impl::ensure_frame_index_capacity(uint32_t frame, size_t bytes) {
  const size_t ring = persistent_index_ring_bytes > 0
                          ? persistent_index_ring_bytes
                          : VulkanRenderer::kPersistentIndexRingBytes;
  return ensure_ring_slot(frame_indices[frame], bytes, ring, persistent_index_ring_max_bytes,
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

size_t VulkanRenderer::Impl::next_ring_target(size_t min_bytes) const noexcept {
  size_t t = persistent_ring_bytes;
  const size_t cap = persistent_ring_max_bytes > 0 ? persistent_ring_max_bytes : t;
  size_t need = min_bytes;
  if (ring_grow_pending_bytes > need) {
    need = ring_grow_pending_bytes;
  }
  if (need <= t) {
    return t;
  }
  while (t < need && t < cap) {
    if (t > cap / 2) {
      t = cap;
      break;
    }
    t *= 2;
  }
  if (t < need && need <= cap) {
    t = need;
  }
  constexpr size_t kStep = 16 * 1024 * 1024;
  t = ((t + kStep - 1) / kStep) * kStep;
  if (t > cap) {
    t = cap;
  }
  if (t < persistent_ring_bytes) {
    t = persistent_ring_bytes;
  }
  return t;
}

bool VulkanRenderer::Impl::commit_ring_grow(size_t min_bytes) {
  const size_t next = next_ring_target(min_bytes);
  if (next > persistent_ring_bytes) {
    WDS_LOG("vb_ring grow %zu -> %zu (need %zu max %zu)\n", persistent_ring_bytes, next, min_bytes,
            persistent_ring_max_bytes);
    persistent_ring_bytes = next;
    ring_grow_pending_bytes = 0;
    ring_high_water_frames = 0;
  }
  return min_bytes <= persistent_ring_bytes;
}

void VulkanRenderer::Impl::note_vb_high_water(size_t used_bytes) {
  const size_t ring = persistent_ring_bytes;
  if (ring == 0 || used_bytes == 0) {
    ring_high_water_frames = 0;
    return;
  }
  if (used_bytes * 4 > ring * 3) {
    ++ring_high_water_frames;
    if (ring_high_water_frames >= 8) {
      const size_t pending = next_ring_target(ring + 1);
      if (pending > ring) {
        ring_grow_pending_bytes = pending;
      }
    }
  } else {
    ring_high_water_frames = 0;
  }
}

size_t VulkanRenderer::Impl::next_ib_ring_target(size_t min_bytes) const noexcept {
  size_t t = persistent_index_ring_bytes;
  const size_t cap = persistent_index_ring_max_bytes > 0 ? persistent_index_ring_max_bytes : t;
  size_t need = min_bytes;
  if (ib_grow_pending_bytes > need) {
    need = ib_grow_pending_bytes;
  }
  if (need <= t) {
    return t;
  }
  while (t < need && t < cap) {
    if (t > cap / 2) {
      t = cap;
      break;
    }
    t *= 2;
  }
  if (t < need && need <= cap) {
    t = need;
  }
  constexpr size_t kStep = 8 * 1024 * 1024;
  t = ((t + kStep - 1) / kStep) * kStep;
  if (t > cap) {
    t = cap;
  }
  if (t < persistent_index_ring_bytes) {
    t = persistent_index_ring_bytes;
  }
  return t;
}

bool VulkanRenderer::Impl::commit_ib_ring_grow(size_t min_bytes) {
  const size_t next = next_ib_ring_target(min_bytes);
  if (next > persistent_index_ring_bytes) {
    WDS_LOG("ib_ring grow %zu -> %zu (need %zu max %zu)\n", persistent_index_ring_bytes, next,
            min_bytes, persistent_index_ring_max_bytes);
    persistent_index_ring_bytes = next;
    ib_grow_pending_bytes = 0;
    ib_high_water_frames = 0;
  }
  return min_bytes <= persistent_index_ring_bytes;
}

void VulkanRenderer::Impl::note_ib_high_water(size_t used_bytes) {
  const size_t ring = persistent_index_ring_bytes;
  if (ring == 0 || used_bytes == 0) {
    ib_high_water_frames = 0;
    return;
  }
  if (used_bytes * 4 > ring * 3) {
    ++ib_high_water_frames;
    if (ib_high_water_frames >= 8) {
      const size_t pending = next_ib_ring_target(ring + 1);
      if (pending > ring) {
        ib_grow_pending_bytes = pending;
      }
    }
  } else {
    ib_high_water_frames = 0;
  }
}

void VulkanRenderer::Impl::unmap_frame_vertices() {
  if (persistent_vb_locked) {
    return;
  }
  auto unmap = [&](FrameVertexBuffer& slot) {
    const VkDeviceMemory mem = slot.uses_staging ? slot.staging_memory : slot.memory;
    if (slot.mapped != nullptr && mem != VK_NULL_HANDLE) {
      vkUnmapMemory(device, mem);
      slot.mapped = nullptr;
    }
  };
  for (auto& slot : frame_vertices) {
    unmap(slot);
  }
  for (auto& slot : frame_indices) {
    unmap(slot);
  }
}

bool VulkanRenderer::Impl::remap_frame_vertices() {
  bool ok = true;
  auto remap = [&](FrameVertexBuffer& slot) {
    if (slot.mapped != nullptr || slot.buffer == VK_NULL_HANDLE) {
      return;
    }
    VkDeviceMemory mem = slot.uses_staging ? slot.staging_memory : slot.memory;
    if (mem == VK_NULL_HANDLE) {
      return;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
        mapped == nullptr) {
      ok = false;
      return;
    }
    slot.mapped = mapped;
  };
  for (auto& slot : frame_vertices) {
    remap(slot);
  }
  for (auto& slot : frame_indices) {
    remap(slot);
  }
  return ok;
}

void VulkanRenderer::Impl::destroy_frame_vertices() {
  auto destroy_one = [&](FrameVertexBuffer& slot) {
    const VkDeviceMemory mapped_mem = slot.uses_staging ? slot.staging_memory : slot.memory;
    if (slot.mapped != nullptr && mapped_mem != VK_NULL_HANDLE) {
      vkUnmapMemory(device, mapped_mem);
      slot.mapped = nullptr;
    }
    if (slot.staging_buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, slot.staging_buffer, nullptr);
      slot.staging_buffer = VK_NULL_HANDLE;
    }
    if (slot.staging_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, slot.staging_memory, nullptr);
      slot.staging_memory = VK_NULL_HANDLE;
    }
    if (slot.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, slot.buffer, nullptr);
      slot.buffer = VK_NULL_HANDLE;
    }
    if (slot.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, slot.memory, nullptr);
      slot.memory = VK_NULL_HANDLE;
    }
    slot.capacity_bytes = 0;
    slot.mapped_alloc_bytes = 0;
    slot.coherent = true;
    slot.uses_staging = false;
  };
  for (auto& slot : frame_vertices) {
    destroy_one(slot);
  }
  for (auto& slot : frame_indices) {
    destroy_one(slot);
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

void VulkanRenderer::Impl::note_slot_fence_signalled(uint32_t frame) noexcept {
  if (frame < slot_submit_seq.size()) {
    completed_submit_seq = advance_completed_submit(completed_submit_seq, slot_submit_seq[frame]);
  }
}

void VulkanRenderer::Impl::destroy_render_finished(std::vector<VkSemaphore>& sems) {
  if (device != VK_NULL_HANDLE) {
    for (VkSemaphore sem : sems) {
      if (sem) vkDestroySemaphore(device, sem, nullptr);
    }
  }
  sems.clear();
}

bool VulkanRenderer::Impl::create_render_finished(size_t count) {
  destroy_render_finished(render_finished);
  VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  render_finished.assign(count, VK_NULL_HANDLE);
  for (size_t i = 0; i < count; ++i) {
    if (!create_sync_object_ok(vkCreateSemaphore(device, &sem_info, nullptr, &render_finished[i]))) {
      destroy_render_finished(render_finished);
      return false;
    }
  }
  return true;
}

void VulkanRenderer::Impl::reap_retired_textures() {
  for (size_t i = 1; i < textures.size(); ++i) {
    GpuTexture& tex = textures[i];
    if (tex.alive || tex.image == VK_NULL_HANDLE) {
      continue;
    }
    const bool retire_due =
        texture_retire_gate_open(tex.retiring, tex.retire_after_submit, completed_submit_seq);
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

  if (cached_props_for != physical) {
    VkPhysicalDeviceProperties dev_props{};
    vkGetPhysicalDeviceProperties(physical, &dev_props);
    vkGetPhysicalDeviceMemoryProperties(physical, &cached_mem_props);
    cached_non_coherent_atom =
        dev_props.limits.nonCoherentAtomSize > 0 ? dev_props.limits.nonCoherentAtomSize : 1;
    cached_props_for = physical;
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
        device, cached_mem_props, cached_non_coherent_atom, bytes,
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
    bool found_unorm = false;
    for (const auto& f : formats) {
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
        swapchain_format = f.format;
        found_unorm = true;
        break;
      }
    }
    if (!found_unorm) {
      for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB) {
          swapchain_format = f.format;
          break;
        }
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
#if WDS_ENABLE_LOGGING
    WDS_LOG("vkCreateRenderPass failed format=%s/%u samples=%u\n", vk_format_name(swapchain_format),
            static_cast<unsigned>(swapchain_format), static_cast<unsigned>(msaa_samples));
#endif
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
#if WDS_ENABLE_LOGGING
    WDS_LOG("create_shader_module failed vert=%d frag=%d\n", vert ? 1 : 0, frag ? 1 : 0);
#endif
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

  // OpenWDS particle materials declare separate RGB/alpha factors.  Keep the
  // generic textured pipeline above on its existing Sonolus contract, and use
  // this exact attachment only for the split-particle pipelines below:
  //   ParticleAlpha:    SrcAlpha OneMinusSrcAlpha, SrcAlpha OneMinusSrcAlpha
  //   ParticleAdditive: SrcAlpha One,              SrcAlpha One
  // The previous particle variants inherited srcAlpha=ONE, which matched RGB
  // but changed the destination alpha channel relative to the source shader.
  VkPipelineColorBlendAttachmentState split_particle_blend_attachment = blend_attachment;
  split_particle_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  VkPipelineColorBlendStateCreateInfo split_particle_blend = blend;
  split_particle_blend.pAttachments = &split_particle_blend_attachment;

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
#if WDS_ENABLE_LOGGING
    WDS_LOG("vkCreateGraphicsPipelines failed pipeline=textured format=%s/%u samples=%u\n",
            vk_format_name(swapchain_format), static_cast<unsigned>(swapchain_format),
            static_cast<unsigned>(msaa_samples));
#endif
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    last_wsi_action = WsiRecoverAction::Fatal;
    return false;
  }

  // OpenWDS preview replaces ParticleAlpha with clip(a-0.01) AFTER
  // Custom1*(0.5)+1. Mid overlay matches that order (vertex color already
  // includes the multiply; the shader clips the product).
  int32_t mid_alpha_clip = 1;
  VkSpecializationMapEntry mid_clip_entry{};
  mid_clip_entry.constantID = 0;
  mid_clip_entry.offset = 0;
  mid_clip_entry.size = sizeof(int32_t);
  VkSpecializationInfo mid_clip_spec{};
  mid_clip_spec.mapEntryCount = 1;
  mid_clip_spec.pMapEntries = &mid_clip_entry;
  mid_clip_spec.dataSize = sizeof(int32_t);
  mid_clip_spec.pData = &mid_alpha_clip;
  stages[1].pSpecializationInfo = &mid_clip_spec;
  pipeline_info.pColorBlendState = &split_particle_blend;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                &pipeline_alpha_soft) != VK_SUCCESS) {
#if WDS_ENABLE_LOGGING
    WDS_LOG("vkCreateGraphicsPipelines failed pipeline=alpha_soft format=%s/%u samples=%u\n",
            vk_format_name(swapchain_format), static_cast<unsigned>(swapchain_format),
            static_cast<unsigned>(msaa_samples));
#endif
    pipeline_alpha_soft = VK_NULL_HANDLE;
  }

  // Additive particles (hit VFX): SRC_ALPHA + ONE, no depth write so layers stack.
  // OpenWDS SplitParticleAdditive.shader: clip(color.a - 0.001) → kAlphaClip=2.
  int32_t additive_alpha_clip = 2;
  VkSpecializationMapEntry additive_clip_entry{};
  additive_clip_entry.constantID = 0;
  additive_clip_entry.offset = 0;
  additive_clip_entry.size = sizeof(int32_t);
  VkSpecializationInfo additive_clip_spec{};
  additive_clip_spec.mapEntryCount = 1;
  additive_clip_spec.pMapEntries = &additive_clip_entry;
  additive_clip_spec.dataSize = sizeof(int32_t);
  additive_clip_spec.pData = &additive_alpha_clip;
  stages[1].pSpecializationInfo = &additive_clip_spec;
  VkPipelineDepthStencilStateCreateInfo depth_additive = depth_state;
  depth_additive.depthWriteEnable = VK_FALSE;
  VkPipelineColorBlendAttachmentState blend_add_attachment = split_particle_blend_attachment;
  blend_add_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_add_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  VkPipelineColorBlendStateCreateInfo blend_add = blend;
  blend_add.pAttachments = &blend_add_attachment;
  pipeline_info.pDepthStencilState = &depth_additive;
  pipeline_info.pColorBlendState = &blend_add;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                &pipeline_additive) != VK_SUCCESS) {
#if WDS_ENABLE_LOGGING
    WDS_LOG("vkCreateGraphicsPipelines failed pipeline=additive format=%s/%u samples=%u\n",
            vk_format_name(swapchain_format), static_cast<unsigned>(swapchain_format),
            static_cast<unsigned>(msaa_samples));
#endif
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    last_wsi_action = WsiRecoverAction::Fatal;
    return false;
  }

  // OpenWDS ButterflyEffect.shader has no fragment alpha clip.  Keep this
  // specialization isolated to DrawBucket::disable_alpha_clip so ordinary
  // ParticleAdditive sprites continue to use clip(a - 0.001).
  int32_t no_alpha_clip = 0;
  VkSpecializationInfo no_clip_spec = additive_clip_spec;
  no_clip_spec.pData = &no_alpha_clip;
  stages[1].pSpecializationInfo = &no_clip_spec;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                &pipeline_additive_no_clip) != VK_SUCCESS) {
    pipeline_additive_no_clip = VK_NULL_HANDLE;
    WDS_LOG("Butterfly no-clip additive pipeline unavailable; retaining generic additive path\n");
  }

  // OpenWDS SplitParticleTrailAdditive.shader: clip(color.a - 0.01) after
  // color *= _AddtiveIntencity (RGBA). Same SrcAlpha+One as Additive.
  int32_t trail_alpha_clip = 1;
  VkSpecializationInfo trail_clip_spec = additive_clip_spec;
  trail_clip_spec.pData = &trail_alpha_clip;
  stages[1].pSpecializationInfo = &trail_clip_spec;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                &pipeline_additive_clip01) != VK_SUCCESS) {
    pipeline_additive_clip01 = VK_NULL_HANDLE;
    WDS_LOG("Trail Additive clip(a-0.01) pipeline unavailable; falling back to clip 0.001\n");
  }

  const auto syuriken_vert_code = read_file(shader_dir + "/syuriken.vert.spv");
  const auto syuriken_frag_code = read_file(shader_dir + "/syuriken.frag.spv");
  VkShaderModule svert = create_shader_module(device, syuriken_vert_code);
  VkShaderModule sfrag = create_shader_module(device, syuriken_frag_code);
  if (svert && sfrag && pipeline_layout_syuriken != VK_NULL_HANDLE) {
    VkPipelineShaderStageCreateInfo sstages[2]{};
    sstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    sstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    sstages[0].module = svert;
    sstages[0].pName = "main";
    sstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    sstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    sstages[1].module = sfrag;
    sstages[1].pName = "main";
    // SplitEffectSyuriken's Unity material uses _Cull=2 (Back). Keep this
    // state local to the Syuriken pipeline; the regular particle materials
    // explicitly declare Cull Off and must continue using VK_CULL_MODE_NONE.
    VkPipelineRasterizationStateCreateInfo syuriken_raster = raster;
    syuriken_raster.cullMode = VK_CULL_MODE_BACK_BIT;
    pipeline_info.pStages = sstages;
    pipeline_info.layout = pipeline_layout_syuriken;
    pipeline_info.pRasterizationState = &syuriken_raster;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                  &pipeline_syuriken_add) != VK_SUCCESS) {
      pipeline_syuriken_add = VK_NULL_HANDLE;
      WDS_LOG("syuriken add pipeline unavailable — plate stays CPU-tessellated\n");
    }
    pipeline_info.pStages = stages;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.pRasterizationState = &raster;
  }
  if (svert) {
    vkDestroyShaderModule(device, svert, nullptr);
  }
  if (sfrag) {
    vkDestroyShaderModule(device, sfrag, nullptr);
  }

  vkDestroyShaderModule(device, vert, nullptr);
  vkDestroyShaderModule(device, frag, nullptr);

  if (!create_bloom_pipeline()) {
    WDS_LOG("bloom pipeline unavailable — Add glow stays unbloomed\n");
  }

  const auto hdr_vert_code = read_file(shader_dir + "/bloom.vert.spv");
  const auto hdr_frag_code = read_file(shader_dir + "/hdr_composite.frag.spv");
  VkShaderModule hdr_vert = create_shader_module(device, hdr_vert_code);
  VkShaderModule hdr_frag = create_shader_module(device, hdr_frag_code);
  if (hdr_vert && hdr_frag) {
    VkPipelineShaderStageCreateInfo hdr_stages[2]{};
    hdr_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    hdr_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    hdr_stages[0].module = hdr_vert;
    hdr_stages[0].pName = "main";
    hdr_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    hdr_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    hdr_stages[1].module = hdr_frag;
    hdr_stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo hdr_vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState hdr_blend_att = blend_add_attachment;
    hdr_blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    hdr_blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    hdr_blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    hdr_blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    VkPipelineColorBlendStateCreateInfo hdr_blend = blend_add;
    hdr_blend.pAttachments = &hdr_blend_att;
    pipeline_info.pStages = hdr_stages;
    pipeline_info.pVertexInputState = &hdr_vi;
    pipeline_info.pDepthStencilState = &depth_additive;
    pipeline_info.pColorBlendState = &hdr_blend;
    pipeline_info.renderPass = render_pass;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                  &pipeline_hdr_composite) != VK_SUCCESS) {
      pipeline_hdr_composite = VK_NULL_HANDLE;
    }
  }
  if (hdr_vert) {
    vkDestroyShaderModule(device, hdr_vert, nullptr);
  }
  if (hdr_frag) {
    vkDestroyShaderModule(device, hdr_frag, nullptr);
  }

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

void VulkanRenderer::Impl::destroy_bloom_resources() {
  bloom_ready = false;
  hdr_late_ready = false;
  hdr_can_scene_carve = false;
  auto free_hdr_desc = [&](VkDescriptorSet& desc, VkDescriptorPool& pool) {
    if (desc != VK_NULL_HANDLE && pool != VK_NULL_HANDLE) {
      vkFreeDescriptorSets(device, pool, 1, &desc);
      for (auto& block : descriptor_blocks) {
        if (block.pool == pool) {
          block.live = descriptor_pool_live_after_free(block.live);
          break;
        }
      }
    }
    desc = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
  };
  if (device == VK_NULL_HANDLE) {
    bloom_framebuffers.clear();
    bloom_extract_fb = VK_NULL_HANDLE;
    bloom_src_descriptor = VK_NULL_HANDLE;
    bloom_src_pool = VK_NULL_HANDLE;
    bloom_src_view = VK_NULL_HANDLE;
    bloom_src_image = VK_NULL_HANDLE;
    bloom_src_memory = VK_NULL_HANDLE;
    hdr_late_fb = VK_NULL_HANDLE;
    hdr_late_descriptor = VK_NULL_HANDLE;
    hdr_late_pool = VK_NULL_HANDLE;
    hdr_late_view = VK_NULL_HANDLE;
    hdr_late_image = VK_NULL_HANDLE;
    hdr_late_memory = VK_NULL_HANDLE;
    bloom_extent = {};
    return;
  }
  if (bloom_extract_fb) {
    vkDestroyFramebuffer(device, bloom_extract_fb, nullptr);
    bloom_extract_fb = VK_NULL_HANDLE;
  }
  if (hdr_late_fb) {
    vkDestroyFramebuffer(device, hdr_late_fb, nullptr);
    hdr_late_fb = VK_NULL_HANDLE;
  }
  for (auto fb : bloom_framebuffers) {
    if (fb) {
      vkDestroyFramebuffer(device, fb, nullptr);
    }
  }
  bloom_framebuffers.clear();
  free_hdr_desc(bloom_src_descriptor, bloom_src_pool);
  free_hdr_desc(hdr_late_descriptor, hdr_late_pool);
  if (bloom_src_view) {
    vkDestroyImageView(device, bloom_src_view, nullptr);
    bloom_src_view = VK_NULL_HANDLE;
  }
  if (hdr_late_view) {
    vkDestroyImageView(device, hdr_late_view, nullptr);
    hdr_late_view = VK_NULL_HANDLE;
  }
  if (bloom_src_image) {
    vkDestroyImage(device, bloom_src_image, nullptr);
    bloom_src_image = VK_NULL_HANDLE;
  }
  if (hdr_late_image) {
    vkDestroyImage(device, hdr_late_image, nullptr);
    hdr_late_image = VK_NULL_HANDLE;
  }
  if (bloom_src_memory) {
    vkFreeMemory(device, bloom_src_memory, nullptr);
    bloom_src_memory = VK_NULL_HANDLE;
  }
  if (hdr_late_memory) {
    vkFreeMemory(device, hdr_late_memory, nullptr);
    hdr_late_memory = VK_NULL_HANDLE;
  }
  bloom_extent = {};
}

void VulkanRenderer::Impl::destroy_bloom_pipeline() {
  if (device == VK_NULL_HANDLE) {
    pipeline_bloom = VK_NULL_HANDLE;
    pipeline_bloom_extract = VK_NULL_HANDLE;
    pipeline_bloom_extract_no_clip = VK_NULL_HANDLE;
    pipeline_bloom_extract_clip01 = VK_NULL_HANDLE;
    pipeline_bloom_extract_alpha = VK_NULL_HANDLE;
    pipeline_syuriken_extract = VK_NULL_HANDLE;
    pipeline_hdr_replace = VK_NULL_HANDLE;
    pipeline_alpha_1x = VK_NULL_HANDLE;
    bloom_pass = VK_NULL_HANDLE;
    bloom_extract_pass = VK_NULL_HANDLE;
    bloom_extract_load_pass = VK_NULL_HANDLE;
    return;
  }
  if (pipeline_bloom_extract) {
    vkDestroyPipeline(device, pipeline_bloom_extract, nullptr);
    pipeline_bloom_extract = VK_NULL_HANDLE;
  }
  if (pipeline_bloom_extract_no_clip) {
    vkDestroyPipeline(device, pipeline_bloom_extract_no_clip, nullptr);
    pipeline_bloom_extract_no_clip = VK_NULL_HANDLE;
  }
  if (pipeline_bloom_extract_clip01) {
    vkDestroyPipeline(device, pipeline_bloom_extract_clip01, nullptr);
    pipeline_bloom_extract_clip01 = VK_NULL_HANDLE;
  }
  if (pipeline_bloom_extract_alpha) {
    vkDestroyPipeline(device, pipeline_bloom_extract_alpha, nullptr);
    pipeline_bloom_extract_alpha = VK_NULL_HANDLE;
  }
  if (pipeline_syuriken_extract) {
    vkDestroyPipeline(device, pipeline_syuriken_extract, nullptr);
    pipeline_syuriken_extract = VK_NULL_HANDLE;
  }
  if (pipeline_hdr_replace) {
    vkDestroyPipeline(device, pipeline_hdr_replace, nullptr);
    pipeline_hdr_replace = VK_NULL_HANDLE;
  }
  if (pipeline_alpha_1x) {
    vkDestroyPipeline(device, pipeline_alpha_1x, nullptr);
    pipeline_alpha_1x = VK_NULL_HANDLE;
  }
  if (pipeline_bloom) {
    vkDestroyPipeline(device, pipeline_bloom, nullptr);
    pipeline_bloom = VK_NULL_HANDLE;
  }
  if (bloom_extract_load_pass) {
    vkDestroyRenderPass(device, bloom_extract_load_pass, nullptr);
    bloom_extract_load_pass = VK_NULL_HANDLE;
  }
  if (bloom_extract_pass) {
    vkDestroyRenderPass(device, bloom_extract_pass, nullptr);
    bloom_extract_pass = VK_NULL_HANDLE;
  }
  if (bloom_pass) {
    vkDestroyRenderPass(device, bloom_pass, nullptr);
    bloom_pass = VK_NULL_HANDLE;
  }
}

bool VulkanRenderer::Impl::create_bloom_pipeline() {
  destroy_bloom_pipeline();
  bloom_src_format = swapchain_format;
  const VkFormat hdr_candidates[] = {VK_FORMAT_R16G16B16A16_SFLOAT,
                                     VK_FORMAT_B10G11R11_UFLOAT_PACK32};
  for (VkFormat cand : hdr_candidates) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, cand, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((props.optimalTilingFeatures & need) == need) {
      bloom_src_format = cand;
      break;
    }
  }
  const auto vert_code = read_file(shader_dir + "/bloom.vert.spv");
  const auto frag_code = read_file(shader_dir + "/bloom.frag.spv");
  if (vert_code.empty() || frag_code.empty()) {
    WDS_LOG("bloom shaders missing in %s — Add glow stays unbloomed\n", shader_dir.c_str());
    return false;
  }
  VkShaderModule vert = create_shader_module(device, vert_code);
  VkShaderModule frag = create_shader_module(device, frag_code);
  if (!vert || !frag) {
    if (vert) {
      vkDestroyShaderModule(device, vert, nullptr);
    }
    if (frag) {
      vkDestroyShaderModule(device, frag, nullptr);
    }
    return false;
  }

  VkAttachmentDescription color{};
  color.format = swapchain_format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp_info.attachmentCount = 1;
  rp_info.pAttachments = &color;
  rp_info.subpassCount = 1;
  rp_info.pSubpasses = &subpass;
  rp_info.dependencyCount = 1;
  rp_info.pDependencies = &dep;
  if (vkCreateRenderPass(device, &rp_info, nullptr, &bloom_pass) != VK_SUCCESS) {
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    bloom_pass = VK_NULL_HANDLE;
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

  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth_state{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.blendEnable = VK_TRUE;
  blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
  blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
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
  pipeline_info.renderPass = bloom_pass;
  const bool ok =
      vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                &pipeline_bloom) == VK_SUCCESS;
  vkDestroyShaderModule(device, vert, nullptr);
  vkDestroyShaderModule(device, frag, nullptr);
  if (!ok) {
    destroy_bloom_pipeline();
    return false;
  }

  VkAttachmentDescription extract_color{};
  extract_color.format = bloom_src_format;
  extract_color.samples = VK_SAMPLE_COUNT_1_BIT;
  extract_color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  extract_color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  extract_color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  extract_color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  extract_color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  extract_color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference extract_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription extract_sub{};
  extract_sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  extract_sub.colorAttachmentCount = 1;
  extract_sub.pColorAttachments = &extract_ref;
  VkSubpassDependency extract_deps[2]{};
  extract_deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  extract_deps[0].dstSubpass = 0;
  extract_deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  extract_deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  extract_deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  extract_deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  extract_deps[1].srcSubpass = 0;
  extract_deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  extract_deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  extract_deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  extract_deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  extract_deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  VkRenderPassCreateInfo extract_rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  extract_rp.attachmentCount = 1;
  extract_rp.pAttachments = &extract_color;
  extract_rp.subpassCount = 1;
  extract_rp.pSubpasses = &extract_sub;
  extract_rp.dependencyCount = 2;
  extract_rp.pDependencies = extract_deps;
  if (vkCreateRenderPass(device, &extract_rp, nullptr, &bloom_extract_pass) != VK_SUCCESS) {
    bloom_extract_pass = VK_NULL_HANDLE;
    destroy_bloom_pipeline();
    return false;
  }
  VkAttachmentDescription load_color = extract_color;
  load_color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  load_color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkSubpassDependency load_deps[2]{};
  load_deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  load_deps[0].dstSubpass = 0;
  load_deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  load_deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  load_deps[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  load_deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  load_deps[1] = extract_deps[1];
  VkRenderPassCreateInfo load_rp = extract_rp;
  load_rp.pAttachments = &load_color;
  load_rp.pDependencies = load_deps;
  if (vkCreateRenderPass(device, &load_rp, nullptr, &bloom_extract_load_pass) != VK_SUCCESS) {
    bloom_extract_load_pass = VK_NULL_HANDLE;
  }

  const auto quad_vert_code = read_file(shader_dir + "/textured_quad.vert.spv");
  const auto quad_frag_code = read_file(shader_dir + "/textured_quad.frag.spv");
  VkShaderModule qvert = create_shader_module(device, quad_vert_code);
  VkShaderModule qfrag = create_shader_module(device, quad_frag_code);
  if (!qvert || !qfrag) {
    if (qvert) {
      vkDestroyShaderModule(device, qvert, nullptr);
    }
    if (qfrag) {
      vkDestroyShaderModule(device, qfrag, nullptr);
    }
    destroy_bloom_pipeline();
    return false;
  }
  VkPipelineShaderStageCreateInfo qstages[2]{};
  qstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  qstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  qstages[0].module = qvert;
  qstages[0].pName = "main";
  qstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  qstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  qstages[1].module = qfrag;
  qstages[1].pName = "main";
  // Bloom extract of Add particles: same ParticleAdditive clip 0.001.
  int32_t additive_alpha_clip = 2;
  VkSpecializationMapEntry clip_entry{};
  clip_entry.constantID = 0;
  clip_entry.size = sizeof(int32_t);
  VkSpecializationInfo clip_spec{};
  clip_spec.mapEntryCount = 1;
  clip_spec.pMapEntries = &clip_entry;
  clip_spec.dataSize = sizeof(int32_t);
  clip_spec.pData = &additive_alpha_clip;
  qstages[1].pSpecializationInfo = &clip_spec;

  VkVertexInputBindingDescription binding_desc{};
  binding_desc.stride = sizeof(DrawVertex);
  binding_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 3> attrs{};
  attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  attrs[0].offset = offsetof(DrawVertex, x);
  attrs[1].location = 1;
  attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
  attrs[1].offset = offsetof(DrawVertex, u);
  attrs[2].location = 2;
  attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
  attrs[2].offset = offsetof(DrawVertex, r);
  VkPipelineVertexInputStateCreateInfo qvertex{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  qvertex.vertexBindingDescriptionCount = 1;
  qvertex.pVertexBindingDescriptions = &binding_desc;
  qvertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  qvertex.pVertexAttributeDescriptions = attrs.data();
  VkPipelineInputAssemblyStateCreateInfo qia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  qia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo qvp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  qvp.viewportCount = 1;
  qvp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo qraster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  qraster.polygonMode = VK_POLYGON_MODE_FILL;
  qraster.cullMode = VK_CULL_MODE_NONE;
  qraster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  qraster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo qms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  qms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo qdepth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState qblend_att{};
  qblend_att.blendEnable = VK_TRUE;
  qblend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  qblend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  qblend_att.colorBlendOp = VK_BLEND_OP_ADD;
  qblend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  qblend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  qblend_att.alphaBlendOp = VK_BLEND_OP_ADD;
  qblend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo qblend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  qblend.attachmentCount = 1;
  qblend.pAttachments = &qblend_att;
  std::array<VkDynamicState, 2> qdyn_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo qdyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  qdyn.dynamicStateCount = 2;
  qdyn.pDynamicStates = qdyn_states.data();
  VkGraphicsPipelineCreateInfo qinfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  qinfo.stageCount = 2;
  qinfo.pStages = qstages;
  qinfo.pVertexInputState = &qvertex;
  qinfo.pInputAssemblyState = &qia;
  qinfo.pViewportState = &qvp;
  qinfo.pRasterizationState = &qraster;
  qinfo.pMultisampleState = &qms;
  qinfo.pDepthStencilState = &qdepth;
  qinfo.pColorBlendState = &qblend;
  qinfo.pDynamicState = &qdyn;
  qinfo.layout = pipeline_layout;
  qinfo.renderPass = bloom_extract_pass;
  const bool extract_ok =
      vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &qinfo, nullptr,
                                &pipeline_bloom_extract) == VK_SUCCESS;
  // ButterflyEffect.shader has no clip(), including when its bucket is
  // rendered into the additive/HDR extract.  Use the same blend state with a
  // separate specialization so generic additive extraction stays at 0.001.
  int32_t no_clip = 0;
  VkSpecializationInfo no_clip_spec = clip_spec;
  no_clip_spec.pData = &no_clip;
  qstages[1].pSpecializationInfo = &no_clip_spec;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &qinfo, nullptr,
                                &pipeline_bloom_extract_no_clip) != VK_SUCCESS) {
    pipeline_bloom_extract_no_clip = VK_NULL_HANDLE;
    WDS_LOG("Butterfly no-clip bloom pipeline unavailable; retaining generic extract path\n");
  }
  int32_t extract_clip01 = 1;
  VkSpecializationInfo extract_clip01_spec = clip_spec;
  extract_clip01_spec.pData = &extract_clip01;
  qstages[1].pSpecializationInfo = &extract_clip01_spec;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &qinfo, nullptr,
                                &pipeline_bloom_extract_clip01) != VK_SUCCESS) {
    pipeline_bloom_extract_clip01 = VK_NULL_HANDLE;
    WDS_LOG("Trail clip(a-0.01) bloom extract unavailable; falling back to clip 0.001\n");
  }
  VkPipelineColorBlendAttachmentState qblend_alpha_att = qblend_att;
  qblend_alpha_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  qblend_alpha_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  VkPipelineColorBlendStateCreateInfo qblend_alpha = qblend;
  qblend_alpha.pAttachments = &qblend_alpha_att;
  qinfo.pColorBlendState = &qblend_alpha;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &qinfo, nullptr,
                                &pipeline_bloom_extract_alpha) != VK_SUCCESS) {
    pipeline_bloom_extract_alpha = VK_NULL_HANDLE;
    WDS_LOG("ParticleAlpha bloom extract unavailable; Alpha particles stay swapchain-only\n");
  }
  qinfo.pColorBlendState = &qblend;
  vkDestroyShaderModule(device, qvert, nullptr);
  vkDestroyShaderModule(device, qfrag, nullptr);
  if (!extract_ok) {
    destroy_bloom_pipeline();
    return false;
  }

  const auto sy_vert_code = read_file(shader_dir + "/syuriken.vert.spv");
  const auto sy_frag_code = read_file(shader_dir + "/syuriken.frag.spv");
  VkShaderModule syvert = create_shader_module(device, sy_vert_code);
  VkShaderModule syfrag = create_shader_module(device, sy_frag_code);
  if (syvert && syfrag && pipeline_layout_syuriken != VK_NULL_HANDLE) {
    VkPipelineShaderStageCreateInfo systages[2]{};
    systages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    systages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    systages[0].module = syvert;
    systages[0].pName = "main";
    systages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    systages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    systages[1].module = syfrag;
    systages[1].pName = "main";
    // Match SplitEffectSyuriken material _Cull=2 (Back), without changing
    // the bloom extract pipeline used by ordinary additive particles.
    VkPipelineRasterizationStateCreateInfo syuriken_raster = qraster;
    syuriken_raster.cullMode = VK_CULL_MODE_BACK_BIT;
    qinfo.pStages = systages;
    qinfo.layout = pipeline_layout_syuriken;
    qinfo.pRasterizationState = &syuriken_raster;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &qinfo, nullptr,
                                  &pipeline_syuriken_extract) != VK_SUCCESS) {
      pipeline_syuriken_extract = VK_NULL_HANDLE;
      WDS_LOG("syuriken extract pipeline unavailable — plate stays CPU-tessellated\n");
    }
    qinfo.pRasterizationState = &qraster;
  }
  if (syvert) {
    vkDestroyShaderModule(device, syvert, nullptr);
  }
  if (syfrag) {
    vkDestroyShaderModule(device, syfrag, nullptr);
  }

  // 1-sample swapchain tail: replace carved HDR, then editor overlays.
  // Official dest+signed Add already happened in FP16; this is present only.
  if (bloom_pass != VK_NULL_HANDLE) {
    const auto hdr_vert_code = read_file(shader_dir + "/bloom.vert.spv");
    const auto hdr_frag_code = read_file(shader_dir + "/hdr_composite.frag.spv");
    VkShaderModule hdr_vert = create_shader_module(device, hdr_vert_code);
    VkShaderModule hdr_frag = create_shader_module(device, hdr_frag_code);
    if (hdr_vert && hdr_frag) {
      VkPipelineShaderStageCreateInfo hdr_stages[2]{};
      hdr_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      hdr_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      hdr_stages[0].module = hdr_vert;
      hdr_stages[0].pName = "main";
      hdr_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      hdr_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      hdr_stages[1].module = hdr_frag;
      hdr_stages[1].pName = "main";
      VkPipelineVertexInputStateCreateInfo hdr_vi{
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineColorBlendAttachmentState replace_att = qblend_att;
      replace_att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      replace_att.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
      replace_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      replace_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      VkPipelineColorBlendStateCreateInfo replace_blend = qblend;
      replace_blend.pAttachments = &replace_att;
      VkGraphicsPipelineCreateInfo replace_info = qinfo;
      replace_info.pStages = hdr_stages;
      replace_info.pVertexInputState = &hdr_vi;
      replace_info.pColorBlendState = &replace_blend;
      replace_info.layout = pipeline_layout;
      replace_info.renderPass = bloom_pass;
      if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &replace_info, nullptr,
                                    &pipeline_hdr_replace) != VK_SUCCESS) {
        pipeline_hdr_replace = VK_NULL_HANDLE;
      }
    }
    if (hdr_vert) {
      vkDestroyShaderModule(device, hdr_vert, nullptr);
    }
    if (hdr_frag) {
      vkDestroyShaderModule(device, hdr_frag, nullptr);
    }

    const auto a_vert_code = read_file(shader_dir + "/textured_quad.vert.spv");
    const auto a_frag_code = read_file(shader_dir + "/textured_quad.frag.spv");
    VkShaderModule a_vert = create_shader_module(device, a_vert_code);
    VkShaderModule a_frag = create_shader_module(device, a_frag_code);
    if (a_vert && a_frag) {
      int32_t clip = 1;
      VkSpecializationMapEntry clip_e{};
      clip_e.size = sizeof(int32_t);
      VkSpecializationInfo clip_i{};
      clip_i.mapEntryCount = 1;
      clip_i.pMapEntries = &clip_e;
      clip_i.dataSize = sizeof(int32_t);
      clip_i.pData = &clip;
      VkPipelineShaderStageCreateInfo a_stages[2]{};
      a_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      a_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      a_stages[0].module = a_vert;
      a_stages[0].pName = "main";
      a_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      a_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      a_stages[1].module = a_frag;
      a_stages[1].pName = "main";
      a_stages[1].pSpecializationInfo = &clip_i;
      VkPipelineColorBlendAttachmentState alpha_att = qblend_att;
      alpha_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      alpha_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      alpha_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      alpha_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      VkPipelineColorBlendStateCreateInfo alpha_blend = qblend;
      alpha_blend.pAttachments = &alpha_att;
      VkGraphicsPipelineCreateInfo a_info = qinfo;
      a_info.pStages = a_stages;
      a_info.pColorBlendState = &alpha_blend;
      a_info.layout = pipeline_layout;
      a_info.renderPass = bloom_pass;
      if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &a_info, nullptr,
                                    &pipeline_alpha_1x) != VK_SUCCESS) {
        pipeline_alpha_1x = VK_NULL_HANDLE;
      }
    }
    if (a_vert) {
      vkDestroyShaderModule(device, a_vert, nullptr);
    }
    if (a_frag) {
      vkDestroyShaderModule(device, a_frag, nullptr);
    }
  }
  return true;
}

bool VulkanRenderer::Impl::create_bloom_resources() {
  destroy_bloom_resources();
  // OpenWDS OfflineRhythmPreview camera: HDR on (m_HDR=1), post-processing
  // off. Official Add is SrcAlpha+One on that camera. sort≤1 extracts stay
  // cleared (fog) and One+One composite after notes. sort≥2 blits the
  // resolved scene into hdr_late, dest+signed Add, then One Zero replace.
  // Hits use the second cleared extract. Do not run record_bloom.
  if (device == VK_NULL_HANDLE || bloom_extract_pass == VK_NULL_HANDLE ||
      pipeline_bloom_extract == VK_NULL_HANDLE || pipeline_hdr_composite == VK_NULL_HANDLE ||
      swapchain_extent.width == 0 || swapchain_extent.height == 0) {
    return false;
  }
  bloom_extent = swapchain_extent;

  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {bloom_extent.width, bloom_extent.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.format = bloom_src_format;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateImage(device, &image_info, nullptr, &bloom_src_image) != VK_SUCCESS) {
    destroy_bloom_resources();
    return false;
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device, bloom_src_image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  try {
    alloc.memoryTypeIndex =
        find_memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  } catch (...) {
    destroy_bloom_resources();
    return false;
  }
  if (vkAllocateMemory(device, &alloc, nullptr, &bloom_src_memory) != VK_SUCCESS) {
    destroy_bloom_resources();
    return false;
  }
  vkBindImageMemory(device, bloom_src_image, bloom_src_memory, 0);

  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = bloom_src_image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = bloom_src_format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device, &view_info, nullptr, &bloom_src_view) != VK_SUCCESS) {
    destroy_bloom_resources();
    return false;
  }

  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = bloom_extract_pass;
  fb.attachmentCount = 1;
  fb.pAttachments = &bloom_src_view;
  fb.width = bloom_extent.width;
  fb.height = bloom_extent.height;
  fb.layers = 1;
  if (vkCreateFramebuffer(device, &fb, nullptr, &bloom_extract_fb) != VK_SUCCESS) {
    destroy_bloom_resources();
    return false;
  }

  GpuTexture probe{};
  probe.view = bloom_src_view;
  const UploadResult desc = create_texture_descriptor(probe);
  if (!desc.ok() || probe.descriptor == VK_NULL_HANDLE) {
    destroy_bloom_resources();
    return false;
  }
  bloom_src_descriptor = probe.descriptor;
  bloom_src_pool = probe.descriptor_pool;
  bloom_ready = true;

  // Second FP16 target: hit VFX extract, then reused as the scene-carve dest
  // (blit swapchain, official sort≥2 Add). Official camera HDR is camera-wide.
  auto alloc_late = [&]() -> bool {
    if (vkCreateImage(device, &image_info, nullptr, &hdr_late_image) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements late_req{};
    vkGetImageMemoryRequirements(device, hdr_late_image, &late_req);
    VkMemoryAllocateInfo late_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    late_alloc.allocationSize = late_req.size;
    try {
      late_alloc.memoryTypeIndex =
          find_memory_type(physical, late_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (...) {
      return false;
    }
    if (vkAllocateMemory(device, &late_alloc, nullptr, &hdr_late_memory) != VK_SUCCESS) {
      return false;
    }
    vkBindImageMemory(device, hdr_late_image, hdr_late_memory, 0);
    view_info.image = hdr_late_image;
    if (vkCreateImageView(device, &view_info, nullptr, &hdr_late_view) != VK_SUCCESS) {
      return false;
    }
    fb.pAttachments = &hdr_late_view;
    if (vkCreateFramebuffer(device, &fb, nullptr, &hdr_late_fb) != VK_SUCCESS) {
      return false;
    }
    GpuTexture late_probe{};
    late_probe.view = hdr_late_view;
    const UploadResult late_desc = create_texture_descriptor(late_probe);
    if (!late_desc.ok() || late_probe.descriptor == VK_NULL_HANDLE) {
      return false;
    }
    hdr_late_descriptor = late_probe.descriptor;
    hdr_late_pool = late_probe.descriptor_pool;
    return true;
  };
  if (alloc_late()) {
    hdr_late_ready = true;
  } else {
    if (hdr_late_fb) {
      vkDestroyFramebuffer(device, hdr_late_fb, nullptr);
      hdr_late_fb = VK_NULL_HANDLE;
    }
    if (hdr_late_view) {
      vkDestroyImageView(device, hdr_late_view, nullptr);
      hdr_late_view = VK_NULL_HANDLE;
    }
    if (hdr_late_image) {
      vkDestroyImage(device, hdr_late_image, nullptr);
      hdr_late_image = VK_NULL_HANDLE;
    }
    if (hdr_late_memory) {
      vkFreeMemory(device, hdr_late_memory, nullptr);
      hdr_late_memory = VK_NULL_HANDLE;
    }
    hdr_late_descriptor = VK_NULL_HANDLE;
    hdr_late_pool = VK_NULL_HANDLE;
    hdr_late_ready = false;
  }

  hdr_can_scene_carve = false;
  if (bloom_pass != VK_NULL_HANDLE && !swapchain_views.empty()) {
    bloom_framebuffers.assign(swapchain_views.size(), VK_NULL_HANDLE);
    VkFramebufferCreateInfo tail_fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    tail_fb.renderPass = bloom_pass;
    tail_fb.attachmentCount = 1;
    tail_fb.width = bloom_extent.width;
    tail_fb.height = bloom_extent.height;
    tail_fb.layers = 1;
    bool tail_ok = true;
    for (size_t i = 0; i < swapchain_views.size(); ++i) {
      tail_fb.pAttachments = &swapchain_views[i];
      if (vkCreateFramebuffer(device, &tail_fb, nullptr, &bloom_framebuffers[i]) != VK_SUCCESS) {
        tail_ok = false;
        break;
      }
    }
    if (!tail_ok) {
      for (auto fb : bloom_framebuffers) {
        if (fb) {
          vkDestroyFramebuffer(device, fb, nullptr);
        }
      }
      bloom_framebuffers.clear();
    }
  }
  if (hdr_late_ready && swapchain_can_blit && pipeline_hdr_replace != VK_NULL_HANDLE &&
      bloom_extract_load_pass != VK_NULL_HANDLE && !bloom_framebuffers.empty()) {
    VkFormatProperties sw{};
    VkFormatProperties hdr{};
    vkGetPhysicalDeviceFormatProperties(physical, swapchain_format, &sw);
    vkGetPhysicalDeviceFormatProperties(physical, bloom_src_format, &hdr);
    hdr_can_scene_carve =
        (sw.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 &&
        (hdr.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
  }
  return true;
}


struct VbSliceFix {
  const DrawVertex* src = nullptr;
  uint32_t dest_bucket = 0;
  uint32_t local_off = 0;
  uint32_t count = 0;
};

void vb_plan_append_fixed(std::vector<GpuDrawBucket>& buckets,
                          std::vector<VbSliceFix>& slices, const DrawBatch& src, bool skip_empty) {
  for (const auto& bucket : src.buckets) {
    if (skip_empty && bucket.vertices.empty()) {
      continue;
    }
    size_t idx = buckets.size();
    for (size_t i = 0; i < buckets.size(); ++i) {
      if (buckets[i].texture == bucket.texture &&
          buckets[i].disable_alpha_clip == bucket.disable_alpha_clip) {
        idx = i;
        break;
      }
    }
    if (idx == buckets.size()) {
      GpuDrawBucket g;
      g.texture = bucket.texture;
      g.disable_alpha_clip = bucket.disable_alpha_clip;
      g.overlay_pipe = bucket.overlay_pipe;
      buckets.push_back(g);
    }
    if (!bucket.vertices.empty()) {
      VbSliceFix slice;
      slice.src = bucket.vertices.data();
      slice.dest_bucket = static_cast<uint32_t>(idx);
      slice.local_off = buckets[idx].vert_count;
      slice.count = static_cast<uint32_t>(bucket.vertices.size());
      slices.push_back(slice);
      buckets[idx].vert_count += slice.count;
    }
  }
}

[[maybe_unused]] void vb_plan_append_list_fixed(std::vector<GpuDrawBucket>& buckets,
                               std::vector<VbSliceFix>& slices, const DrawBatch* const* extras,
                               size_t n) {
  if (extras == nullptr) {
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    if (extras[i] != nullptr) {
      vb_plan_append_fixed(buckets, slices, *extras[i], true);
    }
  }
}

[[maybe_unused]] uint32_t vb_plan_layout(std::vector<GpuDrawBucket>& buckets,
                        std::vector<VbSliceFix>& slices, uint32_t cursor) {
  for (auto& bucket : buckets) {
    bucket.first_vertex = cursor;
    cursor += bucket.vert_count;
  }
  for (auto& slice : slices) {
    slice.local_off = buckets[slice.dest_bucket].first_vertex + slice.local_off;
  }
  return cursor;
}

void vb_copy_slices(DrawVertex* dst, const std::vector<VbSliceFix>& slices) {
  for (const auto& slice : slices) {
    if (slice.src != nullptr && slice.count > 0) {
      std::memcpy(dst + slice.local_off, slice.src, static_cast<size_t>(slice.count) * sizeof(DrawVertex));
    }
  }
}

void vb_fill_dump_keep_empty(DrawBatch& out, const std::vector<GpuDrawBucket>& buckets,
                             const DrawVertex* mapped, const uint32_t* idx_mapped) {
  out.reset();
  out.overlay_ordered = true;
  for (const auto& bucket : buckets) {
    out.overlay_pipe = bucket.overlay_pipe;
    auto& dst = out.bucket_for(bucket.texture, bucket.disable_alpha_clip);
    if (bucket.index_count > 0 && mapped != nullptr && idx_mapped != nullptr) {
      const size_t i = dst.vertices.size();
      dst.vertices.resize(i + static_cast<size_t>(bucket.index_count));
      for (uint32_t k = 0; k < bucket.index_count; ++k) {
        dst.vertices[i + k] = mapped[idx_mapped[bucket.first_index + k]];
      }
      continue;
    }
    if (bucket.vert_count == 0) {
      continue;
    }
    const size_t i = dst.vertices.size();
    dst.vertices.resize(i + static_cast<size_t>(bucket.vert_count));
    if (mapped != nullptr) {
      std::memcpy(dst.vertices.data() + i, mapped + bucket.first_vertex,
                  static_cast<size_t>(bucket.vert_count) * sizeof(DrawVertex));
    }
  }
}

void vb_add_tex_key(std::vector<TexKeyFix>& order, TextureId texture, bool clip) {
  for (const auto& k : order) {
    if (k.texture == texture && k.disable_alpha_clip == clip) {
      return;
    }
  }
  order.push_back({texture, clip});
}

// Pack CPU heads from `cursor`, then emit draws in append_from merge order
// (same-texture concat). In-place extras keep their mapped addresses.
uint32_t vb_plan_layer_draws(std::vector<GpuDrawBucket>& draws, const DrawBatch* head,
                             bool head_keep_empty, bool head_first,
                             const DrawBatch* const* extras, size_t extra_n, uint32_t cursor,
                             DrawVertex* dst, bool extras_in_place, const DrawVertex* mapped,
                             uint32_t* idx_dst, uint32_t* idx_cursor, DrawPlanScratch& scratch) {
  std::vector<GpuDrawBucket>& head_packed = scratch.head_packed;
  head_packed.clear();
  if (head != nullptr) {
    for (const auto& bucket : head->buckets) {
      GpuDrawBucket g;
      g.texture = bucket.texture;
      g.disable_alpha_clip = bucket.disable_alpha_clip;
      g.overlay_pipe = bucket.overlay_pipe;
      g.first_vertex = cursor;
      g.vert_count = static_cast<uint32_t>(bucket.vertices.size());
      if (g.vert_count > 0 && dst != nullptr) {
        std::memcpy(dst + cursor, bucket.vertices.data(),
                    static_cast<size_t>(g.vert_count) * sizeof(DrawVertex));
      }
      if (!bucket.indices.empty() && idx_dst != nullptr && idx_cursor != nullptr) {
        g.first_index = *idx_cursor;
        g.index_count = static_cast<uint32_t>(bucket.indices.size());
        for (uint32_t i = 0; i < g.index_count; ++i) {
          idx_dst[(*idx_cursor)++] = cursor + bucket.indices[i];
        }
      }
      cursor += g.vert_count;
      if (head_keep_empty || g.vert_count > 0 || g.index_count > 0) {
        head_packed.push_back(g);
      }
    }
  }

  std::vector<TexKeyFix>& order = scratch.order;
  order.clear();
  auto add_head_keys = [&]() {
    for (const auto& g : head_packed) {
      vb_add_tex_key(order, g.texture, g.disable_alpha_clip);
    }
  };
  auto add_extra_keys = [&]() {
    if (extras == nullptr) {
      return;
    }
    for (size_t i = 0; i < extra_n; ++i) {
      if (extras[i] == nullptr) {
        continue;
      }
      const DrawBatch& ex = *extras[i];
      if (extras_in_place && ex.ext_dst != nullptr) {
        for (const auto& run : ex.ext_runs) {
          vb_add_tex_key(order, run.texture, run.disable_alpha_clip);
        }
      } else {
        for (const auto& bucket : ex.buckets) {
          if (!bucket.vertices.empty()) {
            vb_add_tex_key(order, bucket.texture, bucket.disable_alpha_clip);
          }
        }
      }
    }
  };
  if (head_first) {
    add_head_keys();
    add_extra_keys();
  } else {
    add_extra_keys();
    add_head_keys();
  }

  auto emit_head = [&](const TexKeyFix& key) {
    for (const auto& g : head_packed) {
      if (g.texture == key.texture && g.disable_alpha_clip == key.disable_alpha_clip) {
        draws.push_back(g);
      }
    }
  };
  auto emit_extras = [&](const TexKeyFix& key) {
    if (extras == nullptr) {
      return;
    }
    for (size_t i = 0; i < extra_n; ++i) {
      if (extras[i] == nullptr) {
        continue;
      }
      const DrawBatch& ex = *extras[i];
      if (extras_in_place && ex.ext_dst != nullptr && mapped != nullptr) {
        for (const auto& run : ex.ext_runs) {
          if (run.texture != key.texture || run.disable_alpha_clip != key.disable_alpha_clip ||
              run.count == 0) {
            continue;
          }
          GpuDrawBucket g;
          g.texture = run.texture;
          g.disable_alpha_clip = run.disable_alpha_clip;
          g.overlay_pipe = run.overlay_pipe;
          g.first_vertex = static_cast<uint32_t>(ex.ext_dst - mapped) + run.first;
          g.vert_count = run.count;
          if (run.index_count > 0 && idx_dst != nullptr && ex.ext_idx != nullptr) {
            g.first_index = static_cast<uint32_t>(ex.ext_idx - idx_dst) + run.index_first;
            g.index_count = run.index_count;
          }
          draws.push_back(g);
        }
        continue;
      }
      for (const auto& bucket : ex.buckets) {
        if (bucket.texture != key.texture || bucket.disable_alpha_clip != key.disable_alpha_clip ||
            bucket.vertices.empty()) {
          continue;
        }
        GpuDrawBucket g;
        g.texture = bucket.texture;
        g.disable_alpha_clip = bucket.disable_alpha_clip;
        g.overlay_pipe = bucket.overlay_pipe;
        g.first_vertex = cursor;
        g.vert_count = static_cast<uint32_t>(bucket.vertices.size());
        if (dst != nullptr) {
          std::memcpy(dst + cursor, bucket.vertices.data(),
                      static_cast<size_t>(g.vert_count) * sizeof(DrawVertex));
        }
        if (!bucket.indices.empty() && idx_dst != nullptr && idx_cursor != nullptr) {
          g.first_index = *idx_cursor;
          g.index_count = static_cast<uint32_t>(bucket.indices.size());
          for (uint32_t k = 0; k < g.index_count; ++k) {
            idx_dst[(*idx_cursor)++] = cursor + bucket.indices[k];
          }
        }
        cursor += g.vert_count;
        draws.push_back(g);
      }
    }
  };
  for (const auto& key : order) {
    if (head_first) {
      emit_head(key);
      emit_extras(key);
    } else {
      emit_extras(key);
      emit_head(key);
    }
  }
  return cursor;
}

// Keep DrawBatch / ext_run order (no same-texture regroup). Used for
// split-effect particles so sortingOrder survives blend changes.
uint32_t vb_plan_ordered_draws(std::vector<GpuDrawBucket>& draws, const DrawBatch* head,
                               const DrawBatch* const* extras, size_t extra_n, uint32_t cursor,
                               DrawVertex* dst, bool extras_in_place, const DrawVertex* mapped,
                               uint32_t* idx_dst, uint32_t* idx_cursor) {
  auto emit_batch = [&](const DrawBatch& src) {
    if (extras_in_place && src.ext_dst != nullptr && mapped != nullptr) {
      for (const auto& run : src.ext_runs) {
        if (run.count == 0 && run.index_count == 0) {
          continue;
        }
        GpuDrawBucket g;
        g.texture = run.texture;
        g.disable_alpha_clip = run.disable_alpha_clip;
        g.overlay_pipe = run.overlay_pipe;
        g.first_vertex = static_cast<uint32_t>(src.ext_dst - mapped) + run.first;
        g.vert_count = run.count;
        if (run.index_count > 0 && idx_dst != nullptr && src.ext_idx != nullptr) {
          g.first_index = static_cast<uint32_t>(src.ext_idx - idx_dst) + run.index_first;
          g.index_count = run.index_count;
        }
        draws.push_back(g);
      }
      return;
    }
    for (const auto& bucket : src.buckets) {
      if (bucket.vertices.empty()) {
        continue;
      }
      GpuDrawBucket g;
      g.texture = bucket.texture;
      g.disable_alpha_clip = bucket.disable_alpha_clip;
      g.overlay_pipe = bucket.overlay_pipe;
      g.first_vertex = cursor;
      g.vert_count = static_cast<uint32_t>(bucket.vertices.size());
      if (dst != nullptr) {
        std::memcpy(dst + cursor, bucket.vertices.data(),
                    static_cast<size_t>(g.vert_count) * sizeof(DrawVertex));
      }
      if (!bucket.indices.empty() && idx_dst != nullptr && idx_cursor != nullptr) {
        g.first_index = *idx_cursor;
        g.index_count = static_cast<uint32_t>(bucket.indices.size());
        for (uint32_t k = 0; k < g.index_count; ++k) {
          idx_dst[(*idx_cursor)++] = cursor + bucket.indices[k];
        }
      }
      cursor += g.vert_count;
      draws.push_back(g);
    }
  };
  if (extras != nullptr) {
    for (size_t i = 0; i < extra_n; ++i) {
      if (extras[i] != nullptr) {
        emit_batch(*extras[i]);
      }
    }
  }
  if (head != nullptr) {
    emit_batch(*head);
  }
  return cursor;
}

uint32_t gpu_buckets_vert_count(const std::vector<GpuDrawBucket>& buckets) {
  uint32_t n = 0;
  for (const auto& b : buckets) {
    n += b.index_count > 0 ? b.index_count : b.vert_count;
  }
  return n;
}

// Consecutive same texture/clip whose firstVertex ranges abut become one
// vkCmdDraw. Indexed draws merge on abutting firstIndex ranges.
void coalesce_contiguous_gpu_buckets(std::vector<GpuDrawBucket>& draws) {
  size_t w = 0;
  for (size_t i = 0; i < draws.size(); ++i) {
    if (draws[i].vert_count == 0 && draws[i].index_count == 0) {
      continue;
    }
    if (w > 0 && draws[w - 1].texture == draws[i].texture &&
        draws[w - 1].disable_alpha_clip == draws[i].disable_alpha_clip &&
        draws[w - 1].overlay_pipe == draws[i].overlay_pipe) {
      const bool a_idx = draws[w - 1].index_count > 0;
      const bool b_idx = draws[i].index_count > 0;
      if (a_idx && b_idx &&
          draws[w - 1].first_index + draws[w - 1].index_count == draws[i].first_index) {
        draws[w - 1].index_count += draws[i].index_count;
        draws[w - 1].vert_count += draws[i].vert_count;
        continue;
      }
      if (!a_idx && !b_idx &&
          draws[w - 1].first_vertex + draws[w - 1].vert_count == draws[i].first_vertex) {
        draws[w - 1].vert_count += draws[i].vert_count;
        continue;
      }
    }
    if (w != i) {
      draws[w] = draws[i];
    }
    ++w;
  }
  draws.resize(w);
}

void VulkanRenderer::Impl::record_glow_extract(VkCommandBuffer cmd, const DrawBatch& early,
                                               const std::vector<uint32_t>& first_vertex,
                                               VkBuffer vb, const float* mvp,
                                               VkRect2D scissor_full, VkFramebuffer extract_fb,
                                               bool load_existing) {
  std::vector<GpuDrawBucket> gpu;
  gpu.reserve(early.buckets.size());
  for (size_t i = 0; i < early.buckets.size(); ++i) {
    GpuDrawBucket b;
    b.texture = early.buckets[i].texture;
    b.disable_alpha_clip = early.buckets[i].disable_alpha_clip;
    b.overlay_pipe = early.buckets[i].overlay_pipe;
    b.first_vertex = i < first_vertex.size() ? first_vertex[i] : 0;
    b.vert_count = static_cast<uint32_t>(early.buckets[i].vertices.size());
    gpu.push_back(b);
  }
  record_glow_extract_gpu(cmd, gpu.data(), gpu.size(), static_cast<uint32_t>(early.vertex_count()),
                          vb, mvp, scissor_full, extract_fb, load_existing);
}

void VulkanRenderer::Impl::record_glow_extract_gpu(VkCommandBuffer cmd, const GpuDrawBucket* buckets,
                                                  size_t bucket_n, uint32_t vert_count, VkBuffer vb,
                                                  const float* mvp, VkRect2D scissor_full,
                                                  VkFramebuffer extract_fb, bool load_existing,
                                                  VkBuffer ib) {
  const VkRenderPass pass = load_existing ? bloom_extract_load_pass : bloom_extract_pass;
  if (!bloom_ready || extract_fb == VK_NULL_HANDLE || pipeline_bloom_extract == VK_NULL_HANDLE ||
      pass == VK_NULL_HANDLE || bloom_extent.width == 0 || bloom_extent.height == 0 ||
      vert_count == 0 || buckets == nullptr || bucket_n == 0) {
    return;
  }

  VkClearValue clear{};
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = pass;
  rp.framebuffer = extract_fb;
  rp.renderArea.extent = bloom_extent;
  if (!load_existing) {
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
  }
  vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  ++last_render_passes;

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = static_cast<float>(bloom_extent.height);
  viewport.width = static_cast<float>(bloom_extent.width);
  viewport.height = -static_cast<float>(bloom_extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor{};
  if (swapchain_extent.width > 0 && swapchain_extent.height > 0) {
    const float sx = static_cast<float>(bloom_extent.width) /
                     static_cast<float>(swapchain_extent.width);
    const float sy = static_cast<float>(bloom_extent.height) /
                     static_cast<float>(swapchain_extent.height);
    const int x0 = static_cast<int>(static_cast<float>(scissor_full.offset.x) * sx);
    const int y0 = static_cast<int>(static_cast<float>(scissor_full.offset.y) * sy);
    const int x1 = static_cast<int>(
        static_cast<float>(scissor_full.offset.x + static_cast<int32_t>(scissor_full.extent.width)) *
        sx);
    const int y1 = static_cast<int>(
        static_cast<float>(scissor_full.offset.y + static_cast<int32_t>(scissor_full.extent.height)) *
        sy);
    const int bw = static_cast<int>(bloom_extent.width);
    const int bh = static_cast<int>(bloom_extent.height);
    const int cx0 = std::clamp(x0, 0, bw);
    const int cy0 = std::clamp(y0, 0, bh);
    const int cx1 = std::clamp(x1, 0, bw);
    const int cy1 = std::clamp(y1, 0, bh);
    scissor.offset.x = static_cast<int32_t>(cx0);
    scissor.offset.y = static_cast<int32_t>(cy0);
    scissor.extent.width = static_cast<uint32_t>(std::max(0, cx1 - cx0));
    scissor.extent.height = static_cast<uint32_t>(std::max(0, cy1 - cy0));
  } else {
    scissor.extent = bloom_extent;
  }
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
  ++last_vb_binds;
  if (ib != VK_NULL_HANDLE) {
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
  }
  VkPipeline bound = VK_NULL_HANDLE;
  TextureId bound_tex = kInvalidTextureId;
  VkPipelineLayout bound_layout = VK_NULL_HANDLE;
  for (size_t bi = 0; bi < bucket_n; ++bi) {
    const GpuDrawBucket& bucket = buckets[bi];
    if ((bucket.vert_count == 0 && bucket.index_count == 0) ||
        bucket.texture == kInvalidTextureId || bucket.texture >= textures.size() ||
        !textures[bucket.texture].alive) {
      continue;
    }
    DrawBucket probe;
    probe.texture = bucket.texture;
    probe.disable_alpha_clip = bucket.disable_alpha_clip;
    const bool syuriken =
        bucket_is_syuriken(probe) && pipeline_syuriken_extract != VK_NULL_HANDLE;
    VkPipeline want = pipeline_bloom_extract;
    if (syuriken) {
      want = pipeline_syuriken_extract;
    } else if (bucket.overlay_pipe == static_cast<uint8_t>(DrawOverlayPipe::Alpha) &&
               pipeline_bloom_extract_alpha != VK_NULL_HANDLE) {
      want = pipeline_bloom_extract_alpha;
    } else if (bucket.overlay_pipe == static_cast<uint8_t>(DrawOverlayPipe::AdditiveClip01) &&
               pipeline_bloom_extract_clip01 != VK_NULL_HANDLE) {
      want = pipeline_bloom_extract_clip01;
    } else if (bucket.disable_alpha_clip && pipeline_bloom_extract_no_clip != VK_NULL_HANDLE) {
      want = pipeline_bloom_extract_no_clip;
    }
    const VkPipelineLayout layout = syuriken ? pipeline_layout_syuriken : pipeline_layout;
    if (want != bound) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
      ++last_pipe_binds;
      if (syuriken) {
        push_syuriken_pc(cmd, mvp);
      } else {
        vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 16,
                           mvp);
      }
      ++last_push_consts;
    }
    textures[bucket.texture].sampled_seq = frame_seq;
    if (want != bound || bound_tex != bucket.texture || bound_layout != layout) {
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                              &textures[bucket.texture].descriptor, 0, nullptr);
      ++last_desc_binds;
      bound_tex = bucket.texture;
      bound_layout = layout;
    }
    bound = want;
    if (bucket.index_count > 0) {
      vkCmdDrawIndexed(cmd, bucket.index_count, 1, bucket.first_index, 0, 0);
    } else {
      vkCmdDraw(cmd, bucket.vert_count, 1, bucket.first_vertex, 0);
    }
    ++last_vk_draws;
  }
  vkCmdEndRenderPass(cmd);
}

void VulkanRenderer::Impl::record_bloom(VkCommandBuffer cmd, uint32_t image_index) {
  if (!bloom_ready || bloom_extent.width == 0 || bloom_extent.height == 0 ||
      image_index >= swapchain_images.size() || image_index >= bloom_framebuffers.size() ||
      bloom_framebuffers[image_index] == VK_NULL_HANDLE) {
    return;
  }

  image_barrier(cmd, swapchain_images[image_index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = bloom_pass;
  rp.framebuffer = bloom_framebuffers[image_index];
  rp.renderArea.extent = swapchain_extent;
  vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  ++last_render_passes;
  VkViewport viewport{};
  viewport.width = static_cast<float>(swapchain_extent.width);
  viewport.height = static_cast<float>(swapchain_extent.height);
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = swapchain_extent;
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_bloom);
  ++last_pipe_binds;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                          &bloom_src_descriptor, 0, nullptr);
  ++last_desc_binds;
  vkCmdDraw(cmd, 3, 1, 0, 0);
  ++last_vk_draws;
  vkCmdEndRenderPass(cmd);
}

void VulkanRenderer::Impl::destroy_render_pass_and_pipelines() {
  if (device == VK_NULL_HANDLE) {
    return;
  }
  destroy_bloom_resources();
  destroy_bloom_pipeline();
  if (pipeline) {
    vkDestroyPipeline(device, pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
  }
  if (pipeline_alpha_soft) {
    vkDestroyPipeline(device, pipeline_alpha_soft, nullptr);
    pipeline_alpha_soft = VK_NULL_HANDLE;
  }
  if (pipeline_additive) {
    vkDestroyPipeline(device, pipeline_additive, nullptr);
    pipeline_additive = VK_NULL_HANDLE;
  }
  if (pipeline_additive_no_clip) {
    vkDestroyPipeline(device, pipeline_additive_no_clip, nullptr);
    pipeline_additive_no_clip = VK_NULL_HANDLE;
  }
  if (pipeline_additive_clip01) {
    vkDestroyPipeline(device, pipeline_additive_clip01, nullptr);
    pipeline_additive_clip01 = VK_NULL_HANDLE;
  }
  if (pipeline_syuriken_add) {
    vkDestroyPipeline(device, pipeline_syuriken_add, nullptr);
    pipeline_syuriken_add = VK_NULL_HANDLE;
  }
  if (pipeline_hdr_composite) {
    vkDestroyPipeline(device, pipeline_hdr_composite, nullptr);
    pipeline_hdr_composite = VK_NULL_HANDLE;
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
  }
  // vkEnumeratePhysicalDeviceGroups is 1.1 core and is not enabled here as an
  // extension. A null pointer means the adopted instance is Vulkan 1.0, where
  // a negative viewport is illegal unless VK_KHR_maintenance1 is enabled.
  // Do not log app.apiVersion for a host instance: that value is only a local
  // request and previously reported 1.1 while Qt had created a 1.0 instance.
  const bool instance_core_11 =
      vkGetInstanceProcAddr(impl_->instance, "vkEnumeratePhysicalDeviceGroups") != nullptr;
  if (host.external_instance != VK_NULL_HANDLE) {
    WDS_LOG("using host-owned Vulkan instance core=%s\n", instance_core_11 ? "1.1+" : "1.0");
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
    impl_->non_coherent_atom =
        props.limits.nonCoherentAtomSize > 0 ? props.limits.nonCoherentAtomSize : 1;
    impl_->persistent_ring_bytes =
        choose_persistent_ring_bytes(impl_->physical, kMaxFramesInFlight);
    impl_->persistent_ring_max_bytes = clamp_persistent_ring_bytes(
        impl_->physical, kMaxFramesInFlight, VulkanRenderer::kPersistentVertexRingMaxWantBytes);
    if (impl_->persistent_ring_max_bytes < impl_->persistent_ring_bytes) {
      impl_->persistent_ring_max_bytes = impl_->persistent_ring_bytes;
    }
    impl_->persistent_index_ring_bytes = clamp_persistent_ring_bytes(
        impl_->physical, kMaxFramesInFlight, VulkanRenderer::kPersistentIndexRingBytes);
    impl_->persistent_index_ring_max_bytes = clamp_persistent_ring_bytes(
        impl_->physical, kMaxFramesInFlight, VulkanRenderer::kPersistentIndexRingMaxWantBytes);
    if (impl_->persistent_index_ring_max_bytes < impl_->persistent_index_ring_bytes) {
      impl_->persistent_index_ring_max_bytes = impl_->persistent_index_ring_bytes;
    }
    WDS_LOG("selected GPU='%s' type=%s vendor=0x%x api=%u.%u.%u drv=%u.%u.%u "
            "queue_family=%u msaa=%u vb_ring=%zu vb_ring_max=%zu ib_ring=%zu ib_ring_max=%zu "
            "atom=%llu\n",
            props.deviceName, physical_device_type_name(props.deviceType), props.vendorID,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion), VK_VERSION_MAJOR(props.driverVersion),
            VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion),
            impl_->graphics_family, static_cast<unsigned>(impl_->msaa_samples),
            impl_->persistent_ring_bytes, impl_->persistent_ring_max_bytes,
            impl_->persistent_index_ring_bytes, impl_->persistent_index_ring_max_bytes,
            static_cast<unsigned long long>(impl_->non_coherent_atom));
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
    bool maintenance1 = false;
    for (const auto& e : avail) {
      if (std::strcmp(e.extensionName, portability_subset) == 0) {
        device_exts.push_back(portability_subset);
        WDS_LOG("enabling device extension %s\n", portability_subset);
      }
      if (std::strcmp(e.extensionName, VK_KHR_MAINTENANCE1_EXTENSION_NAME) == 0) {
        device_exts.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
        maintenance1 = true;
        WDS_LOG("enabling device extension %s\n", VK_KHR_MAINTENANCE1_EXTENSION_NAME);
      }
#if defined(_WIN32)
      if (std::strcmp(e.extensionName, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME) == 0) {
        device_exts.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
        impl_->fse_extension = true;
        WDS_LOG("enabling device extension %s\n", VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
      }
#endif
    }
#if WDS_ENABLE_LOGGING
    WDS_LOG("viewport flip legal=%d instance_core_11=%d maintenance1=%d\n",
            (instance_core_11 || maintenance1) ? 1 : 0, instance_core_11 ? 1 : 0,
            maintenance1 ? 1 : 0);
#endif
    if (!instance_core_11 && !maintenance1) {
      WDS_LOG("negative viewport unavailable: instance is Vulkan 1.0 and %s is missing\n",
              VK_KHR_MAINTENANCE1_EXTENSION_NAME);
      return false;
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

  VkPushConstantRange push_syuriken{};
  push_syuriken.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  push_syuriken.offset = 0;
  push_syuriken.size = sizeof(float) * 20;
  VkPipelineLayoutCreateInfo pl_syuriken = pl_info;
  pl_syuriken.pPushConstantRanges = &push_syuriken;
  if (vkCreatePipelineLayout(impl_->device, &pl_syuriken, nullptr,
                             &impl_->pipeline_layout_syuriken) != VK_SUCCESS) {
    impl_->pipeline_layout_syuriken = VK_NULL_HANDLE;
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
            vkCreateFence(impl_->device, &fence_info, nullptr, &impl_->in_flight[i]))) {
      return false;
    }
  }

  impl_->textures.resize(1);  // slot 0 unused
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (!impl_->ensure_frame_vertex_capacity(i, impl_->persistent_ring_bytes)) {
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
      if (impl_->in_flight[i]) vkDestroyFence(impl_->device, impl_->in_flight[i], nullptr);
    }

    impl_->cleanup_swapchain();  // also destroys the per-image render_finished semaphores
    impl_->destroy_render_finished(impl_->render_finished);  // no-op normally
    impl_->destroy_gpu_timestamp_pools();
    impl_->destroy_bloom_pipeline();
    if (impl_->pipeline) vkDestroyPipeline(impl_->device, impl_->pipeline, nullptr);
    if (impl_->pipeline_alpha_soft)
      vkDestroyPipeline(impl_->device, impl_->pipeline_alpha_soft, nullptr);
    if (impl_->pipeline_additive) vkDestroyPipeline(impl_->device, impl_->pipeline_additive, nullptr);
    if (impl_->pipeline_additive_no_clip)
      vkDestroyPipeline(impl_->device, impl_->pipeline_additive_no_clip, nullptr);
    if (impl_->pipeline_additive_clip01)
      vkDestroyPipeline(impl_->device, impl_->pipeline_additive_clip01, nullptr);
    if (impl_->pipeline_syuriken_add)
      vkDestroyPipeline(impl_->device, impl_->pipeline_syuriken_add, nullptr);
    if (impl_->pipeline_layout_syuriken)
      vkDestroyPipelineLayout(impl_->device, impl_->pipeline_layout_syuriken, nullptr);
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

void VulkanRenderer::abandon_host_surface() {
  if (impl_ == nullptr || renderer_health_unrecoverable(health_)) {
    return;
  }
  if (impl_->device != VK_NULL_HANDLE) {
    const VkResult idle = vkDeviceWaitIdle(impl_->device);
    if (idle == VK_ERROR_DEVICE_LOST) {
      emit_health(RendererHealthEvent::DeviceLost);
      return;
    }
  }
  impl_->cleanup_swapchain();
  if (impl_->owns_surface && impl_->surface != VK_NULL_HANDLE && impl_->instance != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
  }
  impl_->surface = VK_NULL_HANDLE;
  emit_health(RendererHealthEvent::SurfaceLost);
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

int64_t VulkanRenderer::last_draw_record_us() const noexcept {
  return impl_ != nullptr ? impl_->last_draw_record_us : 0;
}

VulkanRenderer::DrawFrameCpuDiag VulkanRenderer::last_draw_cpu() const noexcept {
  DrawFrameCpuDiag out;
  if (impl_ == nullptr) {
    return out;
  }
  out.record_us = impl_->last_draw_record_us;
  out.used_bytes = impl_->last_vb_used_bytes;
  out.cap_bytes = impl_->last_vb_cap_bytes;
  out.ring_bytes = impl_->persistent_ring_bytes;
  out.ib_used_bytes = impl_->last_ib_used_bytes;
  out.ib_cap_bytes = impl_->last_ib_cap_bytes;
  out.ib_ring_bytes = impl_->persistent_index_ring_bytes;
  out.total_verts = impl_->last_total_verts;
  out.early_verts = impl_->last_early_verts;
  out.carve_verts = impl_->last_carve_verts;
  out.staging = impl_->last_vb_staging;
  out.bloom = impl_->last_bloom_pass;
  out.carve_pass = impl_->last_carve_pass;
  out.draws = impl_->last_vk_draws;
  out.pipe_binds = impl_->last_pipe_binds;
  out.desc_binds = impl_->last_desc_binds;
  out.push_consts = impl_->last_push_consts;
  out.render_passes = impl_->last_render_passes;
  out.vb_binds = impl_->last_vb_binds;
  return out;
}

const std::vector<std::pair<const char*, int64_t>>& VulkanRenderer::last_gpu_segments() const {
  static const std::vector<std::pair<const char*, int64_t>> kEmpty;
  return impl_ != nullptr ? impl_->last_gpu_segments : kEmpty;
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
      !recover_surface_has_reacquire(static_cast<bool>(impl_->create_surface),
                                    static_cast<bool>(impl_->acquire_surface),
                                    impl_->owns_surface) ||
      renderer_health_unrecoverable(health_)) {
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

void VulkanRenderer::set_syuriken_uniforms(float time_sec, TextureId plate_tex) noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->syuriken_time = time_sec;
  impl_->syuriken_plate_tex = plate_tex;
}

bool VulkanRenderer::syuriken_ready() const noexcept {
  return impl_ != nullptr && impl_->pipeline_layout_syuriken != VK_NULL_HANDLE &&
         (impl_->pipeline_syuriken_extract != VK_NULL_HANDLE ||
          impl_->pipeline_syuriken_add != VK_NULL_HANDLE);
}

void VulkanRenderer::Impl::push_syuriken_pc(VkCommandBuffer cmd, const float* mvp) const {
  float pc[20];
  std::memcpy(pc, mvp, 16 * sizeof(float));
  pc[16] = syuriken_time;
  pc[17] = 0.0f;
  pc[18] = syuriken_sprite;
  pc[19] = 0.0f;
  vkCmdPushConstants(cmd, pipeline_layout_syuriken,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
}

bool VulkanRenderer::Impl::bucket_is_syuriken(const DrawBucket& bucket) const noexcept {
  return syuriken_plate_tex != kInvalidTextureId && bucket.texture == syuriken_plate_tex &&
         pipeline_layout_syuriken != VK_NULL_HANDLE;
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
  impl_->reap_retired_textures();
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
      tex.retiring = true;
      tex.retire_after_submit = impl_->submit_seq;
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

bool VulkanRenderer::lock_persistent_vb(size_t min_bytes, PersistentVbLock* out,
                                        size_t min_index_bytes) {
  if (out != nullptr) {
    *out = PersistentVbLock{};
  }
  if (!ready_ || impl_ == nullptr || impl_->device == VK_NULL_HANDLE || out == nullptr) {
    return false;
  }
  if (min_bytes == 0) {
    return false;
  }
  // Grow now only if this lock cannot fit, or the 75% hint is pending and
  // this frame is light (avoid realloc during a 1M-vert peak).
  if (min_bytes > impl_->persistent_ring_bytes) {
    impl_->commit_ring_grow(min_bytes);
  } else if (impl_->ring_grow_pending_bytes > 0 &&
             min_bytes * 4 <= impl_->persistent_ring_bytes) {
    impl_->commit_ring_grow(impl_->ring_grow_pending_bytes);
  }
  if (min_index_bytes > impl_->persistent_index_ring_bytes) {
    impl_->commit_ib_ring_grow(min_index_bytes);
  } else if (impl_->ib_grow_pending_bytes > 0 &&
             min_index_bytes * 4 <= impl_->persistent_index_ring_bytes) {
    impl_->commit_ib_ring_grow(impl_->ib_grow_pending_bytes);
  }
  const size_t ring = impl_->persistent_ring_bytes > 0 ? impl_->persistent_ring_bytes
                                                       : kPersistentVertexRingBytes;
  const size_t ib_ring = impl_->persistent_index_ring_bytes > 0
                             ? impl_->persistent_index_ring_bytes
                             : kPersistentIndexRingBytes;
  if (min_bytes > ring) {
    return false;
  }
  if (impl_->persistent_vb_locked) {
    const uint32_t frame = impl_->frame_index;
    auto& slot = impl_->frame_vertices[frame];
    auto& ib = impl_->frame_indices[frame];
    if (slot.mapped != nullptr && slot.capacity_bytes >= min_bytes) {
      out->mapped = static_cast<DrawVertex*>(slot.mapped);
      out->capacity_bytes = slot.capacity_bytes;
      out->idx_mapped = static_cast<uint32_t*>(ib.mapped);
      out->idx_capacity_bytes = ib.capacity_bytes;
      out->frame = frame;
      return true;
    }
    // Tessellation already holds ext_dst into this mapping. Do not destroy it.
    return false;
  }
  const uint32_t frame = impl_->frame_index;
  if (frame >= impl_->in_flight.size() || impl_->in_flight[frame] == VK_NULL_HANDLE) {
    return false;
  }
  const auto fence_t0 = std::chrono::steady_clock::now();
  if (!check_device_result(vkWaitForFences(impl_->device, 1, &impl_->in_flight[frame], VK_TRUE,
                                          UINT64_MAX))) {
    return false;
  }
  impl_->last_fence_wait_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                           fence_t0)
          .count();
  impl_->note_slot_fence_signalled(frame);
  if (!impl_->remap_frame_vertices()) {
    return false;
  }
  if (!impl_->ensure_frame_vertex_capacity(frame, ring)) {
    return false;
  }
  const size_t ib_need = std::max(min_index_bytes, ib_ring);
  if (!impl_->ensure_frame_index_capacity(frame, ib_need)) {
    return false;
  }
  auto& slot = impl_->frame_vertices[frame];
  auto& ib = impl_->frame_indices[frame];
  if (slot.mapped == nullptr || slot.capacity_bytes < min_bytes) {
    return false;
  }
  impl_->persistent_vb_locked = true;
  out->mapped = static_cast<DrawVertex*>(slot.mapped);
  out->capacity_bytes = slot.capacity_bytes;
  out->idx_mapped = static_cast<uint32_t*>(ib.mapped);
  out->idx_capacity_bytes = ib.capacity_bytes;
  out->frame = frame;
  return true;
}

size_t VulkanRenderer::persistent_vertex_ring_bytes() const noexcept {
  if (impl_ == nullptr || impl_->persistent_ring_bytes == 0) {
    return kPersistentVertexRingBytes;
  }
  return impl_->persistent_ring_bytes;
}

size_t VulkanRenderer::persistent_vertex_ring_max_bytes() const noexcept {
  if (impl_ == nullptr || impl_->persistent_ring_max_bytes == 0) {
    return kPersistentVertexRingMaxWantBytes;
  }
  return impl_->persistent_ring_max_bytes;
}

size_t VulkanRenderer::persistent_index_ring_bytes() const noexcept {
  if (impl_ == nullptr || impl_->persistent_index_ring_bytes == 0) {
    return kPersistentIndexRingBytes;
  }
  return impl_->persistent_index_ring_bytes;
}

size_t VulkanRenderer::persistent_index_ring_max_bytes() const noexcept {
  if (impl_ == nullptr || impl_->persistent_index_ring_max_bytes == 0) {
    return kPersistentIndexRingMaxWantBytes;
  }
  return impl_->persistent_index_ring_max_bytes;
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
  // Reaped once every submit issued so far has completed (fence observed),
  // independent of how many draw_frame calls bail before submitting.
  tex.retiring = true;
  tex.retire_after_submit = impl_->submit_seq;
}

bool VulkanRenderer::draw_frame(const DrawBatch& batch, const ScreenBounds& screen, float clear_r,
                                float clear_g, float clear_b, const DrawBatch* additive,
                                const DrawBatch* post_overlay, const DrawBatch* post_overlay2,
                                const ScissorRect* additive_scissor, const DrawBatch* mid_overlay,
                                const DrawBatch* early_additive, const DrawBatch* scene_carve,
                                const DrawFrameExtras* extras, DrawFrameDump* dump) {
  if (impl_ == nullptr || renderer_health_unrecoverable(health_)) {
    if (impl_ != nullptr) {
      impl_->persistent_vb_locked = false;
      impl_->note_draw_stopped("unrecoverable");
    }
    return false;
  }
  struct PersistentVbUnlock {
    Impl* impl = nullptr;
    ~PersistentVbUnlock() {
      if (impl != nullptr) {
        impl->persistent_vb_locked = false;
      }
    }
  } vb_unlock{impl_.get()};
  auto reap_completed_keep_health = [&]() {
    if (!draw_frame_reaps_completed_uploads_before_zero_extent_return()) {
      return;
    }
    (void)impl_->reap_pending_uploads();
    if (!draw_frame_zero_extent_reap_applies_health()) {
      impl_->reap_retired_textures();
    }
  };
  if (health_ == RendererHealth::SurfaceLost) {
    if (!recover_surface_and_swapchain()) {
      impl_->note_draw_stopped("surface-recover-failed");
      return false;
    }
    if (health_ != RendererHealth::Ready) {
      reap_completed_keep_health();
      return health_ == RendererHealth::Occluded;
    }
  }
  const bool live_swapchain = impl_->swapchain != VK_NULL_HANDLE;
  if (draw_frame_blocks_before_recovery(health_, ready_, live_swapchain)) {
    impl_->note_draw_stopped("not-ready");
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
      impl_->note_draw_stopped("zero-extent");
      reap_completed_keep_health();
      return apply_zero_extent_now();
    }
    if (!resize(w, h) || impl_->swapchain_occluded || impl_->swapchain == VK_NULL_HANDLE) {
      impl_->note_draw_stopped("swapchain-recover-failed");
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
    impl_->reap_retired_textures();
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
  impl_->reap_gpu_timestamps(frame);
  // This slot's fence is signalled: every submit up to its last one is complete.
  impl_->note_slot_fence_signalled(frame);
  ++impl_->frame_seq;
  impl_->reap_retired_textures();

  uint32_t image_index = 0;
  const auto acquire_t0 = clock::now();
  VkResult acquire = vkAcquireNextImageKHR(impl_->device, impl_->swapchain, UINT64_MAX,
                                           impl_->image_available[frame], VK_NULL_HANDLE,
                                           &image_index);
  impl_->last_acquire_wait_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - acquire_t0).count();
  impl_->note_fullscreen_exclusive_lost("vkAcquireNextImageKHR", acquire);
  impl_->note_wsi_result("acquire", acquire, &impl_->have_logged_acquire, &impl_->logged_acquire);
  const WsiRecoverAction acquire_action = classify_wsi_result(acquire);
  bool recreate_swapchain_after_present = false;
  if (acquire_action == WsiRecoverAction::RecreateSwapchain && acquire == VK_SUBOPTIMAL_KHR) {
    recreate_swapchain_after_present = true;
  } else if (acquire_action != WsiRecoverAction::None) {
    impl_->note_draw_stopped("acquire");
    return apply_wsi_action(acquire_action);
  }

  // From here on an image is acquired and image_available[frame] is signalled.
  // Any failure return must go through this: it consumes the semaphore with a
  // command-buffer-less submit and, when vkResetFences already ran, passes the
  // fence so it is signalled again (the next vkWaitForFences is UINT64_MAX and
  // would hang the UI forever on an unsignalled fence). The acquired image can
  // never be presented without a render, so the swapchain is rebuilt to
  // release it; otherwise repeated failures exhaust the images and acquire
  // blocks.
  auto abort_after_acquire = [&](bool fence_was_reset) -> bool {
    VkPipelineStageFlags drain_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    drain.waitSemaphoreCount = 1;
    drain.pWaitSemaphores = &impl_->image_available[frame];
    drain.pWaitDstStageMask = &drain_stage;
    const VkResult drain_r = vkQueueSubmit(impl_->graphics_queue, 1, &drain,
                                           fence_was_reset ? impl_->in_flight[frame]
                                                           : VK_NULL_HANDLE);
    if (!check_device_result(drain_r)) {
      return false;
    }
    // recover_swapchain() idles the device, which also advances the completed
    // watermark. The drain submit never signals render_finished.
    vkQueueWaitIdle(impl_->graphics_queue);
    (void)recover_swapchain();
    return false;
  };

  // Do not reuse a swapchain image that is still referenced by an in-flight submit.
  if (image_index < impl_->images_in_flight.size() &&
      impl_->images_in_flight[image_index] != VK_NULL_HANDLE) {
    if (!check_device_result(vkWaitForFences(impl_->device, 1, &impl_->images_in_flight[image_index],
                                            VK_TRUE, UINT64_MAX))) {
      return abort_after_acquire(false);
    }
  }
  if (!render_finished_index_valid(image_index, impl_->render_finished.size())) {
    impl_->note_draw_stopped("render-finished-missing");
    return abort_after_acquire(false);
  }

  auto extra_vert_count = [](const DrawBatch* const* xs, size_t n) -> size_t {
    size_t v = 0;
    if (xs == nullptr) {
      return 0;
    }
    for (size_t i = 0; i < n; ++i) {
      if (xs[i] != nullptr) {
        v += xs[i]->vertex_count();
      }
    }
    return v;
  };
  auto extra_index_count = [](const DrawBatch* const* xs, size_t n) -> size_t {
    size_t v = 0;
    if (xs == nullptr) {
      return 0;
    }
    for (size_t i = 0; i < n; ++i) {
      if (xs[i] != nullptr) {
        v += xs[i]->index_count();
      }
    }
    return v;
  };
  const size_t early_extra_n = extras != nullptr ? extras->early_extra_n : 0;
  const size_t mid_prefix_n = extras != nullptr ? extras->mid_prefix_n : 0;
  const size_t carve_prefix_n = extras != nullptr ? extras->carve_prefix_n : 0;
  const DrawBatch* const* early_extra = extras != nullptr ? extras->early_extra : nullptr;
  const DrawBatch* const* mid_prefix = extras != nullptr ? extras->mid_prefix : nullptr;
  const DrawBatch* const* carve_prefix = extras != nullptr ? extras->carve_prefix : nullptr;
  const bool extras_in_place = extras != nullptr && extras->extras_in_mapped_vb;
  const uint32_t mapped_extra_low = extras != nullptr ? extras->mapped_extra_low : 0;
  const size_t early_verts = (early_additive ? early_additive->vertex_count() : 0) +
                             extra_vert_count(early_extra, early_extra_n);
  const size_t mid_verts = extra_vert_count(mid_prefix, mid_prefix_n) +
                           (mid_overlay ? mid_overlay->vertex_count() : 0);
  const size_t additive_verts = additive ? additive->vertex_count() : 0;
  const size_t carve_verts = extra_vert_count(carve_prefix, carve_prefix_n) +
                             (scene_carve ? scene_carve->vertex_count() : 0);
  const size_t post_verts = post_overlay ? post_overlay->vertex_count() : 0;
  const size_t post2_verts = post_overlay2 ? post_overlay2->vertex_count() : 0;
  const size_t total_verts = batch.vertex_count() + early_verts + mid_verts + additive_verts +
                            carve_verts + post_verts + post2_verts;
  const size_t ring_bytes = impl_->persistent_ring_bytes > 0 ? impl_->persistent_ring_bytes
                                                             : kPersistentVertexRingBytes;
  const size_t packed_bytes = total_verts * sizeof(DrawVertex);
  auto& grow_slot = impl_->frame_vertices[frame];
  if (extras_in_place) {
    // Tessellation already wrote extras into this slot. Growing the ring
    // number is fine; reallocating the slot would free ext_dst (SIGBUS).
    if (impl_->ring_grow_pending_bytes > 0 && packed_bytes * 4 <= ring_bytes) {
      impl_->commit_ring_grow(impl_->ring_grow_pending_bytes);
    }
  } else if (packed_bytes > ring_bytes) {
    impl_->commit_ring_grow(packed_bytes);
  } else if (impl_->ring_grow_pending_bytes > 0 && packed_bytes * 4 <= ring_bytes) {
    impl_->commit_ring_grow(impl_->ring_grow_pending_bytes);
  }
  const size_t grow_ring = impl_->persistent_ring_bytes > 0 ? impl_->persistent_ring_bytes
                                                           : kPersistentVertexRingBytes;
  const size_t bytes = extras_in_place
                           ? (grow_slot.capacity_bytes > 0 ? grow_slot.capacity_bytes : ring_bytes)
                           : std::max(packed_bytes, grow_ring);
  if (!impl_->remap_frame_vertices()) {
    impl_->note_draw_stopped("vertex-remap-failed");
    return abort_after_acquire(false);
  }
  if (!impl_->ensure_frame_vertex_capacity(frame, bytes)) {
    impl_->note_draw_stopped("vertex-capacity-failed");
    // Fence still signaled (not reset yet). Drain the acquire semaphore only.
    return abort_after_acquire(false);
  }
  const size_t packed_idx =
      batch.index_count() + (early_additive ? early_additive->index_count() : 0) +
      extra_index_count(early_extra, early_extra_n) +
      extra_index_count(mid_prefix, mid_prefix_n) +
      (mid_overlay ? mid_overlay->index_count() : 0) +
      (additive ? additive->index_count() : 0) + extra_index_count(carve_prefix, carve_prefix_n) +
      (scene_carve ? scene_carve->index_count() : 0) + (post_overlay ? post_overlay->index_count() : 0) +
      (post_overlay2 ? post_overlay2->index_count() : 0);
  const size_t packed_idx_bytes = packed_idx * sizeof(uint32_t);
  auto& grow_ib = impl_->frame_indices[frame];
  const size_t ib_ring = impl_->persistent_index_ring_bytes > 0
                             ? impl_->persistent_index_ring_bytes
                             : kPersistentIndexRingBytes;
  if (extras_in_place) {
    if (impl_->ib_grow_pending_bytes > 0 && packed_idx_bytes * 4 <= ib_ring) {
      impl_->commit_ib_ring_grow(impl_->ib_grow_pending_bytes);
    }
  } else if (packed_idx_bytes > ib_ring) {
    impl_->commit_ib_ring_grow(packed_idx_bytes);
  } else if (impl_->ib_grow_pending_bytes > 0 && packed_idx_bytes * 4 <= ib_ring) {
    impl_->commit_ib_ring_grow(impl_->ib_grow_pending_bytes);
  }
  const size_t grow_ib_ring = impl_->persistent_index_ring_bytes > 0
                                  ? impl_->persistent_index_ring_bytes
                                  : kPersistentIndexRingBytes;
  const size_t ib_bytes = extras_in_place
                              ? (grow_ib.capacity_bytes > 0 ? grow_ib.capacity_bytes : ib_ring)
                              : std::max(packed_idx_bytes, grow_ib_ring);
  if (ib_bytes > 0 && !impl_->ensure_frame_index_capacity(frame, ib_bytes)) {
    impl_->note_draw_stopped("index-capacity-failed");
    return abort_after_acquire(false);
  }

  auto& vb = impl_->frame_vertices[frame];
  auto& ib = impl_->frame_indices[frame];

  // Decide where the CPU heads go BEFORE vkResetFences and before any write.
  // In-place extras already occupy the mapped VB/IB (spaced by reserved
  // capacity), so the head pack must not overwrite them or run past the
  // mapping. Extras are NOT heads: only buckets the plan functions actually
  // memcpy count here.
  const uint64_t vb_cap_verts = vb.capacity_bytes / sizeof(DrawVertex);
  const uint64_t ib_cap_idx = ib.mapped != nullptr ? ib.capacity_bytes / sizeof(uint32_t) : 0;
  uint64_t ext_v_low = UINT64_MAX;  // lowest vertex any in-place extra occupies
  uint64_t ext_v_hi = 0;            // end of the extras' used vertices
  uint64_t ext_i_low = UINT64_MAX;
  uint64_t ext_i_hi = 0;
  bool extras_layout_ok = true;
  uint64_t head_verts = 0;
  uint64_t head_idx = 0;
  auto count_head = [&](const DrawBatch* b, bool is_extra) {
    if (b == nullptr) {
      return;
    }
    if (is_extra && extras_in_place && b->ext_dst != nullptr) {
      // Lives in the mapped ring already; fold its extent instead.
      if (vb.mapped == nullptr || b->ext_dst < static_cast<const DrawVertex*>(vb.mapped)) {
        extras_layout_ok = false;
        return;
      }
      const uint64_t off =
          static_cast<uint64_t>(b->ext_dst - static_cast<const DrawVertex*>(vb.mapped));
      ext_v_low = std::min(ext_v_low, off);
      ext_v_hi = extras_extent_merge(ext_v_hi, off, b->ext_used);
      if (b->ext_idx != nullptr) {
        if (ib.mapped == nullptr || b->ext_idx < static_cast<const uint32_t*>(ib.mapped)) {
          extras_layout_ok = false;
          return;
        }
        const uint64_t ioff =
            static_cast<uint64_t>(b->ext_idx - static_cast<const uint32_t*>(ib.mapped));
        ext_i_low = std::min(ext_i_low, ioff);
        ext_i_hi = extras_extent_merge(ext_i_hi, ioff, b->ext_idx_used);
      }
      return;
    }
    for (const auto& bucket : b->buckets) {
      head_verts += bucket.vertices.size();
      head_idx += bucket.indices.size();
    }
  };
  count_head(&batch, false);
  count_head(early_additive, false);
  count_head(mid_overlay, false);
  count_head(additive, false);
  count_head(scene_carve, false);
  count_head(post_overlay, false);
  count_head(post_overlay2, false);
  for (size_t i = 0; early_extra != nullptr && i < early_extra_n; ++i) {
    count_head(early_extra[i], true);
  }
  for (size_t i = 0; mid_prefix != nullptr && i < mid_prefix_n; ++i) {
    count_head(mid_prefix[i], true);
  }
  for (size_t i = 0; carve_prefix != nullptr && i < carve_prefix_n; ++i) {
    count_head(carve_prefix[i], true);
  }
  if (extras_in_place) {
    if (mapped_extra_low > 0) {
      ext_v_low = std::min<uint64_t>(ext_v_low, mapped_extra_low);
    }
    if (ext_v_hi > vb_cap_verts || ext_i_hi > ib_cap_idx) {
      extras_layout_ok = false;
    }
  }
  const HeadPlacement vert_place = place_cpu_heads(head_verts, ext_v_low, ext_v_hi, vb_cap_verts);
  // Without a mapped IB the plan functions skip index writes entirely.
  const HeadPlacement idx_place =
      ib.mapped != nullptr ? place_cpu_heads(head_idx, ext_i_low, ext_i_hi, ib_cap_idx)
                           : HeadPlacement{true, 0};
  if (!extras_layout_ok || !vert_place.fits || !idx_place.fits) {
    // Nothing written, fence not reset yet: still signalled.
    impl_->note_draw_stopped("vb-head-overflow");
    return abort_after_acquire(false);
  }

  // Reset only once capacity is ensured so a failed path cannot leave the fence unsignaled.
  if (!check_device_result(vkResetFences(impl_->device, 1, &impl_->in_flight[frame]))) {
    return abort_after_acquire(false);
  }

  // Reused across frames (capacity kept); cleared here, never referenced after draw_frame returns.
  DrawPlanScratch& scratch = impl_->plan_scratch;
  std::vector<GpuDrawBucket>& scene_gpu = scratch.scene;
  std::vector<GpuDrawBucket>& early_gpu = scratch.early;
  std::vector<GpuDrawBucket>& mid_gpu = scratch.mid;
  std::vector<GpuDrawBucket>& additive_gpu = scratch.additive;
  std::vector<GpuDrawBucket>& carve_gpu = scratch.carve;
  std::vector<GpuDrawBucket>& post_gpu = scratch.post;
  std::vector<GpuDrawBucket>& post2_gpu = scratch.post2;
  scene_gpu.clear();
  early_gpu.clear();
  mid_gpu.clear();
  additive_gpu.clear();
  carve_gpu.clear();
  post_gpu.clear();
  post2_gpu.clear();
  {
    WDS_PERF_SCOPE("vk.upload");
    auto* dst = static_cast<DrawVertex*>(vb.mapped);
    auto* idx_dst = static_cast<uint32_t*>(ib.mapped);
    // Normally 0; past the extras only when the heads would not fit below them.
    uint32_t cursor = vert_place.base;
    uint32_t idx_cursor = idx_place.base;
    cursor = vb_plan_layer_draws(scene_gpu, &batch, true, true, nullptr, 0, cursor, dst, false,
                                 dst, idx_dst, &idx_cursor, scratch);
    cursor = vb_plan_layer_draws(early_gpu, early_additive, true, true, early_extra, early_extra_n,
                                 cursor, dst, extras_in_place, dst, idx_dst, &idx_cursor, scratch);
    cursor = vb_plan_layer_draws(mid_gpu, mid_overlay, true, false, mid_prefix, mid_prefix_n,
                                 cursor, dst, extras_in_place, dst, idx_dst, &idx_cursor, scratch);
    cursor = vb_plan_layer_draws(additive_gpu, additive, true, true, nullptr, 0, cursor, dst,
                                 false, dst, idx_dst, &idx_cursor, scratch);
    cursor = vb_plan_ordered_draws(carve_gpu, scene_carve, carve_prefix, carve_prefix_n, cursor, dst,
                                   extras_in_place, dst, idx_dst, &idx_cursor);
    cursor = vb_plan_layer_draws(post_gpu, post_overlay, true, true, nullptr, 0, cursor, dst,
                                 false, dst, idx_dst, &idx_cursor, scratch);
    cursor = vb_plan_layer_draws(post2_gpu, post_overlay2, true, true, nullptr, 0, cursor, dst,
                                 false, dst, idx_dst, &idx_cursor, scratch);
    uint32_t used_verts = cursor;
    if (extras_in_place) {
      // Parts are spaced by reserved capacity, so the real end is the max of
      // (offset + used) (ext_v_hi), not mapped_extra_low + sum(used). The
      // staging copy and the non-coherent flush both size from this.
      used_verts = std::max(used_verts, static_cast<uint32_t>(ext_v_hi));
    }
    const VkDeviceSize used_bytes =
        static_cast<VkDeviceSize>(used_verts) * static_cast<VkDeviceSize>(sizeof(DrawVertex));
    impl_->last_vb_used_bytes = static_cast<size_t>(used_bytes);
    impl_->last_vb_cap_bytes = vb.capacity_bytes;
    impl_->note_vb_high_water(impl_->last_vb_used_bytes);
    uint32_t used_idx = idx_cursor;
    if (extras_in_place) {
      // Same max(offset + used) extent, computed before the fence reset.
      used_idx = std::max(used_idx, static_cast<uint32_t>(ext_i_hi));
    }
    const VkDeviceSize used_idx_bytes =
        static_cast<VkDeviceSize>(used_idx) * static_cast<VkDeviceSize>(sizeof(uint32_t));
    impl_->last_ib_used_bytes = static_cast<size_t>(used_idx_bytes);
    impl_->last_ib_cap_bytes = ib.capacity_bytes;
    impl_->note_ib_high_water(impl_->last_ib_used_bytes);
    impl_->last_total_verts = total_verts;
    impl_->last_early_verts = early_verts;
    impl_->last_carve_verts = carve_verts;
    impl_->last_vb_staging = vb.uses_staging ? 1 : 0;
    if (dump != nullptr && dst != nullptr) {
      if (dump->scene != nullptr) {
        vb_fill_dump_keep_empty(*dump->scene, scene_gpu, dst, idx_dst);
      }
      if (dump->glow != nullptr) {
        vb_fill_dump_keep_empty(*dump->glow, early_gpu, dst, idx_dst);
      }
      if (dump->notes != nullptr) {
        vb_fill_dump_keep_empty(*dump->notes, mid_gpu, dst, idx_dst);
      }
      if (dump->late != nullptr) {
        vb_fill_dump_keep_empty(*dump->late, carve_gpu, dst, idx_dst);
      }
      if (dump->additive != nullptr) {
        vb_fill_dump_keep_empty(*dump->additive, additive_gpu, dst, idx_dst);
      }
    }
    // Dump first (WDSV identity = unmerged buckets). Merge only consecutive
    // same-tex/clip draws whose firstVertex ranges already abut. Reserved-cap
    // gaps stay (packing would memcpy ~1M verts and is not bit-cheaper).
    coalesce_contiguous_gpu_buckets(scene_gpu);
    coalesce_contiguous_gpu_buckets(early_gpu);
    coalesce_contiguous_gpu_buckets(mid_gpu);
    coalesce_contiguous_gpu_buckets(additive_gpu);
    coalesce_contiguous_gpu_buckets(carve_gpu);
    coalesce_contiguous_gpu_buckets(post_gpu);
    coalesce_contiguous_gpu_buckets(post2_gpu);
    const VkDeviceMemory flush_mem = vb.uses_staging ? vb.staging_memory : vb.memory;
    // Unused tail of the 64MiB ring is stale; flushing/copying it is not
    // bit-exact work and costs ~8ms/frame on staging after a peak grow.
    flush_mapped_vertex_range(impl_->device, flush_mem, vb.coherent, impl_->non_coherent_atom, 0,
                              used_bytes, vb.mapped_alloc_bytes);
    if (ib.mapped != nullptr && used_idx_bytes > 0) {
      const VkDeviceMemory ib_flush = ib.uses_staging ? ib.staging_memory : ib.memory;
      flush_mapped_vertex_range(impl_->device, ib_flush, ib.coherent, impl_->non_coherent_atom, 0,
                                used_idx_bytes, ib.mapped_alloc_bytes);
    }
  }

  const auto record_t0 = clock::now();
  impl_->last_vk_draws = 0;
  impl_->last_pipe_binds = 0;
  impl_->last_desc_binds = 0;
  impl_->last_push_consts = 0;
  impl_->last_render_passes = 0;
  impl_->last_vb_binds = 0;
  VkCommandBuffer cmd = impl_->command_buffers[frame];
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cmd, &begin);
  const VkDeviceSize copy_bytes = static_cast<VkDeviceSize>(impl_->last_vb_used_bytes);
  if (vb.uses_staging && vb.staging_buffer != VK_NULL_HANDLE && vb.buffer != VK_NULL_HANDLE &&
      copy_bytes > 0) {
    VkBufferCopy copy{};
    copy.size = copy_bytes;
    vkCmdCopyBuffer(cmd, vb.staging_buffer, vb.buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vb.buffer;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
  }
  const VkDeviceSize idx_copy_bytes = static_cast<VkDeviceSize>(impl_->last_ib_used_bytes);
  if (ib.uses_staging && ib.staging_buffer != VK_NULL_HANDLE && ib.buffer != VK_NULL_HANDLE &&
      idx_copy_bytes > 0) {
    VkBufferCopy copy{};
    copy.size = idx_copy_bytes;
    vkCmdCopyBuffer(cmd, ib.staging_buffer, ib.buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDEX_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = ib.buffer;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
  }
  const bool gpu_time = impl_->begin_gpu_timestamps(frame, cmd);
  auto gpu_stamp = [&](VkPipelineStageFlagBits stage) {
    if (gpu_time) {
      impl_->write_gpu_timestamp(cmd, stage);
    }
  };
  // Timestamp queries stay outside render passes (MoltenVK/Apple may only
  // resolve them at encoder boundaries). Shared-pass draws are timed as one
  // segment: glow extract, late extract, main (opaque+mid+HDR+inline post),
  // carve blit+extract, carve tail composite/post.
  gpu_stamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

  float mvp[16];
  ortho_rh(screen.l, screen.r, screen.b, screen.t, -1.0f, 1.0f, mvp);
  VkRect2D glow_scissor{};
  glow_scissor.extent = impl_->swapchain_extent;
  if (additive_scissor != nullptr && additive_scissor->valid()) {
    const int fb_w = static_cast<int>(impl_->swapchain_extent.width);
    const int fb_h = static_cast<int>(impl_->swapchain_extent.height);
    const int x0 = std::clamp(additive_scissor->x, 0, fb_w);
    const int y0 = std::clamp(additive_scissor->y, 0, fb_h);
    const int x1 = std::clamp(additive_scissor->x + additive_scissor->w, 0, fb_w);
    const int y1 = std::clamp(additive_scissor->y + additive_scissor->h, 0, fb_h);
    glow_scissor.offset.x = static_cast<int32_t>(x0);
    glow_scissor.offset.y = static_cast<int32_t>(y0);
    glow_scissor.extent.width = static_cast<uint32_t>(std::max(0, x1 - x0));
    glow_scissor.extent.height = static_cast<uint32_t>(std::max(0, y1 - y0));
  }
  if (impl_->bloom_ready && early_verts > 0) {
    impl_->record_glow_extract_gpu(cmd, early_gpu.data(), early_gpu.size(),
                                  static_cast<uint32_t>(early_verts), vb.buffer, mvp, glow_scissor,
                                  impl_->bloom_extract_fb, false, ib.buffer);
  }
  gpu_stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  if (impl_->bloom_ready && impl_->hdr_late_ready && additive_verts > 0) {
    impl_->record_glow_extract_gpu(cmd, additive_gpu.data(), additive_gpu.size(),
                                  static_cast<uint32_t>(additive_verts), vb.buffer, mvp,
                                  glow_scissor, impl_->hdr_late_fb, false, ib.buffer);
  }
  gpu_stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

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
  ++impl_->last_render_passes;

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

  vkCmdPushConstants(cmd, impl_->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), mvp);
  ++impl_->last_push_consts;
  {
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buffer, &offset);
    ++impl_->last_vb_binds;
    if (ib.buffer != VK_NULL_HANDLE) {
      vkCmdBindIndexBuffer(cmd, ib.buffer, 0, VK_INDEX_TYPE_UINT32);
    }
  }

  uint32_t drawn_verts = 0;
  uint32_t skipped_buckets = 0;
  uint32_t skipped_verts = 0;
  int64_t vk_draws = 0;
  auto draw_gpu_buckets = [&](VkPipeline pipeline, const std::vector<GpuDrawBucket>& src,
                             VkPipeline syuriken_pipe = VK_NULL_HANDLE,
                             VkPipeline no_clip_pipe = VK_NULL_HANDLE) {
    if (gpu_buckets_vert_count(src) == 0) {
      return;
    }
    VkPipeline bound = VK_NULL_HANDLE;
    TextureId bound_tex = kInvalidTextureId;
    VkPipelineLayout bound_layout = VK_NULL_HANDLE;
    for (const auto& bucket : src) {
      if ((bucket.vert_count == 0 && bucket.index_count == 0) ||
          bucket.texture == kInvalidTextureId || bucket.texture >= impl_->textures.size() ||
          !impl_->textures[bucket.texture].alive) {
        if (bucket.vert_count > 0 || bucket.index_count > 0) {
          ++skipped_buckets;
          skipped_verts += bucket.vert_count;
        }
        continue;
      }
      DrawBucket probe;
      probe.texture = bucket.texture;
      probe.disable_alpha_clip = bucket.disable_alpha_clip;
      const bool syuriken = syuriken_pipe != VK_NULL_HANDLE && impl_->bucket_is_syuriken(probe);
      VkPipeline want = pipeline;
      if (bucket.overlay_pipe == static_cast<uint8_t>(DrawOverlayPipe::Alpha)) {
        want = impl_->pipeline_alpha_soft != VK_NULL_HANDLE ? impl_->pipeline_alpha_soft
                                                           : impl_->pipeline;
      } else if (bucket.overlay_pipe == static_cast<uint8_t>(DrawOverlayPipe::AdditiveClip01)) {
        want = impl_->pipeline_additive_clip01 != VK_NULL_HANDLE ? impl_->pipeline_additive_clip01
                                                                : impl_->pipeline_additive;
      } else if (syuriken) {
        want = syuriken_pipe;
      } else if (bucket.disable_alpha_clip && no_clip_pipe != VK_NULL_HANDLE) {
        want = no_clip_pipe;
      } else if (bucket.overlay_pipe == static_cast<uint8_t>(DrawOverlayPipe::Additive) &&
                 impl_->pipeline_additive != VK_NULL_HANDLE) {
        want = impl_->pipeline_additive;
      }
      const VkPipelineLayout layout =
          syuriken ? impl_->pipeline_layout_syuriken : impl_->pipeline_layout;
      if (want != bound) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
        ++impl_->last_pipe_binds;
        if (syuriken) {
          impl_->push_syuriken_pc(cmd, mvp);
        } else {
          vkCmdPushConstants(cmd, impl_->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                             sizeof(mvp), mvp);
        }
        ++impl_->last_push_consts;
      }
      drawn_verts += bucket.vert_count;
      impl_->textures[bucket.texture].sampled_seq = impl_->frame_seq;
      if (want != bound || bound_tex != bucket.texture || bound_layout != layout) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                &impl_->textures[bucket.texture].descriptor, 0, nullptr);
        ++impl_->last_desc_binds;
        bound_tex = bucket.texture;
        bound_layout = layout;
      }
      bound = want;
      if (bucket.index_count > 0) {
        vkCmdDrawIndexed(cmd, bucket.index_count, 1, bucket.first_index, 0, 0);
      } else {
        vkCmdDraw(cmd, bucket.vert_count, 1, bucket.first_vertex, 0);
      }
      ++vk_draws;
    }
  };

  auto apply_additive_scissor = [&]() {
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
  };

  draw_gpu_buckets(impl_->pipeline, scene_gpu);
  auto composite_hdr = [&](VkDescriptorSet desc, VkPipeline pipe) {
    if (pipe == VK_NULL_HANDLE) {
      return;
    }
    VkViewport hdr_vp{};
    hdr_vp.width = static_cast<float>(impl_->swapchain_extent.width);
    hdr_vp.height = static_cast<float>(impl_->swapchain_extent.height);
    hdr_vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &hdr_vp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    ++impl_->last_pipe_binds;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline_layout, 0, 1,
                            &desc, 0, nullptr);
    ++impl_->last_desc_binds;
    vkCmdDraw(cmd, 3, 1, 0, 0);
    ++vk_draws;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
  };
  // Official SortingGroup 20 > Note sortingOrder 10: notes first, then Add.
  if (mid_verts > 0) {
    draw_gpu_buckets(impl_->pipeline_alpha_soft != VK_NULL_HANDLE ? impl_->pipeline_alpha_soft
                                                                : impl_->pipeline,
                    mid_gpu);
  }
  const bool hdr_early = impl_->bloom_ready && impl_->pipeline_hdr_composite != VK_NULL_HANDLE &&
                         impl_->bloom_src_descriptor != VK_NULL_HANDLE && early_verts > 0;
  if (hdr_early) {
    composite_hdr(impl_->bloom_src_descriptor, impl_->pipeline_hdr_composite);
  } else if (early_verts > 0 && impl_->pipeline_additive) {
    apply_additive_scissor();
    draw_gpu_buckets(impl_->pipeline_additive, early_gpu, impl_->pipeline_syuriken_add,
                     impl_->pipeline_additive_no_clip);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
  }
  const bool hdr_late = impl_->bloom_ready && impl_->hdr_late_ready &&
                        impl_->pipeline_hdr_composite != VK_NULL_HANDLE &&
                        impl_->hdr_late_descriptor != VK_NULL_HANDLE && additive_verts > 0;
  if (hdr_late) {
    composite_hdr(impl_->hdr_late_descriptor, impl_->pipeline_hdr_composite);
  } else if (additive_verts > 0 && impl_->pipeline_additive) {
    apply_additive_scissor();
    draw_gpu_buckets(impl_->pipeline_additive, additive_gpu, VK_NULL_HANDLE,
                     impl_->pipeline_additive_no_clip);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
  }
  const bool scene_carve_ok = carve_verts > 0 && impl_->hdr_can_scene_carve &&
                              impl_->hdr_late_image != VK_NULL_HANDLE &&
                              image_index < impl_->swapchain_images.size() &&
                              image_index < impl_->bloom_framebuffers.size() &&
                              impl_->bloom_framebuffers[image_index] != VK_NULL_HANDLE;
  const bool posts_after_carve = scene_carve_ok && impl_->pipeline_alpha_1x != VK_NULL_HANDLE;
  // Modal / top overlays stay after the carved HDR replace so bats do not
  // dest-blend editor chrome. Without carve they stay in this pass.
  if (!posts_after_carve) {
    if (post_verts > 0) {
      draw_gpu_buckets(impl_->pipeline, post_gpu);
    }
    if (post2_verts > 0) {
      draw_gpu_buckets(impl_->pipeline, post2_gpu);
    }
  }
  // ParticleAdd sort≥1 lives in scene_carve. If HDR blit/carve cannot run,
  // those verts used to vanish (no else). Draw them SrcAlpha+One here so
  // Additive Stretch/Billboard still appear on UNORM.
  if (!scene_carve_ok && carve_verts > 0 && impl_->pipeline_additive) {
    apply_additive_scissor();
    draw_gpu_buckets(impl_->pipeline_additive, carve_gpu, impl_->pipeline_syuriken_add,
                     impl_->pipeline_additive_no_clip);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
  }

  vkCmdEndRenderPass(cmd);
  gpu_stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

  if (scene_carve_ok) {
    image_barrier(cmd, impl_->swapchain_images[image_index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(cmd, impl_->hdr_late_image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[1].x = static_cast<int32_t>(impl_->swapchain_extent.width);
    blit.srcOffsets[1].y = static_cast<int32_t>(impl_->swapchain_extent.height);
    blit.srcOffsets[1].z = 1;
    blit.dstOffsets[1].x = static_cast<int32_t>(impl_->bloom_extent.width);
    blit.dstOffsets[1].y = static_cast<int32_t>(impl_->bloom_extent.height);
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(cmd, impl_->swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   impl_->hdr_late_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);
    image_barrier(cmd, impl_->hdr_late_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    impl_->record_glow_extract_gpu(cmd, carve_gpu.data(), carve_gpu.size(),
                                  static_cast<uint32_t>(carve_verts), vb.buffer, mvp, glow_scissor,
                                  impl_->hdr_late_fb, true, ib.buffer);
    image_barrier(cmd, impl_->swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    gpu_stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VkRenderPassBeginInfo tail{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    tail.renderPass = impl_->bloom_pass;
    tail.framebuffer = impl_->bloom_framebuffers[image_index];
    tail.renderArea.extent = impl_->swapchain_extent;
    vkCmdBeginRenderPass(cmd, &tail, VK_SUBPASS_CONTENTS_INLINE);
    ++impl_->last_render_passes;
    VkViewport tail_vp{};
    tail_vp.width = static_cast<float>(impl_->swapchain_extent.width);
    tail_vp.height = static_cast<float>(impl_->swapchain_extent.height);
    tail_vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &tail_vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    composite_hdr(impl_->hdr_late_descriptor, impl_->pipeline_hdr_replace);
    if (posts_after_carve) {
      {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buffer, &offset);
        ++impl_->last_vb_binds;
      }
      if (post_verts > 0) {
        draw_gpu_buckets(impl_->pipeline_alpha_1x, post_gpu);
      }
      if (post2_verts > 0) {
        draw_gpu_buckets(impl_->pipeline_alpha_1x, post2_gpu);
      }
    }
    vkCmdEndRenderPass(cmd);
    gpu_stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  } else {
    gpu_stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    gpu_stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  }

  impl_->last_vk_draws += vk_draws;
  WDS_PERF_COUNT("vk.draws", vk_draws);

  vkEndCommandBuffer(cmd);
  impl_->last_bloom_pass = (hdr_early || hdr_late) ? 1 : 0;
  impl_->last_carve_pass = scene_carve_ok ? 1 : 0;
  impl_->last_draw_record_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - record_t0).count();

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &impl_->image_available[frame];
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &impl_->render_finished[image_index];
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
    return abort_after_acquire(true);
  }
  impl_->images_in_flight[image_index] = impl_->in_flight[frame];
  // Real frame submitted: remember which submit this slot's fence now guards.
  ++impl_->submit_seq;
  impl_->slot_submit_seq[frame] = impl_->submit_seq;
  if (gpu_time) {
    impl_->commit_gpu_timestamps(frame);
  }
  impl_->last_gpu_submit_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - submit_t0).count();

  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &impl_->render_finished[image_index];
  present.swapchainCount = 1;
  present.pSwapchains = &impl_->swapchain;
  present.pImageIndices = &image_index;
  const auto present_t0 = clock::now();
  VkResult present_result = vkQueuePresentKHR(impl_->graphics_queue, &present);
  impl_->last_present_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - present_t0).count();
  impl_->note_fullscreen_exclusive_lost("vkQueuePresentKHR", present_result);
  impl_->note_wsi_result("present", present_result, &impl_->have_logged_present,
                         &impl_->logged_present);
#if WDS_ENABLE_LOGGING
  // One line that separates "nothing was submitted", "submitted but the driver
  // showed only the clear", and "present itself failed".
  if (!impl_->logged_first_present) {
    impl_->logged_first_present = true;
    WDS_LOG("first present drawn_verts=%u skipped_buckets=%u skipped_verts=%u queued_verts=%zu "
            "clear=%.3f,%.3f,%.3f viewport_h=%.0f extent=%ux%u screen=%.3f,%.3f,%.3f,%.3f "
            "acquire=%s present=%s\n",
            drawn_verts, skipped_buckets, skipped_verts, total_verts, clear_r, clear_g, clear_b,
            viewport.height, impl_->swapchain_extent.width, impl_->swapchain_extent.height,
            screen.l, screen.r, screen.b, screen.t, vk_result_name(acquire),
            vk_result_name(present_result));
  }
#endif
  if (present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR) {
    impl_->draw_stop_reason = nullptr;
    impl_->last_presented_image = image_index;
    impl_->last_present_ok = true;
  } else {
    impl_->note_draw_stopped("present");
    impl_->last_present_ok = false;
  }
  (void)drawn_verts;
  (void)skipped_buckets;
  (void)skipped_verts;
  const uint32_t fif = static_cast<uint32_t>(std::max(1, impl_->frames_in_flight));
  impl_->frame_index = (frame + 1) % fif;
  const WsiRecoverAction present_action = classify_wsi_result(present_result);
  if (present_action == WsiRecoverAction::None) {
    if (recreate_swapchain_after_present) {
      impl_->last_present_ok = false;
      return recover_swapchain();
    }
    return true;
  }
  impl_->last_present_ok = false;
  return apply_wsi_action(present_action);
}

bool VulkanRenderer::readback_rgba8(std::vector<std::uint8_t>& out_rgba, int& out_w, int& out_h) {
  out_rgba.clear();
  out_w = 0;
  out_h = 0;
  if (impl_ == nullptr || !ready_ || impl_->device == VK_NULL_HANDLE ||
      impl_->swapchain == VK_NULL_HANDLE || !impl_->last_present_ok ||
      !impl_->swapchain_can_blit) {
    return false;
  }
  const uint32_t image_index = impl_->last_presented_image;
  if (image_index >= impl_->swapchain_images.size() ||
      impl_->swapchain_images[image_index] == VK_NULL_HANDLE) {
    return false;
  }
  const uint32_t width = impl_->swapchain_extent.width;
  const uint32_t height = impl_->swapchain_extent.height;
  if (width == 0 || height == 0) {
    return false;
  }

  device_wait_idle();

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(impl_->physical, &props);
  VkDeviceSize row_align = props.limits.optimalBufferCopyRowPitchAlignment;
  if (row_align < 4) {
    row_align = 4;
  }
  const VkDeviceSize pixel_row = static_cast<VkDeviceSize>(width) * 4u;
  const VkDeviceSize row_pitch = (pixel_row + row_align - 1u) / row_align * row_align;
  const VkDeviceSize buf_bytes = row_pitch * static_cast<VkDeviceSize>(height);

  VkBufferCreateInfo buf_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buf_info.size = buf_bytes;
  buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  if (vkCreateBuffer(impl_->device, &buf_info, nullptr, &buffer) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(impl_->device, buffer, &req);
  uint32_t mem_type = 0;
  try {
    mem_type = find_memory_type(impl_->physical, req.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  } catch (...) {
    vkDestroyBuffer(impl_->device, buffer, nullptr);
    return false;
  }
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = mem_type;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (vkAllocateMemory(impl_->device, &alloc, nullptr, &memory) != VK_SUCCESS) {
    vkDestroyBuffer(impl_->device, buffer, nullptr);
    return false;
  }
  if (vkBindBufferMemory(impl_->device, buffer, memory, 0) != VK_SUCCESS) {
    vkFreeMemory(impl_->device, memory, nullptr);
    vkDestroyBuffer(impl_->device, buffer, nullptr);
    return false;
  }

  VkCommandBufferAllocateInfo cmd_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmd_alloc.commandPool = impl_->command_pool;
  cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(impl_->device, &cmd_alloc, &cmd) != VK_SUCCESS) {
    vkFreeMemory(impl_->device, memory, nullptr);
    vkDestroyBuffer(impl_->device, buffer, nullptr);
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  image_barrier(cmd, impl_->swapchain_images[image_index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy region{};
  region.bufferRowLength = static_cast<uint32_t>(row_pitch / 4u);
  region.bufferImageHeight = height;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent.width = width;
  region.imageExtent.height = height;
  region.imageExtent.depth = 1;
  vkCmdCopyImageToBuffer(cmd, impl_->swapchain_images[image_index],
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
  image_barrier(cmd, impl_->swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  const VkResult submit_r = vkQueueSubmit(impl_->graphics_queue, 1, &submit, VK_NULL_HANDLE);
  if (submit_r != VK_SUCCESS) {
    vkFreeCommandBuffers(impl_->device, impl_->command_pool, 1, &cmd);
    vkFreeMemory(impl_->device, memory, nullptr);
    vkDestroyBuffer(impl_->device, buffer, nullptr);
    return false;
  }
  vkQueueWaitIdle(impl_->graphics_queue);
  vkFreeCommandBuffers(impl_->device, impl_->command_pool, 1, &cmd);

  void* mapped = nullptr;
  if (vkMapMemory(impl_->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
      mapped == nullptr) {
    vkFreeMemory(impl_->device, memory, nullptr);
    vkDestroyBuffer(impl_->device, buffer, nullptr);
    return false;
  }

  const bool bgra = impl_->swapchain_format == VK_FORMAT_B8G8R8A8_UNORM ||
                    impl_->swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ||
                    impl_->swapchain_format == VK_FORMAT_B8G8R8A8_UINT;
  const bool rgba = impl_->swapchain_format == VK_FORMAT_R8G8B8A8_UNORM ||
                    impl_->swapchain_format == VK_FORMAT_R8G8B8A8_SRGB;
  out_rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
  const auto* src = static_cast<const std::uint8_t*>(mapped);
  for (uint32_t y = 0; y < height; ++y) {
    const std::uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(row_pitch);
    std::uint8_t* dst = out_rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
    if (rgba) {
      std::memcpy(dst, row, static_cast<size_t>(width) * 4u);
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        const std::uint8_t* p = row + static_cast<size_t>(x) * 4u;
        if (bgra) {
          dst[0] = p[2];
          dst[1] = p[1];
          dst[2] = p[0];
          dst[3] = p[3];
        } else {
          dst[0] = p[0];
          dst[1] = p[1];
          dst[2] = p[2];
          dst[3] = p[3];
        }
        dst += 4;
      }
    }
  }
  vkUnmapMemory(impl_->device, memory);
  vkFreeMemory(impl_->device, memory, nullptr);
  vkDestroyBuffer(impl_->device, buffer, nullptr);

  out_w = static_cast<int>(width);
  out_h = static_cast<int>(height);
  return true;
}

}  // namespace wds::renderer
