#include "wds/audio/waveform_overview.hpp"

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <wds/common/log.hpp>

#include "bass.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wds::audio {
namespace {

namespace fs = std::filesystem;

constexpr float kPi = 3.14159265f;

fs::path path_from_utf8(const std::string& utf8) {
#if defined(_WIN32)
  return fs::u8path(utf8);
#else
  return fs::path(utf8);
#endif
}

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string& utf8) {
  std::wstring out;
  out.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = 0;
    size_t n = 0;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      cp = (c & 0x1F) << 6;
      cp |= static_cast<unsigned char>(utf8[i + 1]) & 0x3F;
      n = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      cp = (c & 0x0F) << 12;
      cp |= (static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6;
      cp |= (static_cast<unsigned char>(utf8[i + 2]) & 0x3F);
      n = 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
      cp = (c & 0x07) << 18;
      cp |= (static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 12;
      cp |= (static_cast<unsigned char>(utf8[i + 2]) & 0x3F) << 6;
      cp |= static_cast<unsigned char>(utf8[i + 3]) & 0x3F;
      n = 4;
    } else {
      cp = 0xFFFD;
      n = 1;
    }
    i += n;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<wchar_t>(cp));
    }
  }
  return out;
}

HSTREAM stream_from_file(const std::string& utf8_path, DWORD flags) {
  const std::wstring wide = utf8_to_wide(utf8_path);
  return BASS_StreamCreateFile(FALSE, wide.c_str(), 0, 0, flags | BASS_UNICODE);
}
#else
HSTREAM stream_from_file(const std::string& utf8_path, DWORD flags) {
  return BASS_StreamCreateFile(FALSE, utf8_path.c_str(), 0, 0, flags);
}
#endif

void fft_radix2(std::vector<float>& re, std::vector<float>& im) {
  const int n = static_cast<int>(re.size());
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[static_cast<std::size_t>(i)], re[static_cast<std::size_t>(j)]);
      std::swap(im[static_cast<std::size_t>(i)], im[static_cast<std::size_t>(j)]);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const float ang = -2.0f * kPi / static_cast<float>(len);
    const float wlen_re = std::cos(ang);
    const float wlen_im = std::sin(ang);
    for (int i = 0; i < n; i += len) {
      float w_re = 1.0f;
      float w_im = 0.0f;
      for (int j = 0; j < len / 2; ++j) {
        const auto u = static_cast<std::size_t>(i + j);
        const auto v = static_cast<std::size_t>(i + j + len / 2);
        const float t_re = w_re * re[v] - w_im * im[v];
        const float t_im = w_re * im[v] + w_im * re[v];
        re[v] = re[u] - t_re;
        im[v] = im[u] - t_im;
        re[u] += t_re;
        im[u] += t_im;
        const float nw_re = w_re * wlen_re - w_im * wlen_im;
        w_im = w_re * wlen_im + w_im * wlen_re;
        w_re = nw_re;
      }
    }
  }
}

void spec_color(float mag01, float freq_t, unsigned char* rgba) {
  const float t = std::clamp(freq_t, 0.0f, 1.0f);
  const float g = std::clamp(mag01, 0.0f, 1.0f);
  // Low → indigo, mid → teal, high → amber.
  float r = 0.0f, gc = 0.0f, b = 0.0f;
  if (t < 0.5f) {
    const float u = t * 2.0f;
    r = 0.48f + (0.22f - 0.48f) * u;
    gc = 0.42f + (0.88f - 0.42f) * u;
    b = 1.00f + (0.68f - 1.00f) * u;
  } else {
    const float u = (t - 0.5f) * 2.0f;
    r = 0.22f + (1.00f - 0.22f) * u;
    gc = 0.88f + (0.78f - 0.88f) * u;
    b = 0.68f + (0.28f - 0.68f) * u;
  }
  const float gain = 0.58f + 0.42f * std::pow(g, 0.42f);
  const float a = std::pow(g, 0.42f) * 0.88f;
  rgba[0] = static_cast<unsigned char>(std::clamp(r * gain, 0.0f, 1.0f) * 255.0f + 0.5f);
  rgba[1] = static_cast<unsigned char>(std::clamp(gc * gain, 0.0f, 1.0f) * 255.0f + 0.5f);
  rgba[2] = static_cast<unsigned char>(std::clamp(b * gain, 0.0f, 1.0f) * 255.0f + 0.5f);
  rgba[3] = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
}

std::uint8_t mag_to_u8(float mag, int fft_size) {
  const float n = std::max(1.0f, static_cast<float>(fft_size));
  const float db = 20.0f * std::log10(mag * 2.0f / n + 1.0e-8f);
  const float n01 = std::clamp((db + 56.0f) / 56.0f, 0.0f, 1.0f);
  return static_cast<std::uint8_t>(n01 * 255.0f + 0.5f);
}

}  // namespace

void WaveformOverview::clear() noexcept {
  peaks_.clear();
  spec_mag_l_.clear();
  spec_mag_r_.clear();
  spec_bins_ = 0;
  spec_frames_ = 0;
  spec_hop_ms_ = 0.0;
}

void WaveformOverview::set_spectrogram_for_test(int bins, int frames, double hop_ms,
                                                std::vector<std::uint8_t> mag_l,
                                                std::vector<std::uint8_t> mag_r) {
  spec_bins_ = std::max(0, bins);
  spec_frames_ = std::max(0, frames);
  spec_hop_ms_ = std::max(0.0, hop_ms);
  const std::size_t want =
      static_cast<std::size_t>(spec_bins_) * static_cast<std::size_t>(spec_frames_);
  if (mag_l.size() != want) {
    spec_mag_l_.clear();
    spec_mag_r_.clear();
    spec_bins_ = 0;
    spec_frames_ = 0;
    return;
  }
  if (mag_r.empty()) mag_r = mag_l;
  if (mag_r.size() != want) {
    spec_mag_l_.clear();
    spec_mag_r_.clear();
    spec_bins_ = 0;
    spec_frames_ = 0;
    return;
  }
  spec_mag_l_ = std::move(mag_l);
  spec_mag_r_ = std::move(mag_r);
}

float WaveformOverview::peak_in_range(double ms_lo, double ms_hi) const noexcept {
  if (peaks_.empty()) return 0.0f;
  if (!std::isfinite(ms_lo) || !std::isfinite(ms_hi)) return 0.0f;
  if (ms_hi < ms_lo) std::swap(ms_lo, ms_hi);
  const double dur = static_cast<double>(peaks_.size());
  if (ms_hi <= 0.0 || ms_lo >= dur) return 0.0f;
  ms_lo = std::max(0.0, ms_lo);
  ms_hi = std::min(dur, ms_hi);
  if (ms_hi <= ms_lo) {
    const auto i = static_cast<std::size_t>(ms_lo);
    return i < peaks_.size() ? peaks_[i] : 0.0f;
  }
  const auto begin = static_cast<std::size_t>(ms_lo);
  auto end = static_cast<std::size_t>(std::ceil(ms_hi));
  if (end > peaks_.size()) end = peaks_.size();
  float peak = 0.0f;
  for (std::size_t i = begin; i < end; ++i) {
    peak = std::max(peak, peaks_[i]);
  }
  return peak;
}

bool WaveformOverview::rasterize_rgba(std::vector<unsigned char>& out, int& out_w,
                                      int& out_h) const {
  out.clear();
  out_w = 0;
  out_h = 0;
  if (!has_spectrogram()) return false;
  const int bins = spec_bins_;
  const int frames = spec_frames_;
  const int tex_h = std::clamp(frames, 1, kMaxSpecTexHeight);
  const int tex_w = bins * 2;
  out_w = tex_w;
  out_h = tex_h;
  out.assign(static_cast<std::size_t>(tex_w) * static_cast<std::size_t>(tex_h) * 4u, 0);
  const int denom = std::max(1, bins - 1);
  auto peak_bin = [&](const std::vector<std::uint8_t>& mag, int f0, int f1, int bin) {
    std::uint8_t m = 0;
    for (int f = f0; f < f1; ++f) {
      const std::size_t idx =
          static_cast<std::size_t>(f) * static_cast<std::size_t>(bins) + static_cast<std::size_t>(bin);
      m = std::max(m, mag[idx]);
    }
    return m;
  };
  auto write_px = [&](int x, int y, std::uint8_t m, int bin) {
    spec_color(static_cast<float>(m) / 255.0f, static_cast<float>(bin) / static_cast<float>(denom),
               &out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(tex_w) +
                     static_cast<std::size_t>(x)) *
                    4u]);
  };
  for (int y = 0; y < tex_h; ++y) {
    const int f0 = static_cast<int>(static_cast<std::int64_t>(y) * frames / tex_h);
    int f1 = static_cast<int>(static_cast<std::int64_t>(y + 1) * frames / tex_h);
    if (f1 <= f0) f1 = std::min(frames, f0 + 1);
    for (int x = 0; x < bins; ++x) {
      const int bin_l = bins - 1 - x;
      write_px(x, y, peak_bin(spec_mag_l_, f0, f1, bin_l), bin_l);
      write_px(bins + x, y, peak_bin(spec_mag_r_, f0, f1, x), x);
    }
  }
  return true;
}

bool WaveformOverview::load(const std::string& path) {
  clear();
  if (path.empty()) return true;

  std::error_code ec;
  if (!fs::is_regular_file(path_from_utf8(path), ec) || ec) {
    WDS_LOG("WaveformOverview: missing %s\n", path.c_str());
    return false;
  }

  const HSTREAM dec =
      stream_from_file(path, BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT | BASS_STREAM_PRESCAN);
  if (dec == 0) {
    WDS_LOG("WaveformOverview: decode open failed %s code=%d\n", path.c_str(),
            BASS_ErrorGetCode());
    return false;
  }

  BASS_CHANNELINFO info{};
  if (!BASS_ChannelGetInfo(dec, &info) || info.freq == 0 || info.chans == 0) {
    WDS_LOG("WaveformOverview: ChannelGetInfo failed %s code=%d\n", path.c_str(),
            BASS_ErrorGetCode());
    BASS_StreamFree(dec);
    return false;
  }

  const QWORD bytes = BASS_ChannelGetLength(dec, BASS_POS_BYTE);
  const double sec = bytes == static_cast<QWORD>(-1) ? 0.0 : BASS_ChannelBytes2Seconds(dec, bytes);
  constexpr std::size_t kMaxBuckets = 2ull * 60ull * 60ull * 1000ull;  // 2 hours
  std::size_t buckets = 1;
  if (std::isfinite(sec) && sec > 0.0) {
    buckets = static_cast<std::size_t>(std::ceil(sec * 1000.0));
    buckets = std::clamp(buckets, std::size_t{1}, kMaxBuckets);
  }
  peaks_.assign(buckets, 0.0f);

  const int chans = static_cast<int>(info.chans);
  const double freq = static_cast<double>(info.freq);
  spec_hop_ms_ = 1000.0 * static_cast<double>(kFftHop) / freq;
  spec_bins_ = kSpecBins;

  std::vector<float> ring_l(static_cast<std::size_t>(kFftSize), 0.0f);
  std::vector<float> ring_r(static_cast<std::size_t>(kFftSize), 0.0f);
  std::size_t ring_write = 0;
  std::uint64_t samples_seen = 0;
  std::vector<float> re(static_cast<std::size_t>(kFftSize));
  std::vector<float> im(static_cast<std::size_t>(kFftSize));
  std::vector<float> hann(static_cast<std::size_t>(kFftSize));
  for (int n = 0; n < kFftSize; ++n) {
    hann[static_cast<std::size_t>(n)] =
        0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(n) /
                                static_cast<float>(kFftSize - 1)));
  }
  const int fft_bins = kFftSize / 2;
  const double nyquist = freq * 0.5;
  const double fmin = 40.0;
  constexpr std::size_t kMaxSpecFrames = 2ull * 60ull * 60ull * 200ull;
  const std::size_t spec_reserve =
      std::min(kMaxSpecFrames, buckets / std::max<std::size_t>(1, static_cast<std::size_t>(
                                                                      spec_hop_ms_ + 0.5))) *
      static_cast<std::size_t>(kSpecBins);
  spec_mag_l_.reserve(spec_reserve);
  spec_mag_r_.reserve(spec_reserve);

  auto fill_log_bins = [&](std::uint8_t* dst) {
    for (int col = 0; col < kSpecBins; ++col) {
      const double t0 = static_cast<double>(col) / static_cast<double>(kSpecBins);
      const double t1 = static_cast<double>(col + 1) / static_cast<double>(kSpecBins);
      const double hz0 = fmin * std::pow(nyquist / fmin, t0);
      const double hz1 = fmin * std::pow(nyquist / fmin, t1);
      int b0 = static_cast<int>(hz0 / nyquist * static_cast<double>(fft_bins - 1));
      int b1 = static_cast<int>(hz1 / nyquist * static_cast<double>(fft_bins - 1));
      b0 = std::clamp(b0, 1, fft_bins - 1);
      b1 = std::clamp(b1, b0 + 1, fft_bins);
      float m = 0.0f;
      for (int b = b0; b < b1; ++b) {
        const float rr = re[static_cast<std::size_t>(b)];
        const float ii = im[static_cast<std::size_t>(b)];
        m = std::max(m, std::sqrt(rr * rr + ii * ii));
      }
      dst[col] = mag_to_u8(m, kFftSize);
    }
  };

  auto run_fft = [&](const std::vector<float>& ring) {
    for (int n = 0; n < kFftSize; ++n) {
      const std::size_t src =
          (ring_write + static_cast<std::size_t>(n)) % static_cast<std::size_t>(kFftSize);
      re[static_cast<std::size_t>(n)] = ring[src] * hann[static_cast<std::size_t>(n)];
      im[static_cast<std::size_t>(n)] = 0.0f;
    }
    fft_radix2(re, im);
  };

  auto emit_fft_frame = [&]() {
    if (spec_frames_ >= static_cast<int>(kMaxSpecFrames)) return;
    spec_mag_l_.resize(spec_mag_l_.size() + static_cast<std::size_t>(kSpecBins), 0);
    spec_mag_r_.resize(spec_mag_r_.size() + static_cast<std::size_t>(kSpecBins), 0);
    auto* dst_l = spec_mag_l_.data() + static_cast<std::size_t>(spec_frames_) *
                                           static_cast<std::size_t>(kSpecBins);
    auto* dst_r = spec_mag_r_.data() + static_cast<std::size_t>(spec_frames_) *
                                           static_cast<std::size_t>(kSpecBins);
    run_fft(ring_l);
    fill_log_bins(dst_l);
    run_fft(ring_r);
    fill_log_bins(dst_r);
    ++spec_frames_;
  };

  std::vector<float> buf(16384);
  std::uint64_t pcm_frame = 0;
  float max_peak = 0.0f;

  while (true) {
    const DWORD want = static_cast<DWORD>(buf.size() * sizeof(float));
    const DWORD got = BASS_ChannelGetData(dec, buf.data(), want);
    if (got == static_cast<DWORD>(-1) || got == 0) break;
    const std::size_t nfloat = static_cast<std::size_t>(got) / sizeof(float);
    const std::size_t frames = nfloat / static_cast<std::size_t>(chans);
    for (std::size_t f = 0; f < frames; ++f) {
      float mag = 0.0f;
      const std::size_t base = f * static_cast<std::size_t>(chans);
      for (int c = 0; c < chans; ++c) {
        mag = std::max(mag, std::abs(buf[base + static_cast<std::size_t>(c)]));
      }
      const float left = buf[base];
      const float right = chans >= 2 ? buf[base + 1] : left;

      const std::size_t bucket =
          static_cast<std::size_t>(static_cast<double>(pcm_frame) * 1000.0 / freq);
      if (bucket < peaks_.size()) {
        peaks_[bucket] = std::max(peaks_[bucket], mag);
        max_peak = std::max(max_peak, peaks_[bucket]);
      } else if (bucket < kMaxBuckets) {
        peaks_.resize(bucket + 1, 0.0f);
        peaks_[bucket] = mag;
        max_peak = std::max(max_peak, mag);
      }

      ring_l[ring_write] = left;
      ring_r[ring_write] = right;
      ring_write = (ring_write + 1) % static_cast<std::size_t>(kFftSize);
      ++samples_seen;
      if (samples_seen >= static_cast<std::uint64_t>(kFftSize) &&
          (samples_seen - static_cast<std::uint64_t>(kFftSize)) %
                  static_cast<std::uint64_t>(kFftHop) ==
              0) {
        emit_fft_frame();
      }
      ++pcm_frame;
    }
  }

  BASS_StreamFree(dec);

  if (max_peak > 1.0e-6f) {
    const float inv = 1.0f / max_peak;
    for (float& p : peaks_) p *= inv;
  }

  WDS_LOG("WaveformOverview: loaded %s buckets=%zu spec_frames=%d hop_ms=%.2f\n", path.c_str(),
          peaks_.size(), spec_frames_, spec_hop_ms_);
  return !peaks_.empty();
}

}  // namespace wds::audio
