#pragma once

#include "audio_engine.hpp"
#include "recovery_backoff.hpp"

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
  void request_seek_ms(int64_t time_ms);
  // Chart delay (MusicTiming::offset_ms): tick 0 maps to this music time.
  // Negative offset allows a silent preroll before music t=0.
  void set_chart_offset_ms(int64_t offset_ms) noexcept;
  int64_t chart_offset_ms() const noexcept { return chart_offset_ms_; }
  // Earliest playable timeline (chart start when offset is negative, else 0).
  int64_t chart_start_ms() const noexcept {
    return chart_offset_ms_ < 0 ? chart_offset_ms_ : 0;
  }

  // Preview clock rate (and BGM rate). Hit SFX locks to BASS music POS (1× samples);
  // the committed note timeline must stay phase-locked to that same music clock.
  void set_playback_rate(float rate);

  // Apply queued intents, advance committed timeline, drive music when present.
  // wall_delta_us is real frame elapsed time when Playing.
  // With music: audio-primary clock — wall*rate predicts between BASS updates, then
  // an EMA (tau ~20–30ms) absorbs UPDATEPERIOD staircases so notes stay phase-locked
  // to music / POS-synced hit SFX without hitchy catch-up. Seek/pause resets the filter.
  // Without music: wall clock advances the timeline directly (rate-scaled).
  // Pass real frame time in microseconds — millisecond truncation stalls wall-clock
  // playback under MAILBOX / high refresh (common on Windows).
  // Caller applies the returned snapshot to core/UI via engine.apply_timeline(snap).
  //
  // On play, music start is deferred until start_pending_music() so the UI can arm
  // POS SFX syncs against a still-paused stream (avoids first-frame catch-up).
  wds::common::TimelineSnapshot poll(int64_t wall_delta_us);

  // Begin audible BGM after hit-SFX schedules have been armed for this play request.
  // True when there is nothing left to start (already started, or no pending work).
  // False when start is still pending (play failed or backoff has not elapsed).
  bool start_pending_music();

  bool music_start_pending() const noexcept { return music_start_pending_; }
  bool music_seek_pending() const noexcept { return music_seek_pending_; }
  int recovery_attempt_count() const noexcept { return recovery_.attempt_count(); }
  bool recovery_pending() const noexcept { return recovery_.pending(); }

  wds::common::TimelineSnapshot committed_snapshot() const noexcept;
  wds::common::Microseconds committed_position() const noexcept { return committed_position_; }
  int64_t committed_ms() const noexcept { return wds::common::us_to_ms_floor(committed_position_); }
  bool playing() const noexcept { return playing_; }
  // Logical playing state after queued play/pause intents are applied.
  bool intends_playing() const noexcept {
    return (playing_ || pending_play_) && !pending_pause_;
  }
  bool pending_play() const noexcept { return pending_play_; }
  bool pending_pause() const noexcept { return pending_pause_; }

 private:
  wds::common::Microseconds clamp_time(wds::common::Microseconds time) const;

  AudioEngine audio_;
  RecoveryBackoff recovery_;
  bool playing_ = false;
  bool music_start_pending_ = false;
  bool music_seek_pending_ = false;
  bool sought_this_poll_ = false;
  wds::common::Microseconds music_seek_target_{0};
  wds::common::Microseconds committed_position_{0};
  // EMA of BASS music position used as the smooth audio master (µs).
  int64_t filtered_audio_us_ = 0;
  bool audio_filter_valid_ = false;
  // Mixer flush after a rate change can report Stalled for one poll. Skip that
  // Recover so we do not immediately seek (which would hitch like a scrub).
  bool skip_stalled_recovery_once_ = false;

  bool pending_play_ = false;
  bool pending_pause_ = false;
  bool pending_seek_ = false;
  wds::common::Microseconds pending_seek_time_{0};
  int64_t chart_offset_ms_ = 0;
  float playback_rate_ = 1.0f;
};

}  // namespace wds::audio
