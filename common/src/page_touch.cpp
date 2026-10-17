#include "wds/common/page_touch.hpp"

namespace wds::common {

void touch_pages(void* p, std::size_t bytes) noexcept {
  if (p == nullptr || bytes == 0) {
    return;
  }
  auto* b = static_cast<volatile unsigned char*>(p);
  for (std::size_t i = 0; i < bytes; i += kPageTouchStride) {
    const unsigned char v = b[i];
    b[i] = v;
  }
  const unsigned char v = b[bytes - 1];
  b[bytes - 1] = v;
}

}  // namespace wds::common
