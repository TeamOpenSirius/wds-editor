#include "wds/audio/transport.hpp"
#include <wds/common/log.hpp>

#include <algorithm>
#include <cmath>

namespace wds::audio {

bool Transport::initialize(const std::string& effects_directory, const std::string& music_path) {
  shutdown();
  if (!audio_.initialize(effects_directory, music_path)) {
    return false;
  }
  playing_ = false;
  committed_position_ = wds::common::Microseconds{0};
  filtered_audio_us_ = 0;
  audio_filter_valid_ = false;
  pending_play_ = false;
  pending_pause_ = false;
  pending_seek_ = false;
  pending_seek_time_ = wds::common::Microseconds{0};
  return true;
}

void Transport::shutdown() {
  audio_.shutdown();
  playing_ = false;
  music_start_pending_ = false;
  committed_position_ = wds::common::Microseconds{0};
  filtered_audio_us_ = 0;
  audio_filter_valid_ = false;
  pending_play_ = false;
  pending_pause_ = false;
  pending_seek_ = false;
}

void Transport::request_play() {
  pending_play_ = true;
  pending_pause_ = false;
}

void Transport::request_pause() {
  pending_pause_ = true;
  pending_play_ = false;
}

void Transport::request_seek_ms(int64_t time_ms) {
  pending_seek_ = true;
  pending_seek_time_ = wds::common::ms_to_us(std::max<int64_t>(0, time_ms));
}

void Transport::set_chart_offset_ms(int64_t offset_ms) noexcept {
  // Chart delay lives in note timeline ms (tick0 → offset). Transport keeps the
  // value for callers, but the playhead is 1:1 with the music stream — music
  // starts at timeline 0; the chart starts later.
  chart_offset_ms_ = offset_ms < 0 ? 0 : offset_ms;
}

void Transport::set_playback_rate(float rate) {
  playback_rate_ = std::clamp(rate, 0.25f, 2.0f);
  audio_.set_playback_rate(playback_rate_);
}

wds::common::Microseconds Transport::clamp_time(wds::common::Microseconds time) const {
  if (time.count() < 0) {
    time = wds::common::Microseconds{0};
  }
  if (audio_.has_music()) {
    const auto dur = audio_.duration();
    if (dur.count() > 0 && time > dur) {
      time = dur;
    }
  }
  return time;
}

wds::common::TimelineSnapshot Transport::committed_snapshot() const noexcept {
  return {committed_position_,
          playing_ ? wds::common::PlaybackState::Playing : wds::common::PlaybackState::Paused};
}

wds::common::TimelineSnapshot Transport::poll(int64_t wall_delta_us) {
  if (!audio_.ready()) {
    return committed_snapshot();
  }

  const bool want_seek = pending_seek_;
  const auto seek_time = pending_seek_time_;
  const bool want_play = pending_play_;
  const bool want_pause = pending_pause_;
  pending_seek_ = false;
  pending_play_ = false;
  pending_pause_ = false;

  auto apply_seek = [&]() {
    committed_position_ = clamp_time(seek_time);
    filtered_audio_us_ = committed_position_.count();
    audio_filter_valid_ = true;
    if (audio_.has_music()) {
      // Timeline == music clock (chart delay is in note times, not a BGM hold-off).
      // set_position silences SFX and bumps position_generation_ for UI resync.
      audio_.set_position(committed_position_);
      if (playing_ && !audio_.stream_playing()) {
        music_start_pending_ = true;
      }
    } else {
      // No BGM: still silence hits and bump control generation so UI releases
      // its monotonic SFX clock (progress-bar scrub / edit seek).
      audio_.begin_timeline_control();
    }
  };

  if (playing_) {
    if (want_seek) {
      apply_seek();
    }

    if (audio_.has_music()) {
      // Audio-primary display clock. Hit SFX is hard-locked to BASS music POS;
      // the note timeline follows a filtered copy of that clock so ~5ms
      // UPDATEPERIOD staircases do not show up as hitchy pull-backs.
      const auto raw = clamp_time(audio_.position());
      constexpr int64_t kHardSnapUs = 100000;  // 100ms — seek / glitch
      // EMA time constant: several UPDATEPERIODs so steps are rounded off, but
      // short enough that wall/device drift cannot accumulate over a phrase.
#if defined(_WIN32)
      constexpr double kFilterTauUs = 30000.0;  // WASAPI/DWM noisier
#else
      constexpr double kFilterTauUs = 22000.0;
#endif
      if (music_start_pending_ || !audio_filter_valid_) {
        // Audible BGM not started yet — keep UI locked to the paused playhead
        // so wall time does not drift ahead and then get yanked back.
        filtered_audio_us_ = raw.count();
        audio_filter_valid_ = true;
        committed_position_ = raw;
      } else {
        const int64_t step_us = std::max<int64_t>(0, wall_delta_us);
        const int64_t advance_us =
            step_us > 0 ? static_cast<int64_t>(std::llround(
                              static_cast<double>(step_us) *
                              static_cast<double>(playback_rate_)))
                        : int64_t{0};
        // Predict with wall*rate between BASS quantize steps.
        if (advance_us > 0) {
          filtered_audio_us_ += advance_us;
        }

        const int64_t err = raw.count() - filtered_audio_us_;
        if (err >= kHardSnapUs || err <= -kHardSnapUs) {
          filtered_audio_us_ = raw.count();
        } else if (step_us > 0) {
          // alpha = 1 - e^{-dt/tau}: independent of error magnitude, so a 5ms
          // BASS step bleeds in smoothly instead of a proportional yank.
          const double alpha =
              1.0 - std::exp(-static_cast<double>(step_us) / kFilterTauUs);
          filtered_audio_us_ +=
              static_cast<int64_t>(std::llround(static_cast<double>(err) * alpha));
        }

        committed_position_ =
            clamp_time(wds::common::Microseconds{filtered_audio_us_});
        filtered_audio_us_ = committed_position_.count();
      }

      // Natural end-of-stream: BASS stops near duration — snap and pause.
      // Do not treat "stopped at t=0 before start" as EOS (play may still be starting).
      if (!music_start_pending_ && audio_.stream_stopped()) {
        const auto dur = audio_.duration();
        constexpr int64_t kEndSlopUs = 100000;  // 100ms
        const bool near_end =
            dur.count() > 0 && committed_position_.count() + kEndSlopUs >= dur.count();
        if (near_end) {
          committed_position_ = dur;
          audio_.stop_all_sfx();
          playing_ = false;
          audio_.pause_music();
        } else {
          // Failed start or unexpected stop mid-track: re-seek then retry so
          // play_music is not a no-op when the channel already passed the cursor.
          audio_.set_position(committed_position_);
          audio_.play_music();
        }
      }
    } else if (wall_delta_us > 0) {
      const auto scaled_us = wds::common::Microseconds{
          static_cast<int64_t>(std::llround(static_cast<double>(wall_delta_us) *
                                            static_cast<double>(playback_rate_)))};
      committed_position_ = clamp_time(committed_position_ + scaled_us);
    }

    if (want_pause) {
      // Snap committed clock to BASS before pause so resume does not jump back
      // to a lagging filtered position.
      if (audio_.has_music()) {
        committed_position_ = clamp_time(audio_.position());
      }
      audio_.pause_music();
      // Silence hits + bump control generation so UI releases its monotonic SFX clock.
      audio_.begin_timeline_control();
      playing_ = false;
      music_start_pending_ = false;
      // Re-seed filter on next play from the paused playhead.
      filtered_audio_us_ = committed_position_.count();
      audio_filter_valid_ = false;
    } else if (playing_ && !music_start_pending_ && audio_.has_music() &&
               !audio_.stream_playing() && !audio_.stream_stopped()) {
      // Recover stalled channel while still intending to play (and already audible).
      audio_.set_position(committed_position_);
      audio_.play_music();
    }
  } else {
    if (want_seek) {
      apply_seek();
    }

    if (want_play) {
      if (audio_.has_music()) {
        const auto dur = audio_.duration();
        constexpr int64_t kEndSlopUs = 100000;
        if (dur.count() > 0 && committed_position_.count() + kEndSlopUs >= dur.count()) {
          // Restart from the beginning when pressing play at EOF.
          committed_position_ = wds::common::Microseconds{0};
        }
        // Always re-seek on play after any pause (not only when jumping). A
        // no-op skip left the decoder/mix frontier in a pause-dependent state so
        // the first DSP-scheduled hits could land late or with a clipped attack.
        // set_position also clears any stale DSP SFX queue.
        audio_.set_position(committed_position_);
        filtered_audio_us_ = committed_position_.count();
        audio_filter_valid_ = false;
        // Keep-alive / DEV_NONSTOP keep the device hot; skip heavy warmup on resume.
        music_start_pending_ = true;
      } else {
        // No BGM: still silence hits and bump control generation so UI re-latches
        // its monotonic SFX clock (same resync contract as set_position with BGM).
        audio_.begin_timeline_control();
        music_start_pending_ = false;
      }
      playing_ = true;
    }
  }

  return committed_snapshot();
}

void Transport::start_pending_music() {
  if (!music_start_pending_) {
    return;
  }
  music_start_pending_ = false;
  if (!playing_ || !audio_.has_music()) {
    return;
  }
  // Warmup already ran in poll's want_play path; play_music warms again safely.
  audio_.play_music();
}

}  // namespace wds::audio
