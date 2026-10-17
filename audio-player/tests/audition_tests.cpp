#include <wds/audio/audio_engine.hpp>
#include <wds/common/utf8_path.hpp>
#include <bass.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void wav(const std::filesystem::path& path, int seconds, int rate, bool floating) {
  std::ofstream file(path, std::ios::binary);
  const auto wr16 = [&](std::uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
  const auto wr32 = [&](std::uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
  const int bytes = floating ? 4 : 2;
  const int size = seconds * rate * 2 * bytes;
  file.write("RIFF", 4); wr32(36 + size); file.write("WAVEfmt ", 8);
  wr32(16); wr16(floating ? 3 : 1); wr16(2); wr32(rate);
  wr32(rate * 2 * bytes); wr16(2 * bytes); wr16(bytes * 8);
  file.write("data", 4); wr32(size);
  std::vector<char> zero(size, 0);
  file.write(zero.data(), zero.size());
}
}

int main() {
  namespace fs = std::filesystem;
  using wds::common::Microseconds;
  using wds::common::path_to_utf8;
  const auto root = fs::temp_directory_path() / ("wds-audition-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  fs::create_directories(root / "effects");
  wav(root / "effects" / "_PERFECT.ogg", 1, 44100, false);
  wav(root / "original.wav", 8, 48000, false);
  wav(root / "stem.wav", 8, 44100, true);
  wav(root / "wrong-length.wav", 3, 44100, true);
  wds::audio::AudioEngine engine;
  if (!engine.initialize(path_to_utf8(root / "effects"), path_to_utf8(root / "original.wav"))) {
    std::fprintf(stderr, "SKIP: no BASS output device\n");
    fs::remove_all(root);
    return 77;
  }
  expect(engine.has_music(), "fixture music loaded");
  const auto duration = engine.duration();
  engine.set_music_gain(.3f);
  engine.set_playback_rate(.75f);
  expect(engine.set_position(Microseconds{2'000'000}), "initial seek");
  expect(engine.schedule_sfx_at(wds::audio::HitSfxClip::Perfect, Microseconds{5'000'000}), "arm hit before switch");
  const auto generation = engine.position_generation();
  expect(engine.replace_music_source(path_to_utf8(root / "stem.wav")), "swap PCM16/48k to float/44.1k");
  expect(std::abs(engine.position().count() - 2'000'000) < 1000, "paused source switch preserves time");
  expect(engine.duration() == duration, "source switch preserves song duration");
  expect(engine.position_generation() > generation, "switch invalidates scheduled hits");
  expect(engine.pending_sfx_sync_count() == 0, "old source hit callbacks cleared");
  expect(engine.schedule_sfx_at(wds::audio::HitSfxClip::Perfect, Microseconds{5'000'000}), "rearm hit on new source");
  expect(engine.pending_sfx_sync_count() == 1, "new source owns pending hit");
  expect(!engine.stream_playing(), "paused state preserved");
  expect(!engine.replace_music_source(path_to_utf8(root / "missing.wav")), "missing stem rejected");
  expect(!engine.replace_music_source(path_to_utf8(root / "wrong-length.wav")), "wrong duration rejected");
  expect(engine.duration() == duration, "failed swap keeps valid source");
  expect(engine.pending_sfx_sync_count() == 1, "rejected swap preserves pending hits");
  expect(engine.play_music(), "play stem");
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  const auto before = engine.position();
  expect(engine.replace_music_source(path_to_utf8(root / "original.wav")), "return to original while playing");
  const auto after = engine.position();
  expect(std::abs(after.count() - before.count()) < 30000, "playing switch keeps heard position");
  expect(engine.stream_playing(), "playing state preserved");
  // Compare song ∆t to measured wall time. A fixed 200 ms sleep + absolute µs window
  // flakes on shared CI when sleep overruns (0.75×280 ms wall ≈ 210 ms song > 195 ms).
  const auto wall0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto wall1 = std::chrono::steady_clock::now();
  const auto wall_us =
      std::chrono::duration_cast<std::chrono::microseconds>(wall1 - wall0).count();
  const auto elapsed = engine.position().count() - after.count();
  const double ratio =
      wall_us > 0 ? static_cast<double>(elapsed) / static_cast<double>(wall_us) : 0.0;
  // 0.75× with slack for BASS device buffering / runner scheduling noise.
  if (!(ratio > 0.50 && ratio < 0.95)) {
    std::fprintf(stderr, "FAIL: 0.75x rate survives audition switching (ratio=%.3f song=%lld wall=%lld)\n",
                 ratio, static_cast<long long>(elapsed), static_cast<long long>(wall_us));
    ++failures;
  }
  engine.pause_music();
  expect(engine.set_position(Microseconds{4'000'000}), "seek after switching");
  expect(std::abs(engine.position().count() - 4'000'000) < 1000, "seek uses current source format");
  engine.shutdown();
  fs::remove_all(root);
  return failures ? 1 : 0;
}
