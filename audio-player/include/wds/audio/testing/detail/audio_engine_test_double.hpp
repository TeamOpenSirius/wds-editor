#pragma once

#include "wds/audio/audio_engine.hpp"

namespace wds::audio {

// Test-only stand-in. Production headers do not expose bind.
//
// Contract: bind is for driving Transport/AudioEngine without BASS_Init.
// It records engine.ready_, then sets ready_ so poll() is live. Unbind
// (explicit, shutdown, or TestDouble destructor) restores the recorded
// ready_ and clears the pointer. Intended for uninitialized engines;
// binding an already-ready engine still records/restores that flag.
struct AudioEngine::TestDouble {
  AudioEngine* owner = nullptr;
  bool saved_ready = false;
  bool holding_ready = false;
  bool has_music = false;
  StreamHealth health = StreamHealth::Stopped;
  wds::common::Microseconds position{0};
  wds::common::Microseconds duration{0};
  bool set_position_ok = true;
  bool play_music_ok = true;
  bool update_health_on_play = true;
  bool update_health_on_pause = true;
  bool update_position_on_seek = true;
  int last_play_error = -1;
  int set_position_calls = 0;
  int play_music_calls = 0;
  int pause_music_calls = 0;

  TestDouble() = default;
  TestDouble(const TestDouble&) = delete;
  TestDouble& operator=(const TestDouble&) = delete;
  ~TestDouble();
};

struct AudioEngineTestAccess {
  using Double = AudioEngine::TestDouble;

  static void bind(AudioEngine& engine, Double& stub) noexcept {
    if (engine.test_double_ != nullptr && engine.test_double_ != &stub) {
      restore_ready(engine);
      engine.test_double_->owner = nullptr;
    }
    if (engine.test_double_ != &stub) {
      stub.saved_ready = engine.ready_;
      stub.holding_ready = true;
    }
    engine.test_double_ = &stub;
    stub.owner = &engine;
    engine.ready_ = true;
  }

  static void unbind(AudioEngine& engine) noexcept {
    if (engine.test_double_ == nullptr) {
      return;
    }
    restore_ready(engine);
    engine.test_double_->owner = nullptr;
    engine.test_double_ = nullptr;
  }

 private:
  static void restore_ready(AudioEngine& engine) noexcept {
    if (engine.test_double_ != nullptr && engine.test_double_->holding_ready) {
      engine.ready_ = engine.test_double_->saved_ready;
      engine.test_double_->holding_ready = false;
    }
  }
};

inline AudioEngine::TestDouble::~TestDouble() {
  if (owner != nullptr) {
    AudioEngineTestAccess::unbind(*owner);
  }
}

}  // namespace wds::audio
