#pragma once

#include "audio_engine.hpp"

#include <wds/common/timeline.hpp>

#include <cstdint>
#include <string>

namespace wds::audio {

// Audio-master clock driver. User intents are queued and applied only inside poll().
class Transport {
 public:
  Transport() = default;
  ~Transport() = default;

  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  bool initialize(const std::string& effects_directory, const std::string& music_path);
  void shutdown();

  AudioEngine& audio() noexcept { return audio_; }
  const AudioEngine& audio() const noexcept { return audio_; }

  void request_play();
  void request_pause();
  void request_toggle();
  void request_seek_ms(int64_t time_ms);
  void request_seek(wds::common::Microseconds time) { request_seek_ms(wds::common::us_to_ms_floor(time)); }
  // Chart delay (MusicTiming::offset_ms): how much later the chart starts than the
  // music. Note hit times already include this (tick0 → offset). Transport keeps
  // the value for UI; the playhead stays 1:1 with the music stream (music t=0 at
  // timeline 0). Hit SFX schedule at note timeline ms on the music clock.
  void set_chart_offset_ms(int64_t offset_ms) noexcept;
  int64_t chart_offset_ms() const noexcept { return chart_offset_ms_; }

  // Preview clock rate (and BGM rate). Hit SFX locks to BASS music POS (1× samples);
  // the committed note timeline must stay phase-locked to that same music clock.
  void set_playback_rate(float rate);
  float playback_rate() const noexcept { return playback_rate_; }

  // Apply queued intents, advance committed timeline, drive music when present.
  // wall_delta_us is real frame elapsed time when Playing.
  // With music: audio-primary clock — wall*rate interpolates between BASS updates,
  // then a small dead zone (~4ms) + strong slew keeps notes phase-locked to music
  // (and therefore to POS-synced hit SFX). Wide dead zones / weak slew let the
  // wall clock drift from the device clock so SFX slowly walks off the notes.
  // Without music: wall clock advances the timeline directly (rate-scaled).
  // Pass real frame time in microseconds — millisecond truncation stalls wall-clock
  // playback under MAILBOX / high refresh (common on Windows).
  // Caller applies the returned snapshot to core/UI via engine.apply_timeline(snap).
  //
  // On play, music start is deferred until start_pending_music() so the UI can arm
  // POS SFX syncs against a still-paused stream (avoids first-frame catch-up).
  wds::common::TimelineSnapshot poll(int64_t wall_delta_us);

  // Begin audible BGM after hit-SFX schedules have been armed for this play request.
  void start_pending_music();
  bool music_start_pending() const noexcept { return music_start_pending_; }

  wds::common::TimelineSnapshot committed_snapshot() const noexcept;
  wds::common::Microseconds committed_position() const noexcept { return committed_position_; }
  int64_t committed_ms() const noexcept { return wds::common::us_to_ms_floor(committed_position_); }
  bool playing() const noexcept { return playing_; }

 private:
  wds::common::Microseconds clamp_time(wds::common::Microseconds time) const;

  AudioEngine audio_;
  bool playing_ = false;
  bool music_start_pending_ = false;
  wds::common::Microseconds committed_position_{0};

  bool pending_play_ = false;
  bool pending_pause_ = false;
  bool pending_seek_ = false;
  wds::common::Microseconds pending_seek_time_{0};
  int64_t chart_offset_ms_ = 0;
  float playback_rate_ = 1.0f;
};

}  // namespace wds::audio
