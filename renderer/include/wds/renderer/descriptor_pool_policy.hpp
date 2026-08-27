#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace wds::renderer {

inline constexpr uint32_t kDescriptorPoolInitialCapacity = 256;
inline constexpr uint32_t kDescriptorPoolSecondCapacity = 512;
inline constexpr uint32_t kDescriptorPoolCappedCapacity = 1024;

inline constexpr uint32_t descriptor_pool_next_capacity(uint32_t previous_capacity) noexcept {
  if (previous_capacity == 0) {
    return kDescriptorPoolInitialCapacity;
  }
  if (previous_capacity < kDescriptorPoolSecondCapacity) {
    return kDescriptorPoolSecondCapacity;
  }
  return kDescriptorPoolCappedCapacity;
}

inline constexpr bool descriptor_alloc_try_next_block(VkResult result) noexcept {
  return result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL;
}

inline constexpr bool descriptor_block_skip_full(uint32_t live, uint32_t capacity) noexcept {
  return capacity == 0 || live >= capacity;
}

enum class DescriptorAllocStep : uint8_t {
  SkipFull = 0,
  TryAllocate,
  Grow,
};

inline constexpr DescriptorAllocStep descriptor_alloc_step_for_block(uint32_t live,
                                                                    uint32_t capacity,
                                                                    bool more_blocks) noexcept {
  if (descriptor_block_skip_full(live, capacity)) {
    return more_blocks ? DescriptorAllocStep::SkipFull : DescriptorAllocStep::Grow;
  }
  return DescriptorAllocStep::TryAllocate;
}

inline constexpr bool descriptor_pool_may_reset(uint32_t live_sets) noexcept {
  return live_sets == 0;
}

inline constexpr uint32_t descriptor_pool_live_after_alloc(uint32_t live, bool allocated) noexcept {
  return allocated ? live + 1 : live;
}

inline constexpr uint32_t descriptor_pool_live_after_free(uint32_t live) noexcept {
  return live > 0 ? live - 1 : 0;
}

struct DescriptorPoolBlockBook {
  uint32_t capacity = 0;
  uint32_t live = 0;
};

inline bool descriptor_chain_book_alloc(std::vector<DescriptorPoolBlockBook>& blocks,
                                        uint32_t& out_block_index) {
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (blocks[i].live < blocks[i].capacity) {
      blocks[i].live = descriptor_pool_live_after_alloc(blocks[i].live, true);
      out_block_index = static_cast<uint32_t>(i);
      return true;
    }
  }
  const uint32_t previous = blocks.empty() ? 0u : blocks.back().capacity;
  DescriptorPoolBlockBook created;
  created.capacity = descriptor_pool_next_capacity(previous);
  created.live = descriptor_pool_live_after_alloc(0, true);
  blocks.push_back(created);
  out_block_index = static_cast<uint32_t>(blocks.size() - 1);
  return true;
}

inline bool descriptor_chain_book_free(std::vector<DescriptorPoolBlockBook>& blocks,
                                       uint32_t block_index) {
  if (block_index >= blocks.size() || blocks[block_index].live == 0) {
    return false;
  }
  blocks[block_index].live = descriptor_pool_live_after_free(blocks[block_index].live);
  return true;
}

}  // namespace wds::renderer
