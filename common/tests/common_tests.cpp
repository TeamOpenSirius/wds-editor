#include "wds/common/common.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,       \
                   __LINE__);                                                   \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

void test_timeline_play_pause_seek() {
  wds::common::Timeline tl;
  CHECK(tl.position_ms() == 0);
  CHECK(!tl.playing());

  tl.seek_ms(1500);
  CHECK(tl.position_ms() == 1500);

  tl.play();
  tl.tick_ms(16);
  CHECK(tl.playing());
  CHECK(tl.position_ms() == 1516);

  tl.pause();
  tl.tick_ms(100);
  CHECK(!tl.playing());
  CHECK(tl.position_ms() == 1516);

  tl.seek_ms(-10);
  CHECK(tl.position_ms() == 0);
}

void test_microsecond_helpers() {
  using wds::common::ms_to_us;
  using wds::common::us_to_ms_floor;
  using wds::common::us_to_ms_round;

  CHECK(ms_to_us(1).count() == 1000);
  CHECK(us_to_ms_floor(wds::common::Microseconds{1999}) == 1);
  CHECK(us_to_ms_round(wds::common::Microseconds{1500}) == 2);
}

}  // namespace

int main() {
  test_timeline_play_pause_seek();
  test_microsecond_helpers();
  if (failures != 0) {
    std::fprintf(stderr, "%d common test failure(s)\n", failures);
    return 1;
  }
  std::printf("wds_common_tests OK\n");
  return 0;
}
