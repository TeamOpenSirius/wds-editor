#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wds::audio {

// Time-domain envelope plus a log-frequency spectrogram for the edit backdrop.
// Time t=0 is music start (same wall-clock as EditViewport::ms_at_y / y_at_ms).
class WaveformOverview {
 public:
  static constexpr int32_t kMsPerBucket = 1;
  static constexpr int kFftSize = 1024;
  static constexpr int kFftHop = 512;
  static constexpr int kSpecBins = 192;
  static constexpr int kMaxSpecTexHeight = 8192;

  WaveformOverview() = default;
  explicit WaveformOverview(std::vector<float> peaks) : peaks_(std::move(peaks)) {}

  void clear() noexcept;
  bool empty() const noexcept { return peaks_.empty(); }
  std::size_t size() const noexcept { return peaks_.size(); }
  int64_t duration_ms() const noexcept {
    return static_cast<int64_t>(peaks_.size()) * kMsPerBucket;
  }

  bool has_spectrogram() const noexcept {
    return spec_frames_ > 0 && spec_bins_ > 0 &&
           spec_mag_l_.size() == static_cast<std::size_t>(spec_frames_) *
                                     static_cast<std::size_t>(spec_bins_) &&
           spec_mag_r_.size() == spec_mag_l_.size();
  }
  int spec_bins() const noexcept { return spec_bins_; }
  int spec_frames() const noexcept { return spec_frames_; }
  double spec_hop_ms() const noexcept { return spec_hop_ms_; }

  // Decode `path` through a dedicated BASS decode stream. Requires BASS_Init
  // (AudioEngine already running). Empty path clears. False on failure (clears).
  // When `cancel` is set, hop/chunk loops abort and return false (clears).
  bool load(const std::string& path, const std::atomic<bool>* cancel = nullptr);

  // Max peak in half-open [ms_lo, ms_hi). 0 when empty or fully outside audio.
  float peak_in_range(double ms_lo, double ms_hi) const noexcept;

  // Packed RGBA8 spectrogram (row 0 = t=0). Width is `2 * bins`:
  // left half = left channel high→low toward center; right half = right channel
  // low→high from center. `out_w` / `out_h` are set on success.
  bool rasterize_rgba(std::vector<unsigned char>& out, int& out_w, int& out_h) const;

  // Test helper: row-major uint8 magnitudes, each `frames * bins` long.
  // Empty `mag_r` copies `mag_l` (mono mirrored to both halves).
  void set_spectrogram_for_test(int bins, int frames, double hop_ms, std::vector<std::uint8_t> mag_l,
                               std::vector<std::uint8_t> mag_r = {});

 private:
  std::vector<float> peaks_;
  std::vector<std::uint8_t> spec_mag_l_;
  std::vector<std::uint8_t> spec_mag_r_;
  int spec_bins_ = 0;
  int spec_frames_ = 0;
  double spec_hop_ms_ = 0.0;
};

}  // namespace wds::audio
