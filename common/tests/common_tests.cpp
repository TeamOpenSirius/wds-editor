#include "wds/common/common.hpp"
#include "wds/common/utf8_path.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

void test_timeline_advance_guards_and_apply() {
  using wds::common::Microseconds;
  using wds::common::PlaybackState;
  using wds::common::TimelineSnapshot;

  wds::common::Timeline tl;
  tl.seek_ms(1000);
  // Not playing → advance is a no-op.
  tl.advance(Microseconds{5000});
  CHECK(tl.position_ms() == 1000);
  tl.tick_ms(-5);
  CHECK(tl.position_ms() == 1000);

  tl.play();
  tl.advance(Microseconds{0});
  CHECK(tl.position_ms() == 1000);
  tl.advance(Microseconds{-1000});
  CHECK(tl.position_ms() == 1000);
  tl.advance(Microseconds{2500});
  CHECK(tl.position_ms() == 1002);

  TimelineSnapshot snap;
  snap.position = Microseconds{-100};
  snap.state = PlaybackState::Playing;
  tl.apply(snap);
  CHECK(tl.position_ms() == 0);
  CHECK(tl.playing());

  tl.toggle_playback();
  CHECK(!tl.playing());
  tl.reset();
  CHECK(tl.position_ms() == 0);
  CHECK(!tl.playing());
}

void test_microsecond_helpers() {
  using wds::common::ms_to_us;
  using wds::common::us_to_ms_floor;
  using wds::common::us_to_ms_round;

  CHECK(ms_to_us(1).count() == 1000);
  CHECK(us_to_ms_floor(wds::common::Microseconds{1999}) == 1);
  CHECK(us_to_ms_round(wds::common::Microseconds{1500}) == 2);
}

// Install-path regression: resource roots under non-ASCII directories must round-trip
// as UTF-8 and remain readable (Windows ACP / GBK must not leak into path strings).
void test_utf8_install_path_io() {
  namespace fs = std::filesystem;
  using wds::common::path_from_utf8;
  using wds::common::path_to_utf8;
  using wds::common::read_file_bytes;
  using wds::common::fopen_utf8;

  const std::string marker = "wds-utf8-path-ok";
  // Mix CJK + ASCII so GBK/ACP mishandling is obvious.
  const std::string rel_utf8 = u8"安装路径测试/資源/skin_ok.txt";

  std::error_code ec;
  const fs::path base = fs::temp_directory_path(ec) / "wds_common_utf8_path_test";
  CHECK(!ec);
  fs::remove_all(base, ec);
  const fs::path file = path_from_utf8(path_to_utf8(base) + "/" + rel_utf8);
  fs::create_directories(file.parent_path(), ec);
  CHECK(!ec);

  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    CHECK(static_cast<bool>(out));
    out << marker;
  }

  const std::string utf8 = path_to_utf8(file);
  CHECK(utf8.find(u8"安装路径测试") != std::string::npos);
  CHECK(utf8.find(u8"資源") != std::string::npos);

  // Round-trip must preserve the Unicode path (not ACP mojibake).
  const fs::path roundtrip = path_from_utf8(utf8);
  CHECK(fs::exists(roundtrip, ec) && !ec);
  CHECK(path_to_utf8(roundtrip.filename()) == "skin_ok.txt");

  std::vector<char> bytes;
  CHECK(read_file_bytes(utf8, bytes));
  CHECK(std::string(bytes.begin(), bytes.end()) == marker);

  FILE* fp = fopen_utf8(utf8, "rb");
  CHECK(fp != nullptr);
  if (fp != nullptr) {
    char buf[64] = {};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    CHECK(std::string(buf, n) == marker);
  }

  CHECK(wds::common::path_exists_utf8(utf8));
  CHECK(wds::common::is_regular_file_utf8(utf8));
  CHECK(wds::common::is_directory_utf8(path_to_utf8(file.parent_path())));

  // executable_dir should at least resolve to an existing directory.
  const fs::path exe = wds::common::executable_dir(nullptr);
  CHECK(!exe.empty());
  CHECK(wds::common::is_directory_utf8(path_to_utf8(exe)));

  fs::remove_all(base, ec);
}

}  // namespace

int main() {
  test_timeline_play_pause_seek();
  test_timeline_advance_guards_and_apply();
  test_microsecond_helpers();
  test_utf8_install_path_io();
  if (failures != 0) {
    std::fprintf(stderr, "%d common test failure(s)\n", failures);
    return 1;
  }
  std::printf("wds_common_tests OK\n");
  return 0;
}
