#pragma once

#include <cstddef>

namespace wds::common {

// Portable commit+touch. malloc / VirtualAlloc(MEM_COMMIT) still fault on
// first access; write one byte per 4 KiB so later first-use is not a minflt.
// 4 KiB is a divisor of 16 KiB (Apple) and 64 KiB (some Windows).
inline constexpr std::size_t kPageTouchStride = 4096;

void touch_pages(void* p, std::size_t bytes) noexcept;

}  // namespace wds::common
