#include "wds/audio/audio_engine.hpp"
#include "wds/audio/sfx_sync_policy.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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
};

bool make_fixture(FixturePaths& paths) {
  namespace fs = std::filesystem;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  paths.root = fs::temp_directory_path() / ("wds-sfx-pool-" + std::to_string(stamp));
  paths.fx_dir = paths.root / "effects";
  paths.music_path = paths.root / "music.wav";
  const fs::path perfect = paths.fx_dir / "_PERFECT.ogg";
  std::error_code ec;
  fs::create_directories(paths.fx_dir, ec);
  if (ec || !write_pcm16_wav(paths.music_path, 12) || !write_pcm16_wav(perfect, 1)) {
    std::fprintf(stderr, "skip: could not write SFX sync pool WAV fixture\n");
    fs::remove_all(paths.root, ec);
    return false;
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

}  // namespace

int main() {
  FixturePaths paths;
  if (!make_fixture(paths)) {
    return 0;
  }

  {
    AudioEngine engine;
    if (init_or_skip(engine, paths)) {
      test_stale_slot_after_clear(engine);
      engine.shutdown();
    }
  }

  test_shutdown_with_hammer_thread(paths);
  test_shutdown_releases_pool(paths);

  std::error_code ec;
  std::filesystem::remove_all(paths.root, ec);
  return failures == 0 ? 0 : 1;
}
