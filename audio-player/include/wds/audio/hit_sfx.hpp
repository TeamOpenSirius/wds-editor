#pragma once

#include <wds/common/time.hpp>

#include <cstdint>

namespace wds::audio {

class AudioEngine;

// Sirius auto-preview SFX clips (sonolus-sirius-engine/engine/shared/effects.cpp).
enum class HitSfxClip : uint8_t {
  Perfect = 0,
  Great,
  Good,
  Bad,
  Scratch,
  Critical,
  Sound,
  Hold,
  Stage,
  Count,
};

// Thin facade over AudioEngine SFX. Does not own the BASS device.
class HitSfxPlayer {
 public:
  HitSfxPlayer() = default;
  ~HitSfxPlayer() = default;

  HitSfxPlayer(const HitSfxPlayer&) = delete;
  HitSfxPlayer& operator=(const HitSfxPlayer&) = delete;

  void attach(AudioEngine* audio) noexcept;
  void detach() noexcept { attach(nullptr); }

  bool ready() const noexcept;
  bool has_music() const noexcept;
  // Audible music-stream playtime (0 when no music). Not the decode frontier.
  int64_t music_position_ms() const noexcept;
  wds::common::Microseconds music_position() const noexcept;
  // Increments on set_position / begin_timeline_control (seek / scrub / play).
  uint64_t position_generation() const noexcept;

  // Returns false when the engine could not start/arm the voice (retry later).
  bool play(HitSfxClip clip);
  // Absolute music-stream time. Armed via BASS_SYNC_POS → 1× ChannelPlay.
  bool schedule_at(HitSfxClip clip, wds::common::Microseconds music_time);
  // Relative to the live music clock: fires after `delay` of stream time.
  bool schedule_after(HitSfxClip clip, wds::common::Microseconds delay);
  void clear_scheduled();
  // Drop pending syncs and cut audible one-shots / Hold (pause, seek, scrub).
  void stop_all();
  void set_hold_looping(bool enabled);

 private:
  AudioEngine* audio_ = nullptr;
};

}  // namespace wds::audio
