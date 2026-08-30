#include "wds/audio/transport.hpp"
#include <wds/common/log.hpp>

#include <algorithm>
#include <cmath>

namespace wds::audio {

namespace {

constexpr int64_t kEndSlopUs = 100000;

}  // namespace

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
  music_start_pending_ = false;
  music_seek_pending_ = false;
  sought_this_poll_ = false;
  music_seek_target_ = wds::common::Microseconds{0};
  recovery_.reset();
  return true;
}

void Transport::shutdown() {
  audio_.shutdown();
  playing_ = false;
  music_start_pending_ = false;
  music_seek_pending_ = false;
  sought_this_poll_ = false;
  music_seek_target_ = wds::common::Microseconds{0};
  committed_position_ = wds::common::Microseconds{0};
  filtered_audio_us_ = 0;
  audio_filter_valid_ = false;
  pending_play_ = false;
  pending_pause_ = false;
  pending_seek_ = false;
  recovery_.reset();
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
  sought_this_poll_ = false;

  auto apply_music_seek = [&](wds::common::Microseconds target) {
    music_seek_target_ = target;
    if (audio_.set_position(target)) {
      committed_position_ = target;
      filtered_audio_us_ = target.count();
      music_seek_pending_ = false;
      sought_this_poll_ = true;
      return true;
    }
    music_seek_pending_ = true;
    audio_filter_valid_ = false;
    return false;
  };

  auto apply_user_seek = [&]() {
    const auto target = clamp_time(seek_time);
    recovery_.reset();
    if (!audio_.has_music()) {
      committed_position_ = target;
      filtered_audio_us_ = committed_position_.count();
      audio_filter_valid_ = true;
      music_seek_pending_ = false;
      audio_.begin_timeline_control();
      return;
    }
    if (apply_music_seek(target)) {
      audio_filter_valid_ = true;
      if (playing_ && !want_pause) {
        music_start_pending_ = true;
      }
    } else {
      if (recovery_.try_acquire()) {
        recovery_.on_failure();
      }
      if (playing_ && !want_pause) {
        music_start_pending_ = true;
        audio_.pause_music();
      }
    }
  };

  auto retry_pending_seek = [&]() {
    if (!music_seek_pending_ || sought_this_poll_ || want_seek || !audio_.has_music()) {
      return;
    }
    recovery_.add_elapsed(wall_delta_us);
    if (!recovery_.try_acquire()) {
      return;
    }
    if (apply_music_seek(music_seek_target_)) {
      if (playing_ && !want_pause) {
        music_start_pending_ = true;
      }
    } else {
      recovery_.on_failure();
    }
  };

  if (playing_) {
    if (want_seek) {
      apply_user_seek();
    }
    retry_pending_seek();

    if (!want_pause && audio_.has_music()) {
      const auto raw = clamp_time(audio_.position());
      constexpr int64_t kHardSnapUs = 100000;
#if defined(_WIN32)
      constexpr double kFilterTauUs = 30000.0;
#else
      constexpr double kFilterTauUs = 22000.0;
#endif
      if (music_seek_pending_) {
        audio_filter_valid_ = false;
      } else if (music_start_pending_ || !audio_filter_valid_) {
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
        if (advance_us > 0) {
          filtered_audio_us_ += advance_us;
        }

        const int64_t err = raw.count() - filtered_audio_us_;
        if (err >= kHardSnapUs || err <= -kHardSnapUs) {
          filtered_audio_us_ = raw.count();
        } else if (step_us > 0) {
          const double alpha =
              1.0 - std::exp(-static_cast<double>(step_us) / kFilterTauUs);
          filtered_audio_us_ +=
              static_cast<int64_t>(std::llround(static_cast<double>(err) * alpha));
        }

        committed_position_ =
            clamp_time(wds::common::Microseconds{filtered_audio_us_});
        filtered_audio_us_ = committed_position_.count();
      }

      if (music_seek_pending_) {
        // Cursor frozen until a seek lands; start_pending may re-seek after sync.
      } else if (music_start_pending_) {
        recovery_.add_elapsed(wall_delta_us);
      } else {
        const auto health = audio_.stream_health();
        const auto dur = audio_.duration();
        const bool near_end =
            dur.count() > 0 && committed_position_.count() + kEndSlopUs >= dur.count();
        const auto kind = classify_stream_recovery(health, near_end);
        if (kind == StreamRecoveryKind::NaturalEnd) {
          committed_position_ = dur;
          audio_.stop_all_sfx();
          playing_ = false;
          audio_.pause_music();
          recovery_.reset();
          music_start_pending_ = false;
          music_seek_pending_ = false;
        } else if (kind == StreamRecoveryKind::Recover) {
          recovery_.add_elapsed(wall_delta_us);
          if (recovery_.try_acquire()) {
            if (apply_music_seek(committed_position_)) {
              music_start_pending_ = true;
            } else {
              recovery_.on_failure();
              music_start_pending_ = true;
            }
          }
        } else if (health == StreamHealth::Playing) {
          recovery_.reset();
        }
      }
    } else if (!want_pause && wall_delta_us > 0) {
      const auto scaled_us = wds::common::Microseconds{
          static_cast<int64_t>(std::llround(static_cast<double>(wall_delta_us) *
                                            static_cast<double>(playback_rate_)))};
      committed_position_ = clamp_time(committed_position_ + scaled_us);
    }

    if (want_pause) {
      if (audio_.has_music() && !sought_this_poll_) {
        committed_position_ = clamp_time(audio_.position());
      }
      audio_.pause_music();
      audio_.begin_timeline_control();
      playing_ = false;
      music_start_pending_ = false;
      music_seek_pending_ = false;
      recovery_.reset();
      filtered_audio_us_ = committed_position_.count();
      audio_filter_valid_ = false;
    }
  } else {
    if (want_seek) {
      apply_user_seek();
    }
    retry_pending_seek();

    if (want_play) {
      recovery_.reset();
      if (audio_.has_music()) {
        auto target = committed_position_;
        const auto dur = audio_.duration();
        if (dur.count() > 0 && target.count() + kEndSlopUs >= dur.count()) {
          target = wds::common::Microseconds{0};
        }
        filtered_audio_us_ = target.count();
        audio_filter_valid_ = false;
        music_start_pending_ = true;
        if (music_seek_pending_) {
          // Keep the failed user-seek target; do not play from the old cursor.
        } else if (!apply_music_seek(target)) {
          recovery_.try_acquire();
          recovery_.on_failure();
        }
      } else {
        audio_.begin_timeline_control();
        music_start_pending_ = false;
        music_seek_pending_ = false;
      }
      playing_ = true;
    }
  }

  return committed_snapshot();
}

bool Transport::start_pending_music() {
  if (!music_start_pending_) {
    return true;
  }
  if (!playing_ || !audio_.has_music()) {
    music_start_pending_ = false;
    music_seek_pending_ = false;
    recovery_.reset();
    return true;
  }
  if (music_seek_pending_) {
    if (!recovery_.try_acquire()) {
      return false;
    }
    if (!audio_.set_position(music_seek_target_)) {
      recovery_.on_failure();
      return false;
    }
    committed_position_ = music_seek_target_;
    filtered_audio_us_ = music_seek_target_.count();
    music_seek_pending_ = false;
    sought_this_poll_ = true;
    // Re-seek after UI arming clears just-scheduled POS syncs. Play next tick.
    return false;
  }
  if (!sought_this_poll_ && !recovery_.try_acquire()) {
    return false;
  }
  if (!audio_.play_music() || audio_.stream_health() != StreamHealth::Playing) {
    if (recovery_.attempt_count() == 0) {
      (void)recovery_.try_acquire();
    }
    recovery_.on_failure();
    return false;
  }
  music_start_pending_ = false;
  recovery_.on_success();
  return true;
}

}  // namespace wds::audio
