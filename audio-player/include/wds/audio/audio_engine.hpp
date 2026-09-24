#pragma once

#include "hit_sfx.hpp"
#include "recovery_backoff.hpp"
#include "sfx_sync_policy.hpp"

#include <wds/common/time.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace wds::audio {

struct AudioEngineTestAccess;

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
  bool has_music() const noexcept;
  bool sfx_ready() const noexcept { return sfx_ready_.load(std::memory_order_acquire); }

  // --- position ---
  wds::common::Microseconds position() const;
  wds::common::Microseconds duration() const;
  int64_t duration_ms() const noexcept { return wds::common::us_to_ms_floor(duration()); }
  bool set_position(wds::common::Microseconds time);
  // Bumps on set_position and begin_timeline_control (seek / scrub / play resync).
  // UI uses this instead of inferring seeks from BASS playtime regressions.
  uint64_t position_generation() const noexcept { return position_generation_; }
  // Silence SFX and bump position_generation_ without moving the music cursor.
  // Used for timeline scrub when there is no BGM stream.
  void begin_timeline_control();

  // --- music ---
  // True on ChannelPlay success. No stream is an explicit failure.
  // Success clears last_bass_error(); failure stores BASS_ErrorGetCode()
  // (or a non-zero handle error when there is no music).
  bool play_music();
  void pause_music();
  int last_bass_error() const noexcept { return last_bass_error_; }
  StreamHealth stream_health() const noexcept;
  // Compatibility wrappers around stream_health().
  bool stream_playing() const noexcept;
  bool stream_stopped() const noexcept;

  // --- SFX ---
  // Returns false when no voice could be started (caller may retry).
  bool play_sfx(HitSfxClip clip);
  // Prime Hold channel / ensure keep-alive. Safe to call repeatedly.
  void warmup_sfx();
  // Arm a one-shot at an absolute music time. With BGM, MIXTIME POS on the
  // decode source plugs a 1× DECODE stream into the same BASSmix output (not a
  // device ChannelPlay). Falls back to immediate play when there is no music.
  // Returns false if the hit could not be armed or played (caller may retry).
  bool schedule_sfx_at(HitSfxClip clip, wds::common::Microseconds at);
  // Armed MIXTIME POS syncs still waiting to fire. Thread-safe (same lock as push).
  size_t pending_sfx_sync_count() const noexcept;
  void clear_scheduled_sfx();
  // Drop only HoldOn/HoldOff POS gates. One-shots stay armed.
  void clear_hold_gates();
  bool schedule_hold_gate(bool enabled, wds::common::Microseconds at);
  void set_hold_looping(bool enabled);
  bool hold_looping() const noexcept;
  // Stop currently audible sample voices (one-shots / Hold). Pending music
  // syncs are cleared via clear_scheduled_sfx.
  void stop_playing_sfx();
  void stop_all_sfx();

  // --- volume (0..1) ---
  void set_music_gain(float gain);
  void set_sfx_gain(float gain);

  // Playback rate for BGM only (pitch scales with rate via BASS_ATTRIB_FREQ). SFX stay at 1x.
  // A real change flushes already-mixed output and bumps position_generation_ so
  // the preview re-arms POS hits (same as seek) instead of re-firing the decode window.
  void set_playback_rate(float rate);

  // BASS MIXTIME POS SYNCPROC entry. `payload` is a stable SfxSyncSlot* from the
  // fixed Impl pool (never a heap object that can be freed while BASS is alive).
  // Not for UI callers.
  void handle_sfx_sync(unsigned long long sync_handle, void* payload, HitSfxClip clip);
  void handle_hold_region_sync();

  // Pending-slot snapshot for lifetime tests. Pointers stay valid until
  // shutdown() releases Impl; after clear they are stale SYNCPROC user pointers.
  size_t snapshot_pending_sfx_syncs(void** out_slots, unsigned long long* out_handles,
                                    size_t max_count) const noexcept;
  // Read one pool slot. False if `slot` is not in this engine's pool (or no Impl).
  bool inspect_sfx_sync_slot(const void* slot, unsigned long long* handle, int* claim,
                             bool* in_use) const noexcept;

 private:
  friend struct AudioEngineTestAccess;
  struct TestDouble;
  void apply_music_volume();
  // flush_output drops mixer samples already mixed at the previous rate.
  void apply_music_rate(bool flush_output = false);
  void apply_sfx_volume();
  float effective_music_volume() const noexcept;
  float effective_sfx_volume() const noexcept;
  void ensure_keep_alive();
  void pause_keep_alive();
  bool play_sfx_internal(HitSfxClip clip);
  bool mix_sfx_on_mixer(HitSfxClip clip);
  void remove_mixer_sfx_sources();
  bool schedule_music_sync(SfxSyncKind kind, HitSfxClip clip, wds::common::Microseconds at);
  void fire_sync_action(SfxSyncKind kind, HitSfxClip clip);
  void warmup_hold_channel();
  void arm_hold_region_loop();
  void detach_hold_region_loop();
  void load_hold_loop_sidecar(const std::string& effects_directory);
  bool resolve_hold_loop_bytes();
  void cache_music_format();
  std::uint64_t align_music_bytes(std::uint64_t bytes) const noexcept;
  std::uint64_t music_heard_bytes() const;

  struct Impl;
  // Published via std::atomic_load / std::atomic_store (C++17 shared_ptr
  // overloads). Impl owns a fixed SfxSyncSlot pool; SYNCPROC user pointers are
  // those slot addresses and stay valid for the life of Impl.
  std::shared_ptr<Impl> impl_;
  bool ready_ = false;
  std::atomic<bool> sfx_ready_{false};
  std::atomic<bool> music_playing_{false};
  unsigned long long music_ = 0;  // HSTREAM as opaque
  std::atomic<float> music_gain_{1.0f};
  std::atomic<float> sfx_gain_{1.0f};
  float playback_rate_ = 1.0f;
  float music_base_freq_ = 0.0f;
  uint64_t position_generation_ = 0;
  std::atomic<bool> shutting_down_{false};
  int last_bass_error_ = 0;
  TestDouble* test_double_ = nullptr;
};

}  // namespace wds::audio
