#include "wds/audio/audio_engine.hpp"
#include <wds/common/log.hpp>

#include "bass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace wds::audio {

namespace {

namespace fs = std::filesystem;

// UTF-8 path → filesystem path. On Windows, narrow `std::string` paths are not UTF-8.
fs::path path_from_utf8(const std::string& utf8) {
#if defined(_WIN32)
  return fs::u8path(utf8);
#else
  return fs::path(utf8);
#endif
}

#if defined(_WIN32)
// BASS_UNICODE expects a UTF-16LE path pointer (wchar_t on Windows).
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
      cp |= static_cast<unsigned char>(utf8[i + 2]) & 0x3F;
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

HSAMPLE sample_from_path(const fs::path& path, DWORD max_ch, DWORD flags) {
  const std::wstring wide = path.wstring();
  return BASS_SampleLoad(FALSE, wide.c_str(), 0, 0, max_ch, flags | BASS_UNICODE);
}
#else
HSTREAM stream_from_file(const std::string& utf8_path, DWORD flags) {
  return BASS_StreamCreateFile(FALSE, utf8_path.c_str(), 0, 0, flags);
}

HSAMPLE sample_from_path(const fs::path& path, DWORD max_ch, DWORD flags) {
  return BASS_SampleLoad(FALSE, path.string().c_str(), 0, 0, max_ch, flags);
}
#endif

const char* filename_for(HitSfxClip clip) noexcept {
  switch (clip) {
    case HitSfxClip::Perfect:
      return "_PERFECT.ogg";
    case HitSfxClip::Great:
      return "_PERFECT_ALTERNATIVE.ogg";
    case HitSfxClip::Good:
      return "_GOOD.ogg";
    case HitSfxClip::Bad:
      return "_GOOD_ALTERNATIVE.ogg";
    case HitSfxClip::Scratch:
      return "Sirius Scratch.ogg";
    case HitSfxClip::Critical:
      return "Sirius Critical.ogg";
    case HitSfxClip::Sound:
      return "Sirius Sound.ogg";
    case HitSfxClip::Hold:
      return "_HOLD.ogg";
    case HitSfxClip::Stage:
      return "_STAGE.ogg";
    default:
      return "_PERFECT.ogg";
  }
}

wds::common::Microseconds bytes_to_us(DWORD handle, QWORD bytes) {
  if (handle == 0) {
    return wds::common::Microseconds{0};
  }
  const double sec = BASS_ChannelBytes2Seconds(handle, bytes);
  if (sec < 0.0) {
    return wds::common::Microseconds{0};
  }
  return wds::common::Microseconds{static_cast<int64_t>(sec * 1'000'000.0 + 0.5)};
}

QWORD us_to_bytes(DWORD handle, wds::common::Microseconds time) {
  if (handle == 0) {
    return 0;
  }
  const double sec = static_cast<double>(std::max<int64_t>(0, time.count())) / 1'000'000.0;
  return BASS_ChannelSeconds2Bytes(handle, sec);
}

float clamp_gain(float gain) noexcept {
  return std::clamp(gain, 0.0f, 1.0f);
}

// Keeps the output device decoding even while music is paused (pairs with
// BASS_CONFIG_DEV_NONSTOP) so the first audible hit after a long pause is not cold.
DWORD CALLBACK silence_keep_alive_proc(HSTREAM /*handle*/, void* buffer, DWORD length,
                                       void* /*user*/) {
  if (buffer != nullptr && length > 0) {
    std::memset(buffer, 0, length);
  }
  return length;
}

}  // namespace

struct AudioEngine::Impl {
  bool device_ok = false;
  HSTREAM music = 0;
  HSTREAM keep_alive = 0;
  DWORD music_chans = 0;
  DWORD music_bpf = 0;  // bytes per frame
  std::array<HSAMPLE, static_cast<size_t>(HitSfxClip::Count)> samples{};
  std::array<bool, static_cast<size_t>(HitSfxClip::Count)> sample_ok{};
  HCHANNEL hold_ch = 0;
  bool hold_playing = false;

  // Music-playtime syncs → ChannelPlay at 1× (independent of BGM FREQ).
  struct PendingSync {
    HSYNC handle = 0;
    HitSfxClip clip = HitSfxClip::Count;
  };

  std::mutex sfx_mu;
  std::vector<PendingSync> pending_syncs;
};

namespace {

void CALLBACK sfx_pos_sync_proc(HSYNC handle, DWORD /*channel*/, DWORD /*data*/, void* user) {
  auto* engine = static_cast<AudioEngine*>(user);
  if (engine == nullptr) {
    return;
  }
  engine->handle_sfx_sync(static_cast<unsigned long long>(handle));
}

}  // namespace

AudioEngine::~AudioEngine() { shutdown(); }

uint32_t linked_bass_version() noexcept {
  return static_cast<uint32_t>(BASS_GetVersion());
}

bool AudioEngine::initialize(const std::string& effects_directory, const std::string& music_path) {
  shutdown();

  auto* impl = new Impl();
  // Sample-accurate SFX lives in the music DSP. Short update period keeps the
  // playhead staircase small (helps UI clock sync) and decode latency stable;
  // very large periods let device buffering "breathe". 5ms is BASS's floor and
  // cheap for a single desktop stream (DEV_PERIOD stays 10).
  BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 5);
  BASS_SetConfig(BASS_CONFIG_DEV_NONSTOP, TRUE);  // do not stop device when idle
  BASS_SetConfig(BASS_CONFIG_DEV_PERIOD, 10);
  if (!BASS_Init(-1, 44100, 0, nullptr, nullptr)) {
    WDS_LOG("AudioEngine: BASS_Init failed code=%d\n", BASS_ErrorGetCode());
    delete impl;
    return false;
  }
  impl->device_ok = true;
  // Wire impl_ before attribute helpers (apply_music_rate / volume) touch it.
  impl_ = impl;
  impl->pending_syncs.reserve(256);

  // Silent keep-alive: CoreAudio / WASAPI otherwise sleep after pause and the
  // next one-shots land late (cold start / collapsed 32nd gaps).
  impl->keep_alive = BASS_StreamCreate(44100, 2, 0, silence_keep_alive_proc, nullptr);
  if (impl->keep_alive != 0) {
    BASS_ChannelSetAttribute(impl->keep_alive, BASS_ATTRIB_VOL, 0.0f);
    if (!BASS_ChannelPlay(impl->keep_alive, TRUE)) {
      WDS_LOG("AudioEngine: keep-alive play failed code=%d\n", BASS_ErrorGetCode());
      BASS_StreamFree(impl->keep_alive);
      impl->keep_alive = 0;
    }
  } else {
    WDS_LOG("AudioEngine: keep-alive create failed code=%d\n", BASS_ErrorGetCode());
  }

  if (!music_path.empty()) {
    std::error_code ec;
    const fs::path music_fs = path_from_utf8(music_path);
    if (fs::is_regular_file(music_fs, ec) && !ec) {
      const HSTREAM music = stream_from_file(music_path, BASS_STREAM_PRESCAN);
      if (music != 0) {
        impl->music = music;
        music_ = static_cast<unsigned long long>(music);
        float freq = 0.0f;
        if (BASS_ChannelGetAttribute(music, BASS_ATTRIB_FREQ, &freq)) {
          music_base_freq_ = freq;
        } else {
          music_base_freq_ = 44100.0f;
        }
        apply_music_rate();
        WDS_LOG("AudioEngine: music loaded %s\n", music_path.c_str());
      } else {
        WDS_LOG("AudioEngine: music load failed %s code=%d\n", music_path.c_str(),
                BASS_ErrorGetCode());
      }
    } else {
      WDS_LOG("AudioEngine: music missing %s\n", music_path.c_str());
    }
  }

  int loaded = 0;
  if (!effects_directory.empty()) {
    const fs::path dir = path_from_utf8(effects_directory);
    std::error_code ec;
    if (fs::is_directory(dir, ec) && !ec) {
      for (uint8_t i = 0; i < static_cast<uint8_t>(HitSfxClip::Count); ++i) {
        const auto clip = static_cast<HitSfxClip>(i);
        const fs::path path = dir / filename_for(clip);
        if (!fs::is_regular_file(path, ec) || ec) {
          WDS_LOG("AudioEngine: missing SFX %s\n", path.string().c_str());
          continue;
        }
        // One-shots play on their own voices (never mixed into FREQ-scaled BGM).
        // OVER_POS: when NEW voices are exhausted, play_sfx_internal falls back to a
        // recycled channel and steals the oldest rather than dropping the hit.
        const DWORD flags =
            (clip == HitSfxClip::Hold) ? BASS_SAMPLE_LOOP : BASS_SAMPLE_OVER_POS;
        const DWORD max_ch = (clip == HitSfxClip::Hold) ? 1 : 128;
        const HSAMPLE sample = sample_from_path(path, max_ch, flags);
        if (sample == 0) {
          WDS_LOG("AudioEngine: SampleLoad failed %s code=%d\n", path.string().c_str(),
                  BASS_ErrorGetCode());
          continue;
        }
        impl->samples[i] = sample;
        impl->sample_ok[i] = true;
        ++loaded;
        if (clip == HitSfxClip::Hold) {
          impl->hold_ch = BASS_SampleGetChannel(sample, FALSE);
          if (impl->hold_ch == 0) {
            WDS_LOG("AudioEngine: Hold channel failed code=%d\n", BASS_ErrorGetCode());
          }
        }
      }
    } else {
      WDS_LOG("AudioEngine: effects dir missing: %s\n", effects_directory.c_str());
    }
  }

  sfx_ready_ = loaded > 0;
  ready_ = true;
  cache_music_format();
  apply_music_volume();
  apply_sfx_volume();
  warmup_sfx();
  WDS_LOG("AudioEngine: ready music=%d sfx_clips=%d\n", has_music() ? 1 : 0, loaded);
  return true;
}

void AudioEngine::shutdown() {
  if (impl_ == nullptr) {
    ready_ = false;
    sfx_ready_ = false;
    music_playing_ = false;
    music_ = 0;
    return;
  }

  stop_all_sfx();

  if (impl_->keep_alive != 0) {
    BASS_ChannelStop(impl_->keep_alive);
    BASS_StreamFree(impl_->keep_alive);
    impl_->keep_alive = 0;
  }

  if (impl_->music != 0) {
    BASS_ChannelStop(impl_->music);
    BASS_StreamFree(impl_->music);
    impl_->music = 0;
  }
  music_ = 0;
  music_playing_ = false;
  music_base_freq_ = 0.0f;

  for (auto& sample : impl_->samples) {
    if (sample != 0) {
      BASS_SampleFree(sample);
      sample = 0;
    }
  }
  impl_->hold_ch = 0;
  impl_->hold_playing = false;

  if (impl_->device_ok) {
    BASS_Free();
    impl_->device_ok = false;
  }

  delete impl_;
  impl_ = nullptr;
  ready_ = false;
  sfx_ready_ = false;
}

wds::common::Microseconds AudioEngine::position() const {
  if (impl_ == nullptr || impl_->music == 0) {
    return wds::common::Microseconds{0};
  }
  return bytes_to_us(impl_->music, BASS_ChannelGetPosition(impl_->music, BASS_POS_BYTE));
}

wds::common::Microseconds AudioEngine::decode_position() const {
  if (impl_ == nullptr || impl_->music == 0) {
    return wds::common::Microseconds{0};
  }
  return bytes_to_us(impl_->music,
                     BASS_ChannelGetPosition(impl_->music, BASS_POS_BYTE | BASS_POS_DECODE));
}

wds::common::Microseconds AudioEngine::duration() const {
  if (impl_ == nullptr || impl_->music == 0) {
    return wds::common::Microseconds{0};
  }
  return bytes_to_us(impl_->music, BASS_ChannelGetLength(impl_->music, BASS_POS_BYTE));
}

void AudioEngine::begin_timeline_control() {
  // Pause / scrub / seek: cut scheduled + audible SFX and invalidate UI mono state
  // via position_generation_ (even when there is no music stream).
  stop_all_sfx();
  ++position_generation_;
}

bool AudioEngine::set_position(wds::common::Microseconds time) {
  if (impl_ == nullptr || impl_->music == 0) {
    return false;
  }
  begin_timeline_control();
  const QWORD pos = us_to_bytes(impl_->music, time);
  return BASS_ChannelSetPosition(impl_->music, pos, BASS_POS_BYTE) != FALSE;
}

void AudioEngine::play_music() {
  if (impl_ == nullptr || impl_->music == 0) {
    music_playing_ = false;
    return;
  }
  // Avoid mixing a silent keep-alive stream alongside BGM — dual-stream device
  // resampling can slowly modulate perceived inter-hit gaps.
  pause_keep_alive();
  apply_music_volume();
  apply_music_rate();
  if (BASS_ChannelPlay(impl_->music, FALSE)) {
    music_playing_ = true;
  } else {
    WDS_LOG("AudioEngine: music play failed code=%d\n", BASS_ErrorGetCode());
    music_playing_ = false;
    ensure_keep_alive();
  }
}

void AudioEngine::pause_music() {
  if (impl_ == nullptr || impl_->music == 0) {
    music_playing_ = false;
    return;
  }
  BASS_ChannelPause(impl_->music);
  music_playing_ = false;
  // Keep the output device decoding while paused so resume is not cold.
  ensure_keep_alive();
}

bool AudioEngine::stream_playing() const noexcept {
  if (impl_ == nullptr || impl_->music == 0) {
    return false;
  }
  return BASS_ChannelIsActive(impl_->music) == BASS_ACTIVE_PLAYING;
}

bool AudioEngine::stream_stopped() const noexcept {
  if (impl_ == nullptr || impl_->music == 0) {
    return true;
  }
  return BASS_ChannelIsActive(impl_->music) == BASS_ACTIVE_STOPPED;
}

float AudioEngine::effective_music_volume() const noexcept {
  return clamp_gain(master_gain_) * clamp_gain(music_gain_);
}

float AudioEngine::effective_sfx_volume() const noexcept {
  return clamp_gain(master_gain_) * clamp_gain(sfx_gain_);
}

void AudioEngine::apply_music_volume() {
  if (impl_ == nullptr || impl_->music == 0) {
    return;
  }
  // Hits use separate sample voices, so music_gain can sit on the BGM channel
  // without muting SFX.
  BASS_ChannelSetAttribute(impl_->music, BASS_ATTRIB_VOL, effective_music_volume());
}

void AudioEngine::apply_music_rate() {
  if (impl_ == nullptr || impl_->music == 0 || music_base_freq_ <= 0.0f) {
    return;
  }
  BASS_ChannelSetAttribute(impl_->music, BASS_ATTRIB_FREQ, music_base_freq_ * playback_rate_);
}

void AudioEngine::apply_sfx_volume() {
  if (impl_ == nullptr) {
    return;
  }
  const float vol = effective_sfx_volume();
  for (uint8_t i = 0; i < static_cast<uint8_t>(HitSfxClip::Count); ++i) {
    if (!impl_->sample_ok[i]) {
      continue;
    }
    BASS_SAMPLE info{};
    if (BASS_SampleGetInfo(impl_->samples[i], &info)) {
      info.volume = vol;
      BASS_SampleSetInfo(impl_->samples[i], &info);
    }
  }
  if (impl_->hold_ch != 0) {
    BASS_ChannelSetAttribute(impl_->hold_ch, BASS_ATTRIB_VOL, vol);
  }
}

void AudioEngine::set_master_gain(float gain) {
  master_gain_ = clamp_gain(gain);
  apply_music_volume();
  apply_sfx_volume();
}

void AudioEngine::set_music_gain(float gain) {
  music_gain_ = clamp_gain(gain);
  apply_music_volume();
}

void AudioEngine::set_sfx_gain(float gain) {
  sfx_gain_ = clamp_gain(gain);
  apply_sfx_volume();
}

void AudioEngine::set_playback_rate(float rate) {
  playback_rate_ = std::clamp(rate, 0.25f, 2.0f);
  apply_music_rate();
}

bool AudioEngine::play_sfx(HitSfxClip clip) {
  return play_sfx_internal(clip, /*lock_music=*/false);
}

void AudioEngine::ensure_keep_alive() {
  if (impl_ == nullptr || impl_->keep_alive == 0) {
    return;
  }
  if (BASS_ChannelIsActive(impl_->keep_alive) != BASS_ACTIVE_PLAYING) {
    BASS_ChannelSetAttribute(impl_->keep_alive, BASS_ATTRIB_VOL, 0.0f);
    BASS_ChannelPlay(impl_->keep_alive, TRUE);
  }
}

void AudioEngine::pause_keep_alive() {
  if (impl_ == nullptr || impl_->keep_alive == 0) {
    return;
  }
  if (BASS_ChannelIsActive(impl_->keep_alive) == BASS_ACTIVE_PLAYING) {
    BASS_ChannelPause(impl_->keep_alive);
  }
}

std::uint64_t AudioEngine::align_music_bytes(std::uint64_t bytes) const noexcept {
  if (impl_ == nullptr || impl_->music_bpf == 0) {
    return bytes;
  }
  return (bytes / impl_->music_bpf) * impl_->music_bpf;
}

bool AudioEngine::play_sfx_internal(HitSfxClip clip, bool lock_music) {
  if (!sfx_ready_ || impl_ == nullptr || clip == HitSfxClip::Hold || clip == HitSfxClip::Count) {
    return false;
  }
  const size_t idx = static_cast<size_t>(clip);
  if (!impl_->sample_ok[idx]) {
    return false;
  }
  // BGM already keeps the device hot. Starting keep-alive alongside music causes
  // dual-stream resampling that can intermittently drop or smear hit attacks.
  if (music_playing_) {
    pause_keep_alive();
  } else {
    ensure_keep_alive();
  }
  // Prefer a fresh voice so rapid same-clip hits (e.g. 32nds) do not restart an
  // older channel. If the sample's max polyphony is exhausted, fall back to a
  // recycled OVER_POS voice so the new hit is heard instead of silently dropped.
  HCHANNEL ch = BASS_SampleGetChannel(impl_->samples[idx], BASS_SAMCHAN_NEW);
  if (ch == 0) {
    ch = BASS_SampleGetChannel(impl_->samples[idx], 0);
  }
  if (ch == 0) {
    return false;
  }
  BASS_ChannelSetAttribute(ch, BASS_ATTRIB_VOL, effective_sfx_volume());
  const bool do_lock = lock_music && impl_->music != 0;
  if (do_lock) {
    BASS_ChannelLock(impl_->music, TRUE);
  }
  const BOOL ok = BASS_ChannelPlay(ch, TRUE);
  if (do_lock) {
    BASS_ChannelLock(impl_->music, FALSE);
  }
  return ok != FALSE;
}

void AudioEngine::cache_music_format() {
  if (impl_ == nullptr || impl_->music == 0) {
    if (impl_ != nullptr) {
      impl_->music_chans = 0;
      impl_->music_bpf = 0;
    }
    return;
  }
  BASS_CHANNELINFO ci{};
  if (!BASS_ChannelGetInfo(impl_->music, &ci) || ci.chans == 0) {
    impl_->music_chans = 0;
    impl_->music_bpf = 0;
    return;
  }
  impl_->music_chans = ci.chans;
  DWORD bytes_per_sample = 2;
  if ((ci.flags & BASS_SAMPLE_FLOAT) != 0) {
    bytes_per_sample = sizeof(float);
  } else if ((ci.flags & BASS_SAMPLE_8BITS) != 0) {
    bytes_per_sample = 1;
  }
  impl_->music_bpf = bytes_per_sample * ci.chans;
}

void AudioEngine::handle_sfx_sync(unsigned long long sync_handle) {
  if (impl_ == nullptr) {
    return;
  }
  HitSfxClip clip = HitSfxClip::Count;
  {
    std::lock_guard<std::mutex> lock(impl_->sfx_mu);
    auto& syncs = impl_->pending_syncs;
    for (size_t i = 0; i < syncs.size(); ++i) {
      if (static_cast<unsigned long long>(syncs[i].handle) == sync_handle) {
        clip = syncs[i].clip;
        syncs.erase(syncs.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
  }
  if (clip != HitSfxClip::Count) {
    play_sfx_internal(clip, /*lock_music=*/false);
  }
}

void AudioEngine::warmup_sfx() {
  if (!sfx_ready_ || impl_ == nullptr) {
    return;
  }
  ensure_keep_alive();
  // One-shots use ChannelPlay; only prime the Hold loop channel.
  const size_t hold_idx = static_cast<size_t>(HitSfxClip::Hold);
  if (!impl_->sample_ok[hold_idx] || impl_->hold_ch != 0) {
    return;
  }
  impl_->hold_ch = BASS_SampleGetChannel(impl_->samples[hold_idx], FALSE);
  if (impl_->hold_ch == 0) {
    return;
  }
  BASS_ChannelSetAttribute(impl_->hold_ch, BASS_ATTRIB_VOL, 0.0f);
  BASS_ChannelPlay(impl_->hold_ch, TRUE);
  BASS_ChannelPause(impl_->hold_ch);
  BASS_ChannelSetAttribute(impl_->hold_ch, BASS_ATTRIB_VOL, effective_sfx_volume());
}

bool AudioEngine::schedule_sfx_at(HitSfxClip clip, wds::common::Microseconds at) {
  if (!sfx_ready_ || impl_ == nullptr || clip == HitSfxClip::Hold || clip == HitSfxClip::Count) {
    return false;
  }
  if (!impl_->sample_ok[static_cast<size_t>(clip)]) {
    return false;
  }
  if (impl_->music == 0) {
    return play_sfx_internal(clip, /*lock_music=*/false);
  }

  // Separate sample voices keep hit pitch at 1× while BGM uses BASS_ATTRIB_FREQ.
  // POS syncs are armed against the render path: if decode already passed `at`,
  // SetSync will never fire — play immediately. Otherwise playtime ONETIME sync
  // fires with audible BGM (not mixtime, which would be early by the buffer).
  const QWORD target = align_music_bytes(us_to_bytes(impl_->music, at));
  auto frontier_bytes = [&]() -> QWORD {
    const QWORD decode_pos =
        BASS_ChannelGetPosition(impl_->music, BASS_POS_BYTE | BASS_POS_DECODE);
    const QWORD play_pos = BASS_ChannelGetPosition(impl_->music, BASS_POS_BYTE);
    return decode_pos > play_pos ? decode_pos : play_pos;
  };
  if (target <= frontier_bytes()) {
    return play_sfx_internal(clip, /*lock_music=*/false);
  }

  const HSYNC sync = BASS_ChannelSetSync(
      impl_->music, BASS_SYNC_POS | BASS_SYNC_ONETIME, target, sfx_pos_sync_proc, this);
  if (sync == 0) {
    WDS_LOG("AudioEngine: ChannelSetSync failed code=%d\n", BASS_ErrorGetCode());
    return play_sfx_internal(clip, /*lock_music=*/false);
  }

  // TOCTOU: decode may pass `target` between the frontier check and SetSync.
  // A playtime ONETIME sync set behind the playhead never fires on a non-looping
  // stream — fall back to immediate play.
  if (target <= frontier_bytes()) {
    BASS_ChannelRemoveSync(impl_->music, sync);
    return play_sfx_internal(clip, /*lock_music=*/false);
  }

  std::lock_guard<std::mutex> lock(impl_->sfx_mu);
  impl_->pending_syncs.push_back(Impl::PendingSync{sync, clip});
  return true;
}

bool AudioEngine::schedule_sfx_after(HitSfxClip clip, wds::common::Microseconds delay) {
  if (!sfx_ready_ || impl_ == nullptr) {
    return false;
  }
  if (impl_->music == 0) {
    if (delay.count() <= 0) {
      return play_sfx_internal(clip, /*lock_music=*/false);
    }
    return false;
  }
  if (delay.count() <= 0) {
    return schedule_sfx_at(clip, position());
  }
  return schedule_sfx_at(clip, position() + delay);
}

void AudioEngine::clear_scheduled_sfx() {
  if (impl_ == nullptr) {
    return;
  }
  std::vector<HSYNC> to_remove;
  {
    std::lock_guard<std::mutex> lock(impl_->sfx_mu);
    to_remove.reserve(impl_->pending_syncs.size());
    for (const auto& s : impl_->pending_syncs) {
      if (s.handle != 0) {
        to_remove.push_back(s.handle);
      }
    }
    impl_->pending_syncs.clear();
  }
  if (impl_->music != 0) {
    for (HSYNC sync : to_remove) {
      BASS_ChannelRemoveSync(impl_->music, sync);
    }
  }
}

void AudioEngine::set_hold_looping(bool enabled) {
  if (!sfx_ready_ || impl_ == nullptr) {
    return;
  }
  const size_t hold_idx = static_cast<size_t>(HitSfxClip::Hold);
  if (!impl_->sample_ok[hold_idx]) {
    return;
  }
  if (enabled == impl_->hold_playing) {
    return;
  }
  if (enabled) {
    // BASS_ChannelStop frees sample channel handles. After pause/EOS, stop_all_sfx
    // nulls hold_ch; acquire a fresh channel before playing again.
    if (impl_->hold_ch == 0) {
      impl_->hold_ch = BASS_SampleGetChannel(impl_->samples[hold_idx], FALSE);
      if (impl_->hold_ch == 0) {
        WDS_LOG("AudioEngine: Hold channel acquire failed code=%d\n", BASS_ErrorGetCode());
        return;
      }
    }
    BASS_ChannelSetAttribute(impl_->hold_ch, BASS_ATTRIB_VOL, effective_sfx_volume());
    BASS_ChannelSetPosition(impl_->hold_ch, 0, BASS_POS_BYTE);
    if (!BASS_ChannelPlay(impl_->hold_ch, TRUE)) {
      impl_->hold_ch = BASS_SampleGetChannel(impl_->samples[hold_idx], FALSE);
      if (impl_->hold_ch == 0 || !BASS_ChannelPlay(impl_->hold_ch, TRUE)) {
        WDS_LOG("AudioEngine: Hold ChannelPlay failed code=%d\n", BASS_ErrorGetCode());
        impl_->hold_ch = 0;
        return;
      }
    }
    impl_->hold_playing = true;
  } else {
    if (impl_->hold_ch != 0) {
      BASS_ChannelPause(impl_->hold_ch);
    }
    impl_->hold_playing = false;
  }
}

void AudioEngine::stop_playing_sfx() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->hold_ch != 0) {
    BASS_ChannelStop(impl_->hold_ch);
    impl_->hold_ch = 0;
    impl_->hold_playing = false;
  }
  for (uint8_t i = 0; i < static_cast<uint8_t>(HitSfxClip::Count); ++i) {
    if (static_cast<HitSfxClip>(i) == HitSfxClip::Hold) {
      continue;
    }
    if (impl_->sample_ok[i]) {
      BASS_SampleStop(impl_->samples[i]);
    }
  }
}

void AudioEngine::stop_all_sfx() {
  clear_scheduled_sfx();
  stop_playing_sfx();
}

}  // namespace wds::audio
