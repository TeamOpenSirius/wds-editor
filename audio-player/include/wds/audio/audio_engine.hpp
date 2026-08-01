#pragma once

#include "hit_sfx.hpp"

#include <wds/common/time.hpp>

#include <cstdint>
#include <string>

namespace wds::audio {

// Linked BASS library version (same packing as BASS_GetVersion). 0 if the
// symbol is unavailable. UI/startup must use this instead of including bass.h.
uint32_t linked_bass_version() noexcept;

// BASS-backed audio engine: one device, optional music stream, predecoded SFX samples.
class AudioEngine {
 public:
  AudioEngine() = default;
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // effects_directory may be empty (SFX optional). music_path may be empty (wall-clock fallback).
  bool initialize(const std::string& effects_directory, const std::string& music_path);
  void shutdown();

  bool ready() const noexcept { return ready_; }
  bool has_music() const noexcept { return music_ != 0; }
  bool sfx_ready() const noexcept { return sfx_ready_; }

  // --- position ---
  wds::common::Microseconds position() const;
  int64_t position_ms() const noexcept { return wds::common::us_to_ms_round(position()); }
  // Decoder/mixer clock. POS syncs are driven by rendering — if decode has already
  // passed a hit byte, ChannelSetSync will never fire (must play immediately).
  wds::common::Microseconds decode_position() const;
  int64_t decode_position_ms() const noexcept {
    return wds::common::us_to_ms_round(decode_position());
  }
  wds::common::Microseconds duration() const;
  int64_t duration_ms() const noexcept { return wds::common::us_to_ms_round(duration()); }
  bool set_position(wds::common::Microseconds time);
  bool set_position_ms(int64_t time_ms) { return set_position(wds::common::ms_to_us(time_ms)); }
  // Bumps on set_position and begin_timeline_control (seek / scrub / play resync).
  // UI uses this instead of inferring seeks from BASS playtime regressions.
  uint64_t position_generation() const noexcept { return position_generation_; }
  // Silence SFX and bump position_generation_ without moving the music cursor.
  // Used for timeline scrub when there is no BGM stream.
  void begin_timeline_control();

  // --- music ---
  void play_music();
  void pause_music();
  // Intent flag set by play_music/pause_music (not raw device state).
  bool music_active() const noexcept { return music_playing_; }
  // True when the BASS music channel is currently outputting (PLAYING).
  bool stream_playing() const noexcept;
  // True when the stream has stopped (natural end or never started).
  bool stream_stopped() const noexcept;

  // --- SFX ---
  void play_sfx(HitSfxClip clip);
  // Prime Hold channel / ensure keep-alive. Safe to call repeatedly.
  void warmup_sfx();
  // Arm a one-shot at an absolute music playtime. Fired via BASS_SYNC_POS on a
  // separate sample voice (1× pitch) so BGM BASS_ATTRIB_FREQ does not stretch
  // hits. Falls back to immediate play when there is no music.
  void schedule_sfx_at(HitSfxClip clip, wds::common::Microseconds at);
  void schedule_sfx_after(HitSfxClip clip, wds::common::Microseconds delay);
  void clear_scheduled_sfx();
  void set_hold_looping(bool enabled);
  // Stop currently audible sample voices (one-shots / Hold). Pending music
  // syncs are cleared via clear_scheduled_sfx.
  void stop_playing_sfx();
  void stop_all_sfx();

  // --- volume (0..1) ---
  void set_master_gain(float gain);
  void set_music_gain(float gain);
  void set_sfx_gain(float gain);
  float master_gain() const noexcept { return master_gain_; }
  float music_gain() const noexcept { return music_gain_; }
  float sfx_gain() const noexcept { return sfx_gain_; }

  // Playback rate for BGM only (pitch scales with rate via BASS_ATTRIB_FREQ). SFX stay at 1x.
  void set_playback_rate(float rate);
  float playback_rate() const noexcept { return playback_rate_; }

  // BASS mixtime/playtime sync entry — not for UI callers.
  void handle_sfx_sync(unsigned long long sync_handle);

 private:
  void apply_music_volume();
  void apply_music_rate();
  void apply_sfx_volume();
  float effective_music_volume() const noexcept;
  float effective_sfx_volume() const noexcept;
  void ensure_keep_alive();
  void pause_keep_alive();
  void play_sfx_internal(HitSfxClip clip, bool lock_music);
  void cache_music_format();
  std::uint64_t align_music_bytes(std::uint64_t bytes) const noexcept;

  struct Impl;
  Impl* impl_ = nullptr;
  bool ready_ = false;
  bool sfx_ready_ = false;
  bool music_playing_ = false;
  unsigned long long music_ = 0;  // HSTREAM as opaque
  float master_gain_ = 1.0f;
  float music_gain_ = 1.0f;
  float sfx_gain_ = 1.0f;
  float playback_rate_ = 1.0f;
  float music_base_freq_ = 0.0f;
  uint64_t position_generation_ = 0;
};

}  // namespace wds::audio
