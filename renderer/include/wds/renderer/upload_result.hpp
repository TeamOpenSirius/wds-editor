#pragma once

#include "vulkan_renderer.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace wds::renderer {

enum class UploadStage : uint8_t {
  None = 0,
  Buffer,
  Image,
  Map,
  Bind,
  View,
  Descriptor,
  CommandAllocate,
  CommandBegin,
  CommandEnd,
  FenceCreate,
  FenceReset,
  QueueSubmit,
  Wait,
};

struct UploadResult {
  VkResult result = VK_SUCCESS;
  UploadStage stage = UploadStage::None;

  constexpr bool ok() const noexcept { return result == VK_SUCCESS; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// Never rewrite a failing VkResult to VK_SUCCESS. Success clears the stage so
// callers only inspect `stage` when `ok()` is false.
inline constexpr UploadResult make_upload_result(VkResult result, UploadStage stage) noexcept {
  if (result == VK_SUCCESS) {
    return UploadResult{VK_SUCCESS, UploadStage::None};
  }
  return UploadResult{result, stage};
}

// vkMapMemory may return VK_SUCCESS with a null host pointer.
inline constexpr UploadResult make_upload_map_result(VkResult result, const void* mapped) noexcept {
  if (result != VK_SUCCESS) {
    return UploadResult{result, UploadStage::Map};
  }
  if (mapped == nullptr) {
    return UploadResult{VK_ERROR_MEMORY_MAP_FAILED, UploadStage::Map};
  }
  return UploadResult{VK_SUCCESS, UploadStage::None};
}

inline constexpr UploadResult upload_no_memory_type(UploadStage stage) noexcept {
  return UploadResult{VK_ERROR_OUT_OF_DEVICE_MEMORY, stage};
}

struct UploadHealthDelta {
  bool apply = false;
  // Valid only when apply is true. DEVICE_LOST always DeviceLost; wait
  // failures that are not device-lost are Fatal. Other upload errors do not
  // change renderer health.
  RendererHealthEvent event = RendererHealthEvent::Fatal;
};

// Allocate failure never obtained a command buffer. Begin failure must free it.
inline constexpr bool upload_begin_should_release_command(const UploadResult& begin) noexcept {
  return !begin.ok() && begin.stage == UploadStage::CommandBegin;
}

// After submit succeeds the CB stays queued until the per-upload fence is
// reaped (or the queue is proven idle / device-lost teardown). Pre-submit
// end-chain failures never queued the CB.
inline constexpr bool upload_may_release_command(const UploadResult& result, bool submitted,
                                                 bool queue_idle_proven) noexcept {
  if (result.ok()) {
    return queue_idle_proven;
  }
  if (!submitted) {
    switch (result.stage) {
      case UploadStage::CommandEnd:
      case UploadStage::FenceCreate:
      case UploadStage::FenceReset:
      case UploadStage::QueueSubmit:
        return true;
      default:
        return false;
    }
  }
  return result.stage == UploadStage::Wait && queue_idle_proven;
}

inline constexpr bool upload_may_destroy_resources(const UploadResult& result, bool submitted,
                                                   bool queue_idle_proven) noexcept {
  return upload_may_release_command(result, submitted, queue_idle_proven);
}

inline constexpr bool upload_may_reset_fence(bool submit_succeeded, bool wait_completed) noexcept {
  return !submit_succeeded || wait_completed;
}

inline constexpr bool upload_slot_reusable_after_failure(bool resources_held) noexcept {
  return !resources_held;
}

inline constexpr UploadHealthDelta upload_health_delta(VkResult result, UploadStage stage) noexcept {
  if (result == VK_SUCCESS) {
    return {};
  }
  if (result == VK_ERROR_DEVICE_LOST) {
    return UploadHealthDelta{true, RendererHealthEvent::DeviceLost};
  }
  if (stage == UploadStage::Wait) {
    return UploadHealthDelta{true, RendererHealthEvent::Fatal};
  }
  return {};
}

// Convenience for pre-submit end-chain. Success and wait/reap failures must use
// upload_may_release_command(..., submitted, idle/reaped).
inline constexpr bool upload_end_should_release_command(const UploadResult& end) noexcept {
  const bool submitted = !end.ok() && end.stage == UploadStage::Wait;
  return upload_may_release_command(end, submitted, false);
}

enum class UploadFencePoll : uint8_t {
  Pending = 0,
  Ready,
  DeviceLost,
  Fatal,
};

inline constexpr UploadFencePoll classify_upload_fence_status(VkResult status) noexcept {
  if (status == VK_SUCCESS) {
    return UploadFencePoll::Ready;
  }
  if (status == VK_NOT_READY) {
    return UploadFencePoll::Pending;
  }
  if (status == VK_ERROR_DEVICE_LOST) {
    return UploadFencePoll::DeviceLost;
  }
  return UploadFencePoll::Fatal;
}

inline constexpr UploadHealthDelta upload_fence_poll_health(UploadFencePoll poll) noexcept {
  switch (poll) {
    case UploadFencePoll::DeviceLost:
      return UploadHealthDelta{true, RendererHealthEvent::DeviceLost};
    case UploadFencePoll::Fatal:
      return UploadHealthDelta{true, RendererHealthEvent::Fatal};
    case UploadFencePoll::Pending:
    case UploadFencePoll::Ready:
      break;
  }
  return {};
}

inline constexpr bool upload_publish_texture_after_submit(bool submit_ok) noexcept {
  return submit_ok;
}

inline constexpr bool upload_submit_failure_recycles_slot(bool submitted) noexcept {
  return !submitted;
}

inline constexpr bool upload_reap_may_release(UploadFencePoll poll, bool queue_idle_proven) noexcept {
  if (poll == UploadFencePoll::Ready) {
    return true;
  }
  return queue_idle_proven &&
         (poll == UploadFencePoll::DeviceLost || poll == UploadFencePoll::Fatal ||
          poll == UploadFencePoll::Pending);
}

inline constexpr bool upload_destroy_may_release_pending(bool wait_completed,
                                                        bool device_lost) noexcept {
  return wait_completed || device_lost;
}

// Blocking drain of pending upload fences: reap staging only after wait succeeds.
inline constexpr bool wait_pending_uploads_reaps_after_ok_wait(VkResult wait) noexcept {
  return wait == VK_SUCCESS;
}

inline constexpr bool wait_pending_uploads_holds_staging_on_failure(VkResult wait) noexcept {
  return wait != VK_SUCCESS;
}

// Windows ICDs (and some mapped-memory budgets) fail vkMapMemory on a 32MiB
// HOST_VISIBLE allocation. Cap each staging map/alloc; large images use several.
inline constexpr VkDeviceSize kHostVisibleMapChunkBytes = 4ull * 1024ull * 1024ull;

inline constexpr VkDeviceSize staging_copy_chunk_bytes(VkDeviceSize row_bytes) noexcept {
  if (row_bytes == 0) {
    return kHostVisibleMapChunkBytes;
  }
  if (row_bytes >= kHostVisibleMapChunkBytes) {
    return row_bytes;
  }
  return (kHostVisibleMapChunkBytes / row_bytes) * row_bytes;
}

inline constexpr uint32_t staging_copy_chunk_count(VkDeviceSize image_bytes,
                                                   VkDeviceSize chunk_bytes) noexcept {
  if (chunk_bytes == 0) {
    return 0;
  }
  return static_cast<uint32_t>((image_bytes + chunk_bytes - 1) / chunk_bytes);
}

// Device create persistently maps per-frame vertex rings. Some Windows ICDs refuse
// a further vkMapMemory (MEMORY_MAP_FAILED) until those maps are released.
inline constexpr bool texture_upload_unmaps_persistent_vertex_maps() noexcept {
  return true;
}

inline constexpr bool upload_may_destroy_texture_image(bool upload_in_flight, bool retire_due,
                                                       bool queue_idle_proven) noexcept {
  if (queue_idle_proven) {
    return true;
  }
  return !upload_in_flight && retire_due;
}

struct PendingUploadHold {
  bool has_cmd = false;
  bool has_staging = false;
  bool has_fence = false;
  bool submitted = false;
  uint32_t texture_id = 0;
};

inline constexpr bool pending_upload_occupied(const PendingUploadHold& hold) noexcept {
  return hold.has_cmd || hold.has_staging || hold.has_fence || hold.submitted;
}

// Submit must not be the first allocation: reserve a rollback-able record first.
// A later push_back after vkQueueSubmit can throw and leak GPU-owned handles.
inline constexpr bool pending_submit_requires_reserved_record() noexcept {
  return true;
}

inline constexpr bool pending_record_rollback_if_submit_fails() noexcept {
  return true;
}

inline constexpr bool pending_push_after_submit_allowed() noexcept {
  return false;
}

struct PendingUploadCommitTrace {
  bool record_reserved = false;
  bool submitted = false;
  bool committed = false;
  bool leaked_after_submit = false;
};

inline constexpr bool pending_commit_reserve(PendingUploadCommitTrace& trace) noexcept {
  trace.record_reserved = true;
  trace.leaked_after_submit = false;
  return true;
}

inline constexpr bool pending_commit_submit(PendingUploadCommitTrace& trace,
                                            bool submit_ok) noexcept {
  if (!trace.record_reserved) {
    trace.leaked_after_submit = submit_ok;
    return false;
  }
  trace.submitted = submit_ok;
  return submit_ok;
}

inline constexpr void pending_commit_rollback(PendingUploadCommitTrace& trace) noexcept {
  if (!trace.submitted) {
    trace.record_reserved = false;
  }
}

inline constexpr void pending_commit_finish(PendingUploadCommitTrace& trace) noexcept {
  if (trace.record_reserved && trace.submitted) {
    trace.committed = true;
  }
}

inline constexpr bool upload_pool_create_failure_tears_down_renderer() noexcept {
  return true;
}

inline constexpr VkCommandPoolCreateFlags upload_command_pool_create_flags() noexcept {
  return VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
}

inline const char* upload_stage_name(UploadStage stage) noexcept {
  switch (stage) {
    case UploadStage::None:
      return "none";
    case UploadStage::Buffer:
      return "buffer";
    case UploadStage::Image:
      return "image";
    case UploadStage::Map:
      return "map";
    case UploadStage::Bind:
      return "bind";
    case UploadStage::View:
      return "view";
    case UploadStage::Descriptor:
      return "descriptor";
    case UploadStage::CommandAllocate:
      return "command_allocate";
    case UploadStage::CommandBegin:
      return "command_begin";
    case UploadStage::CommandEnd:
      return "command_end";
    case UploadStage::FenceCreate:
      return "fence_create";
    case UploadStage::FenceReset:
      return "fence_reset";
    case UploadStage::QueueSubmit:
      return "queue_submit";
    case UploadStage::Wait:
      return "wait";
  }
  return "unknown";
}

}  // namespace wds::renderer
