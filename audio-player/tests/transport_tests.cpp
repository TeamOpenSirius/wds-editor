#include "wds/audio/transport.hpp"

#include <cassert>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

}  // namespace

int main() {
  wds::audio::Transport transport;
  // No BASS init — exercise intent queue + wall-clock poll path only when initialize is skipped.
  // Fake transport: manually set committed state via poll without audio device.
  // When uninitialized, poll still returns a consistent snapshot for paused state.
  transport.request_seek_ms(1500);
  const auto snap = transport.poll(0);
  expect(snap.position_ms() == 0, "uninitialized transport ignores pending seek");
  expect(snap.state == wds::common::PlaybackState::Paused, "default paused");

  if (transport.initialize("", "")) {
    const uint64_t gen_before_play = transport.audio().position_generation();
    transport.request_seek_ms(250);
    transport.request_play();
    auto playing = transport.poll(16000);
    expect(playing.state == wds::common::PlaybackState::Playing, "play intent");
    expect(playing.position_ms() == 250, "seek while paused then play");
    expect(transport.audio().position_generation() > gen_before_play,
           "no-music play bumps position_generation for SFX resync");

    auto advanced = transport.poll(100000);
    expect(advanced.position_ms() == 350, "100ms wall-clock step without music");
    expect(advanced.state == wds::common::PlaybackState::Playing, "still playing");

    // Sub-millisecond frames (MAILBOX / high refresh): old ms truncation would stall.
    // 33 × 300µs = 9900µs → +9ms on the timeline.
    for (int i = 0; i < 33; ++i) {
      transport.poll(300);
    }
    expect(transport.committed_ms() == 359, "sub-ms frames accumulate (33×300µs)");

    transport.set_playback_rate(2.0f);
    transport.poll(50000);  // 50ms wall → 100ms timeline at 2×
    expect(transport.committed_ms() == 459, "playback_rate scales wall-clock µs");

    transport.request_pause();
    const auto paused = transport.poll(0);
    expect(paused.state == wds::common::PlaybackState::Paused, "pause intent");
    transport.shutdown();
  } else {
    std::fprintf(stderr, "skip: BASS device unavailable in test environment\n");
  }

  return failures == 0 ? 0 : 1;
}
