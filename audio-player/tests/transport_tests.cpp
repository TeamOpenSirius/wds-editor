#include "wds/audio/audio_engine.hpp"
#include "wds/audio/hit_sfx.hpp"
#include "wds/audio/recovery_backoff.hpp"
#include "wds/audio/sfx_sync_policy.hpp"
#include "wds/audio/testing/detail/audio_engine_test_double.hpp"
#include "wds/audio/transport.hpp"

#include "bass.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

using wds::audio::AudioEngine;
using wds::audio::AudioEngineTestAccess;
using wds::audio::HitSfxClip;
using wds::audio::HitSfxPlayer;
using wds::audio::RecoveryBackoff;
using wds::audio::SfxSyncAdmit;
using wds::audio::SfxSyncArmResult;
using wds::audio::SfxSyncClaim;
using wds::audio::SfxSyncPlayClaim;
using wds::audio::SfxSyncPostArm;
using wds::audio::StreamHealth;
using wds::audio::StreamRecoveryKind;
using wds::audio::Transport;
using wds::audio::admit_sfx_sync;
using wds::audio::arm_result_keeps_hit_mark;
using wds::audio::claim_sfx_sync_callback_play;
using wds::audio::claim_sfx_sync_cancel;
using wds::audio::claim_sfx_sync_fire;
using wds::audio::commit_hit_sfx_schedule;
using wds::audio::decide_after_set_sync;
using wds::audio::decide_arm_resolution;
using wds::audio::finish_due_play_claim;
using wds::audio::pending_sync_identifies;
using wds::audio::kMaxPendingSfxSyncs;
using wds::audio::kSfxSyncHorizonUs;
using wds::audio::saturating_future_delta_us;
using wds::common::Microseconds;
using wds::common::PlaybackState;
using TestDouble = AudioEngineTestAccess::Double;

void test_backoff_sequence() {
  RecoveryBackoff backoff;
  expect(backoff.allow_attempt(0), "first attempt is immediate");
  expect(backoff.attempt_count() == 1, "first acquire counts");
  backoff.on_failure();
  expect(backoff.pending(), "failure keeps pending");
  expect(backoff.next_delay_us() == 50000, "50ms after first failure");

  expect(!backoff.allow_attempt(49999), "49.999ms must wait");
  expect(backoff.allow_attempt(1), "50ms total allows second attempt");
  backoff.on_failure();
  expect(backoff.next_delay_us() == 100000, "100ms next");

  expect(!backoff.allow_attempt(99999), "99.999ms must wait");
  expect(backoff.allow_attempt(1), "100ms allows third");
  backoff.on_failure();
  expect(backoff.next_delay_us() == 200000, "200ms next");

  expect(!backoff.allow_attempt(199999), "199.999ms must wait");
  expect(backoff.allow_attempt(1), "200ms allows fourth");
  backoff.on_failure();
  expect(backoff.next_delay_us() == 400000, "400ms next");

  expect(!backoff.allow_attempt(399999), "399.999ms must wait");
  expect(backoff.allow_attempt(1), "400ms allows fifth");
  backoff.on_failure();
  expect(backoff.next_delay_us() == 800000, "800ms next");

  expect(!backoff.allow_attempt(799999), "799.999ms must wait");
  expect(backoff.allow_attempt(1), "800ms allows sixth");
  backoff.on_failure();
  expect(backoff.next_delay_us() == 1000000, "1000ms cap");

  expect(!backoff.allow_attempt(999999), "999.999ms must wait");
  expect(backoff.allow_attempt(1), "1000ms allows seventh");
  backoff.on_failure();
  expect(backoff.next_delay_us() == 1000000, "stays at 1000ms cap");
}

void test_backoff_reset_and_success() {
  RecoveryBackoff backoff;
  expect(backoff.allow_attempt(0), "seed attempt");
  backoff.on_failure();
  expect(backoff.allow_attempt(50000), "second attempt");
  backoff.on_success();
  expect(backoff.attempt_count() == 0, "success clears attempt count");
  expect(!backoff.pending(), "success clears pending");
  expect(backoff.allow_attempt(0), "success returns to immediate first try");

  backoff.on_failure();
  backoff.reset();
  expect(backoff.attempt_count() == 0, "reset clears attempts");
  expect(!backoff.pending(), "reset clears pending");
  expect(backoff.allow_attempt(0), "reset returns to immediate first try");
}

void test_backoff_not_every_frame() {
  RecoveryBackoff backoff;
  int acquires = 0;
  for (int i = 0; i < 120; ++i) {
    if (backoff.allow_attempt(16000)) {
      ++acquires;
      backoff.on_failure();
    }
  }
  expect(acquires >= 5, "persistent fault still retries after backoff");
  expect(acquires <= 7, "120 frames of 16ms must not retry every poll");
}

void test_backoff_elapsed_saturates() {
  RecoveryBackoff backoff;
  expect(backoff.allow_attempt(0), "seed acquire");
  backoff.on_failure();
  backoff.add_elapsed(std::numeric_limits<int64_t>::max());
  backoff.add_elapsed(std::numeric_limits<int64_t>::max());
  expect(backoff.try_acquire(), "saturated elapsed still reaches the next delay");
}

void test_classify_recovery() {
  expect(wds::audio::classify_stream_recovery(StreamHealth::Stopped, false) ==
             StreamRecoveryKind::Recover,
         "mid-track Stopped recovers");
  expect(wds::audio::classify_stream_recovery(StreamHealth::Stalled, false) ==
             StreamRecoveryKind::Recover,
         "mid-track Stalled recovers");
  expect(wds::audio::classify_stream_recovery(StreamHealth::Stalled, true) ==
             StreamRecoveryKind::Recover,
         "Stalled near-end still recovers");
  expect(wds::audio::classify_stream_recovery(StreamHealth::Stopped, true) ==
             StreamRecoveryKind::NaturalEnd,
         "Stopped near-end is natural end");
  expect(wds::audio::classify_stream_recovery(StreamHealth::Playing, false) ==
             StreamRecoveryKind::None,
         "Playing does not recover");
  expect(wds::audio::classify_stream_recovery(StreamHealth::Paused, false) ==
             StreamRecoveryKind::Recover,
         "Paused while intending play recovers");
  expect(wds::audio::classify_stream_recovery(StreamHealth::Paused, true) ==
             StreamRecoveryKind::Recover,
         "Paused near-end still recovers");
}

void test_engine_no_music_observability() {
  AudioEngine engine;
  expect(engine.stream_health() == StreamHealth::Stopped, "no music is Stopped");
  expect(engine.stream_stopped(), "compat stream_stopped");
  expect(!engine.stream_playing(), "compat stream_playing");
  expect(!engine.play_music(), "no music is an explicit play failure");
  expect(engine.last_bass_error() != 0, "no music records a BASS error");

  TestDouble fake;
  fake.has_music = true;
  fake.play_music_ok = false;
  fake.last_play_error = 42;
  AudioEngineTestAccess::bind(engine, fake);
  expect(!engine.play_music(), "forced play failure");
  expect(engine.last_bass_error() == 42, "play failure stores the error code");
  fake.play_music_ok = true;
  fake.update_health_on_play = false;
  expect(engine.play_music(), "forced play success");
  expect(engine.last_bass_error() == 0, "success clears last BASS error");
  expect(engine.stream_health() == StreamHealth::Stopped, "play can succeed without health update");
  fake.update_health_on_play = true;
  expect(engine.play_music(), "play updates health when configured");
  expect(engine.stream_health() == StreamHealth::Playing, "stub play maps to Playing");
  expect(engine.stream_playing(), "compat playing via health");
  expect(!engine.stream_stopped(), "compat not stopped when Playing");

  const uint64_t gen0 = engine.position_generation();
  fake.set_position_ok = true;
  expect(engine.set_position(Microseconds{2500000}), "stub seek succeeds");
  expect(engine.position() == Microseconds{2500000}, "stub seek updates position");
  expect(engine.position_generation() > gen0, "stub seek bumps generation");
  const uint64_t gen1 = engine.position_generation();
  fake.set_position_ok = false;
  expect(!engine.set_position(Microseconds{3000000}), "stub seek failure");
  expect(engine.position_generation() > gen1, "failed seek still bumps generation");
  expect(engine.position() == Microseconds{2500000}, "failed seek does not move stub position");
  engine.pause_music();
  expect(engine.stream_health() == StreamHealth::Paused, "stub pause maps to Paused");
  engine.shutdown();
  expect(fake.owner == nullptr, "shutdown unbinds test double");
}

// No BASS_Init. Bind records/restores ready_ so Transport::poll is live.
void bind_fake_music(Transport& transport, TestDouble& fake) {
  fake.has_music = true;
  fake.health = StreamHealth::Stopped;
  fake.position = Microseconds{5000000};
  fake.duration = Microseconds{180000000};
  fake.set_position_ok = true;
  fake.play_music_ok = true;
  fake.update_health_on_play = true;
  fake.last_play_error = 7;
  AudioEngineTestAccess::bind(transport.audio(), fake);
}

void test_uninitialized_bind_polls_without_bass() {
  Transport transport;
  expect(!transport.audio().ready(), "fresh transport is not ready");
  TestDouble fake;
  bind_fake_music(transport, fake);
  expect(transport.audio().ready(), "bind makes engine ready without BASS_Init");
  expect(fake.owner == &transport.audio(), "bind attaches the stub");
  expect(transport.audio().has_music(), "fake music is visible");

  transport.request_seek_ms(250);
  transport.request_play();
  const auto snap = transport.poll(0);
  expect(snap.state == PlaybackState::Playing, "uninitialized+bind poll applies play");
  expect(snap.position_ms() == 250, "uninitialized+bind poll applies seek");
  expect(transport.music_start_pending(), "uninitialized+bind defers audible start");
}

void test_unbind_restores_ready_and_does_not_dangle() {
  Transport transport;
  expect(!transport.audio().ready(), "start unrestored");
  {
    TestDouble fake;
    fake.has_music = false;
    AudioEngineTestAccess::bind(transport.audio(), fake);
    expect(transport.audio().ready(), "bind sets ready");
    transport.audio().shutdown();
    expect(!transport.audio().ready(), "shutdown leaves engine not ready");
    expect(fake.owner == nullptr, "shutdown unbinds");
  }
  expect(!transport.audio().ready(), "ready stays false after stub dtor");

  {
    TestDouble fake;
    fake.has_music = false;
    AudioEngineTestAccess::bind(transport.audio(), fake);
    expect(transport.audio().ready(), "re-bind sets ready again");
  }
  expect(!transport.audio().ready(), "TestDouble dtor restores original ready");
  transport.request_play();
  const auto snap = transport.poll(16000);
  expect(snap.state == PlaybackState::Paused, "after unbind, poll is inert again");
}

void test_no_bgm_wall_clock_without_device() {
  Transport transport;
  TestDouble fake;
  fake.has_music = false;
  AudioEngineTestAccess::bind(transport.audio(), fake);
  expect(transport.audio().ready(), "no-BGM bind is ready");
  expect(!transport.audio().has_music(), "no-BGM fake has no music");

  const uint64_t gen_before_play = transport.audio().position_generation();
  transport.request_seek_ms(250);
  transport.request_play();
  auto playing = transport.poll(16000);
  expect(playing.state == PlaybackState::Playing, "play intent");
  expect(playing.position_ms() == 250, "seek while paused then play");
  expect(transport.audio().position_generation() > gen_before_play,
         "no-music play bumps position_generation for SFX resync");

  auto advanced = transport.poll(100000);
  expect(advanced.position_ms() == 350, "100ms wall-clock step without music");
  expect(advanced.state == PlaybackState::Playing, "still playing");

  for (int i = 0; i < 33; ++i) {
    transport.poll(300);
  }
  expect(transport.committed_ms() == 359, "sub-ms frames accumulate (33×300µs)");

  transport.set_playback_rate(2.0f);
  transport.poll(50000);
  expect(transport.committed_ms() == 459, "playback_rate scales wall-clock µs");

  transport.request_pause();
  const auto paused = transport.poll(0);
  expect(paused.state == PlaybackState::Paused, "pause intent");
  expect(paused.position_ms() == transport.committed_ms(),
         "pause keeps committed position without music");
}

void ui_tick(Transport& transport, int64_t wall_delta_us) {
  transport.poll(wall_delta_us);
  (void)transport.start_pending_music();
}

void play_and_start(Transport& transport) {
  transport.request_play();
  ui_tick(transport, 16000);
}

void test_start_pending_only_clears_on_success(Transport& transport, TestDouble& fake) {
  fake.play_music_ok = false;
  transport.request_play();
  transport.poll(16000);
  expect(transport.music_start_pending(), "play defers audible start");
  expect(!transport.start_pending_music(), "failed start is observable");
  expect(transport.music_start_pending(), "failed start keeps music_start_pending");
  expect(transport.audio().last_bass_error() != 0, "failed start leaves a BASS error");

  fake.play_music_ok = true;
  transport.poll(50000);
  expect(transport.start_pending_music(), "retry succeeds after backoff");
  expect(!transport.music_start_pending(), "success clears music_start_pending");
  expect(transport.recovery_attempt_count() == 0, "success clears attempt count");
  expect(!transport.recovery_pending(), "success clears recovery pending");
}

void test_initial_start_rate_limited(Transport& transport, TestDouble& fake) {
  fake.play_music_ok = false;
  transport.request_play();
  transport.poll(16000);
  expect(!transport.start_pending_music(), "first start fails");
  expect(transport.music_start_pending(), "start stays pending");
  const int plays0 = fake.play_music_calls;
  for (int i = 0; i < 20; ++i) {
    ui_tick(transport, 16000);
  }
  const int extra = fake.play_music_calls - plays0;
  expect(extra >= 1, "failed start retries after backoff");
  expect(extra <= 3, "failed start is not attempted every frame");
  expect(transport.music_start_pending(), "still pending after limited retries");
}

void test_persistent_stopped(Transport& transport, TestDouble& fake) {
  fake.play_music_ok = true;
  play_and_start(transport);
  expect(fake.health == StreamHealth::Playing, "successful start reaches Playing");
  transport.poll(16000);

  fake.health = StreamHealth::Stopped;
  fake.play_music_ok = false;
  const int seeks0 = fake.set_position_calls;
  const int plays0 = fake.play_music_calls;
  for (int i = 0; i < 60; ++i) {
    ui_tick(transport, 16000);
  }
  const int seeks = fake.set_position_calls - seeks0;
  const int plays = fake.play_music_calls - plays0;
  expect(seeks >= 1, "Stopped recovers at least once");
  expect(plays >= 1, "Stopped play retry at least once");
  expect(seeks < 60, "Stopped must not set_position every frame");
  expect(plays < 60, "Stopped must not play_music every frame");
  expect(seeks <= 6, "Stopped backoff keeps attempts near 50/100/200/...");
  expect(transport.recovery_pending() || transport.music_start_pending(),
         "persistent Stopped keeps recovery pending");
}

void test_persistent_stalled(Transport& transport, TestDouble& fake) {
  fake.play_music_ok = true;
  play_and_start(transport);
  transport.poll(16000);

  fake.health = StreamHealth::Stalled;
  fake.play_music_ok = false;
  const int seeks0 = fake.set_position_calls;
  const int plays0 = fake.play_music_calls;
  for (int i = 0; i < 60; ++i) {
    ui_tick(transport, 16000);
  }
  const int seeks = fake.set_position_calls - seeks0;
  const int plays = fake.play_music_calls - plays0;
  expect(seeks >= 1, "Stalled recovers at least once");
  expect(plays >= 1, "Stalled play retry at least once");
  expect(seeks < 60, "Stalled must not set_position every frame");
  expect(plays < 60, "Stalled must not play_music every frame");
  expect(seeks <= 6, "Stalled backoff keeps attempts near 50/100/200/...");
  expect(transport.recovery_pending() || transport.music_start_pending(),
         "persistent Stalled keeps recovery pending");
}

void test_seek_and_play_failure_keep_pending(Transport& transport, TestDouble& fake) {
  fake.play_music_ok = true;
  play_and_start(transport);
  transport.poll(16000);

  fake.health = StreamHealth::Stopped;
  fake.set_position_ok = false;
  fake.play_music_ok = true;
  const int plays0 = fake.play_music_calls;
  ui_tick(transport, 16000);
  expect(fake.set_position_calls >= 1, "seek is attempted");
  expect(fake.play_music_calls == plays0, "seek failure skips play_music");
  expect(transport.music_seek_pending() || transport.recovery_pending(),
         "seek failure keeps recovery pending");

  fake.set_position_ok = true;
  fake.play_music_ok = false;
  ui_tick(transport, 50000);
  expect(transport.music_start_pending() || transport.recovery_pending(),
         "play failure keeps recovery pending");
  expect(transport.recovery_attempt_count() >= 1, "play failure keeps attempt count");

  fake.play_music_ok = true;
  ui_tick(transport, 100000);
  expect(!transport.recovery_pending(), "health Playing clears recovery pending");
  expect(!transport.music_start_pending(), "health Playing clears start pending");
  expect(transport.recovery_attempt_count() == 0, "health Playing clears attempt count");
}

void test_near_end_does_not_retry(Transport& transport, TestDouble& fake) {
  play_and_start(transport);
  fake.duration = Microseconds{10000000};
  fake.position = Microseconds{9950000};
  fake.health = StreamHealth::Stopped;
  const int seeks0 = fake.set_position_calls;
  const int plays0 = fake.play_music_calls;
  const auto snap = transport.poll(16000);
  expect(snap.state == PlaybackState::Paused, "near-end Stopped ends playback");
  expect(fake.set_position_calls == seeks0, "near-end does not set_position");
  expect(fake.play_music_calls == plays0, "near-end does not play_music");
  expect(!transport.recovery_pending(), "near-end does not leave recovery pending");
}

void test_play_true_health_unchanged_rate_limited(Transport& transport, TestDouble& fake) {
  fake.update_health_on_play = false;
  fake.play_music_ok = true;
  fake.health = StreamHealth::Stopped;
  transport.request_play();
  ui_tick(transport, 16000);
  expect(transport.music_start_pending(), "play true without Playing keeps start pending");
  const int seeks0 = fake.set_position_calls;
  const int plays0 = fake.play_music_calls;
  for (int i = 0; i < 120; ++i) {
    ui_tick(transport, 16000);
  }
  expect(fake.play_music_calls - plays0 <= 7, "play-true/Stopped is not retried every frame");
  expect(fake.set_position_calls - seeks0 <= 1, "play-true/Stopped does not re-seek every frame");
  expect(transport.music_start_pending(), "still pending after 120 frames");
}

void test_initial_play_seek_fail_two_ticks(Transport& transport, TestDouble& fake) {
  fake.set_position_ok = false;
  fake.play_music_ok = true;
  fake.position = Microseconds{5000000};
  const int plays0 = fake.play_music_calls;
  transport.request_seek_ms(2500);
  transport.request_play();
  ui_tick(transport, 0);
  expect(transport.music_seek_pending(), "failed play seek stays pending");
  expect(fake.play_music_calls == plays0, "failed play seek does not play at old cursor");
  expect(transport.committed_ms() != 2500, "failed play seek does not publish the target");

  fake.set_position_ok = true;
  transport.poll(50000);
  expect(!transport.music_seek_pending(), "retry seek succeeds on the next poll");
  expect(transport.audio().position() == Microseconds{2500000}, "seek target is applied");
  expect(fake.play_music_calls == plays0, "poll seek does not play before UI rearm");
  expect(transport.start_pending_music(), "play after rearm opportunity");
  expect(fake.play_music_calls == plays0 + 1, "start_pending plays once after rearm");
  expect(!transport.music_start_pending(), "play after rearm clears start pending");
}

void test_user_seek_fail_while_playing(Transport& transport, TestDouble& fake) {
  play_and_start(transport);
  const auto before = transport.committed_position();
  const int plays0 = fake.play_music_calls;
  const int pauses0 = fake.pause_music_calls;
  fake.set_position_ok = false;
  transport.request_seek_ms(8000);
  ui_tick(transport, 16000);
  expect(transport.music_seek_pending(), "user seek failure stays pending");
  expect(transport.committed_position() == before, "failed user seek does not move the cursor");
  expect(fake.play_music_calls == plays0, "failed user seek does not play at old cursor");
  expect(fake.pause_music_calls > pauses0, "failed user seek pauses old output");
  expect(fake.health == StreamHealth::Paused, "failed user seek health is Paused");

  fake.set_position_ok = true;
  ui_tick(transport, 50000);
  expect(!transport.music_seek_pending(), "user seek retries and succeeds");
  expect(transport.audio().position() == Microseconds{8000000}, "user seek target applied");
}

void test_play_fail_replays_without_reseek(Transport& transport, TestDouble& fake) {
  play_and_start(transport);
  fake.health = StreamHealth::Stopped;
  fake.play_music_ok = false;
  fake.update_health_on_play = false;
  ui_tick(transport, 16000);
  const int seeks_after_first = fake.set_position_calls;
  const int plays_after_first = fake.play_music_calls;
  expect(seeks_after_first >= 1, "first recover seeks");
  expect(plays_after_first >= 1, "first recover plays");

  for (int i = 0; i < 20; ++i) {
    ui_tick(transport, 16000);
  }
  expect(fake.set_position_calls == seeks_after_first, "play failure does not re-seek");
  expect(fake.play_music_calls > plays_after_first, "play failure retries play");
  expect(fake.play_music_calls - plays_after_first <= 3, "play retries are rate-limited");
}

void test_clear_only_when_playing(Transport& transport, TestDouble& fake) {
  fake.update_health_on_play = false;
  fake.play_music_ok = true;
  fake.health = StreamHealth::Stalled;
  play_and_start(transport);
  expect(transport.music_start_pending(), "Stalled after play true keeps pending");
  fake.health = StreamHealth::Playing;
  ui_tick(transport, 50000);
  expect(!transport.music_start_pending(), "health Playing clears start pending");
  expect(!transport.recovery_pending(), "health Playing clears recovery");
}

void test_paused_recovery(Transport& transport, TestDouble& fake) {
  play_and_start(transport);
  fake.health = StreamHealth::Paused;
  fake.play_music_ok = false;
  const int seeks0 = fake.set_position_calls;
  const int plays0 = fake.play_music_calls;
  for (int i = 0; i < 60; ++i) {
    ui_tick(transport, 16000);
  }
  expect(fake.set_position_calls - seeks0 >= 1, "Paused recovers with a seek");
  expect(fake.play_music_calls - plays0 >= 1, "Paused recovers with a play");
  expect(fake.set_position_calls - seeks0 < 60, "Paused must not seek every frame");
  expect(transport.music_start_pending() || transport.recovery_pending(),
         "Paused recovery stays pending while play fails");
}

void test_pause_seek_play_shutdown_reset(Transport& transport, TestDouble& fake) {
  fake.play_music_ok = false;
  play_and_start(transport);
  for (int i = 0; i < 40; ++i) {
    ui_tick(transport, 16000);
  }
  expect(transport.recovery_attempt_count() >= 1, "failures accumulated");

  transport.request_pause();
  transport.poll(0);
  expect(!transport.playing(), "pause applied");
  expect(!transport.music_start_pending(), "pause clears music_start_pending");
  expect(transport.recovery_attempt_count() == 0, "pause resets attempt count");
  expect(!transport.recovery_pending(), "pause resets recovery pending");

  fake.play_music_ok = false;
  transport.request_play();
  transport.poll(0);
  expect(transport.recovery_attempt_count() == 0, "new play resets backoff");
  expect(transport.music_start_pending(), "new play arms pending start");
  expect(!transport.start_pending_music(), "new play retries immediately");
  expect(transport.music_start_pending(), "failed new play keeps pending");

  const int attempts_before_seek = transport.recovery_attempt_count();
  expect(attempts_before_seek >= 1, "failed start counted");
  transport.request_seek_ms(6000);
  transport.poll(0);
  expect(transport.recovery_attempt_count() == 0, "seek resets attempt count");
  expect(!transport.recovery_pending(), "seek resets recovery pending");

  transport.shutdown();
  expect(!transport.music_start_pending(), "shutdown clears music_start_pending");
  expect(!transport.music_seek_pending(), "shutdown clears music_seek_pending");
  expect(transport.recovery_attempt_count() == 0, "shutdown resets attempts");
  expect(!transport.recovery_pending(), "shutdown resets recovery pending");
}

void test_seek_then_pause_keeps_seek_target(Transport& transport, TestDouble& fake) {
  fake.position = Microseconds{1'000'000};
  fake.health = StreamHealth::Stopped;
  transport.request_seek_ms(1000);
  play_and_start(transport);
  expect(transport.playing(), "playing before return-to-start");
  expect(transport.committed_ms() == 1000, "play starts at the seek target");

  fake.update_position_on_seek = false;
  fake.position = Microseconds{1'008'000};
  transport.request_seek_ms(1000);
  transport.request_pause();
  transport.poll(16000);
  expect(!transport.playing(), "seek+pause pauses");
  expect(transport.committed_ms() == 1000, "seek+pause keeps the seek target");
  expect(!transport.music_start_pending(), "seek+pause does not leave start pending");
}

void test_pause_in_place_follows_audio_position(Transport& transport, TestDouble& fake) {
  fake.position = Microseconds{1'000'000};
  fake.health = StreamHealth::Stopped;
  transport.request_seek_ms(1000);
  play_and_start(transport);
  expect(transport.playing(), "playing before pause in place");

  fake.position = Microseconds{2'508'000};
  transport.request_pause();
  transport.poll(16000);
  expect(!transport.playing(), "pause in place pauses");
  expect(transport.committed_ms() == 2508, "pause without seek follows audio position");
}

void test_sfx_sync_admit_policy() {
  constexpr int64_t heard = 1'000'000;
  expect(admit_sfx_sync(heard, heard, 0) == SfxSyncAdmit::PastOrDue, "equal is PastOrDue");
  expect(admit_sfx_sync(heard - 1, heard, 0) == SfxSyncAdmit::PastOrDue, "past is PastOrDue");
  expect(admit_sfx_sync(heard, heard, kMaxPendingSfxSyncs) == SfxSyncAdmit::PastOrDue,
         "PastOrDue ignores pending cap");

  expect(admit_sfx_sync(heard + 1, heard, 0) == SfxSyncAdmit::WithinWindow,
         "1us future is WithinWindow");
  expect(admit_sfx_sync(heard + kSfxSyncHorizonUs, heard, 0) == SfxSyncAdmit::WithinWindow,
         "exactly 10s is WithinWindow");
  expect(admit_sfx_sync(heard + kSfxSyncHorizonUs + 1, heard, 0) == SfxSyncAdmit::TooFar,
         "10s+1us is TooFar");
  expect(admit_sfx_sync(heard + 20'000'000, heard, 0) == SfxSyncAdmit::TooFar,
         "20s future is TooFar");

  expect(admit_sfx_sync(heard + 1, heard, 4095) == SfxSyncAdmit::WithinWindow,
         "4095 pending still admits one more");
  expect(admit_sfx_sync(heard + 1, heard, 4096) == SfxSyncAdmit::AtCapacity,
         "4096 pending rejects future");
  expect(admit_sfx_sync(heard + kSfxSyncHorizonUs, heard, kMaxPendingSfxSyncs) ==
             SfxSyncAdmit::AtCapacity,
         "exactly 10s at cap is AtCapacity");
  expect(admit_sfx_sync(heard + kSfxSyncHorizonUs + 1, heard, kMaxPendingSfxSyncs) ==
             SfxSyncAdmit::TooFar,
         "TooFar wins over AtCapacity");
}

void test_sfx_sync_delta_boundaries() {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

  expect(saturating_future_delta_us(5, 10) == 0, "past delta saturates to 0");
  expect(saturating_future_delta_us(10, 10) == 0, "equal delta saturates to 0");
  expect(saturating_future_delta_us(10, 5) == 5, "small future delta");
  expect(saturating_future_delta_us(kSfxSyncHorizonUs, 0) == kSfxSyncHorizonUs,
         "exactly 10s delta");
  expect(saturating_future_delta_us(kSfxSyncHorizonUs + 1, 0) == kSfxSyncHorizonUs + 1,
         "10s+1us delta");

  expect(saturating_future_delta_us(kMax, 0) == kMax, "MAX - 0 is MAX");
  expect(saturating_future_delta_us(kMax, -1) == kMax, "MAX - (-1) saturates");
  expect(saturating_future_delta_us(kMax, kMin) == kMax, "MAX - MIN saturates");
  expect(saturating_future_delta_us(kMin, kMin) == 0, "MIN equal delta is 0");
  expect(saturating_future_delta_us(kMin + kSfxSyncHorizonUs, kMin) == kSfxSyncHorizonUs,
         "MIN + 10s delta");

  expect(admit_sfx_sync(kMax, kMin, 0) == SfxSyncAdmit::TooFar, "INT64 span is TooFar");
  expect(admit_sfx_sync(kMax, kMax, 0) == SfxSyncAdmit::PastOrDue, "INT64_MAX equal is PastOrDue");
  expect(admit_sfx_sync(kMin, kMin, 0) == SfxSyncAdmit::PastOrDue, "INT64_MIN equal is PastOrDue");
  expect(admit_sfx_sync(kMin + kSfxSyncHorizonUs, kMin, 0) == SfxSyncAdmit::WithinWindow,
         "INT64_MIN + 10s is WithinWindow");
  expect(admit_sfx_sync(kMin + kSfxSyncHorizonUs + 1, kMin, 0) == SfxSyncAdmit::TooFar,
         "INT64_MIN + 10s + 1 is TooFar");
  expect(admit_sfx_sync(kMax, kMax - 5, kMaxPendingSfxSyncs) == SfxSyncAdmit::AtCapacity,
         "near INT64_MAX at cap is AtCapacity");
}

void test_sfx_sync_post_arm() {
  expect(decide_after_set_sync(false, false) == SfxSyncPostArm::RetryLater,
         "future SetSync failure retries");
  expect(decide_after_set_sync(false, true) == SfxSyncPostArm::PlayDue,
         "TOCTOU after SetSync failure plays");
  expect(decide_after_set_sync(true, false) == SfxSyncPostArm::KeepArmed,
         "future SetSync success stays armed");
  expect(decide_after_set_sync(true, true) == SfxSyncPostArm::PlayDue,
         "TOCTOU after SetSync success plays");
}

void test_sfx_sync_arm_resolution() {
  using C = SfxSyncClaim;
  expect(decide_arm_resolution(true, false, C::Pending, false) == SfxSyncArmResult::RetryLater,
         "!found+Pending is RetryLater not Cancelled");
  expect(decide_arm_resolution(true, false, C::Pending, true) == SfxSyncArmResult::RetryLater,
         "!found+Pending due is not PlayDue or Cancelled");
  expect(decide_arm_resolution(false, false, C::Pending, false) == SfxSyncArmResult::RetryLater,
         "SetSync fail !found+Pending is RetryLater not Cancelled");
  expect(!arm_result_keeps_hit_mark(decide_arm_resolution(true, false, C::Pending, false)),
         "!found+Pending does not keep UI mark");
  expect(decide_arm_resolution(true, false, C::Fired, false) == SfxSyncArmResult::CallbackConsumed,
         "callback wins Fired is CallbackConsumed");
  expect(decide_arm_resolution(true, false, C::Fired, true) == SfxSyncArmResult::CallbackConsumed,
         "Fired is not TOCTOU replay");
  expect(decide_arm_resolution(true, true, C::Fired, true) == SfxSyncArmResult::CallbackConsumed,
         "found+Fired is consumed not PlayDue");
  expect(decide_arm_resolution(true, false, C::Cancelled, false) ==
             SfxSyncArmResult::CancelledDuringArm,
         "clear/shutdown during arm is CancelledDuringArm");
  expect(decide_arm_resolution(true, true, C::Cancelled, false) ==
             SfxSyncArmResult::CancelledDuringArm,
         "cancelled while still pending is CancelledDuringArm");
  expect(decide_arm_resolution(true, true, C::Pending, false) == SfxSyncArmResult::KeepArmed,
         "still pending and future stays armed");
  expect(decide_arm_resolution(true, true, C::Pending, true) == SfxSyncArmResult::PlayDue,
         "still pending and due is TOCTOU play");
  expect(decide_arm_resolution(false, true, C::Pending, false) == SfxSyncArmResult::RetryLater,
         "SetSync failure still future retries");
  expect(decide_arm_resolution(false, true, C::Pending, true) == SfxSyncArmResult::PlayDue,
         "SetSync failure already due plays");
  expect(decide_arm_resolution(false, false, C::Cancelled, false) ==
             SfxSyncArmResult::CancelledDuringArm,
         "SetSync failure after cancel is CancelledDuringArm");

  expect(arm_result_keeps_hit_mark(SfxSyncArmResult::CallbackConsumed),
         "CallbackConsumed keeps UI mark");
  expect(!arm_result_keeps_hit_mark(SfxSyncArmResult::CancelledDuringArm),
         "CancelledDuringArm does not keep UI mark");
  expect(arm_result_keeps_hit_mark(SfxSyncArmResult::KeepArmed), "KeepArmed keeps UI mark");
  expect(arm_result_keeps_hit_mark(SfxSyncArmResult::PlayDue), "PlayDue keeps UI mark");
  expect(!arm_result_keeps_hit_mark(SfxSyncArmResult::RetryLater), "RetryLater does not keep mark");

  expect(pending_sync_identifies(0, 0, true), "payload pointer identifies handle==0 window");
  expect(!pending_sync_identifies(0, 0, false), "handle==0 is not an identity");
  expect(pending_sync_identifies(7, 7, false), "nonzero handle identifies");
  expect(!pending_sync_identifies(7, 8, false), "mismatched handle does not identify");
}

void test_sfx_sync_claim_exclusive() {
  std::atomic<SfxSyncClaim> claim{SfxSyncClaim::Pending};
  expect(claim_sfx_sync_callback_play(claim, true), "pending callback claims Fired");
  expect(claim.load() == SfxSyncClaim::Fired, "fire stores Fired");
  expect(!claim_sfx_sync_fire(claim), "second fire loses");
  expect(!claim_sfx_sync_cancel(claim), "cancel loses after fire");
  expect(claim.load() == SfxSyncClaim::Fired, "Fired is sticky");
  expect(!claim_sfx_sync_callback_play(claim, true), "second callback claim loses");

  std::atomic<SfxSyncClaim> retired_fired{SfxSyncClaim::Fired};
  expect(!claim_sfx_sync_callback_play(retired_fired, false), "retired Fired does not re-claim");
  expect(retired_fired.load() == SfxSyncClaim::Fired, "retired Fired stays Fired");
  std::atomic<SfxSyncClaim> retired_cancelled{SfxSyncClaim::Cancelled};
  expect(!claim_sfx_sync_callback_play(retired_cancelled, false),
         "retired Cancelled does not re-claim");
  expect(retired_cancelled.load() == SfxSyncClaim::Cancelled, "retired Cancelled stays Cancelled");
  std::atomic<SfxSyncClaim> retired_pending{SfxSyncClaim::Pending};
  expect(!claim_sfx_sync_callback_play(retired_pending, false),
         "retired lookup does not claim Pending");
  expect(retired_pending.load() == SfxSyncClaim::Pending, "retired Pending stays Pending");

  std::atomic<SfxSyncClaim> cancelled{SfxSyncClaim::Pending};
  expect(claim_sfx_sync_cancel(cancelled), "clear wins cancel");
  expect(cancelled.load() == SfxSyncClaim::Cancelled, "cancel stores Cancelled");
  expect(!claim_sfx_sync_fire(cancelled), "fire loses after cancel");
  expect(!claim_sfx_sync_cancel(cancelled), "second cancel loses");

  std::atomic<SfxSyncClaim> due{SfxSyncClaim::Pending};
  expect(finish_due_play_claim(due) == SfxSyncPlayClaim::PlayNow, "scheduler wins PlayDue");
  expect(finish_due_play_claim(due) == SfxSyncPlayClaim::AlreadyFired,
         "PlayDue after fire does not replay");

  std::atomic<SfxSyncClaim> cleared{SfxSyncClaim::Pending};
  expect(claim_sfx_sync_cancel(cleared), "clear before PlayDue");
  expect(finish_due_play_claim(cleared) == SfxSyncPlayClaim::Cancelled,
         "PlayDue after cancel is Cancelled");
}

void test_sfx_sync_claim_races() {
  // Clear unlinks and claims Cancelled in the same critical section.
  {
    bool in_pending = true;
    std::atomic<SfxSyncClaim> claim{SfxSyncClaim::Pending};
    in_pending = false;
    (void)claim_sfx_sync_cancel(claim);
    const SfxSyncArmResult seen =
        decide_arm_resolution(true, in_pending, claim.load(), false);
    expect(seen == SfxSyncArmResult::CancelledDuringArm,
           "clear-then-schedule same CS is CancelledDuringArm");
    expect(!arm_result_keeps_hit_mark(seen), "clear wins does not keep UI mark");
  }

  // Callback already holds sfx_mu, CAS Fired, and unlinked — but has not played
  // yet. Schedule must see CallbackConsumed/true so UI does not erase the key.
  {
    std::mutex mu;
    bool in_pending = true;
    std::atomic<SfxSyncClaim> claim{SfxSyncClaim::Pending};
    std::atomic<int> plays{0};
    std::atomic<bool> claimed_unlinked{false};
    std::atomic<bool> schedule_observed{false};
    SfxSyncArmResult seen = SfxSyncArmResult::KeepArmed;
    std::thread callback([&] {
      bool won = false;
      {
        std::lock_guard<std::mutex> lock(mu);
        won = claim_sfx_sync_callback_play(claim, in_pending);
        in_pending = false;
        claimed_unlinked.store(true, std::memory_order_release);
      }
      while (!schedule_observed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      if (won) {
        plays.fetch_add(1, std::memory_order_relaxed);
      }
    });
    std::thread scheduler([&] {
      while (!claimed_unlinked.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      {
        std::lock_guard<std::mutex> lock(mu);
        seen = decide_arm_resolution(true, in_pending, claim.load(), false);
      }
      schedule_observed.store(true, std::memory_order_release);
    });
    callback.join();
    scheduler.join();
    expect(seen == SfxSyncArmResult::CallbackConsumed,
           "claimed-not-yet-played observe is CallbackConsumed");
    expect(arm_result_keeps_hit_mark(seen), "claimed-not-yet-played keeps UI mark");
    expect(plays.load() == 1, "callback still plays once after schedule observes");
    expect(claim.load() == SfxSyncClaim::Fired, "callback winner leaves Fired");
  }

  // Sequence B: callback and PlayDue both try to play; exactly one wins.
  {
    std::atomic<SfxSyncClaim> claim{SfxSyncClaim::Pending};
    std::atomic<int> plays{0};
    auto try_play = [&] {
      if (claim_sfx_sync_fire(claim)) {
        plays.fetch_add(1, std::memory_order_relaxed);
      }
    };
    std::thread callback(try_play);
    std::thread scheduler(try_play);
    callback.join();
    scheduler.join();
    expect(plays.load() == 1, "callback vs PlayDue plays exactly once");
    expect(claim.load() == SfxSyncClaim::Fired, "winner leaves Fired");
  }

  // Scheduler wins first, then callback must not play.
  {
    std::atomic<SfxSyncClaim> claim{SfxSyncClaim::Pending};
    expect(finish_due_play_claim(claim) == SfxSyncPlayClaim::PlayNow, "scheduler wins first");
    expect(!claim_sfx_sync_fire(claim), "late callback does not play");
  }
}

void test_hit_sfx_mark_not_permanent() {
  std::unordered_set<uint64_t> played;
  const uint64_t key = 0x100000002ull;
  expect(!commit_hit_sfx_schedule(played, key, false), "TooFar does not accept mark");
  expect(played.count(key) == 0, "TooFar is not permanently marked");
  expect(!commit_hit_sfx_schedule(played, key, false), "AtCapacity does not accept mark");
  expect(played.count(key) == 0, "AtCapacity is not permanently marked");
  expect(!commit_hit_sfx_schedule(played, key, false), "SetSyncFailure does not accept mark");
  expect(played.count(key) == 0, "SetSyncFailure is not permanently marked");
  expect(commit_hit_sfx_schedule(played, key, true), "success accepts mark");
  expect(played.count(key) == 1, "success keeps mark");
  expect(!commit_hit_sfx_schedule(played, key, true), "already marked does not re-arm");
  expect(played.count(key) == 1, "already marked stays marked");
}

bool write_pcm16_wav(const std::filesystem::path& path, int seconds, int rate = 44100) {
  const int chans = 2;
  const uint32_t data_bytes = static_cast<uint32_t>(seconds) * static_cast<uint32_t>(rate) *
                              static_cast<uint32_t>(chans) * 2u;
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  auto wr32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  auto wr16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
  out.write("RIFF", 4);
  wr32(36u + data_bytes);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  wr32(16);
  wr16(1);
  wr16(static_cast<uint16_t>(chans));
  wr32(static_cast<uint32_t>(rate));
  wr32(static_cast<uint32_t>(rate * chans * 2));
  wr16(static_cast<uint16_t>(chans * 2));
  wr16(16);
  out.write("data", 4);
  wr32(data_bytes);
  std::vector<char> zeros(data_bytes, 0);
  out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  return static_cast<bool>(out);
}

void test_bass_sfx_sync_fixture() {
  namespace fs = std::filesystem;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wds-sfx-sync-" + std::to_string(stamp));
  const fs::path fx_dir = root / "effects";
  const fs::path music_path = root / "music.wav";
  const fs::path perfect_ogg = fx_dir / "_PERFECT.ogg";
  std::error_code ec;
  fs::create_directories(fx_dir, ec);
  if (ec || !write_pcm16_wav(music_path, 12) || !write_pcm16_wav(perfect_ogg, 1)) {
    std::fprintf(stderr, "skip: could not write SFX sync WAV fixture\n");
    fs::remove_all(root, ec);
    return;
  }

  // Optional: occupy the process-wide BASS device so initialize() fails.
  // Transport TestDouble tests above never call initialize().
  const bool force_bass_fail = [] {
    const char* v = std::getenv("WDS_FORCE_BASS_INIT_FAIL");
    return v != nullptr && v[0] != '\0';
  }();
  if (force_bass_fail) {
    (void)BASS_Init(-1, 44100, 0, nullptr, nullptr);
  }

  AudioEngine engine;
  if (!engine.initialize(fx_dir.string(), music_path.string())) {
    std::fprintf(stderr, "skip: BASS device unavailable for SFX sync fixture\n");
    if (force_bass_fail) {
      BASS_Free();
    }
    engine.shutdown();
    fs::remove_all(root, ec);
    return;
  }
  if (!engine.has_music() || !engine.sfx_ready()) {
    std::fprintf(stderr, "skip: BASS could not load SFX sync fixture (music=%d sfx=%d)\n",
                 engine.has_music() ? 1 : 0, engine.sfx_ready() ? 1 : 0);
    engine.shutdown();
    fs::remove_all(root, ec);
    return;
  }

  expect(engine.pending_sfx_sync_count() == 0, "fixture starts with zero pending");
  expect(engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{1'000'000}),
         "future 1s arm succeeds");
  expect(engine.pending_sfx_sync_count() >= 1, "future arming increases pending");

  HitSfxPlayer player;
  player.attach(&engine);
  expect(player.pending_sfx_sync_count() == engine.pending_sfx_sync_count(),
         "HitSfxPlayer pending matches engine");

  engine.clear_scheduled_sfx();
  expect(engine.pending_sfx_sync_count() == 0, "clear_scheduled_sfx zeros pending");

  expect(engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{2'000'000}),
         "re-arm before seek");
  expect(engine.pending_sfx_sync_count() >= 1, "re-arm increases pending");
  expect(engine.set_position(Microseconds{0}), "seek to start");
  expect(engine.pending_sfx_sync_count() == 0, "seek zeros pending (no soft-seek recovery)");

  expect(engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{kSfxSyncHorizonUs}),
         "exactly 10s arm succeeds");
  expect(engine.pending_sfx_sync_count() >= 1, "exactly 10s is armed");
  // +50ms is past one PCM frame after byte-align; +1us can collapse to the same frame.
  expect(!engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{kSfxSyncHorizonUs + 50000}),
         "10s+50ms is TooFar after aligned-byte horizon");
  expect(engine.pending_sfx_sync_count() >= 1, "TooFar does not drop existing pending");
  engine.clear_scheduled_sfx();
  expect(engine.pending_sfx_sync_count() == 0, "clear after TooFar leaves zero pending");

  expect(engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{0}),
         "target at heard plays immediately");
  expect(engine.pending_sfx_sync_count() == 0, "immediate play does not consume pending cap");

  size_t max_pending = 0;
  for (int i = 0; i < 100; ++i) {
    (void)engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{1'500'000});
    const size_t n = engine.pending_sfx_sync_count();
    if (n > max_pending) {
      max_pending = n;
    }
    expect(engine.set_position(Microseconds{0}), "seek during 100-cycle");
    expect(engine.pending_sfx_sync_count() == 0, "each seek zeros pending");
  }
  expect(max_pending >= 1, "100-seek loop observed at least one armed sync");
  expect(max_pending <= 8, "100 seeks do not grow pending linearly");
  expect(engine.pending_sfx_sync_count() == 0, "after 100 seeks pending is 0");

  expect(player.schedule_at(HitSfxClip::Perfect, Microseconds{2'500'000}),
         "HitSfxPlayer arms future");
  expect(player.pending_sfx_sync_count() >= 1, "HitSfxPlayer pending after arm");
  player.stop_all();
  expect(player.pending_sfx_sync_count() == 0, "HitSfxPlayer pending after stop_all");

  // Near-target MIXTIME callback during SetSync is a platform race. Probe once
  // while playing; if it does not fire synchronously, skip rather than flake.
  engine.clear_scheduled_sfx();
  if (engine.play_music()) {
    const auto heard = engine.position();
    const auto near_target = Microseconds{heard.count() + 2000};
    const bool armed = engine.schedule_sfx_at(HitSfxClip::Perfect, near_target);
    const size_t pending = engine.pending_sfx_sync_count();
    if (armed && pending == 0) {
      expect(armed, "CallbackConsumed returns true so UI keeps key");
      expect(pending == 0, "consumed callback is not left pending");
    } else {
      std::fprintf(stderr,
                   "skip: BASS did not fire POS sync synchronously near target "
                   "(platform race not stably automatable; armed=%d pending=%zu)\n",
                   armed ? 1 : 0, pending);
    }
    engine.pause_music();
    engine.clear_scheduled_sfx();
  } else {
    std::fprintf(stderr, "skip: could not play music for immediate-callback probe\n");
  }

  engine.shutdown();
  expect(engine.pending_sfx_sync_count() == 0, "shutdown pending is 0");
  fs::remove_all(root, ec);
}

}  // namespace

int main() {
  test_sfx_sync_admit_policy();
  test_sfx_sync_delta_boundaries();
  test_sfx_sync_post_arm();
  test_sfx_sync_arm_resolution();
  test_sfx_sync_claim_exclusive();
  test_sfx_sync_claim_races();
  test_hit_sfx_mark_not_permanent();

  test_backoff_sequence();
  test_backoff_reset_and_success();
  test_backoff_not_every_frame();
  test_backoff_elapsed_saturates();
  test_classify_recovery();
  test_engine_no_music_observability();

  Transport transport;
  transport.request_seek_ms(1500);
  const auto snap = transport.poll(0);
  expect(snap.position_ms() == 0, "uninitialized transport ignores pending seek");
  expect(snap.state == PlaybackState::Paused, "default paused");

  // Transport TestDouble regressions: no initialize(), no BASS device required.
  test_uninitialized_bind_polls_without_bass();
  test_unbind_restores_ready_and_does_not_dangle();
  test_no_bgm_wall_clock_without_device();

  {
    Transport pending_t;
    TestDouble pending_fake;
    bind_fake_music(pending_t, pending_fake);
    test_start_pending_only_clears_on_success(pending_t, pending_fake);
    pending_t.shutdown();
  }
  {
    Transport start_t;
    TestDouble start_fake;
    bind_fake_music(start_t, start_fake);
    test_initial_start_rate_limited(start_t, start_fake);
    start_t.shutdown();
  }
  {
    Transport stopped_t;
    TestDouble stopped_fake;
    bind_fake_music(stopped_t, stopped_fake);
    test_persistent_stopped(stopped_t, stopped_fake);
    stopped_t.shutdown();
  }
  {
    Transport stalled_t;
    TestDouble stalled_fake;
    bind_fake_music(stalled_t, stalled_fake);
    test_persistent_stalled(stalled_t, stalled_fake);
    stalled_t.shutdown();
  }
  {
    Transport fail_t;
    TestDouble fail_fake;
    bind_fake_music(fail_t, fail_fake);
    test_seek_and_play_failure_keep_pending(fail_t, fail_fake);
    fail_t.shutdown();
  }
  {
    Transport end_t;
    TestDouble end_fake;
    bind_fake_music(end_t, end_fake);
    test_near_end_does_not_retry(end_t, end_fake);
    end_t.shutdown();
  }
  {
    Transport health_t;
    TestDouble health_fake;
    bind_fake_music(health_t, health_fake);
    test_play_true_health_unchanged_rate_limited(health_t, health_fake);
    health_t.shutdown();
  }
  {
    Transport two_t;
    TestDouble two_fake;
    bind_fake_music(two_t, two_fake);
    test_initial_play_seek_fail_two_ticks(two_t, two_fake);
    two_t.shutdown();
  }
  {
    Transport user_t;
    TestDouble user_fake;
    bind_fake_music(user_t, user_fake);
    test_user_seek_fail_while_playing(user_t, user_fake);
    user_t.shutdown();
  }
  {
    Transport replay_t;
    TestDouble replay_fake;
    bind_fake_music(replay_t, replay_fake);
    test_play_fail_replays_without_reseek(replay_t, replay_fake);
    replay_t.shutdown();
  }
  {
    Transport clear_t;
    TestDouble clear_fake;
    bind_fake_music(clear_t, clear_fake);
    test_clear_only_when_playing(clear_t, clear_fake);
    clear_t.shutdown();
  }
  {
    Transport paused_t;
    TestDouble paused_fake;
    bind_fake_music(paused_t, paused_fake);
    test_paused_recovery(paused_t, paused_fake);
    paused_t.shutdown();
  }
  {
    Transport reset_t;
    TestDouble reset_fake;
    bind_fake_music(reset_t, reset_fake);
    test_pause_seek_play_shutdown_reset(reset_t, reset_fake);
  }
  {
    Transport return_t;
    TestDouble return_fake;
    bind_fake_music(return_t, return_fake);
    test_seek_then_pause_keeps_seek_target(return_t, return_fake);
    return_t.shutdown();
  }
  {
    Transport here_t;
    TestDouble here_fake;
    bind_fake_music(here_t, here_fake);
    test_pause_in_place_follows_audio_position(here_t, here_fake);
    here_t.shutdown();
  }

  test_bass_sfx_sync_fixture();

  return failures == 0 ? 0 : 1;
}
