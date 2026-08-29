#include "wds/renderer/descriptor_pool_policy.hpp"
#include "wds/renderer/draw_batch.hpp"
#include "wds/renderer/upload_result.hpp"
#include "wds/renderer/vulkan_renderer.hpp"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifndef WDS_HAS_TEXTURE_CACHE_TESTS
void run_texture_cache_tests() {}
#else
void run_texture_cache_tests();
#endif

namespace {

int failures = 0;

#define CHECK(cond)                                                           \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,     \
                   __LINE__);                                                 \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

using wds::renderer::apply_msaa_failure_event;
using wds::renderer::apply_msaa_may_teardown;
using wds::renderer::apply_msaa_rebuild_failure_health;
using wds::renderer::apply_msaa_zero_extent_ok;
using wds::renderer::apply_renderer_health;
using wds::renderer::apply_swapchain_create_failure_health;
using wds::renderer::apply_zero_extent;
using wds::renderer::classify_upload_fence_status;
using wds::renderer::classify_wsi_result;
using wds::renderer::create_sync_object_ok;
using wds::renderer::descriptor_alloc_step_for_block;
using wds::renderer::descriptor_alloc_try_next_block;
using wds::renderer::descriptor_block_skip_full;
using wds::renderer::descriptor_chain_book_alloc;
using wds::renderer::DescriptorAllocStep;
using wds::renderer::descriptor_chain_book_free;
using wds::renderer::descriptor_pool_live_after_alloc;
using wds::renderer::descriptor_pool_live_after_free;
using wds::renderer::descriptor_pool_may_reset;
using wds::renderer::descriptor_pool_next_capacity;
using wds::renderer::DescriptorPoolBlockBook;
using wds::renderer::graphics_fence_proves_presentation_complete;
using wds::renderer::kDescriptorPoolCappedCapacity;
using wds::renderer::kDescriptorPoolInitialCapacity;
using wds::renderer::kDescriptorPoolSecondCapacity;
using wds::renderer::draw_frame_blocks_before_recovery;
using wds::renderer::draw_frame_reaps_completed_uploads_before_zero_extent_return;
using wds::renderer::draw_frame_waits_pending_uploads_before_sample;
using wds::renderer::draw_frame_zero_extent_reap_applies_health;
using wds::renderer::create_texture_rgba_waits_upload_fence;
using wds::renderer::wait_pending_uploads_is_noop;
using wds::renderer::next_frame_recovers_swapchain;
using wds::renderer::surface_formats_query_ok;
using wds::renderer::swapchain_create_failure_reports_occluded;
using wds::renderer::kInvalidTextureId;
using wds::renderer::make_upload_map_result;
using wds::renderer::make_upload_result;
using wds::renderer::pending_commit_finish;
using wds::renderer::pending_commit_reserve;
using wds::renderer::pending_commit_rollback;
using wds::renderer::pending_commit_submit;
using wds::renderer::pending_push_after_submit_allowed;
using wds::renderer::pending_record_rollback_if_submit_fails;
using wds::renderer::pending_submit_requires_reserved_record;
using wds::renderer::pending_upload_occupied;
using wds::renderer::PendingUploadCommitTrace;
using wds::renderer::PendingUploadHold;
using wds::renderer::retain_device_wait_idle_on_msaa_and_resize;
using wds::renderer::resize_introduces_delay_semantics;
using wds::renderer::resize_recreates_for_call;
using wds::renderer::resize_same_extent_short_circuits;
using wds::renderer::renderer_health_occluded_valid;
using wds::renderer::renderer_health_ready;
using wds::renderer::RendererHealth;
using wds::renderer::RendererHealthEvent;
using wds::renderer::TextureInfo;
using wds::renderer::upload_begin_should_release_command;
using wds::renderer::upload_command_pool_create_flags;
using wds::renderer::upload_destroy_may_release_pending;
using wds::renderer::upload_end_should_release_command;
using wds::renderer::upload_fence_poll_health;
using wds::renderer::upload_health_delta;
using wds::renderer::upload_may_destroy_resources;
using wds::renderer::upload_may_destroy_texture_image;
using wds::renderer::upload_may_release_command;
using wds::renderer::upload_may_reset_fence;
using wds::renderer::upload_no_memory_type;
using wds::renderer::upload_pool_create_failure_tears_down_renderer;
using wds::renderer::upload_publish_texture_after_submit;
using wds::renderer::upload_reap_may_release;
using wds::renderer::upload_slot_reusable_after_failure;
using wds::renderer::upload_submit_failure_recycles_slot;
using wds::renderer::wait_pending_uploads_holds_staging_on_failure;
using wds::renderer::wait_pending_uploads_reaps_after_ok_wait;
using wds::renderer::kHostVisibleMapChunkBytes;
using wds::renderer::staging_copy_chunk_bytes;
using wds::renderer::staging_copy_chunk_count;
using wds::renderer::texture_upload_unmaps_persistent_vertex_maps;
using wds::renderer::UploadFencePoll;
using wds::renderer::upload_stage_name;
using wds::renderer::UploadHealthDelta;
using wds::renderer::UploadResult;
using wds::renderer::UploadStage;
using wds::renderer::VulkanRenderer;
using wds::renderer::WsiRecoverAction;

void test_default_upload_result_is_success() {
  const UploadResult ok{};
  CHECK(ok.result == VK_SUCCESS);
  CHECK(ok.stage == UploadStage::None);
  CHECK(ok.ok());
  CHECK(static_cast<bool>(ok));
}

void test_failed_result_keeps_exact_stage_and_vk_result() {
  const UploadStage stages[] = {
      UploadStage::Buffer,          UploadStage::Image,
      UploadStage::Map,             UploadStage::Bind,
      UploadStage::View,            UploadStage::Descriptor,
      UploadStage::CommandAllocate, UploadStage::CommandBegin,
      UploadStage::CommandEnd,      UploadStage::FenceCreate,
      UploadStage::FenceReset,      UploadStage::QueueSubmit,
      UploadStage::Wait,
  };
  const VkResult errors[] = {
      VK_ERROR_OUT_OF_HOST_MEMORY,   VK_ERROR_OUT_OF_DEVICE_MEMORY,
      VK_ERROR_DEVICE_LOST,          VK_ERROR_MEMORY_MAP_FAILED,
      VK_ERROR_TOO_MANY_OBJECTS,     VK_ERROR_INITIALIZATION_FAILED,
  };

  for (const UploadStage stage : stages) {
    for (const VkResult error : errors) {
      const UploadResult failed = make_upload_result(error, stage);
      CHECK(failed.result == error);
      CHECK(failed.result != VK_SUCCESS);
      CHECK(failed.stage == stage);
      CHECK(!failed.ok());
      CHECK(!static_cast<bool>(failed));
    }
  }
}

void test_success_does_not_invent_a_failure_stage() {
  const UploadResult ok = make_upload_result(VK_SUCCESS, UploadStage::CommandBegin);
  CHECK(ok.result == VK_SUCCESS);
  CHECK(ok.stage == UploadStage::None);
  CHECK(ok.ok());
}

void test_null_map_pointer_is_not_swallowed_as_success() {
  const char mapped_addr = 0;
  const UploadResult mapped_ok = make_upload_map_result(VK_SUCCESS, &mapped_addr);
  CHECK(mapped_ok.ok());
  CHECK(mapped_ok.result == VK_SUCCESS);
  CHECK(mapped_ok.stage == UploadStage::None);

  const UploadResult null_ok = make_upload_map_result(VK_SUCCESS, nullptr);
  CHECK(!null_ok.ok());
  CHECK(null_ok.result == VK_ERROR_MEMORY_MAP_FAILED);
  CHECK(null_ok.result != VK_SUCCESS);
  CHECK(null_ok.stage == UploadStage::Map);

  const UploadResult map_error =
      make_upload_map_result(VK_ERROR_MEMORY_MAP_FAILED, nullptr);
  CHECK(!map_error.ok());
  CHECK(map_error.result == VK_ERROR_MEMORY_MAP_FAILED);
  CHECK(map_error.stage == UploadStage::Map);
}

void test_missing_memory_type_is_not_vk_success() {
  const UploadResult buffer = upload_no_memory_type(UploadStage::Buffer);
  CHECK(!buffer.ok());
  CHECK(buffer.result != VK_SUCCESS);
  CHECK(buffer.stage == UploadStage::Buffer);

  const UploadResult image = upload_no_memory_type(UploadStage::Image);
  CHECK(!image.ok());
  CHECK(image.result != VK_SUCCESS);
  CHECK(image.stage == UploadStage::Image);
}

void test_begin_failure_releases_allocated_command() {
  const UploadResult alloc_fail =
      make_upload_result(VK_ERROR_OUT_OF_DEVICE_MEMORY, UploadStage::CommandAllocate);
  CHECK(!upload_begin_should_release_command(alloc_fail));

  const UploadResult begin_fail =
      make_upload_result(VK_ERROR_OUT_OF_HOST_MEMORY, UploadStage::CommandBegin);
  CHECK(upload_begin_should_release_command(begin_fail));

  const UploadResult begin_ok = make_upload_result(VK_SUCCESS, UploadStage::CommandBegin);
  CHECK(!upload_begin_should_release_command(begin_ok));
}

void test_end_chain_pre_submit_releases_command() {
  const UploadStage pre_submit[] = {
      UploadStage::CommandEnd,
      UploadStage::FenceCreate,
      UploadStage::FenceReset,
      UploadStage::QueueSubmit,
  };
  for (const UploadStage stage : pre_submit) {
    const UploadResult failed = make_upload_result(VK_ERROR_OUT_OF_HOST_MEMORY, stage);
    CHECK(upload_end_should_release_command(failed));
    CHECK(upload_may_release_command(failed, false, false));
    CHECK(upload_may_destroy_resources(failed, false, false));
  }

  const UploadResult success{};
  CHECK(!upload_end_should_release_command(success));
  CHECK(!upload_may_release_command(success, true, false));
  CHECK(upload_may_release_command(success, true, true));

  const UploadResult unrelated =
      make_upload_result(VK_ERROR_OUT_OF_HOST_MEMORY, UploadStage::Buffer);
  CHECK(!upload_end_should_release_command(unrelated));
}

void test_wait_failure_holds_until_queue_idle() {
  const UploadResult wait_lost = make_upload_result(VK_ERROR_DEVICE_LOST, UploadStage::Wait);
  const UploadResult wait_timeout = make_upload_result(VK_TIMEOUT, UploadStage::Wait);

  // Submitted + wait failed: must not free pending cmd / destroy image+staging.
  CHECK(!upload_end_should_release_command(wait_lost));
  CHECK(!upload_may_release_command(wait_lost, true, false));
  CHECK(!upload_may_destroy_resources(wait_lost, true, false));
  CHECK(!upload_may_release_command(wait_timeout, true, false));
  CHECK(!upload_may_destroy_resources(wait_timeout, true, false));
  CHECK(!upload_may_reset_fence(true, false));
  CHECK(!upload_slot_reusable_after_failure(true));

  // Only after the queue is proven idle may we release and reuse the slot.
  CHECK(upload_may_release_command(wait_lost, true, true));
  CHECK(upload_may_destroy_resources(wait_timeout, true, true));
  CHECK(upload_may_reset_fence(true, true));
  CHECK(upload_may_reset_fence(false, false));
  CHECK(upload_slot_reusable_after_failure(false));
}

void test_upload_device_lost_and_wait_join_health() {
  const UploadHealthDelta lost = upload_health_delta(VK_ERROR_DEVICE_LOST, UploadStage::Wait);
  CHECK(lost.apply);
  CHECK(lost.event == RendererHealthEvent::DeviceLost);
  CHECK(apply_renderer_health(RendererHealth::Ready, lost.event) == RendererHealth::DeviceLost);
  CHECK(!renderer_health_ready(RendererHealth::DeviceLost));

  const UploadHealthDelta submit_lost =
      upload_health_delta(VK_ERROR_DEVICE_LOST, UploadStage::QueueSubmit);
  CHECK(submit_lost.apply);
  CHECK(submit_lost.event == RendererHealthEvent::DeviceLost);

  const UploadHealthDelta wait_fatal = upload_health_delta(VK_TIMEOUT, UploadStage::Wait);
  CHECK(wait_fatal.apply);
  CHECK(wait_fatal.event == RendererHealthEvent::Fatal);
  CHECK(apply_renderer_health(RendererHealth::Ready, wait_fatal.event) == RendererHealth::Fatal);
  CHECK(!renderer_health_ready(RendererHealth::Fatal));

  const UploadHealthDelta oom = upload_health_delta(VK_ERROR_OUT_OF_DEVICE_MEMORY, UploadStage::Buffer);
  CHECK(!oom.apply);
}

void test_stage_names_are_distinct_and_non_empty() {
  const UploadStage stages[] = {
      UploadStage::None,            UploadStage::Buffer,
      UploadStage::Image,           UploadStage::Map,
      UploadStage::Bind,            UploadStage::View,
      UploadStage::Descriptor,      UploadStage::CommandAllocate,
      UploadStage::CommandBegin,    UploadStage::CommandEnd,
      UploadStage::FenceCreate,     UploadStage::FenceReset,
      UploadStage::QueueSubmit,     UploadStage::Wait,
  };
  std::set<std::string> names;
  for (const UploadStage stage : stages) {
    const char* name = upload_stage_name(stage);
    CHECK(name != nullptr);
    CHECK(name[0] != '\0');
    CHECK(std::strcmp(name, "unknown") != 0);
    CHECK(names.insert(name).second);
  }
  CHECK(std::strcmp(upload_stage_name(static_cast<UploadStage>(255)), "unknown") == 0);
}

void test_async_submit_publishes_and_submit_failure_recycles() {
  CHECK(upload_publish_texture_after_submit(true));
  CHECK(!upload_publish_texture_after_submit(false));
  CHECK(upload_submit_failure_recycles_slot(false));
  CHECK(!upload_submit_failure_recycles_slot(true));

  const UploadResult submit_fail =
      make_upload_result(VK_ERROR_OUT_OF_DEVICE_MEMORY, UploadStage::QueueSubmit);
  CHECK(upload_may_release_command(submit_fail, false, false));
  CHECK(upload_may_destroy_resources(submit_fail, false, false));
  CHECK(upload_slot_reusable_after_failure(false));

  const UploadResult submit_ok{};
  CHECK(!upload_may_release_command(submit_ok, true, false));
  CHECK(!upload_may_destroy_resources(submit_ok, true, false));
}

void test_fence_poll_classifies_ready_pending_lost_fatal() {
  CHECK(classify_upload_fence_status(VK_SUCCESS) == UploadFencePoll::Ready);
  CHECK(classify_upload_fence_status(VK_NOT_READY) == UploadFencePoll::Pending);
  CHECK(classify_upload_fence_status(VK_ERROR_DEVICE_LOST) == UploadFencePoll::DeviceLost);
  CHECK(classify_upload_fence_status(VK_ERROR_OUT_OF_HOST_MEMORY) == UploadFencePoll::Fatal);
  CHECK(classify_upload_fence_status(VK_TIMEOUT) == UploadFencePoll::Fatal);
}

void test_reap_and_destroy_hold_in_flight_resources() {
  CHECK(upload_reap_may_release(UploadFencePoll::Ready, false));
  CHECK(!upload_reap_may_release(UploadFencePoll::Pending, false));
  CHECK(!upload_reap_may_release(UploadFencePoll::DeviceLost, false));
  CHECK(!upload_reap_may_release(UploadFencePoll::Fatal, false));
  CHECK(upload_reap_may_release(UploadFencePoll::DeviceLost, true));
  CHECK(upload_reap_may_release(UploadFencePoll::Fatal, true));
  CHECK(upload_reap_may_release(UploadFencePoll::Pending, true));

  CHECK(upload_destroy_may_release_pending(true, false));
  CHECK(upload_destroy_may_release_pending(false, true));
  CHECK(!upload_destroy_may_release_pending(false, false));
}

void test_reap_device_lost_and_fatal_join_health() {
  const UploadHealthDelta lost = upload_fence_poll_health(UploadFencePoll::DeviceLost);
  CHECK(lost.apply);
  CHECK(lost.event == RendererHealthEvent::DeviceLost);
  CHECK(apply_renderer_health(RendererHealth::Ready, lost.event) == RendererHealth::DeviceLost);

  const UploadHealthDelta fatal = upload_fence_poll_health(UploadFencePoll::Fatal);
  CHECK(fatal.apply);
  CHECK(fatal.event == RendererHealthEvent::Fatal);
  CHECK(apply_renderer_health(RendererHealth::Ready, fatal.event) == RendererHealth::Fatal);

  CHECK(!upload_fence_poll_health(UploadFencePoll::Ready).apply);
  CHECK(!upload_fence_poll_health(UploadFencePoll::Pending).apply);
}

void test_pending_reserve_before_submit_is_rollbackable() {
  CHECK(pending_submit_requires_reserved_record());
  CHECK(pending_record_rollback_if_submit_fails());
  CHECK(!pending_push_after_submit_allowed());

  PendingUploadCommitTrace leaked{};
  CHECK(!pending_commit_submit(leaked, true));
  CHECK(leaked.leaked_after_submit);
  CHECK(!leaked.committed);

  PendingUploadCommitTrace failed{};
  CHECK(pending_commit_reserve(failed));
  CHECK(!pending_commit_submit(failed, false));
  pending_commit_rollback(failed);
  CHECK(!failed.record_reserved);
  CHECK(!failed.submitted);
  CHECK(!failed.committed);
  CHECK(!failed.leaked_after_submit);

  PendingUploadCommitTrace ok{};
  CHECK(pending_commit_reserve(ok));
  CHECK(pending_commit_submit(ok, true));
  pending_commit_finish(ok);
  CHECK(ok.record_reserved);
  CHECK(ok.submitted);
  CHECK(ok.committed);
  CHECK(!ok.leaked_after_submit);
}

void test_pending_owner_unifies_stage5_hold() {
  const PendingUploadHold empty{};
  CHECK(!pending_upload_occupied(empty));

  PendingUploadHold held{};
  held.has_cmd = true;
  held.has_staging = true;
  held.has_fence = true;
  held.submitted = true;
  held.texture_id = 3;
  CHECK(pending_upload_occupied(held));
  CHECK(!upload_slot_reusable_after_failure(pending_upload_occupied(held)));
}

void test_upload_pool_create_failure_tears_down() {
  CHECK(upload_pool_create_failure_tears_down_renderer());
  CHECK((upload_command_pool_create_flags() & VK_COMMAND_POOL_CREATE_TRANSIENT_BIT) != 0);
  CHECK((upload_command_pool_create_flags() & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) !=
        0);
}

void test_destroy_retire_skips_in_flight_upload() {
  CHECK(!upload_may_destroy_texture_image(true, true, false));
  CHECK(upload_may_destroy_texture_image(false, true, false));
  CHECK(!upload_may_destroy_texture_image(false, false, false));
  CHECK(upload_may_destroy_texture_image(true, false, true));
}

void test_unsampled_destroyed_texture_frees_after_upload() {
  const bool never_sampled = true;
  CHECK(upload_may_destroy_texture_image(false, never_sampled, false));
  CHECK(!upload_may_destroy_texture_image(true, never_sampled, false));
}

void test_atlas_rebake_and_font_replace_hold_async_upload() {
  // Rebake / font replace: create publishes after submit, then destroy old.
  // Neither the new nor the old image may be freed while its upload is queued.
  CHECK(!upload_may_destroy_texture_image(true, true, false));
  CHECK(upload_may_destroy_texture_image(false, true, false));
  CHECK(upload_publish_texture_after_submit(true));
}

void test_staging_copy_chunks_cap_host_maps() {
  CHECK(staging_copy_chunk_bytes(16384) == kHostVisibleMapChunkBytes);
  CHECK(staging_copy_chunk_count(4096ull * 2048ull * 4ull, kHostVisibleMapChunkBytes) == 8);
  CHECK(staging_copy_chunk_count(64ull * 64ull * 4ull, staging_copy_chunk_bytes(64ull * 4ull)) == 1);
  CHECK(staging_copy_chunk_bytes(kHostVisibleMapChunkBytes + 16) == kHostVisibleMapChunkBytes + 16);
  CHECK(texture_upload_unmaps_persistent_vertex_maps());
}

void test_draw_frame_drains_pending_uploads_before_sample() {
  CHECK(draw_frame_waits_pending_uploads_before_sample());
  CHECK(!create_texture_rgba_waits_upload_fence());
  CHECK(wait_pending_uploads_is_noop(false));
  CHECK(!wait_pending_uploads_is_noop(true));
  CHECK(wait_pending_uploads_reaps_after_ok_wait(VK_SUCCESS));
  CHECK(!wait_pending_uploads_reaps_after_ok_wait(VK_ERROR_DEVICE_LOST));
  CHECK(!wait_pending_uploads_reaps_after_ok_wait(VK_TIMEOUT));
  CHECK(wait_pending_uploads_holds_staging_on_failure(VK_ERROR_DEVICE_LOST));
  CHECK(wait_pending_uploads_holds_staging_on_failure(VK_TIMEOUT));
  CHECK(!wait_pending_uploads_holds_staging_on_failure(VK_SUCCESS));
  CHECK(!upload_reap_may_release(UploadFencePoll::Pending, false));
  CHECK(!upload_destroy_may_release_pending(false, false));
}

void test_failed_create_returns_invalid_texture_info() {
  // Real Vk device faults are not injected here; callers must treat a failed
  // UploadResult as "do not publish TextureInfo". The empty handle is the
  // contract create_texture_rgba uses after restoring the slot.
  const TextureInfo fake{};
  CHECK(!static_cast<bool>(fake));
  CHECK(fake.id == kInvalidTextureId);
  CHECK(fake.width == 0);
  CHECK(fake.height == 0);
}

void run_upload_result_tests() {
  test_default_upload_result_is_success();
  test_failed_result_keeps_exact_stage_and_vk_result();
  test_success_does_not_invent_a_failure_stage();
  test_null_map_pointer_is_not_swallowed_as_success();
  test_missing_memory_type_is_not_vk_success();
  test_begin_failure_releases_allocated_command();
  test_end_chain_pre_submit_releases_command();
  test_wait_failure_holds_until_queue_idle();
  test_upload_device_lost_and_wait_join_health();
  test_async_submit_publishes_and_submit_failure_recycles();
  test_fence_poll_classifies_ready_pending_lost_fatal();
  test_reap_and_destroy_hold_in_flight_resources();
  test_reap_device_lost_and_fatal_join_health();
  test_pending_reserve_before_submit_is_rollbackable();
  test_pending_owner_unifies_stage5_hold();
  test_upload_pool_create_failure_tears_down();
  test_destroy_retire_skips_in_flight_upload();
  test_unsampled_destroyed_texture_frees_after_upload();
  test_atlas_rebake_and_font_replace_hold_async_upload();
  test_staging_copy_chunks_cap_host_maps();
  test_draw_frame_drains_pending_uploads_before_sample();
  test_stage_names_are_distinct_and_non_empty();
  test_failed_create_returns_invalid_texture_info();
}

void test_wsi_classify_success_is_none() {
  CHECK(classify_wsi_result(VK_SUCCESS) == WsiRecoverAction::None);
}

void test_wsi_classify_swapchain_rebuild() {
  CHECK(classify_wsi_result(VK_ERROR_OUT_OF_DATE_KHR) == WsiRecoverAction::RecreateSwapchain);
  CHECK(classify_wsi_result(VK_SUBOPTIMAL_KHR) == WsiRecoverAction::RecreateSwapchain);
  CHECK(classify_wsi_result(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) ==
        WsiRecoverAction::RecreateSwapchain);
}

void test_wsi_classify_surface_lost_rebuilds_surface() {
  CHECK(classify_wsi_result(VK_ERROR_SURFACE_LOST_KHR) ==
        WsiRecoverAction::RecreateSurfaceAndSwapchain);
}

void test_wsi_classify_device_lost_is_unrecoverable() {
  CHECK(classify_wsi_result(VK_ERROR_DEVICE_LOST) == WsiRecoverAction::DeviceLost);
}

void test_wsi_classify_other_errors_are_fatal() {
  const VkResult fatals[] = {
      VK_ERROR_OUT_OF_HOST_MEMORY,
      VK_ERROR_OUT_OF_DEVICE_MEMORY,
      VK_ERROR_INITIALIZATION_FAILED,
      VK_ERROR_MEMORY_MAP_FAILED,
      VK_TIMEOUT,
      VK_NOT_READY,
  };
  for (const VkResult error : fatals) {
    CHECK(classify_wsi_result(error) == WsiRecoverAction::Fatal);
  }
}

void test_health_ready_flags() {
  CHECK(!renderer_health_ready(RendererHealth::Uninitialized));
  CHECK(renderer_health_ready(RendererHealth::Ready));
  CHECK(renderer_health_ready(RendererHealth::Occluded));
  CHECK(renderer_health_ready(RendererHealth::SurfaceLost));
  CHECK(!renderer_health_ready(RendererHealth::DeviceLost));
  CHECK(!renderer_health_ready(RendererHealth::Fatal));
}

void test_health_create_destroy_and_zero_extent() {
  CHECK(apply_renderer_health(RendererHealth::Uninitialized, RendererHealthEvent::Created) ==
        RendererHealth::Ready);
  CHECK(apply_renderer_health(RendererHealth::Ready, RendererHealthEvent::Destroyed) ==
        RendererHealth::Uninitialized);
  CHECK(apply_renderer_health(RendererHealth::Ready, RendererHealthEvent::OccludedZeroSize) ==
        RendererHealth::Occluded);
  CHECK(apply_renderer_health(RendererHealth::Occluded, RendererHealthEvent::RecoveredExtent) ==
        RendererHealth::Ready);
  CHECK(apply_renderer_health(RendererHealth::Occluded, RendererHealthEvent::Destroyed) ==
        RendererHealth::Uninitialized);
}

void test_health_surface_lost_keeps_device_usable() {
  CHECK(apply_renderer_health(RendererHealth::Ready, RendererHealthEvent::SurfaceLost) ==
        RendererHealth::SurfaceLost);
  CHECK(renderer_health_ready(RendererHealth::SurfaceLost));
  CHECK(apply_renderer_health(RendererHealth::SurfaceLost, RendererHealthEvent::SurfaceRecovered) ==
        RendererHealth::Ready);
  CHECK(apply_renderer_health(RendererHealth::SurfaceLost, RendererHealthEvent::OccludedZeroSize) ==
        RendererHealth::SurfaceLost);
  CHECK(apply_renderer_health(RendererHealth::SurfaceLost, RendererHealthEvent::RecoveredExtent) ==
        RendererHealth::SurfaceLost);
}

void test_health_device_lost_and_fatal_are_sticky() {
  CHECK(apply_renderer_health(RendererHealth::Ready, RendererHealthEvent::DeviceLost) ==
        RendererHealth::DeviceLost);
  CHECK(apply_renderer_health(RendererHealth::Occluded, RendererHealthEvent::DeviceLost) ==
        RendererHealth::DeviceLost);
  CHECK(apply_renderer_health(RendererHealth::SurfaceLost, RendererHealthEvent::Fatal) ==
        RendererHealth::Fatal);
  CHECK(apply_renderer_health(RendererHealth::DeviceLost, RendererHealthEvent::RecoveredExtent) ==
        RendererHealth::DeviceLost);
  CHECK(apply_renderer_health(RendererHealth::DeviceLost, RendererHealthEvent::Created) ==
        RendererHealth::DeviceLost);
  CHECK(apply_renderer_health(RendererHealth::Fatal, RendererHealthEvent::SurfaceRecovered) ==
        RendererHealth::Fatal);
  CHECK(apply_renderer_health(RendererHealth::DeviceLost, RendererHealthEvent::Destroyed) ==
        RendererHealth::Uninitialized);
  CHECK(apply_renderer_health(RendererHealth::Fatal, RendererHealthEvent::Destroyed) ==
        RendererHealth::Uninitialized);
  CHECK(!renderer_health_ready(RendererHealth::DeviceLost));
  CHECK(!renderer_health_ready(RendererHealth::Fatal));
}

void test_zero_extent_without_live_swapchain_is_surface_lost() {
  CHECK(apply_zero_extent(RendererHealth::Ready, false) == RendererHealth::SurfaceLost);
  CHECK(apply_zero_extent(RendererHealth::SurfaceLost, false) == RendererHealth::SurfaceLost);
  CHECK(apply_zero_extent(RendererHealth::Ready, true) == RendererHealth::Occluded);
  CHECK(apply_zero_extent(RendererHealth::Occluded, true) == RendererHealth::Occluded);
  CHECK(apply_zero_extent(RendererHealth::SurfaceLost, true) == RendererHealth::SurfaceLost);

  CHECK(!renderer_health_occluded_valid(RendererHealth::Occluded, false));
  CHECK(renderer_health_occluded_valid(RendererHealth::Occluded, true));
  CHECK(renderer_health_occluded_valid(RendererHealth::SurfaceLost, false));
  CHECK(renderer_health_occluded_valid(RendererHealth::Ready, true));
}

void test_apply_msaa_does_not_fatal_while_surface_lost() {
  CHECK(apply_msaa_failure_event(RendererHealth::SurfaceLost) == RendererHealthEvent::SurfaceLost);
  CHECK(apply_renderer_health(RendererHealth::SurfaceLost,
                              apply_msaa_failure_event(RendererHealth::SurfaceLost)) ==
        RendererHealth::SurfaceLost);
  CHECK(apply_msaa_failure_event(RendererHealth::Ready) == RendererHealthEvent::Fatal);
  CHECK(apply_renderer_health(RendererHealth::Ready, apply_msaa_failure_event(RendererHealth::Ready)) ==
        RendererHealth::Fatal);
}

void test_apply_msaa_rebuild_failure_after_teardown_is_fatal() {
  // Resize/recover still use apply_swapchain_create_failure_health, which can
  // leave SurfaceLost/Ready when the chain is gone. apply_msaa already destroyed
  // RP; recover only rebuilds swapchain. That swapchain entry is the omission.
  CHECK(apply_swapchain_create_failure_health(
            RendererHealth::Ready, WsiRecoverAction::RecreateSurfaceAndSwapchain, false) ==
        RendererHealth::SurfaceLost);
  CHECK(apply_swapchain_create_failure_health(
            RendererHealth::Ready, WsiRecoverAction::RecreateSwapchain, false) ==
        RendererHealth::Ready);
  CHECK(apply_swapchain_create_failure_health(RendererHealth::Ready, WsiRecoverAction::None,
                                              false) == RendererHealth::SurfaceLost);

  const WsiRecoverAction after_rp[] = {
      WsiRecoverAction::RecreateSurfaceAndSwapchain,
      WsiRecoverAction::RecreateSwapchain,
      WsiRecoverAction::None,
      WsiRecoverAction::Fatal,
  };
  for (const WsiRecoverAction action : after_rp) {
    const RendererHealth next =
        apply_msaa_rebuild_failure_health(RendererHealth::Ready, action);
    CHECK(next == RendererHealth::Fatal);
    CHECK(!renderer_health_ready(next));
    CHECK(!next_frame_recovers_swapchain(next, false));
    CHECK(!next_frame_recovers_swapchain(next, true));
  }

  CHECK(apply_msaa_rebuild_failure_health(RendererHealth::Ready, WsiRecoverAction::DeviceLost) ==
        RendererHealth::DeviceLost);
  CHECK(apply_msaa_rebuild_failure_health(RendererHealth::Occluded,
                                         WsiRecoverAction::RecreateSurfaceAndSwapchain) ==
        RendererHealth::Fatal);
  CHECK(apply_msaa_rebuild_failure_health(RendererHealth::SurfaceLost,
                                         WsiRecoverAction::RecreateSurfaceAndSwapchain) ==
        RendererHealth::Fatal);
  CHECK(apply_msaa_rebuild_failure_health(RendererHealth::DeviceLost,
                                         WsiRecoverAction::RecreateSwapchain) ==
        RendererHealth::DeviceLost);
}

void test_apply_msaa_reads_framebuffer_before_teardown() {
  CHECK(apply_msaa_may_teardown(1920, 1080));
  CHECK(apply_msaa_may_teardown(1, 1));
  CHECK(!apply_msaa_may_teardown(0, 0));
  CHECK(!apply_msaa_may_teardown(0, 720));
  CHECK(!apply_msaa_may_teardown(1280, 0));
  CHECK(!apply_msaa_zero_extent_ok());
  CHECK(apply_zero_extent(RendererHealth::Ready, true) == RendererHealth::Occluded);
  CHECK(apply_zero_extent(RendererHealth::Ready, false) == RendererHealth::SurfaceLost);
}

void test_torn_down_swapchain_create_failure_follows_last_wsi_action() {
  const RendererHealth surface = apply_swapchain_create_failure_health(
      RendererHealth::Ready, WsiRecoverAction::RecreateSurfaceAndSwapchain, false);
  CHECK(surface == RendererHealth::SurfaceLost);
  CHECK(renderer_health_ready(surface));
  CHECK(!next_frame_recovers_swapchain(surface, false));
  CHECK(!swapchain_create_failure_reports_occluded(WsiRecoverAction::RecreateSurfaceAndSwapchain,
                                                   false));

  const RendererHealth lost = apply_swapchain_create_failure_health(
      RendererHealth::Ready, WsiRecoverAction::DeviceLost, false);
  CHECK(lost == RendererHealth::DeviceLost);
  CHECK(!renderer_health_ready(lost));
  CHECK(apply_swapchain_create_failure_health(RendererHealth::DeviceLost,
                                              WsiRecoverAction::RecreateSwapchain, false) ==
        RendererHealth::DeviceLost);

  const RendererHealth fatal = apply_swapchain_create_failure_health(
      RendererHealth::Ready, WsiRecoverAction::Fatal, false);
  CHECK(fatal == RendererHealth::Fatal);
  CHECK(!renderer_health_ready(fatal));

  const RendererHealth retry = apply_swapchain_create_failure_health(
      RendererHealth::Ready, WsiRecoverAction::RecreateSwapchain, false);
  CHECK(retry == RendererHealth::Ready);
  CHECK(renderer_health_ready(retry));
  CHECK(next_frame_recovers_swapchain(retry, false));
  CHECK(!swapchain_create_failure_reports_occluded(WsiRecoverAction::RecreateSwapchain, false));
  CHECK(renderer_health_occluded_valid(retry, false));

  const RendererHealth from_occluded = apply_swapchain_create_failure_health(
      RendererHealth::Occluded, WsiRecoverAction::RecreateSwapchain, false);
  CHECK(from_occluded == RendererHealth::Ready);
  CHECK(!swapchain_create_failure_reports_occluded(WsiRecoverAction::RecreateSwapchain, false));

  const RendererHealth none_no_chain = apply_swapchain_create_failure_health(
      RendererHealth::Ready, WsiRecoverAction::None, false);
  CHECK(none_no_chain == RendererHealth::SurfaceLost);
  CHECK(!swapchain_create_failure_reports_occluded(WsiRecoverAction::None, false));

  const RendererHealth none_live = apply_swapchain_create_failure_health(
      RendererHealth::Ready, WsiRecoverAction::None, true);
  CHECK(none_live == RendererHealth::Ready);
  CHECK(!swapchain_create_failure_reports_occluded(WsiRecoverAction::None, true));
}

void test_draw_frame_ready_null_swapchain_enters_recovery() {
  CHECK(!draw_frame_blocks_before_recovery(RendererHealth::Ready, true, false));
  CHECK(next_frame_recovers_swapchain(RendererHealth::Ready, false));
  CHECK(draw_frame_blocks_before_recovery(RendererHealth::DeviceLost, false, false));
  CHECK(draw_frame_blocks_before_recovery(RendererHealth::Fatal, false, true));
  CHECK(draw_frame_blocks_before_recovery(RendererHealth::Uninitialized, false, false));
  CHECK(!draw_frame_blocks_before_recovery(RendererHealth::Occluded, true, true));
  CHECK(!draw_frame_blocks_before_recovery(RendererHealth::SurfaceLost, true, false));
  CHECK(!next_frame_recovers_swapchain(RendererHealth::SurfaceLost, false));
  CHECK(apply_zero_extent(RendererHealth::Ready, false) == RendererHealth::SurfaceLost);
  CHECK(!renderer_health_occluded_valid(RendererHealth::Occluded, false));
}

void test_surface_formats_query_rejects_empty_or_failed() {
  CHECK(surface_formats_query_ok(VK_SUCCESS, 2, VK_SUCCESS));
  CHECK(surface_formats_query_ok(VK_SUCCESS, 1, VK_SUCCESS));
  CHECK(!surface_formats_query_ok(VK_SUCCESS, 0, VK_SUCCESS));
  CHECK(!surface_formats_query_ok(VK_ERROR_SURFACE_LOST_KHR, 4, VK_SUCCESS));
  CHECK(!surface_formats_query_ok(VK_SUCCESS, 2, VK_ERROR_OUT_OF_HOST_MEMORY));
  CHECK(!surface_formats_query_ok(VK_ERROR_DEVICE_LOST, 0, VK_ERROR_DEVICE_LOST));
  CHECK(classify_wsi_result(VK_ERROR_SURFACE_LOST_KHR) ==
        WsiRecoverAction::RecreateSurfaceAndSwapchain);
  CHECK(classify_wsi_result(VK_ERROR_OUT_OF_DEVICE_MEMORY) == WsiRecoverAction::Fatal);
}

void test_create_sync_object_checks_vk_result() {
  CHECK(create_sync_object_ok(VK_SUCCESS));
  CHECK(!create_sync_object_ok(VK_ERROR_OUT_OF_HOST_MEMORY));
  CHECK(!create_sync_object_ok(VK_ERROR_OUT_OF_DEVICE_MEMORY));
  CHECK(!create_sync_object_ok(VK_ERROR_DEVICE_LOST));
}

void test_renderer_object_starts_and_destroys_uninitialized() {
  VulkanRenderer renderer;
  CHECK(renderer.health() == RendererHealth::Uninitialized);
  CHECK(!renderer.ready());
  const auto desc = renderer.descriptor_pool_diagnostics();
  CHECK(desc.block_count == 0);
  CHECK(desc.live_sets == 0);
  renderer.destroy();
  CHECK(renderer.health() == RendererHealth::Uninitialized);
  CHECK(!renderer.ready());
  CHECK(renderer.descriptor_pool_diagnostics().block_count == 0);
}

void test_path_diagnostics_default_disabled_and_empty() {
  VulkanRenderer renderer;
  CHECK(!renderer.path_diagnostics_enabled());
  const auto snap = renderer.path_diagnostics();
  CHECK(snap.counts.create_texture_rgba == 0);
  CHECK(snap.counts.upload_fence_wait == 0);
  CHECK(snap.counts.upload_submit == 0);
  CHECK(snap.counts.upload_fence_reap == 0);
  CHECK(snap.counts.descriptor_allocate == 0);
  CHECK(snap.counts.swapchain_recreate == 0);
  CHECK(snap.counts.apply_msaa == 0);
  CHECK(snap.counts.apply_msaa_idle_wait == 0);
  CHECK(snap.last.create_texture_rgba_us == 0);
  CHECK(snap.total.upload_fence_wait_us == 0);
  CHECK(renderer.last_path_sample().descriptor_allocate_us == 0);

  const unsigned char pixel[4] = {255, 0, 0, 255};
  CHECK(!static_cast<bool>(renderer.create_texture_rgba(pixel, 1, 1)));
  CHECK(renderer.path_diagnostics().counts.create_texture_rgba == 0);
}

void test_path_diagnostics_enable_reset_keeps_enabled() {
  VulkanRenderer renderer;
  renderer.set_path_diagnostics_enabled(true);
  CHECK(renderer.path_diagnostics_enabled());
  renderer.reset_path_diagnostics();
  CHECK(renderer.path_diagnostics_enabled());
  CHECK(renderer.path_diagnostics().counts.create_texture_rgba == 0);
  CHECK(renderer.last_path_sample().create_texture_rgba_us == 0);
  renderer.set_path_diagnostics_enabled(false);
  CHECK(!renderer.path_diagnostics_enabled());
}

void test_path_sample_aggregation_last_total_and_counts() {
  using wds::renderer::record_path_sample;
  using wds::renderer::reset_path_snapshot;
  using wds::renderer::RendererPathSegment;
  using wds::renderer::RendererPathSnapshot;

  RendererPathSnapshot snap;
  record_path_sample(snap, RendererPathSegment::CreateTextureRgba, 1000);
  record_path_sample(snap, RendererPathSegment::UploadFenceWait, 400);
  record_path_sample(snap, RendererPathSegment::UploadSubmit, 90);
  record_path_sample(snap, RendererPathSegment::UploadFenceReap, 3);
  record_path_sample(snap, RendererPathSegment::DescriptorAllocate, 50);
  record_path_sample(snap, RendererPathSegment::SwapchainRecreate, 2000);
  record_path_sample(snap, RendererPathSegment::ApplyMsaa, 3000);
  record_path_sample(snap, RendererPathSegment::ApplyMsaaIdleWait, 1500);

  CHECK(snap.last.create_texture_rgba_us == 1000);
  CHECK(snap.last.upload_fence_wait_us == 400);
  CHECK(snap.last.upload_submit_us == 90);
  CHECK(snap.last.upload_fence_reap_us == 3);
  CHECK(snap.last.descriptor_allocate_us == 50);
  CHECK(snap.last.swapchain_recreate_us == 2000);
  CHECK(snap.last.apply_msaa_us == 3000);
  CHECK(snap.last.apply_msaa_idle_wait_us == 1500);
  CHECK(snap.counts.create_texture_rgba == 1);
  CHECK(snap.counts.upload_submit == 1);
  CHECK(snap.counts.upload_fence_reap == 1);
  CHECK(snap.counts.apply_msaa_idle_wait == 1);
  CHECK(snap.total.apply_msaa_us == 3000);

  record_path_sample(snap, RendererPathSegment::CreateTextureRgba, 2500);
  CHECK(snap.last.create_texture_rgba_us == 2500);
  CHECK(snap.total.create_texture_rgba_us == 3500);
  CHECK(snap.counts.create_texture_rgba == 2);

  record_path_sample(snap, RendererPathSegment::UploadFenceWait, -7);
  CHECK(snap.last.upload_fence_wait_us == 0);
  CHECK(snap.total.upload_fence_wait_us == 400);
  CHECK(snap.counts.upload_fence_wait == 2);

  reset_path_snapshot(snap);
  CHECK(snap.counts.create_texture_rgba == 0);
  CHECK(snap.counts.upload_fence_wait == 0);
  CHECK(snap.last.create_texture_rgba_us == 0);
  CHECK(snap.total.apply_msaa_us == 0);
}

void test_unready_upload_does_not_record_when_enabled() {
  VulkanRenderer renderer;
  renderer.set_path_diagnostics_enabled(true);
  const unsigned char pixel[4] = {1, 2, 3, 4};
  CHECK(!static_cast<bool>(renderer.create_texture_rgba(pixel, 1, 1)));
  CHECK(!static_cast<bool>(renderer.create_texture_rgba(nullptr, 64, 64)));
  CHECK(renderer.path_diagnostics().counts.create_texture_rgba == 0);
  CHECK(renderer.path_diagnostics().counts.upload_fence_wait == 0);
  CHECK(renderer.path_diagnostics().counts.upload_submit == 0);
  CHECK(renderer.path_diagnostics().counts.upload_fence_reap == 0);
  CHECK(renderer.path_diagnostics().counts.descriptor_allocate == 0);
}

void test_path_diagnostics_distinguish_async_submit_and_reap() {
  using wds::renderer::record_path_sample;
  using wds::renderer::RendererPathSegment;
  using wds::renderer::RendererPathSnapshot;

  RendererPathSnapshot snap;
  record_path_sample(snap, RendererPathSegment::UploadSubmit, 120);
  record_path_sample(snap, RendererPathSegment::UploadFenceReap, 5);
  CHECK(snap.last.upload_submit_us == 120);
  CHECK(snap.last.upload_fence_reap_us == 5);
  CHECK(snap.last.upload_fence_wait_us == 0);
  CHECK(snap.counts.upload_submit == 1);
  CHECK(snap.counts.upload_fence_reap == 1);
  CHECK(snap.counts.upload_fence_wait == 0);

  record_path_sample(snap, RendererPathSegment::UploadFenceWait, 0);
  CHECK(snap.last.upload_fence_wait_us == 0);
  CHECK(snap.counts.upload_fence_wait == 1);
  CHECK(snap.last.upload_submit_us == 120);
}

void test_resize_same_extent_short_circuits_once() {
  CHECK(resize_same_extent_short_circuits(1280, 720, 1280, 720, false, true));
  CHECK(!resize_same_extent_short_circuits(1280, 720, 1280, 720, true, true));
  CHECK(!resize_same_extent_short_circuits(1280, 720, 1280, 720, false, false));
  CHECK(!resize_same_extent_short_circuits(800, 600, 1280, 720, false, true));
  CHECK(!resize_same_extent_short_circuits(0, 720, 0, 720, false, true));
  CHECK(!resize_same_extent_short_circuits(1280, 0, 1280, 0, false, true));
  CHECK(resize_recreates_for_call(true) == 0);
  CHECK(resize_recreates_for_call(false) == 1);
  CHECK(!resize_introduces_delay_semantics());
}

void test_msaa_resize_retain_device_wait_idle() {
  CHECK(!graphics_fence_proves_presentation_complete());
  CHECK(retain_device_wait_idle_on_msaa_and_resize());
}

void test_draw_frame_reaps_before_zero_extent_or_occluded_return() {
  CHECK(draw_frame_reaps_completed_uploads_before_zero_extent_return());
  CHECK(!draw_frame_zero_extent_reap_applies_health());
  CHECK(draw_frame_waits_pending_uploads_before_sample());
}

void test_descriptor_full_block_is_skipped_then_grows() {
  CHECK(descriptor_block_skip_full(256, 256));
  CHECK(descriptor_block_skip_full(512, 512));
  CHECK(!descriptor_block_skip_full(255, 256));
  CHECK(!descriptor_block_skip_full(0, 256));
  CHECK(descriptor_block_skip_full(0, 0));

  CHECK(descriptor_alloc_step_for_block(256, 256, false) == DescriptorAllocStep::Grow);
  CHECK(descriptor_alloc_step_for_block(256, 256, true) == DescriptorAllocStep::SkipFull);
  CHECK(descriptor_alloc_step_for_block(255, 256, false) == DescriptorAllocStep::TryAllocate);
  CHECK(descriptor_alloc_step_for_block(0, 256, false) == DescriptorAllocStep::TryAllocate);
}

void test_descriptor_pool_growth_and_retry_policy() {
  CHECK(descriptor_pool_next_capacity(0) == kDescriptorPoolInitialCapacity);
  CHECK(descriptor_pool_next_capacity(kDescriptorPoolInitialCapacity) ==
        kDescriptorPoolSecondCapacity);
  CHECK(descriptor_pool_next_capacity(kDescriptorPoolSecondCapacity) ==
        kDescriptorPoolCappedCapacity);
  CHECK(descriptor_pool_next_capacity(kDescriptorPoolCappedCapacity) ==
        kDescriptorPoolCappedCapacity);
  CHECK(descriptor_alloc_try_next_block(VK_ERROR_OUT_OF_POOL_MEMORY));
  CHECK(descriptor_alloc_try_next_block(VK_ERROR_FRAGMENTED_POOL));
  CHECK(!descriptor_alloc_try_next_block(VK_ERROR_OUT_OF_HOST_MEMORY));
  CHECK(!descriptor_alloc_try_next_block(VK_ERROR_OUT_OF_DEVICE_MEMORY));
  CHECK(!descriptor_alloc_try_next_block(VK_ERROR_DEVICE_LOST));
  CHECK(!descriptor_alloc_try_next_block(VK_SUCCESS));
  CHECK(descriptor_pool_may_reset(0));
  CHECK(!descriptor_pool_may_reset(1));
  CHECK(descriptor_pool_live_after_alloc(0, true) == 1);
  CHECK(descriptor_pool_live_after_alloc(3, false) == 3);
  CHECK(descriptor_pool_live_after_free(1) == 0);
  CHECK(descriptor_pool_live_after_free(0) == 0);
}

void test_descriptor_chain_300_cross_block_alloc_and_recycle() {
  std::vector<DescriptorPoolBlockBook> blocks;
  std::vector<uint32_t> owners;
  owners.reserve(300);
  for (int i = 0; i < 300; ++i) {
    uint32_t block = 0;
    CHECK(descriptor_chain_book_alloc(blocks, block));
    owners.push_back(block);
  }
  CHECK(blocks.size() >= 2);
  CHECK(blocks[0].capacity == kDescriptorPoolInitialCapacity);
  CHECK(blocks[0].live == kDescriptorPoolInitialCapacity);
  CHECK(blocks[1].capacity == kDescriptorPoolSecondCapacity);
  CHECK(blocks[1].live == 300u - kDescriptorPoolInitialCapacity);
  CHECK(!descriptor_pool_may_reset(blocks[0].live));
  CHECK(!descriptor_pool_may_reset(blocks[1].live));

  uint32_t freed0 = 0;
  for (uint32_t& owner : owners) {
    if (owner == 0) {
      CHECK(descriptor_chain_book_free(blocks, 0));
      ++freed0;
      owner = ~0u;
    }
  }
  CHECK(freed0 == kDescriptorPoolInitialCapacity);
  CHECK(blocks[0].live == 0);
  CHECK(descriptor_pool_may_reset(blocks[0].live));
  CHECK(!descriptor_pool_may_reset(blocks[1].live));

  uint32_t reused = 99;
  CHECK(descriptor_chain_book_alloc(blocks, reused));
  CHECK(reused == 0);
  CHECK(blocks[0].live == 1);
  CHECK(blocks[1].live == 300u - kDescriptorPoolInitialCapacity);

  std::vector<DescriptorPoolBlockBook> grow;
  for (int i = 0; i < 769; ++i) {
    uint32_t block = 0;
    CHECK(descriptor_chain_book_alloc(grow, block));
  }
  CHECK(grow.size() == 3);
  CHECK(grow[0].capacity == kDescriptorPoolInitialCapacity);
  CHECK(grow[1].capacity == kDescriptorPoolSecondCapacity);
  CHECK(grow[2].capacity == kDescriptorPoolCappedCapacity);
  CHECK(grow[2].live == 1);
}

void run_wsi_health_tests() {
  test_wsi_classify_success_is_none();
  test_wsi_classify_swapchain_rebuild();
  test_wsi_classify_surface_lost_rebuilds_surface();
  test_wsi_classify_device_lost_is_unrecoverable();
  test_wsi_classify_other_errors_are_fatal();
  test_health_ready_flags();
  test_health_create_destroy_and_zero_extent();
  test_health_surface_lost_keeps_device_usable();
  test_health_device_lost_and_fatal_are_sticky();
  test_zero_extent_without_live_swapchain_is_surface_lost();
  test_apply_msaa_does_not_fatal_while_surface_lost();
  test_apply_msaa_rebuild_failure_after_teardown_is_fatal();
  test_apply_msaa_reads_framebuffer_before_teardown();
  test_torn_down_swapchain_create_failure_follows_last_wsi_action();
  test_draw_frame_ready_null_swapchain_enters_recovery();
  test_surface_formats_query_rejects_empty_or_failed();
  test_create_sync_object_checks_vk_result();
  test_renderer_object_starts_and_destroys_uninitialized();
  test_path_diagnostics_default_disabled_and_empty();
  test_path_diagnostics_enable_reset_keeps_enabled();
  test_path_sample_aggregation_last_total_and_counts();
  test_unready_upload_does_not_record_when_enabled();
  test_path_diagnostics_distinguish_async_submit_and_reap();
  test_resize_same_extent_short_circuits_once();
  test_msaa_resize_retain_device_wait_idle();
  test_draw_frame_reaps_before_zero_extent_or_occluded_return();
  test_descriptor_full_block_is_skipped_then_grows();
  test_descriptor_pool_growth_and_retry_policy();
  test_descriptor_chain_300_cross_block_alloc_and_recycle();
}

}  // namespace

int main() {
  run_upload_result_tests();
  run_wsi_health_tests();
  run_texture_cache_tests();
  if (failures != 0) {
    std::fprintf(stderr, "%d renderer logic test failure(s)\n", failures);
    return 1;
  }
  std::printf("wds_renderer_tests OK\n");
  return 0;
}
