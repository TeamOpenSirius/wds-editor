#include "wds/audio/audio_engine.hpp"
#include "wds/audio/sfx_sync_policy.hpp"
#include "wds/audio/transport.hpp"
#include "wds/audio/waveform_jobs.hpp"
#include "wds/audio/waveform_overview.hpp"

#include <bass.h>
#include <bassmix.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Hooks of the in-memory BASS stand-in used for sanitizer runs where no audio
// device exists (not part of the product build; real-BASS builds skip the
// checks that need them).
#if defined(WDS_AUDIO_FAKE_BASS)
extern "C" {
void fake_bass_pump(unsigned ms);
void fake_bass_set_auto_pump(int on);
void fake_bass_set_open_delay_ms(int ms);
void fake_bass_set_getdata_delay_us(int us);
int fake_bass_open_calls();
int fake_bass_getdata_calls();
unsigned fake_bass_last_open_flags();
int fake_bass_inits();
int fake_bass_live_streams();
int fake_bass_live_objects();
void fake_bass_set_default_device(int d);
}
#endif

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

using wds::audio::AudioEngine;
using wds::audio::HitSfxClip;
using wds::audio::SfxSyncClaim;
using wds::common::Microseconds;

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

struct FixturePaths {
  std::filesystem::path root;
  std::filesystem::path fx_dir;
  std::filesystem::path music_path;
  std::filesystem::path music2_path;  // 6 s, second song for reload tests
};

bool make_fixture(FixturePaths& paths, bool hold_loop_json = false) {
  namespace fs = std::filesystem;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  paths.root = fs::temp_directory_path() /
               ((hold_loop_json ? "wds-sfx-pool-loop-" : "wds-sfx-pool-") + std::to_string(stamp));
  paths.fx_dir = paths.root / "effects";
  paths.music_path = paths.root / "music.wav";
  paths.music2_path = paths.root / "music2.wav";
  const fs::path perfect = paths.fx_dir / "_PERFECT.ogg";
  const fs::path hold = paths.fx_dir / "_HOLD.ogg";
  std::error_code ec;
  fs::create_directories(paths.fx_dir, ec);
  if (ec || !write_pcm16_wav(paths.music_path, 12) || !write_pcm16_wav(paths.music2_path, 6) ||
      !write_pcm16_wav(perfect, 1) || !write_pcm16_wav(hold, 1)) {
    std::fprintf(stderr, "skip: could not write SFX sync pool WAV fixture\n");
    fs::remove_all(paths.root, ec);
    return false;
  }
  if (hold_loop_json) {
    // Region loop 100..600 ms of the 1 s Hold clip (exercises the loop sync).
    std::ofstream json(paths.fx_dir / "hold_loop.json");
    json << "{\"start_ms\": 100, \"end_ms\": 600}\n";
    if (!json) {
      std::fprintf(stderr, "skip: could not write hold_loop.json fixture\n");
      fs::remove_all(paths.root, ec);
      return false;
    }
  }
  return true;
}

bool init_or_skip(AudioEngine& engine, const FixturePaths& paths) {
  const bool force_bass_fail = [] {
    const char* v = std::getenv("WDS_FORCE_BASS_INIT_FAIL");
    return v != nullptr && v[0] != '\0';
  }();
  if (force_bass_fail) {
    std::fprintf(stderr, "skip: BASS device unavailable for SFX sync pool tests\n");
    return false;
  }
  if (!engine.initialize(paths.fx_dir.string(), paths.music_path.string())) {
    std::fprintf(stderr, "skip: BASS device unavailable for SFX sync pool tests\n");
    engine.shutdown();
    return false;
  }
  if (!engine.has_music() || !engine.sfx_ready()) {
    std::fprintf(stderr, "skip: BASS could not load SFX sync pool fixture (music=%d sfx=%d)\n",
                 engine.has_music() ? 1 : 0, engine.sfx_ready() ? 1 : 0);
    engine.shutdown();
    return false;
  }
  return true;
}

void test_stale_slot_after_clear(AudioEngine& engine) {
  expect(engine.pending_sfx_sync_count() == 0, "stale-slot test starts with zero pending");

  constexpr int kArmCount = 80;
  int armed = 0;
  for (int i = 0; i < kArmCount; ++i) {
    const auto at = Microseconds{1'000'000 + static_cast<int64_t>(i) * 10'000};
    if (engine.schedule_sfx_at(HitSfxClip::Perfect, at)) {
      ++armed;
    }
  }
  expect(armed > 64, "armed more than 64 future syncs");
  expect(engine.pending_sfx_sync_count() > 64, "pending exceeds 64 after bulk arm");

  std::vector<void*> slots(static_cast<size_t>(armed), nullptr);
  std::vector<unsigned long long> handles(static_cast<size_t>(armed), 0);
  const size_t snapped =
      engine.snapshot_pending_sfx_syncs(slots.data(), handles.data(), slots.size());
  expect(snapped > 64, "snapshot captured more than 64 armed slots");

  engine.clear_scheduled_sfx();
  expect(engine.pending_sfx_sync_count() == 0, "clear zeros pending after bulk arm");

  for (size_t i = 0; i < snapped; ++i) {
    engine.handle_sfx_sync(handles[i], slots[i], HitSfxClip::Perfect);
    unsigned long long handle = 0;
    int claim = -1;
    bool in_use = true;
    expect(engine.inspect_sfx_sync_slot(slots[i], &handle, &claim, &in_use),
           "stale slot remains a pool address after clear");
    expect(claim != static_cast<int>(SfxSyncClaim::Fired),
           "stale handle_sfx_sync must not re-claim Fired");
  }
  expect(engine.pending_sfx_sync_count() == 0, "stale callbacks do not re-queue pending");
}

void test_shutdown_with_hammer_thread(const FixturePaths& paths) {
  constexpr int kIters = 50;
  for (int iter = 0; iter < kIters; ++iter) {
    AudioEngine engine;
    if (!init_or_skip(engine, paths)) {
      return;
    }
    for (int i = 0; i < 16; ++i) {
      (void)engine.schedule_sfx_at(HitSfxClip::Perfect,
                                   Microseconds{2'000'000 + static_cast<int64_t>(i) * 20'000});
    }
    void* slots[16]{};
    unsigned long long handles[16]{};
    const size_t n = engine.snapshot_pending_sfx_syncs(slots, handles, 16);
    expect(n >= 1, "shutdown-hammer iteration armed at least one slot");

    std::atomic<bool> stop{false};
    std::thread hammer([&]() {
      while (!stop.load(std::memory_order_acquire)) {
        for (size_t i = 0; i < n; ++i) {
          engine.handle_sfx_sync(handles[i], slots[i], HitSfxClip::Perfect);
        }
      }
      for (int extra = 0; extra < 64; ++extra) {
        for (size_t i = 0; i < n; ++i) {
          engine.handle_sfx_sync(handles[i], slots[i], HitSfxClip::Perfect);
        }
      }
    });
    engine.shutdown();
    stop.store(true, std::memory_order_release);
    hammer.join();
    expect(engine.pending_sfx_sync_count() == 0, "pending is 0 after shutdown while hammering");
    expect(!engine.inspect_sfx_sync_slot(n > 0 ? slots[0] : nullptr, nullptr, nullptr, nullptr),
           "pool is released after shutdown");
  }
}

void test_shutdown_releases_pool(const FixturePaths& paths) {
  AudioEngine engine;
  if (!init_or_skip(engine, paths)) {
    return;
  }
  expect(engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{3'000'000}),
         "arm before shutdown-release check");
  expect(engine.pending_sfx_sync_count() >= 1, "pending before shutdown-release check");
  void* slot = nullptr;
  unsigned long long handle = 0;
  expect(engine.snapshot_pending_sfx_syncs(&slot, &handle, 1) == 1, "snapshot one armed slot");
  engine.shutdown();
  expect(engine.pending_sfx_sync_count() == 0, "pending is 0 after shutdown");
  expect(!engine.inspect_sfx_sync_slot(slot, nullptr, nullptr, nullptr),
         "inspect fails after pool release");
}

// Number of sources in the engine's BASS mixer (music + Hold + live one-shots).
// -1 when the mixer cannot be reached through the Hold channel.
int mixer_source_count(const AudioEngine& engine) {
  const auto hold = static_cast<DWORD>(engine.hold_channel_handle());
  if (hold == 0) {
    return -1;
  }
  const HSTREAM mixer = BASS_Mixer_ChannelGetMixer(hold);
  if (mixer == 0) {
    return -1;
  }
  const DWORD n = BASS_Mixer_StreamGetChannels(mixer, nullptr, 0);
  return n == static_cast<DWORD>(-1) ? -1 : static_cast<int>(n);
}

// The Hold stream is created once, survives stop/seek/rate teardown as a single
// paused mixer source, and one-shot sources added to the mixer are released by
// stop_all_sfx (nothing accumulates across cycles).
void test_hold_stream_not_leaked(const FixturePaths& paths) {
  AudioEngine engine;
  if (!init_or_skip(engine, paths)) {
    return;
  }
  const auto hold0 = engine.hold_channel_handle();
  expect(hold0 != 0, "Hold stream exists after initialize");
  const int baseline = mixer_source_count(engine);
  expect(baseline == 2, "mixer holds exactly music + Hold at baseline");

  for (int i = 0; i < 40; ++i) {
    engine.set_hold_looping(true);
    expect(engine.hold_looping(), "Hold starts");
    for (int k = 0; k < 6; ++k) {
      (void)engine.play_sfx(HitSfxClip::Perfect);
    }
    expect(mixer_source_count(engine) > baseline, "one-shots join the mixer");
    switch (i % 4) {
      case 0:
        engine.stop_all_sfx();
        break;
      case 1:
        (void)engine.set_position(Microseconds{500'000 + i * 1000});
        engine.stop_all_sfx();
        break;
      case 2:
        engine.set_playback_rate(i % 8 == 2 ? 0.5f : 1.0f);
        break;
      default:
        engine.begin_timeline_control();
        engine.stop_all_sfx();
        break;
    }
    expect(!engine.hold_looping(), "stop/seek/rate silences Hold");
    expect(engine.hold_channel_handle() == hold0, "Hold stream is reused, not recreated");
    expect(mixer_source_count(engine) == baseline,
           "stop_all_sfx leaves only music + the single Hold stream");
  }
  BASS_CHANNELINFO info{};
  expect(BASS_ChannelGetInfo(static_cast<DWORD>(hold0), &info) != FALSE,
         "Hold stream is still a live BASS handle");
  engine.shutdown();
  expect(engine.hold_channel_handle() == 0, "shutdown releases the Hold stream");
}

// Emulates BASS' mixer thread: replays HoldOn/HoldOff (and one-shot) MIXTIME
// gate callbacks on its own thread while the control thread toggles Hold,
// reconciles against a clock, clears gates and tears everything down.
void test_hold_gates_vs_control_thread(const FixturePaths& paths, bool region_loop) {
  AudioEngine engine;
  if (!init_or_skip(engine, paths)) {
    return;
  }
  const auto hold0 = engine.hold_channel_handle();
  expect(hold0 != 0, "gate test: Hold stream exists before any gate is armed");

  struct Fire {
    void* slot;
    unsigned long long handle;
  };
  std::mutex mu;
  std::vector<Fire> queue;
  std::atomic<bool> stop{false};
  std::atomic<int> fired{0};

  std::thread mixer([&] {
    std::vector<Fire> local;
    while (!stop.load(std::memory_order_acquire)) {
      {
        std::lock_guard<std::mutex> lock(mu);
        local.swap(queue);
      }
      for (const Fire& f : local) {
        engine.handle_sfx_sync(f.handle, f.slot, HitSfxClip::Hold);
        fired.fetch_add(1, std::memory_order_relaxed);
      }
      local.clear();
      std::this_thread::yield();
    }
  });

  for (int i = 0; i < 300; ++i) {
    // Gates arm only on the control thread, after the Hold stream exists.
    const bool on_first = (i % 2) == 0;
    (void)engine.schedule_hold_gate(on_first, Microseconds{1'000'000 + i * 1000});
    (void)engine.schedule_hold_gate(!on_first, Microseconds{1'500'000 + i * 1000});
    void* slots[8]{};
    unsigned long long handles[8]{};
    const size_t n = engine.snapshot_pending_sfx_syncs(slots, handles, 8);
    {
      std::lock_guard<std::mutex> lock(mu);
      for (size_t k = 0; k < n; ++k) {
        queue.push_back(Fire{slots[k], handles[k]});
      }
    }
    switch (i % 6) {
      case 0:
        engine.reconcile_hold_looping(true);
        break;
      case 1:
        engine.reconcile_hold_looping(false);
        break;
      case 2:
        engine.set_hold_looping(true);
        break;
      case 3:
        engine.stop_all_sfx();
        break;
      case 4:
        engine.clear_hold_gates();
        engine.set_hold_looping(false);
        break;
      default:
        engine.reconcile_hold_looping((i / 6) % 2 == 0);
        break;
    }
    expect(engine.hold_channel_handle() == hold0,
           "Hold handle is never zeroed or replaced under concurrent gates");
  }

  engine.stop_all_sfx();
  stop.store(true, std::memory_order_release);
  mixer.join();
  expect(fired.load() > 0, "mixer-thread emulation delivered gate callbacks");
  expect(!engine.hold_looping(), "stop_all_sfx leaves Hold silent after gate traffic");
  expect(mixer_source_count(engine) == 2, "no extra Hold/one-shot streams after gate traffic");
  (void)region_loop;
  engine.shutdown();
  expect(engine.hold_channel_handle() == 0, "shutdown releases the Hold stream");
}

// A mix-time HoldOn must survive the control thread's clock reconcile while the
// heard clock still says "not yet", and a HoldOff must not be undone while the
// clock still says "inside the interval".
void test_hold_gate_not_overridden_by_reconcile(const FixturePaths& paths) {
  AudioEngine engine;
  if (!init_or_skip(engine, paths)) {
    return;
  }
  engine.stop_all_sfx();
  expect(engine.schedule_hold_gate(true, Microseconds{1'000'000}), "arm HoldOn");
  void* slot = nullptr;
  unsigned long long handle = 0;
  expect(engine.snapshot_pending_sfx_syncs(&slot, &handle, 1) == 1, "snapshot HoldOn");
  engine.handle_sfx_sync(handle, slot, HitSfxClip::Hold);  // gate fires at mix time
  expect(engine.hold_looping(), "HoldOn gate starts Hold");
  engine.reconcile_hold_looping(false);  // heard clock is still before the interval
  expect(engine.hold_looping(), "reconcile does not undo a fired HoldOn");
  engine.reconcile_hold_looping(true);  // heard clock enters the interval: agree
  expect(engine.hold_looping(), "Hold stays on once the clock agrees");

  expect(engine.schedule_hold_gate(false, Microseconds{2'000'000}), "arm HoldOff");
  expect(engine.snapshot_pending_sfx_syncs(&slot, &handle, 1) == 1, "snapshot HoldOff");
  engine.handle_sfx_sync(handle, slot, HitSfxClip::Hold);
  expect(!engine.hold_looping(), "HoldOff gate stops Hold");
  engine.reconcile_hold_looping(true);  // heard clock still inside the interval
  expect(!engine.hold_looping(), "reconcile does not restart Hold after HoldOff");
  engine.reconcile_hold_looping(false);  // clock passed the end: agree
  engine.reconcile_hold_looping(true);   // a later interval is now the GUI's job
  expect(engine.hold_looping(), "reconcile resumes control once the gate has been absorbed");
  engine.stop_all_sfx();
  engine.shutdown();
}

using wds::audio::SfxSlotPoolStats;
using Clock = std::chrono::steady_clock;

int64_t ms_since(Clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// Poll until every slot is back on the free list (callbacks that already won
// their claim finish recycling asynchronously) or the timeout expires.
SfxSlotPoolStats wait_pool_idle(const AudioEngine& engine, int timeout_ms) {
  SfxSlotPoolStats st;
  const auto t0 = Clock::now();
  for (;;) {
    st = SfxSlotPoolStats{};
    (void)engine.sfx_slot_pool_stats(&st);
    if ((st.in_use_count == 0 && st.free_count == st.capacity) || ms_since(t0) > timeout_ms) {
      return st;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// Schedule burst racing MIXTIME callbacks. After a seek / play start the preview
// arms hundreds of hits back to back while the mixer thread fires earlier ones;
// each fired callback must recycle only its own slot. The old code swept all
// 4096 slots under sfx_mu on every schedule, so callbacks queued behind it.
// Asserts the pool never took the whole-pool recovery sweep and that nothing
// leaked (every slot is free again once the burst has drained). Run under TSan.
// Targets are only 5..44 ms past the HEARD position. With real BASS the mixer has
// already mixed ~500 ms beyond that (playback buffer), so such a POS sync could
// never fire: the engine must classify those by the mixed position and play them
// immediately. Every accepted schedule therefore ends up fired or fired-now, and
// none may be left stuck in pending_syncs / in_use.
void test_burst_schedule_vs_callbacks(const FixturePaths& paths) {
  AudioEngine engine;
  if (!init_or_skip(engine, paths)) {
    return;
  }
#if defined(WDS_AUDIO_FAKE_BASS)
  fake_bass_set_auto_pump(0);  // the test's mixer thread drives time instead
#endif
  expect(engine.play_music(), "burst: music plays");
  std::atomic<bool> stop{false};
  std::thread mixer;
#if defined(WDS_AUDIO_FAKE_BASS)
  mixer = std::thread([&] {
    while (!stop.load(std::memory_order_acquire)) {
      fake_bass_pump(1);  // 1 ms of audio per step, holding the BASS lock like the mixer
      // The real mixer thread sleeps between update periods; without this the
      // non-fair fake lock starves the scheduling thread's BASS calls.
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });
#endif
  int accepted = 0;
  constexpr int kBurst = 1000;
  for (int i = 0; i < kBurst; ++i) {
    const auto at = engine.position() + Microseconds{5'000 + (i % 40) * 1'000};
    if (engine.schedule_sfx_at(HitSfxClip::Perfect, at)) {
      ++accepted;
    }
  }
  expect(accepted > kBurst * 9 / 10, "burst: nearly all 1000 schedules accepted");

  const auto t0 = Clock::now();
  while (engine.pending_sfx_sync_count() != 0 && ms_since(t0) < 10'000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const size_t burst_pending = engine.pending_sfx_sync_count();
  expect(burst_pending == 0, "burst: every armed sync fired");
  const SfxSlotPoolStats st = wait_pool_idle(engine, 3000);
  std::fprintf(stderr,
               "burst diag: accepted=%d pending=%zu playing=%d pos_us=%lld free=%zu cap=%zu "
               "in_use=%zu pool_pending=%zu sweeps=%llu\n",
               accepted, burst_pending, engine.stream_playing() ? 1 : 0,
               static_cast<long long>(engine.position().count()),
               static_cast<size_t>(st.free_count), static_cast<size_t>(st.capacity),
               static_cast<size_t>(st.in_use_count), static_cast<size_t>(st.pending_count),
               static_cast<unsigned long long>(st.full_sweeps));
  expect(st.free_count == st.capacity && st.in_use_count == 0,
         "burst: every slot recycled by its own callback (no leak)");
  expect(st.full_sweeps == 0, "burst: schedule path never swept the whole pool");

  stop.store(true, std::memory_order_release);
  if (mixer.joinable()) {
    mixer.join();
  }
  engine.pause_music();
  engine.clear_scheduled_sfx();
  engine.shutdown();
#if defined(WDS_AUDIO_FAKE_BASS)
  fake_bass_set_auto_pump(1);
#endif
}

// Fill the whole pool (4096 armed future syncs), reject the overflow, clear: the
// free list alone must serve every acquire (no recovery sweep), and clear must
// hand every slot back.
void test_full_pool_without_sweep(const FixturePaths& paths) {
  AudioEngine engine;
  if (!init_or_skip(engine, paths)) {
    return;
  }
  constexpr int kTry = static_cast<int>(wds::audio::kMaxPendingSfxSyncs) + 40;
  int accepted = 0;
  for (int i = 0; i < kTry; ++i) {
    if (engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{1'000'000 + i * 2'000})) {
      ++accepted;
    }
  }
  SfxSlotPoolStats st;
  expect(engine.sfx_slot_pool_stats(&st), "stats available");
  expect(accepted == static_cast<int>(wds::audio::kMaxPendingSfxSyncs),
         "exactly the pool capacity is admitted");
  expect(st.pending_count == st.capacity && st.free_count == 0 && st.in_use_count == st.capacity,
         "pool fully armed");
  expect(st.full_sweeps == 0, "filling the pool used the free list only");
  engine.clear_scheduled_sfx();
  st = wait_pool_idle(engine, 1000);
  expect(st.free_count == st.capacity && st.in_use_count == 0 && st.pending_count == 0,
         "clear returns every slot to the free list");
  expect(st.full_sweeps == 0, "clear never swept the pool");
  // And the pool is reusable afterwards.
  expect(engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{2'000'000}), "re-arm after clear");
  engine.shutdown();
}

// Song switch keeps the device, SFX samples, mixer, Hold stream and slot pool and
// replaces only the music stream.
void test_reload_music_keeps_device(const FixturePaths& paths) {
  AudioEngine engine;
  if (!init_or_skip(engine, paths)) {
    return;
  }
  const std::string fx = paths.fx_dir.string();
  const auto hold0 = engine.hold_channel_handle();
  expect(hold0 != 0, "reload: Hold stream exists");
  const auto dur0 = engine.duration();
  expect(dur0.count() > 11'000'000 && dur0.count() < 13'000'000, "reload: first song is 12 s");
  engine.set_music_gain(0.4f);
  engine.set_playback_rate(0.75f);
  expect(engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{5'000'000}), "reload: arm hit");
  expect(engine.set_position(Microseconds{3'000'000}), "reload: seek old song");
  expect(engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{6'000'000}), "reload: re-arm");
  expect(engine.play_music(), "reload: playing old song");
  const uint64_t gen0 = engine.position_generation();
#if defined(WDS_AUDIO_FAKE_BASS)
  const int inits0 = fake_bass_inits();
#endif

  expect(engine.reload_music(fx, paths.music2_path.string()), "reload: second song");
  const auto dur1 = engine.duration();
  expect(dur1.count() > 5'000'000 && dur1.count() < 7'000'000, "reload: duration is the new song's");
  expect(engine.has_music() && engine.ready() && engine.sfx_ready(), "reload: still ready");
  expect(engine.hold_channel_handle() == hold0, "reload: Hold stream kept");
  expect(engine.pending_sfx_sync_count() == 0, "reload: old song's syncs cleared");
  expect(engine.position_generation() > gen0, "reload: position generation bumped");
  expect(!engine.stream_playing(), "reload: paused, like a fresh initialize");
  expect(engine.position().count() < 100'000, "reload: new song starts at 0");
  expect(mixer_source_count(engine) == 2, "reload: mixer holds exactly new music + Hold");
  BASS_CHANNELINFO ci{};
  expect(BASS_ChannelGetInfo(static_cast<DWORD>(hold0), &ci) != FALSE, "reload: Hold handle is live");
#if defined(WDS_AUDIO_FAKE_BASS)
  expect(fake_bass_inits() == inits0, "reload: BASS_Init not called again");
  const int objs = fake_bass_live_objects();
#endif
  // Gains and rate carry over; new syncs arm against the new stream and fire.
  expect(engine.schedule_sfx_at(HitSfxClip::Perfect, Microseconds{4'000'000}), "reload: arm on new song");
  expect(engine.pending_sfx_sync_count() == 1, "reload: new song owns the pending hit");
  engine.clear_scheduled_sfx();

  // Things that must NOT take the in-place path (the caller re-initialises).
  const auto keep_dur = engine.duration();
  expect(!engine.reload_music(fx + "-other", paths.music_path.string()), "reload: other effects dir");
  expect(!engine.reload_music(fx, (paths.root / "missing.wav").string()), "reload: missing file");
  expect(!engine.reload_music(fx, std::string{}), "reload: empty path");
  expect(engine.duration() == keep_dur && engine.has_music(), "reload: failed reload keeps old song");
#if defined(WDS_AUDIO_FAKE_BASS)
  fake_bass_set_default_device(2);
  expect(!engine.reload_music(fx, paths.music_path.string()), "reload: refuses after default device change");
  fake_bass_set_default_device(1);
#endif

  // Repeated song switches neither leak BASS objects nor lose SFX.
  for (int i = 0; i < 12; ++i) {
    const std::string& next = (i % 2 == 0) ? paths.music_path.string() : paths.music2_path.string();
    expect(engine.reload_music(fx, next), "reload: repeated switch");
    (void)engine.play_music();
    (void)engine.schedule_sfx_at(HitSfxClip::Perfect, engine.position() + Microseconds{50'000 + i});
    engine.set_hold_looping(i % 3 == 0);
  }
  expect(engine.hold_channel_handle() == hold0, "reload: Hold stream survives repeated switches");
  engine.stop_all_sfx();
#if defined(WDS_AUDIO_FAKE_BASS)
  expect(fake_bass_live_objects() == objs, "reload: no BASS objects leaked across 12 switches");
#endif
  SfxSlotPoolStats st = wait_pool_idle(engine, 1000);
  expect(st.free_count == st.capacity && st.full_sweeps == 0, "reload: slot pool intact");

  // Transport wrapper resets queued intents like initialize().
  engine.shutdown();
  wds::audio::Transport transport;
  if (!transport.initialize(fx, paths.music_path.string()) || !transport.audio().has_music()) {
    transport.shutdown();
    return;
  }
  transport.request_seek_ms(4000);
  transport.request_play();
  expect(transport.reload_music(fx, paths.music2_path.string()), "transport: reload");
  expect(!transport.pending_play() && !transport.pending_seek() && !transport.playing() &&
             transport.committed_ms() == 0,
         "transport: reload resets intents and position");
  expect(!transport.reload_music(fx + "-other", paths.music_path.string()),
         "transport: unsupported reload reports false and changes nothing");
  transport.shutdown();
}

// WaveformOverview cancel semantics and WaveformJobs (the retire-list the GUI
// uses so a song switch never waits on a decode).
void test_waveform_cancel_and_jobs(const FixturePaths& paths) {
  namespace fs = std::filesystem;
  AudioEngine engine;
  if (!init_or_skip(engine, paths)) {
    return;
  }
  const fs::path wf2 = paths.root / "wf2.wav";
  const fs::path wf3 = paths.root / "wf3.wav";
  if (!write_pcm16_wav(wf2, 2) || !write_pcm16_wav(wf3, 3)) {
    std::fprintf(stderr, "skip: could not write waveform fixtures\n");
    engine.shutdown();
    return;
  }

  {
    wds::audio::WaveformOverview wf;
    expect(wf.load(wf3.string()), "waveform: plain decode succeeds");
    expect(wf.duration_ms() >= 2999 && wf.duration_ms() <= 3001,
           "waveform: duration matches the decoded audio");
#if defined(WDS_AUDIO_FAKE_BASS)
    expect((fake_bass_last_open_flags() & BASS_STREAM_PRESCAN) == 0,
           "waveform: decode stream is opened without PRESCAN");
#endif
    std::atomic<bool> cancelled{true};
#if defined(WDS_AUDIO_FAKE_BASS)
    const int opens = fake_bass_open_calls();
#endif
    expect(!wf.load(wf3.string(), &cancelled) && wf.empty(), "waveform: pre-cancelled load fails clean");
#if defined(WDS_AUDIO_FAKE_BASS)
    expect(fake_bass_open_calls() == opens, "waveform: pre-cancelled load never opens the file");
#endif
  }

#if defined(WDS_AUDIO_FAKE_BASS)
  {
    // Cancelled while the (uninterruptible) open is blocked on slow storage: no
    // decode happens afterwards and the stream is freed.
    wds::audio::WaveformOverview wf;
    std::atomic<bool> cancel{false};
    bool ok = true;
    const int streams0 = fake_bass_live_streams();
    const int reads0 = fake_bass_getdata_calls();
    fake_bass_set_open_delay_ms(400);
    const auto t0 = Clock::now();
    std::thread worker([&] { ok = wf.load(wf3.string(), &cancel); });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    cancel.store(true);
    worker.join();
    fake_bass_set_open_delay_ms(0);
    expect(!ok && wf.empty(), "waveform: cancel during open returns false");
    expect(fake_bass_getdata_calls() == reads0, "waveform: cancel during open skips the decode");
    expect(fake_bass_live_streams() == streams0, "waveform: cancel during open frees the stream");
    expect(ms_since(t0) < 400 + 800, "waveform: returns right after the open completes");
  }
  {
    // Cancelled mid-decode: the per-chunk check stops it promptly.
    wds::audio::WaveformOverview wf;
    std::atomic<bool> cancel{false};
    bool ok = true;
    const int streams0 = fake_bass_live_streams();
    const int reads0 = fake_bass_getdata_calls();
    fake_bass_set_getdata_delay_us(20'000);
    std::thread worker([&] { ok = wf.load(paths.music_path.string(), &cancel); });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto t0 = Clock::now();
    cancel.store(true);
    worker.join();
    const int64_t stop_ms = ms_since(t0);
    fake_bass_set_getdata_delay_us(0);
    expect(!ok && wf.empty(), "waveform: cancel mid-decode returns false");
    expect(stop_ms < 500, "waveform: cancel mid-decode returns promptly");
    expect(fake_bass_getdata_calls() - reads0 < 30, "waveform: decode stopped early");
    expect(fake_bass_live_streams() == streams0, "waveform: cancel mid-decode frees the stream");
  }
#endif

  {
    // Stale generation: the first job is superseded and must never be delivered.
    wds::audio::WaveformJobs jobs;
    (void)jobs.restart(wf2.string());
    (void)jobs.restart(wf3.string());
    wds::audio::WaveformOverview got;
    int delivered = 0;
    const auto t0 = Clock::now();
    while ((jobs.in_flight() != 0) && ms_since(t0) < 15'000) {
      wds::audio::WaveformOverview r;
      if (jobs.poll(r)) {
        ++delivered;
        got = std::move(r);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect(jobs.in_flight() == 0, "jobs: all jobs reaped");
    expect(delivered == 1, "jobs: exactly one result delivered");
    expect(got.duration_ms() >= 2999 && got.duration_ms() <= 3001,
           "jobs: the delivered result is the newest generation's");

    // cancel_all discards whatever is in flight.
    (void)jobs.restart(wf3.string());
    jobs.cancel_all();
    int late = 0;
    const auto t1 = Clock::now();
    while (jobs.in_flight() != 0 && ms_since(t1) < 15'000) {
      wds::audio::WaveformOverview r;
      if (jobs.poll(r)) {
        ++late;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect(late == 0, "jobs: cancelled job's result is discarded");
  }

#if defined(WDS_AUDIO_FAKE_BASS)
  {
    // The control thread never waits on a decode: cancel_all/restart return
    // while the worker is still blocked in its open; only shutdown() joins.
    wds::audio::WaveformJobs jobs;
    const int streams0 = fake_bass_live_streams();
    fake_bass_set_open_delay_ms(600);
    (void)jobs.restart(wf3.string());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t0 = Clock::now();
    jobs.cancel_all();
    (void)jobs.restart(std::string{});
    (void)jobs.restart(std::string{});
    const int64_t control_ms = ms_since(t0);
    expect(control_ms < 100, "jobs: cancel/restart do not block on the running decode");
    expect(jobs.in_flight() == 1, "jobs: cancelled decode is retired, not joined");
    wds::audio::WaveformOverview r;
    expect(!jobs.poll(r), "jobs: poll does not block or deliver a cancelled job");
    const auto t1 = Clock::now();
    jobs.shutdown();
    fake_bass_set_open_delay_ms(0);
    expect(jobs.in_flight() == 0, "jobs: shutdown joins the retired decode");
    expect(ms_since(t1) >= 100, "jobs: shutdown really waited for the blocked open");
    expect(fake_bass_live_streams() == streams0, "jobs: no decode stream outlives shutdown");
  }
#endif
  engine.shutdown();
}

}  // namespace

int main() {
  FixturePaths paths;
  if (!make_fixture(paths)) {
    return 0;
  }
  FixturePaths loop_paths;
  const bool have_loop_paths = make_fixture(loop_paths, /*hold_loop_json=*/true);

  {
    AudioEngine engine;
    if (init_or_skip(engine, paths)) {
      test_stale_slot_after_clear(engine);
      engine.shutdown();
    }
  }

  test_shutdown_with_hammer_thread(paths);
  test_shutdown_releases_pool(paths);
  test_hold_stream_not_leaked(paths);
  test_hold_gate_not_overridden_by_reconcile(paths);
  test_hold_gates_vs_control_thread(paths, /*region_loop=*/false);
  test_burst_schedule_vs_callbacks(paths);
  test_full_pool_without_sweep(paths);
  test_reload_music_keeps_device(paths);
  test_waveform_cancel_and_jobs(paths);
  if (have_loop_paths) {
    test_hold_stream_not_leaked(loop_paths);
    test_hold_gates_vs_control_thread(loop_paths, /*region_loop=*/true);
  }

  std::error_code ec;
  std::filesystem::remove_all(paths.root, ec);
  if (have_loop_paths) {
    std::filesystem::remove_all(loop_paths.root, ec);
  }
  return failures == 0 ? 0 : 1;
}
