#include "wds/common/app_version.hpp"
#include "wds/common/common.hpp"
#include "wds/common/crash_input_journal.hpp"
#include "wds/common/utf8_path.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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
  CHECK(tl.position_ms() == -10);
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
  CHECK(tl.position().count() == -100);
  CHECK(tl.playing());

  tl.toggle_playback();
  CHECK(!tl.playing());
  tl.reset();
  CHECK(tl.position_ms() == 0);
  CHECK(!tl.playing());
}

void test_microsecond_helpers() {
  using wds::common::Microseconds;
  using wds::common::ms_to_us;
  using wds::common::us_to_ms_floor;
  using wds::common::us_to_ms_round;

  CHECK(ms_to_us(1).count() == 1000);
  CHECK(ms_to_us(-1).count() == -1000);
  CHECK(us_to_ms_floor(Microseconds{1999}) == 1);
  CHECK(us_to_ms_round(Microseconds{1500}) == 2);
  CHECK(us_to_ms_round(Microseconds{1499}) == 1);
  CHECK(us_to_ms_round(Microseconds{500}) == 1);
  CHECK(us_to_ms_round(Microseconds{499}) == 0);
  CHECK(us_to_ms_round(Microseconds{-1499}) == -1);
  CHECK(us_to_ms_round(Microseconds{-1500}) == -2);
  CHECK(us_to_ms_round(Microseconds{-500}) == -1);
  CHECK(us_to_ms_round(Microseconds{-499}) == 0);

  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  // Last representable products: C++ `/` truncates toward zero, so kMinMs is
  // INT64_MIN/1000 (not floor-toward-inf). A `>=`/`<=` saturate would fail these.
  constexpr int64_t kMaxMs = kMax / 1000;
  constexpr int64_t kMinMs = kMin / 1000;
  static_assert(ms_to_us(kMaxMs).count() == kMaxMs * 1000, "last-safe max ms");
  static_assert(ms_to_us(kMinMs).count() == kMinMs * 1000, "last-safe min ms");
  static_assert(ms_to_us(kMaxMs + 1).count() == kMax, "max adjacent saturates");
  static_assert(ms_to_us(kMinMs - 1).count() == kMin, "min adjacent saturates");
  static_assert(ms_to_us(kMax).count() == kMax, "INT64_MAX saturates");
  static_assert(ms_to_us(kMin).count() == kMin, "INT64_MIN saturates");

  CHECK(ms_to_us(kMaxMs).count() == kMaxMs * 1000);
  CHECK(ms_to_us(kMinMs).count() == kMinMs * 1000);
  CHECK(ms_to_us(kMaxMs + 1).count() == kMax);
  CHECK(ms_to_us(kMinMs - 1).count() == kMin);
  CHECK(ms_to_us(kMax).count() == kMax);
  CHECK(ms_to_us(kMin).count() == kMin);

  // Half-away-from-zero at INT64 extremes without ±500 overflow.
  CHECK(us_to_ms_round(Microseconds{kMax}) == kMax / 1000 + 1);
  CHECK(us_to_ms_round(Microseconds{kMin}) == kMin / 1000 - 1);
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

std::string dump_journal() {
  std::string out;
  wds::common::journal_write_text(
      [](void* ctx, const char* data, std::size_t n) {
        static_cast<std::string*>(ctx)->append(data, n);
      },
      &out);
  return out;
}

void test_crash_input_journal() {
  using namespace wds::common;
  journal_reset_for_test();
  CHECK(journal_count() == 0);
  CHECK(!journal_allow_sensitive());

  for (int i = 0; i < 1000; ++i) {
    journal_note_move(1.0f + static_cast<float>(i), 2.0f, 3);
  }
  CHECK(journal_count() == 0);
  const CrashInputSlot* pending = journal_pending_move_slot();
  CHECK(pending != nullptr);
  CHECK(pending->move_count == 1000);
  CHECK(pending->move_x == 1000.0f);
  CHECK(pending->drag_mode == 3);

  journal_begin_event(CrashInputKind::Scroll, 10.0f, 20.0f, 0.0f, -1.0f, 0, 0, 0, 0);
  journal_set_route("ChartEditPanel", CrashRouteVia::HitTest);
  CrashTraceSnap before{};
  before.mask = kCrashSnapTimeline;
  before.timeline_ms = 100;
  CrashTraceSnap after{};
  after.mask = kCrashSnapTimeline;
  after.timeline_ms = 250;
  journal_diff_snap(before, after);
  journal_end_event();
  CHECK(journal_count() == 1);
  const CrashInputSlot* first = journal_slot_from_oldest(0);
  CHECK(first != nullptr);
  CHECK(first->kind == CrashInputKind::Scroll);
  CHECK(first->route_target != nullptr);
  CHECK(std::strcmp(first->route_target, "ChartEditPanel") == 0);
  CHECK(first->effect_count == 1);
  CHECK(first->effects[0].id == CrashEffectId::TimelineMs);
  CHECK(std::strstr(first->text, "---") == nullptr);
  CHECK(std::strstr(first->text, "kind:") == nullptr);

  journal_note_move(11.0f, 21.0f, 1);
  journal_note_move(12.0f, 22.0f, 2);
  CHECK(journal_count() == 1);
  CHECK(first->move_count == 2);
  CHECK(first->move_x == 12.0f);
  CHECK(first->drag_mode == 2);

  const std::string dumped = dump_journal();
  CHECK(dumped.find("--- input journal ---") != std::string::npos);
  CHECK(dumped.find("kind=Scroll") != std::string::npos);
  CHECK(dumped.find("timeline_ms") != std::string::npos);
  CHECK(dumped.find("moves_since") != std::string::npos);

  journal_reset_for_test();
  for (int i = 0; i < 101; ++i) {
    journal_begin_event(CrashInputKind::Click, static_cast<float>(i), 0.0f, 0, 0, 0, 0, 1, 0);
    journal_end_event();
  }
  CHECK(journal_count() == 100);
  const CrashInputSlot* oldest = journal_slot_from_oldest(0);
  const CrashInputSlot* newest = journal_slot_from_oldest(99);
  CHECK(oldest != nullptr && newest != nullptr);
  CHECK(oldest->x == 1.0f);
  CHECK(newest->x == 100.0f);

  journal_reset_for_test();
  journal_set_allow_sensitive(false);
  journal_begin_event(CrashInputKind::TextInput, 0, 0, 0, 0, 0, 0, 0, 0);
  journal_set_text("secret-path", 11);
  journal_end_event();
  const CrashInputSlot* hidden = journal_slot_from_oldest(0);
  CHECK(hidden != nullptr);
  CHECK(hidden->text_len == 11);
  CHECK(hidden->text[0] == '\0');
  const std::string redacted = dump_journal();
  CHECK(redacted.find("secret-path") == std::string::npos);
  CHECK(redacted.find("text_len=11") != std::string::npos);

  char path_buf[64] = {};
  journal_copy_path(path_buf, sizeof(path_buf), "/Users/me/charts/song.wds");
  CHECK(std::strcmp(path_buf, "song.wds") == 0);

  journal_reset_for_test();
  journal_set_allow_sensitive(true);
  journal_begin_event(CrashInputKind::TextInput, 0, 0, 0, 0, 0, 0, 0, 0);
  journal_set_text("secret-path", 11);
  journal_end_event();
  const CrashInputSlot* shown = journal_slot_from_oldest(0);
  CHECK(shown != nullptr);
  CHECK(std::strcmp(shown->text, "secret-path") == 0);
  const std::string sensitive = dump_journal();
  CHECK(sensitive.find("secret-path") != std::string::npos);
  journal_copy_path(path_buf, sizeof(path_buf), "/Users/me/charts/song.wds");
  CHECK(std::strcmp(path_buf, "/Users/me/charts/song.wds") == 0);
}

void test_app_version_parse_and_compare() {
  using wds::common::compare_app_version;
  using wds::common::format_app_version;
  using wds::common::parse_app_version;

  const auto newer = parse_app_version("1.0.1-beta.0");
  const auto older_stable = parse_app_version("1.0.0-stable");
  const auto high_beta = parse_app_version("1.0.0-beta.99");
  const auto beta10 = parse_app_version("1.0.0-beta.10");
  const auto beta2 = parse_app_version("1.0.0-beta.2");
  CHECK(newer.has_value());
  CHECK(older_stable.has_value());
  CHECK(high_beta.has_value());
  CHECK(beta10.has_value());
  CHECK(beta2.has_value());
  CHECK(compare_app_version(*newer, *older_stable) > 0);
  CHECK(compare_app_version(*older_stable, *high_beta) > 0);
  CHECK(compare_app_version(*beta10, *beta2) > 0);
  CHECK(compare_app_version(*older_stable, *older_stable) == 0);
  CHECK(format_app_version(*newer) == "1.0.1-beta.0");
  CHECK(format_app_version(*older_stable) == "1.0.0-stable");

  CHECK(!parse_app_version("1.0.0-beta1"));
  CHECK(!parse_app_version("1.0.0"));
  CHECK(!parse_app_version("1.0.0-stable.1"));
  CHECK(!parse_app_version("1.0.0-beta"));
  CHECK(!parse_app_version(""));
}

}  // namespace

int main() {
  test_timeline_play_pause_seek();
  test_timeline_advance_guards_and_apply();
  test_microsecond_helpers();
  test_utf8_install_path_io();
  test_crash_input_journal();
  test_app_version_parse_and_compare();
  if (failures != 0) {
    std::fprintf(stderr, "%d common test failure(s)\n", failures);
    return 1;
  }
  std::printf("wds_common_tests OK\n");
  return 0;
}
