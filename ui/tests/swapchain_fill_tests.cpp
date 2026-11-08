#include "wds/ui/preview_present_policy.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const char* expr, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __FILE__, __LINE__)

using wds::ui::note_swapchain_frame;
using wds::ui::swapchain_fill_covered_count;
using wds::ui::swapchain_fill_invalidate;
using wds::ui::swapchain_fill_pending;
using wds::ui::swapchain_fill_wait_us;
using wds::ui::SwapchainFillOutcome;
using wds::ui::SwapchainFillState;
using wds::ui::SwapchainFrameFacts;

SwapchainFrameFacts frame(bool fill, std::uint32_t index, std::uint64_t serial = 1,
                          std::uint32_t count = 3, bool presented = true,
                          std::uint64_t present_serial = 0) {
  SwapchainFrameFacts f;
  f.requested_fill = fill;
  f.fill_frame = fill;
  f.presented = presented;
  f.present_serial = present_serial != 0 ? present_serial : serial;
  f.image_index = index;
  f.swapchain_serial = serial;
  f.image_count = count;
  return f;
}

void test_covers_all_images() {
  SwapchainFillState s;
  CHECK(!swapchain_fill_pending(s));
  CHECK(note_swapchain_frame(s, frame(false, 0)) == SwapchainFillOutcome::Pending);
  CHECK(swapchain_fill_pending(s));
  CHECK_EQ(swapchain_fill_covered_count(s), 1u);
  CHECK(note_swapchain_frame(s, frame(true, 1)) == SwapchainFillOutcome::Pending);
  CHECK(note_swapchain_frame(s, frame(true, 2)) == SwapchainFillOutcome::Covered);
  CHECK(!swapchain_fill_pending(s));
  CHECK_EQ(swapchain_fill_covered_count(s), 3u);
  CHECK_EQ(s.fill_presents, 2u);
  // A stray fill frame after completion changes nothing.
  CHECK(note_swapchain_frame(s, frame(true, 0)) == SwapchainFillOutcome::Idle);
  CHECK(!swapchain_fill_pending(s));
}

void test_repeated_index_does_not_count() {
  // MAILBOX may hand back the same images over and over.
  SwapchainFillState s;
  note_swapchain_frame(s, frame(false, 0));
  CHECK(note_swapchain_frame(s, frame(true, 1)) == SwapchainFillOutcome::Pending);
  CHECK(note_swapchain_frame(s, frame(true, 0)) == SwapchainFillOutcome::Pending);
  CHECK(note_swapchain_frame(s, frame(true, 1)) == SwapchainFillOutcome::Pending);
  CHECK_EQ(swapchain_fill_covered_count(s), 2u);
  CHECK(note_swapchain_frame(s, frame(true, 2)) == SwapchainFillOutcome::Covered);
  CHECK_EQ(s.fill_presents, 4u);
}

void test_cap_stops_unreachable_image() {
  SwapchainFillState s;
  note_swapchain_frame(s, frame(false, 0));
  // Image 2 is never handed back: the cap is 2 x image_count fill presents.
  for (int i = 0; i < 5; ++i) {
    CHECK(note_swapchain_frame(s, frame(true, static_cast<std::uint32_t>(i % 2))) ==
          SwapchainFillOutcome::Pending);
  }
  CHECK(note_swapchain_frame(s, frame(true, 1)) == SwapchainFillOutcome::CapReached);
  CHECK(!swapchain_fill_pending(s));
  CHECK_EQ(swapchain_fill_covered_count(s), 2u);
  CHECK_EQ(s.fill_presents, 6u);
}

void test_content_change_resets() {
  SwapchainFillState s;
  note_swapchain_frame(s, frame(false, 0));
  note_swapchain_frame(s, frame(true, 1));
  CHECK_EQ(swapchain_fill_covered_count(s), 2u);
  // A new picture invalidates every image except the one it went into.
  CHECK(note_swapchain_frame(s, frame(false, 2)) == SwapchainFillOutcome::Pending);
  CHECK_EQ(swapchain_fill_covered_count(s), 1u);
  CHECK_EQ(s.fill_presents, 0u);
  note_swapchain_frame(s, frame(true, 0));
  CHECK(note_swapchain_frame(s, frame(true, 1)) == SwapchainFillOutcome::Covered);
  // After completion a content frame starts a new fill.
  CHECK(note_swapchain_frame(s, frame(false, 1)) == SwapchainFillOutcome::Pending);
  CHECK(swapchain_fill_pending(s));
  // Playing frames skip the bookkeeping but leave a full fill owed.
  swapchain_fill_invalidate(s);
  CHECK(swapchain_fill_pending(s));
  CHECK_EQ(swapchain_fill_covered_count(s), 0u);
}

void test_rebuild_invalidates() {
  SwapchainFillState s;
  note_swapchain_frame(s, frame(false, 0, 1, 3));
  note_swapchain_frame(s, frame(true, 1, 1, 3));
  note_swapchain_frame(s, frame(true, 2, 1, 3));
  CHECK(!swapchain_fill_pending(s));
  // A fill frame that lands on a rebuilt swapchain (new serial, 5 images):
  // the new images are undefined, so coverage restarts from this present.
  CHECK(note_swapchain_frame(s, frame(true, 3, 2, 5)) == SwapchainFillOutcome::Pending);
  CHECK(swapchain_fill_pending(s));
  CHECK_EQ(s.image_count, 5u);
  CHECK_EQ(swapchain_fill_covered_count(s), 1u);
  // A present into the retired swapchain does not cover the new one.
  SwapchainFillState r;
  note_swapchain_frame(r, frame(false, 0, 7, 3));
  CHECK(note_swapchain_frame(r, frame(true, 1, 8, 3, true, 7)) == SwapchainFillOutcome::Pending);
  CHECK_EQ(swapchain_fill_covered_count(r), 0u);
  // Same serial but a different image count also restarts coverage.
  SwapchainFillState c;
  note_swapchain_frame(c, frame(false, 0, 4, 3));
  note_swapchain_frame(c, frame(true, 1, 4, 3));
  CHECK(note_swapchain_frame(c, frame(true, 2, 4, 4)) == SwapchainFillOutcome::Pending);
  CHECK_EQ(swapchain_fill_covered_count(c), 1u);
}

void test_rebuild_every_present_still_caps() {
  SwapchainFillState s;
  note_swapchain_frame(s, frame(false, 0, 1, 3));
  SwapchainFillOutcome last = SwapchainFillOutcome::Pending;
  int frames = 0;
  for (std::uint64_t serial = 2; serial < 40 && last == SwapchainFillOutcome::Pending; ++serial) {
    last = note_swapchain_frame(s, frame(true, 0, serial, 3));
    ++frames;
  }
  CHECK(last == SwapchainFillOutcome::CapReached);
  CHECK_EQ(frames, 6);
}

// The host asked for a fill, but the picture had changed by render time.
SwapchainFrameFacts changed_fill(std::uint32_t index, std::uint64_t serial = 1,
                                 std::uint32_t count = 3) {
  SwapchainFrameFacts f = frame(true, index, serial, count);
  f.fill_frame = false;
  return f;
}

void test_dirty_every_fill_frame_still_caps() {
  // Something marks the picture dirty before every fill render (a per-frame
  // dirty source): coverage keeps restarting, but the loop must still stop.
  SwapchainFillState s;
  note_swapchain_frame(s, frame(false, 0));
  SwapchainFillOutcome last = SwapchainFillOutcome::Pending;
  int frames = 0;
  while (last == SwapchainFillOutcome::Pending && frames < 100) {
    last = note_swapchain_frame(s, changed_fill(static_cast<std::uint32_t>(frames % 3)));
    ++frames;
    CHECK(swapchain_fill_covered_count(s) <= 1u);
  }
  CHECK(last == SwapchainFillOutcome::CapReached);
  CHECK_EQ(frames, 6);
  CHECK(!swapchain_fill_pending(s));
  // Same with a swapchain rebuilt by every one of those frames.
  SwapchainFillState r;
  note_swapchain_frame(r, frame(false, 0, 1, 3));
  frames = 0;
  last = SwapchainFillOutcome::Pending;
  while (last == SwapchainFillOutcome::Pending && frames < 100) {
    last = note_swapchain_frame(r, changed_fill(0, 2 + static_cast<std::uint64_t>(frames), 3));
    ++frames;
  }
  CHECK(last == SwapchainFillOutcome::CapReached);
  CHECK_EQ(frames, 6);
}

void test_changed_fill_resets_coverage_keeps_count() {
  SwapchainFillState s;
  note_swapchain_frame(s, frame(false, 0));
  note_swapchain_frame(s, frame(true, 1));
  CHECK_EQ(swapchain_fill_covered_count(s), 2u);
  CHECK(note_swapchain_frame(s, changed_fill(2)) == SwapchainFillOutcome::Pending);
  CHECK_EQ(swapchain_fill_covered_count(s), 1u);
  CHECK_EQ(s.fill_presents, 2u);
  note_swapchain_frame(s, frame(true, 0));
  CHECK(note_swapchain_frame(s, frame(true, 1)) == SwapchainFillOutcome::Covered);
  CHECK_EQ(s.fill_presents, 4u);
  // After completion a stray fill request, even one whose picture changed,
  // does not reopen the fill; only the present gate can.
  CHECK(note_swapchain_frame(s, changed_fill(0)) == SwapchainFillOutcome::Idle);
  CHECK(!swapchain_fill_pending(s));
  CHECK_EQ(s.fill_presents, 4u);
  // Only a frame the present gate itself let through restarts the count.
  CHECK(note_swapchain_frame(s, frame(false, 2)) == SwapchainFillOutcome::Pending);
  CHECK_EQ(s.fill_presents, 0u);
}

void test_image_count_limits() {
  // More images than the coverage bitmap can hold: no fill at all.
  SwapchainFillState big;
  CHECK(note_swapchain_frame(big, frame(false, 0, 1, 65)) == SwapchainFillOutcome::Idle);
  CHECK(!swapchain_fill_pending(big));
  CHECK(note_swapchain_frame(big, frame(false, 70, 1, 1000)) == SwapchainFillOutcome::Idle);
  CHECK_EQ(wds::ui::swapchain_fill_cap(1000), 128u);
  CHECK_EQ(wds::ui::swapchain_fill_cap(64), 128u);
  CHECK_EQ(wds::ui::swapchain_fill_cap(3), 6u);
  // Exactly 64 images still works, including the top bit.
  SwapchainFillState edge;
  note_swapchain_frame(edge, frame(false, 63, 1, 64));
  CHECK_EQ(swapchain_fill_covered_count(edge), 1u);
  SwapchainFillOutcome last = SwapchainFillOutcome::Pending;
  for (std::uint32_t i = 0; i < 63 && last == SwapchainFillOutcome::Pending; ++i) {
    last = note_swapchain_frame(edge, frame(true, i, 1, 64));
  }
  CHECK(last == SwapchainFillOutcome::Covered);
  CHECK_EQ(swapchain_fill_covered_count(edge), 64u);
}

void test_fill_wait() {
  CHECK_EQ(swapchain_fill_wait_us(0, 33333), 33333);
  CHECK_EQ(swapchain_fill_wait_us(20000, 33333), 13333);
  CHECK_EQ(swapchain_fill_wait_us(33333, 33333), 0);
  CHECK_EQ(swapchain_fill_wait_us(500000, 33333), 0);
  CHECK_EQ(swapchain_fill_wait_us(0, 0), 0);
}

void test_missing_present_and_no_swapchain() {
  SwapchainFillState s;
  note_swapchain_frame(s, frame(false, 0, 1, 2, false));
  CHECK_EQ(swapchain_fill_covered_count(s), 0u);
  CHECK(note_swapchain_frame(s, frame(true, 0, 1, 2, false)) == SwapchainFillOutcome::Pending);
  CHECK(note_swapchain_frame(s, frame(true, 0)) == SwapchainFillOutcome::Pending);
  CHECK_EQ(s.image_count, 3u);
  // No swapchain at all (occluded / lost): nothing to fill.
  SwapchainFillState none;
  CHECK(note_swapchain_frame(none, frame(false, 0, 0, 0, false)) == SwapchainFillOutcome::Idle);
  CHECK(!swapchain_fill_pending(none));
  // A single-image swapchain is covered by the content frame itself.
  SwapchainFillState one;
  CHECK(note_swapchain_frame(one, frame(false, 0, 1, 1)) == SwapchainFillOutcome::Idle);
  CHECK(!swapchain_fill_pending(one));
  // Out-of-range index never counts.
  SwapchainFillState bad;
  note_swapchain_frame(bad, frame(false, 9, 1, 2));
  CHECK_EQ(swapchain_fill_covered_count(bad), 0u);
}

}  // namespace

int main() {
  test_covers_all_images();
  test_repeated_index_does_not_count();
  test_cap_stops_unreachable_image();
  test_content_change_resets();
  test_rebuild_invalidates();
  test_rebuild_every_present_still_caps();
  test_missing_present_and_no_swapchain();
  test_dirty_every_fill_frame_still_caps();
  test_changed_fill_resets_coverage_keeps_count();
  test_image_count_limits();
  test_fill_wait();
  if (g_failures != 0) {
    std::fprintf(stderr, "swapchain_fill_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("swapchain_fill_tests: ok\n");
  return 0;
}
