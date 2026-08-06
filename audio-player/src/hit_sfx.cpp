#include "wds/audio/hit_sfx.hpp"
#include "wds/audio/audio_engine.hpp"

namespace wds::audio {

void HitSfxPlayer::attach(AudioEngine* audio) noexcept { audio_ = audio; }

bool HitSfxPlayer::ready() const noexcept {
  return audio_ != nullptr && audio_->sfx_ready();
}

bool HitSfxPlayer::has_music() const noexcept {
  return audio_ != nullptr && audio_->has_music();
}

wds::common::Microseconds HitSfxPlayer::music_position() const noexcept {
  if (audio_ == nullptr || !audio_->has_music()) {
    return wds::common::Microseconds{0};
  }
  // Playtime (not decode): must match Transport / what the user hears.
  return audio_->position();
}

uint64_t HitSfxPlayer::position_generation() const noexcept {
  if (audio_ == nullptr) {
    return 0;
  }
  return audio_->position_generation();
}

bool HitSfxPlayer::play(HitSfxClip clip) {
  if (audio_ == nullptr) {
    return false;
  }
  return audio_->play_sfx(clip);
}

bool HitSfxPlayer::schedule_at(HitSfxClip clip, wds::common::Microseconds music_time) {
  if (audio_ == nullptr) {
    return false;
  }
  return audio_->schedule_sfx_at(clip, music_time);
}

void HitSfxPlayer::stop_all() {
  if (audio_ == nullptr) {
    return;
  }
  audio_->stop_all_sfx();
}

void HitSfxPlayer::set_hold_looping(bool enabled) {
  if (audio_ == nullptr) {
    return;
  }
  audio_->set_hold_looping(enabled);
}

}  // namespace wds::audio
