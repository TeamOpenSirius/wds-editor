#include "wds/audio/audio_engine.hpp"
#include "wds/audio/sfx_sync_policy.hpp"
#include "wds/audio/testing/detail/audio_engine_test_double.hpp"
#include <wds/common/crash_handler.hpp>
#include <wds/common/log.hpp>

#include "bass.h"
#include "bassmix.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

// Fixed-pool slot. Address is the SYNCPROC user pointer and stays valid for the
// life of Impl. Recycle (generation++, claim=Pending, return to free_list) only
// when claim is not Pending, the sync is detached (fired or RemoveSync), and
// in_callback == 0. Lives in this TU so the C SYNCPROC can name the type.
struct SfxSyncSlot {
  std::atomic<SfxSyncClaim> claim{SfxSyncClaim::Pending};
  std::atomic<unsigned long long> handle{0};
  std::atomic<unsigned long long> last_handle{0};
  HitSfxClip clip = HitSfxClip::Count;
  std::atomic<uint32_t> generation{0};
  std::atomic<bool> in_use{false};
  std::atomic<int> in_callback{0};
  std::atomic<bool> sync_detached{false};
  std::atomic<AudioEngine*> engine{nullptr};
  std::atomic<int>* sync_in_flight = nullptr;
  std::atomic<bool>* shutting_down = nullptr;
  std::mutex* sfx_mu = nullptr;
  std::vector<SfxSyncSlot*>* pending_syncs = nullptr;
  std::vector<SfxSyncSlot*>* free_list = nullptr;
};

const char* utf8_basename(const std::string& path) noexcept {
  const char* s = path.c_str();
  const char* base = s;
  for (const char* p = s; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base;
}

void copy_basename(char* dst, std::size_t cap, const std::string& path) {
  if (dst == nullptr || cap == 0) {
    return;
  }
  const char* base = utf8_basename(path);
  if (base == nullptr || base[0] == '\0') {
    base = "-";
  }
  std::snprintf(dst, cap, "%s", base);
}

void publish_audio_backend_ok() {
  char buf[512];
  const unsigned ver = static_cast<unsigned>(BASS_GetVersion());
  const int dev = static_cast<int>(BASS_GetDevice());
  BASS_INFO info{};
  if (BASS_GetInfo(&info)) {
    std::snprintf(buf, sizeof(buf), "BASS 0x%08X dev=%d freq=%u latency=%u", ver, dev,
                  static_cast<unsigned>(info.freq), static_cast<unsigned>(info.latency));
  } else {
    std::snprintf(buf, sizeof(buf), "BASS 0x%08X dev=%d", ver, dev);
  }
  wds::common::crash_set_context(wds::common::CrashContextField::AudioBackend, buf);
}

void publish_audio_backend_fail(int code) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "BASS init failed code=%d", code);
  wds::common::crash_set_context(wds::common::CrashContextField::AudioBackend, buf);
}

}  // namespace

struct AudioEngine::Impl {
  bool device_ok = false;
  HSTREAM mixer = 0;
  HSTREAM music = 0;
  HSTREAM keep_alive = 0;
  DWORD music_chans = 0;
  DWORD music_bpf = 0;  // bytes per frame
  std::array<HSAMPLE, static_cast<size_t>(HitSfxClip::Count)> samples{};
  std::array<bool, static_cast<size_t>(HitSfxClip::Count)> sample_ok{};
  HCHANNEL hold_ch = 0;
  bool hold_playing = false;
  char music_basename[128]{};

  std::mutex sfx_mu;
  // Fixed pool: allocated once in init_slot_pool, never resized/reallocated.
  std::unique_ptr<SfxSyncSlot[]> slots;
  std::vector<SfxSyncSlot*> pending_syncs;
  std::vector<SfxSyncSlot*> free_list;
  std::atomic<int> sync_in_flight{0};
  std::atomic<bool>* shutting_down = nullptr;

  void init_slot_pool(AudioEngine* engine);
  bool owns_slot(const SfxSyncSlot* slot) const noexcept;
  SfxSyncSlot* acquire_slot_locked();
  void try_recycle_locked(SfxSyncSlot* slot);
  void try_recycle_all_locked();
  void unlink_pending_locked(SfxSyncSlot* slot);
  void wait_sync_in_flight(const char* where);
};

void AudioEngine::Impl::init_slot_pool(AudioEngine* engine) {
  slots = std::make_unique<SfxSyncSlot[]>(kMaxPendingSfxSyncs);
  free_list.clear();
  free_list.reserve(kMaxPendingSfxSyncs);
  pending_syncs.clear();
  pending_syncs.reserve(kMaxPendingSfxSyncs);
  for (size_t i = 0; i < kMaxPendingSfxSyncs; ++i) {
    SfxSyncSlot& slot = slots[i];
    slot.engine.store(engine, std::memory_order_release);
    slot.sync_in_flight = &sync_in_flight;
    slot.shutting_down = shutting_down;
    slot.sfx_mu = &sfx_mu;
    slot.pending_syncs = &pending_syncs;
    slot.free_list = &free_list;
    free_list.push_back(&slot);
  }
}

bool AudioEngine::Impl::owns_slot(const SfxSyncSlot* slot) const noexcept {
  if (slots == nullptr || slot == nullptr) {
    return false;
  }
  const SfxSyncSlot* const begin = slots.get();
  const SfxSyncSlot* const end = begin + kMaxPendingSfxSyncs;
  if (slot < begin || slot >= end) {
    return false;
  }
  const auto bytes =
      reinterpret_cast<const unsigned char*>(slot) - reinterpret_cast<const unsigned char*>(begin);
  return bytes % static_cast<std::ptrdiff_t>(sizeof(SfxSyncSlot)) == 0;
}

void AudioEngine::Impl::unlink_pending_locked(SfxSyncSlot* slot) {
  auto& syncs = pending_syncs;
  for (size_t i = 0; i < syncs.size(); ++i) {
    if (syncs[i] == slot) {
      syncs.erase(syncs.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
}

void AudioEngine::Impl::try_recycle_locked(SfxSyncSlot* slot) {
  if (slot == nullptr || !slot->in_use.load(std::memory_order_acquire)) {
    return;
  }
  if (slot->claim.load(std::memory_order_acquire) == SfxSyncClaim::Pending) {
    return;
  }
  if (slot->in_callback.load(std::memory_order_acquire) != 0) {
    return;
  }
  if (!slot->sync_detached.load(std::memory_order_acquire)) {
    return;
  }
  for (const SfxSyncSlot* pending : pending_syncs) {
    if (pending == slot) {
      return;
    }
  }
  const unsigned long long handle = slot->handle.load(std::memory_order_acquire);
  if (handle != 0) {
    slot->last_handle.store(handle, std::memory_order_release);
  }
  slot->handle.store(0, std::memory_order_release);
  slot->clip = HitSfxClip::Count;
  slot->claim.store(SfxSyncClaim::Pending, std::memory_order_release);
  slot->sync_detached.store(false, std::memory_order_release);
  slot->generation.fetch_add(1, std::memory_order_acq_rel);
  slot->in_use.store(false, std::memory_order_release);
  free_list.push_back(slot);
}

void AudioEngine::Impl::try_recycle_all_locked() {
  if (slots == nullptr) {
    return;
  }
  for (size_t i = 0; i < kMaxPendingSfxSyncs; ++i) {
    try_recycle_locked(&slots[i]);
  }
}

SfxSyncSlot* AudioEngine::Impl::acquire_slot_locked() {
  try_recycle_all_locked();
  if (free_list.empty()) {
    return nullptr;
  }
  SfxSyncSlot* slot = free_list.back();
  free_list.pop_back();
  return slot;
}

void AudioEngine::Impl::wait_sync_in_flight(const char* where) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(500);
  while (sync_in_flight.load(std::memory_order_seq_cst) != 0) {
    if (clock::now() >= deadline) {
      WDS_LOG("AudioEngine: %s timed out waiting for sync_in_flight=%d\n", where,
              sync_in_flight.load(std::memory_order_seq_cst));
      return;
    }
    std::this_thread::yield();
  }
}

namespace {

struct SyncInFlightGuard {
  std::atomic<int>* counter = nullptr;

  explicit SyncInFlightGuard(std::atomic<int>& c) : counter(&c) {
    counter->fetch_add(1, std::memory_order_seq_cst);
  }

  SyncInFlightGuard(const SyncInFlightGuard&) = delete;
  SyncInFlightGuard& operator=(const SyncInFlightGuard&) = delete;

  ~SyncInFlightGuard() {
    if (counter != nullptr) {
      counter->fetch_sub(1, std::memory_order_seq_cst);
    }
  }
};

struct InCallbackGuard {
  SfxSyncSlot* slot = nullptr;

  explicit InCallbackGuard(SfxSyncSlot* s) : slot(s) {
    if (slot != nullptr) {
      slot->in_callback.fetch_add(1, std::memory_order_seq_cst);
    }
  }

  InCallbackGuard(const InCallbackGuard&) = delete;
  InCallbackGuard& operator=(const InCallbackGuard&) = delete;

  ~InCallbackGuard() {
    if (slot != nullptr) {
      slot->in_callback.fetch_sub(1, std::memory_order_seq_cst);
    }
  }
};

bool slot_matches_callback_handle(const SfxSyncSlot& slot,
                                  unsigned long long sync_handle) noexcept {
  const unsigned long long stored = slot.handle.load(std::memory_order_acquire);
  const unsigned long long last = slot.last_handle.load(std::memory_order_acquire);
  if (stored != 0 && stored != sync_handle) {
    return false;
  }
  if (stored == 0 && last != 0 && last == sync_handle) {
    return false;
  }
  return slot.in_use.load(std::memory_order_acquire);
}

void try_recycle_slot(SfxSyncSlot* slot) {
  if (slot == nullptr || slot->sfx_mu == nullptr || slot->pending_syncs == nullptr ||
      slot->free_list == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(*slot->sfx_mu);
  if (!slot->in_use.load(std::memory_order_acquire)) {
    return;
  }
  if (slot->claim.load(std::memory_order_acquire) == SfxSyncClaim::Pending) {
    return;
  }
  if (slot->in_callback.load(std::memory_order_acquire) != 0) {
    return;
  }
  if (!slot->sync_detached.load(std::memory_order_acquire)) {
    return;
  }
  for (const SfxSyncSlot* pending : *slot->pending_syncs) {
    if (pending == slot) {
      return;
    }
  }
  const unsigned long long handle = slot->handle.load(std::memory_order_acquire);
  if (handle != 0) {
    slot->last_handle.store(handle, std::memory_order_release);
  }
  slot->handle.store(0, std::memory_order_release);
  slot->clip = HitSfxClip::Count;
  slot->claim.store(SfxSyncClaim::Pending, std::memory_order_release);
  slot->sync_detached.store(false, std::memory_order_release);
  slot->generation.fetch_add(1, std::memory_order_acq_rel);
  slot->in_use.store(false, std::memory_order_release);
  slot->free_list->push_back(slot);
}

void CALLBACK sfx_pos_sync_proc(HSYNC handle, DWORD /*channel*/, DWORD /*data*/, void* user) {
  auto* slot = static_cast<SfxSyncSlot*>(user);
  if (slot == nullptr || slot->sync_in_flight == nullptr) {
    return;
  }
  // First action: keep shutdown from freeing BASS objects under this stack.
  SyncInFlightGuard in_flight(*slot->sync_in_flight);

  const bool shutting_down =
      slot->shutting_down != nullptr && slot->shutting_down->load(std::memory_order_seq_cst);
  const unsigned long long sync_handle = static_cast<unsigned long long>(handle);
  const bool ours = slot_matches_callback_handle(*slot, sync_handle);
  if (ours) {
    slot->sync_detached.store(true, std::memory_order_release);
  }

  AudioEngine* engine = slot->engine.load(std::memory_order_acquire);
  if (!shutting_down && engine != nullptr && ours) {
    InCallbackGuard in_callback(slot);
    engine->handle_sfx_sync(sync_handle, slot, slot->clip);
  }

  try_recycle_slot(slot);
}

}  // namespace

AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::has_music() const noexcept {
  return test_double_ != nullptr ? test_double_->has_music : music_ != 0;
}

uint32_t linked_bass_version() noexcept {
  return static_cast<uint32_t>(BASS_GetVersion());
}

bool AudioEngine::initialize(const std::string& effects_directory, const std::string& music_path) {
  const char* music_base = utf8_basename(music_path);
  WDS_LOG("AudioEngine: initialize music=%s\n", music_base[0] != '\0' ? music_base : "-");
  shutdown();

  auto impl = std::make_shared<Impl>();
  // Sample-accurate SFX lives in the music DSP. Short update period keeps the
  // playhead staircase small (helps UI clock sync) and decode latency stable;
  // very large periods let device buffering "breathe". 5ms is BASS's floor and
  // cheap for a single desktop stream (DEV_PERIOD stays 10).
  BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 5);
  BASS_SetConfig(BASS_CONFIG_DEV_NONSTOP, TRUE);  // do not stop device when idle
  BASS_SetConfig(BASS_CONFIG_DEV_PERIOD, 10);
  if (!BASS_Init(-1, 44100, 0, nullptr, nullptr)) {
    const int code = BASS_ErrorGetCode();
    WDS_LOG("AudioEngine: BASS_Init failed code=%d\n", code);
    publish_audio_backend_fail(code);
    return false;
  }
  publish_audio_backend_ok();
  impl->device_ok = true;
  copy_basename(impl->music_basename, sizeof(impl->music_basename), music_path);
  impl->shutting_down = &shutting_down_;
  impl->init_slot_pool(this);
  // Wire impl_ before attribute helpers (apply_music_rate / volume) touch it.
  std::atomic_store(&impl_, impl);

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
      const HSTREAM music =
          stream_from_file(music_path, BASS_STREAM_DECODE | BASS_STREAM_PRESCAN);
      if (music != 0) {
        BASS_INFO info{};
        DWORD mix_freq = 44100;
        if (BASS_GetInfo(&info) && info.freq != 0) {
          mix_freq = info.freq;
        }
        const HSTREAM mixer = BASS_Mixer_StreamCreate(mix_freq, 2, 0);
        if (mixer == 0) {
          WDS_LOG("AudioEngine: Mixer_StreamCreate failed code=%d\n", BASS_ErrorGetCode());
          BASS_StreamFree(music);
        } else if (!BASS_Mixer_StreamAddChannel(mixer, music, 0)) {
          WDS_LOG("AudioEngine: Mixer_StreamAddChannel music failed code=%d\n",
                  BASS_ErrorGetCode());
          BASS_StreamFree(mixer);
          BASS_StreamFree(music);
        } else {
          impl->mixer = mixer;
          impl->music = music;
          music_ = static_cast<unsigned long long>(music);
          float freq = 0.0f;
          if (BASS_ChannelGetAttribute(music, BASS_ATTRIB_FREQ, &freq)) {
            music_base_freq_ = freq;
          } else {
            music_base_freq_ = 44100.0f;
          }
          apply_music_rate();
          WDS_LOG("AudioEngine: music loaded %s mixer_freq=%u\n", music_path.c_str(),
                  static_cast<unsigned>(mix_freq));
        }
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
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl) {
    // Unbind first so the stub restores recorded ready_, then force not-ready.
    AudioEngineTestAccess::unbind(*this);
    ready_ = false;
    sfx_ready_.store(false, std::memory_order_release);
    music_playing_.store(false, std::memory_order_release);
    music_ = 0;
    last_bass_error_ = 0;
    return;
  }

  WDS_LOG("AudioEngine: shutdown music=%s\n",
          impl->music_basename[0] != '\0' ? impl->music_basename : "-");
  shutting_down_.store(true, std::memory_order_seq_cst);
  // Fail new schedule/play immediately; do not wait until the end of teardown.
  sfx_ready_.store(false, std::memory_order_release);

  if (impl->slots != nullptr) {
    for (size_t i = 0; i < kMaxPendingSfxSyncs; ++i) {
      impl->slots[i].engine.store(nullptr, std::memory_order_release);
    }
  }

  clear_scheduled_sfx();
  // RemoveSync leftovers (including handles published after a handle==0 clear).
  if (impl->slots != nullptr && impl->music != 0) {
    for (size_t i = 0; i < kMaxPendingSfxSyncs; ++i) {
      SfxSyncSlot& slot = impl->slots[i];
      if (!slot.in_use.load(std::memory_order_acquire)) {
        continue;
      }
      (void)claim_sfx_sync_cancel(slot.claim);
      const unsigned long long handle = slot.handle.load(std::memory_order_acquire);
      if (handle != 0) {
        BASS_ChannelRemoveSync(impl->music, static_cast<HSYNC>(handle));
        slot.sync_detached.store(true, std::memory_order_release);
      }
    }
  }
  stop_all_sfx();

  impl->wait_sync_in_flight("pre-free");

  if (impl->keep_alive != 0) {
    BASS_ChannelStop(impl->keep_alive);
    BASS_StreamFree(impl->keep_alive);
    impl->keep_alive = 0;
  }

  if (impl->mixer != 0) {
    BASS_ChannelStop(impl->mixer);
    BASS_StreamFree(impl->mixer);
    impl->mixer = 0;
  }
  if (impl->music != 0) {
    BASS_StreamFree(impl->music);
    impl->music = 0;
  }
  music_ = 0;
  music_playing_.store(false, std::memory_order_release);
  music_base_freq_ = 0.0f;

  for (auto& sample : impl->samples) {
    if (sample != 0) {
      BASS_SampleFree(sample);
      sample = 0;
    }
  }
  impl->hold_ch = 0;
  impl->hold_playing = false;

  if (impl->device_ok) {
    BASS_Free();
    impl->device_ok = false;
  }

  impl->wait_sync_in_flight("post-BASS_Free");

  {
    std::lock_guard<std::mutex> lock(impl->sfx_mu);
    impl->pending_syncs.clear();
    impl->free_list.clear();
  }

  std::atomic_store(&impl_, std::shared_ptr<Impl>{});
  AudioEngineTestAccess::unbind(*this);
  ready_ = false;
  sfx_ready_.store(false, std::memory_order_release);
  shutting_down_.store(false, std::memory_order_seq_cst);
  last_bass_error_ = 0;
}

std::uint64_t AudioEngine::music_heard_bytes() const {
  if (impl_ == nullptr || impl_->music == 0) {
    return 0;
  }
  if (impl_->mixer != 0) {
    const QWORD heard = BASS_Mixer_ChannelGetPosition(impl_->music, BASS_POS_BYTE);
    if (heard != static_cast<QWORD>(-1)) {
      return heard;
    }
  }
  return BASS_ChannelGetPosition(impl_->music, BASS_POS_BYTE);
}

wds::common::Microseconds AudioEngine::position() const {
  if (test_double_ != nullptr) {
    return test_double_->position;
  }
  if (impl_ == nullptr || impl_->music == 0) {
    return wds::common::Microseconds{0};
  }
  return bytes_to_us(impl_->music, music_heard_bytes());
}

wds::common::Microseconds AudioEngine::duration() const {
  if (test_double_ != nullptr) {
    return test_double_->duration;
  }
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
  if (test_double_ != nullptr) {
    ++test_double_->set_position_calls;
    begin_timeline_control();
    if (!test_double_->set_position_ok) {
      return false;
    }
    if (test_double_->update_position_on_seek) {
      test_double_->position = time;
    }
    return true;
  }
  if (impl_ == nullptr || impl_->music == 0) {
    return false;
  }
  begin_timeline_control();
  const QWORD pos = us_to_bytes(impl_->music, time);
  BOOL ok = FALSE;
  if (impl_->mixer != 0) {
    ok = BASS_Mixer_ChannelSetPosition(impl_->music, pos, BASS_POS_BYTE | BASS_POS_MIXER_RESET);
    // Flush any leftover mixer output (do not call this from a MIXTIME sync).
    BASS_ChannelSetPosition(impl_->mixer, 0, BASS_POS_BYTE);
  } else {
    ok = BASS_ChannelSetPosition(impl_->music, pos, BASS_POS_BYTE);
  }
  return ok != FALSE;
}

bool AudioEngine::play_music() {
  if (test_double_ != nullptr) {
    ++test_double_->play_music_calls;
    if (test_double_->play_music_ok) {
      last_bass_error_ = 0;
      music_playing_ = true;
      if (test_double_->update_health_on_play) {
        test_double_->health = StreamHealth::Playing;
      }
      return true;
    }
    last_bass_error_ = test_double_->last_play_error;
    music_playing_ = false;
    return false;
  }
  if (impl_ == nullptr || impl_->music == 0) {
    music_playing_ = false;
    last_bass_error_ = BASS_ERROR_HANDLE;
    return false;
  }
  // Avoid mixing a silent keep-alive stream alongside BGM — dual-stream device
  // resampling can slowly modulate perceived inter-hit gaps.
  pause_keep_alive();
  apply_music_volume();
  apply_music_rate();
  const HSTREAM out = impl_->mixer != 0 ? impl_->mixer : impl_->music;
  if (BASS_ChannelPlay(out, FALSE)) {
    music_playing_ = true;
    last_bass_error_ = 0;
    return true;
  }
  last_bass_error_ = BASS_ErrorGetCode();
  WDS_LOG("AudioEngine: music play failed code=%d\n", last_bass_error_);
  music_playing_ = false;
  ensure_keep_alive();
  return false;
}

void AudioEngine::pause_music() {
  if (test_double_ != nullptr) {
    ++test_double_->pause_music_calls;
    music_playing_ = false;
    if (test_double_->update_health_on_pause) {
      test_double_->health = StreamHealth::Paused;
    }
    return;
  }
  if (impl_ == nullptr || impl_->music == 0) {
    music_playing_ = false;
    return;
  }
  const HSTREAM out = impl_->mixer != 0 ? impl_->mixer : impl_->music;
  BASS_ChannelPause(out);
  music_playing_ = false;
  // Keep the output device decoding while paused so resume is not cold.
  ensure_keep_alive();
}

StreamHealth AudioEngine::stream_health() const noexcept {
  if (test_double_ != nullptr) {
    return test_double_->health;
  }
  if (impl_ == nullptr || impl_->music == 0) {
    return StreamHealth::Stopped;
  }
  DWORD active = 0;
  if (impl_->mixer != 0) {
    const DWORD src = BASS_Mixer_ChannelIsActive(impl_->music);
    if (src == BASS_ACTIVE_STOPPED) {
      return StreamHealth::Stopped;
    }
    if (src == BASS_ACTIVE_STALLED) {
      return StreamHealth::Stalled;
    }
#ifdef BASS_ACTIVE_WAITING
    if (src == BASS_ACTIVE_WAITING) {
      return StreamHealth::Stalled;
    }
#endif
#ifdef BASS_ACTIVE_QUEUED
    if (src == BASS_ACTIVE_QUEUED) {
      return StreamHealth::Stalled;
    }
#endif
    if (src == BASS_ACTIVE_PAUSED || src == BASS_ACTIVE_PAUSED_DEVICE) {
      return StreamHealth::Paused;
    }
    active = BASS_ChannelIsActive(impl_->mixer);
  } else {
    active = BASS_ChannelIsActive(impl_->music);
  }
  switch (active) {
    case BASS_ACTIVE_PLAYING:
      return StreamHealth::Playing;
    case BASS_ACTIVE_STALLED:
      return StreamHealth::Stalled;
#ifdef BASS_ACTIVE_WAITING
    case BASS_ACTIVE_WAITING:
#endif
#ifdef BASS_ACTIVE_QUEUED
    case BASS_ACTIVE_QUEUED:
#endif
      return StreamHealth::Stalled;
    case BASS_ACTIVE_PAUSED:
    case BASS_ACTIVE_PAUSED_DEVICE:
      return StreamHealth::Paused;
    case BASS_ACTIVE_STOPPED:
      return StreamHealth::Stopped;
    default:
      return StreamHealth::Stalled;
  }
}

bool AudioEngine::stream_playing() const noexcept {
  return stream_health() == StreamHealth::Playing;
}

bool AudioEngine::stream_stopped() const noexcept {
  return stream_health() == StreamHealth::Stopped;
}

float AudioEngine::effective_music_volume() const noexcept {
  return clamp_gain(music_gain_.load(std::memory_order_acquire));
}

float AudioEngine::effective_sfx_volume() const noexcept {
  return clamp_gain(sfx_gain_.load(std::memory_order_acquire));
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

bool AudioEngine::play_sfx(HitSfxClip clip) { return play_sfx_internal(clip); }

void AudioEngine::ensure_keep_alive() {
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl || impl->keep_alive == 0) {
    return;
  }
  if (BASS_ChannelIsActive(impl->keep_alive) != BASS_ACTIVE_PLAYING) {
    BASS_ChannelSetAttribute(impl->keep_alive, BASS_ATTRIB_VOL, 0.0f);
    BASS_ChannelPlay(impl->keep_alive, TRUE);
  }
}

void AudioEngine::pause_keep_alive() {
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl || impl->keep_alive == 0) {
    return;
  }
  if (BASS_ChannelIsActive(impl->keep_alive) == BASS_ACTIVE_PLAYING) {
    BASS_ChannelPause(impl->keep_alive);
  }
}

std::uint64_t AudioEngine::align_music_bytes(std::uint64_t bytes) const noexcept {
  if (impl_ == nullptr || impl_->music_bpf == 0) {
    return bytes;
  }
  return (bytes / impl_->music_bpf) * impl_->music_bpf;
}

bool AudioEngine::play_sfx_internal(HitSfxClip clip) {
  if (shutting_down_.load(std::memory_order_seq_cst) ||
      !sfx_ready_.load(std::memory_order_acquire) || clip == HitSfxClip::Hold ||
      clip == HitSfxClip::Count) {
    return false;
  }
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl) {
    return false;
  }
  const size_t idx = static_cast<size_t>(clip);
  if (!impl->sample_ok[idx] || impl->samples[idx] == 0) {
    return false;
  }
  // BGM already keeps the device hot. Starting keep-alive alongside music causes
  // dual-stream resampling that can intermittently drop or smear hit attacks.
  if (music_playing_.load(std::memory_order_acquire)) {
    pause_keep_alive();
  } else {
    ensure_keep_alive();
  }
  if (shutting_down_.load(std::memory_order_seq_cst) ||
      std::atomic_load(&impl_).get() != impl.get()) {
    return false;
  }
  if (impl->mixer != 0) {
    return mix_sfx_on_mixer(clip);
  }
  // Prefer a fresh voice so rapid same-clip hits (e.g. 32nds) do not restart an
  // older channel. If the sample's max polyphony is exhausted, fall back to a
  // recycled OVER_POS voice so the new hit is heard instead of silently dropped.
  HCHANNEL ch = BASS_SampleGetChannel(impl->samples[idx], BASS_SAMCHAN_NEW);
  if (ch == 0) {
    ch = BASS_SampleGetChannel(impl->samples[idx], 0);
  }
  if (ch == 0) {
    return false;
  }
  if (shutting_down_.load(std::memory_order_seq_cst)) {
    return false;
  }
  BASS_ChannelSetAttribute(ch, BASS_ATTRIB_VOL, effective_sfx_volume());
  return BASS_ChannelPlay(ch, TRUE) != FALSE;
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

bool AudioEngine::mix_sfx_on_mixer(HitSfxClip clip) {
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl || impl->mixer == 0) {
    return false;
  }
  const size_t idx = static_cast<size_t>(clip);
  if (!impl->sample_ok[idx] || impl->samples[idx] == 0) {
    return false;
  }
  const HSTREAM sfx =
      BASS_SampleGetChannel(impl->samples[idx], BASS_SAMCHAN_STREAM | BASS_STREAM_DECODE);
  if (sfx == 0) {
    WDS_LOG("AudioEngine: SFX decode stream failed code=%d\n", BASS_ErrorGetCode());
    return false;
  }
  BASS_ChannelSetAttribute(sfx, BASS_ATTRIB_VOL, effective_sfx_volume());
  const DWORD flags = BASS_STREAM_AUTOFREE | BASS_MIXER_CHAN_NORAMPIN;
  if (!BASS_Mixer_StreamAddChannel(impl->mixer, sfx, flags)) {
    WDS_LOG("AudioEngine: Mixer add SFX failed code=%d\n", BASS_ErrorGetCode());
    BASS_StreamFree(sfx);
    return false;
  }
  return true;
}

void AudioEngine::remove_mixer_sfx_sources() {
  if (impl_ == nullptr || impl_->mixer == 0) {
    return;
  }
  const DWORD count = BASS_Mixer_StreamGetChannels(impl_->mixer, nullptr, 0);
  if (count == 0 || count == static_cast<DWORD>(-1)) {
    return;
  }
  std::vector<DWORD> chans(count);
  const DWORD got = BASS_Mixer_StreamGetChannels(impl_->mixer, chans.data(), count);
  for (DWORD i = 0; i < got; ++i) {
    if (chans[i] == 0 || chans[i] == impl_->music) {
      continue;
    }
    BASS_Mixer_ChannelRemove(chans[i]);
  }
  impl_->hold_ch = 0;
  impl_->hold_playing = false;
}

void AudioEngine::handle_sfx_sync(unsigned long long sync_handle, void* payload, HitSfxClip clip) {
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl) {
    return;
  }
  SyncInFlightGuard in_flight(impl->sync_in_flight);
  if (shutting_down_.load(std::memory_order_seq_cst)) {
    return;
  }

  auto* slot = static_cast<SfxSyncSlot*>(payload);
  if (!impl->owns_slot(slot) || !slot_matches_callback_handle(*slot, sync_handle)) {
    return;
  }

  InCallbackGuard in_callback(slot);
  bool won_play = false;
  {
    std::lock_guard<std::mutex> lock(impl->sfx_mu);
    auto& syncs = impl->pending_syncs;
    bool found_pending = false;
    for (size_t i = 0; i < syncs.size(); ++i) {
      if (pending_sync_identifies(syncs[i]->handle.load(std::memory_order_acquire), sync_handle,
                                  syncs[i] == slot)) {
        found_pending = true;
        won_play = claim_sfx_sync_callback_play(slot->claim, true);
        slot->sync_detached.store(true, std::memory_order_release);
        syncs.erase(syncs.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    if (!found_pending) {
      (void)claim_sfx_sync_callback_play(slot->claim, false);
    }
  }
  if (won_play && clip != HitSfxClip::Count &&
      !shutting_down_.load(std::memory_order_seq_cst) &&
      std::atomic_load(&impl_).get() == impl.get()) {
    play_sfx_internal(clip);
  }
}

void AudioEngine::warmup_sfx() {
  if (!sfx_ready_ || impl_ == nullptr) {
    return;
  }
  ensure_keep_alive();
  // Mixer path: Hold is plugged in on demand. Device path primes the loop channel.
  if (impl_->mixer != 0) {
    return;
  }
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

size_t AudioEngine::pending_sfx_sync_count() const noexcept {
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl->sfx_mu);
  return impl->pending_syncs.size();
}

size_t AudioEngine::snapshot_pending_sfx_syncs(void** out_slots, unsigned long long* out_handles,
                                              size_t max_count) const noexcept {
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl->sfx_mu);
  const size_t n = std::min(max_count, impl->pending_syncs.size());
  for (size_t i = 0; i < n; ++i) {
    SfxSyncSlot* slot = impl->pending_syncs[i];
    if (out_slots != nullptr) {
      out_slots[i] = slot;
    }
    if (out_handles != nullptr) {
      out_handles[i] = slot != nullptr ? slot->handle.load(std::memory_order_acquire) : 0;
    }
  }
  return n;
}

bool AudioEngine::inspect_sfx_sync_slot(const void* slot, unsigned long long* handle, int* claim,
                                        bool* in_use) const noexcept {
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl) {
    return false;
  }
  auto* typed = static_cast<SfxSyncSlot*>(const_cast<void*>(slot));
  if (!impl->owns_slot(typed)) {
    return false;
  }
  if (handle != nullptr) {
    *handle = typed->handle.load(std::memory_order_acquire);
  }
  if (claim != nullptr) {
    *claim = static_cast<int>(typed->claim.load(std::memory_order_acquire));
  }
  if (in_use != nullptr) {
    *in_use = typed->in_use.load(std::memory_order_acquire);
  }
  return true;
}

bool AudioEngine::schedule_sfx_at(HitSfxClip clip, wds::common::Microseconds at) {
  // Single-writer with clear_scheduled_sfx (audio control / main thread).
  // The MIXTIME SYNCPROC is the only concurrent mutator.
  if (shutting_down_.load(std::memory_order_seq_cst) ||
      !sfx_ready_.load(std::memory_order_acquire) || clip == HitSfxClip::Hold ||
      clip == HitSfxClip::Count) {
    return false;
  }
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl) {
    return false;
  }
  if (!impl->sample_ok[static_cast<size_t>(clip)]) {
    return false;
  }
  if (impl->music == 0) {
    return play_sfx_internal(clip);
  }

  // Heard playtime (mixer source), not decode frontier. MIXTIME sync then plugs
  // SFX into the same mix cycle as the music byte — not a device ChannelPlay.
  const QWORD target = align_music_bytes(us_to_bytes(impl->music, at));
  const QWORD play_pos = music_heard_bytes();
  if (target <= play_pos) {
    // Past/due one-shots ignore the future pending cap.
    return play_sfx_internal(clip);
  }

  // Horizon uses aligned target bytes vs heard bytes (same BASS conversion).
  const int64_t target_us = bytes_to_us(impl->music, target).count();
  const int64_t heard_us = bytes_to_us(impl->music, play_pos).count();

  // Track the slot before SetSync so an immediate callback can resolve the entry.
  // admit + push share one lock; TooFar still wins over AtCapacity. No dummy count.
  // Do not hold sfx_mu across SetSync (callback takes the same lock).
  SfxSyncSlot* slot = nullptr;
  uint32_t slot_gen = 0;
  SfxSyncAdmit admit = SfxSyncAdmit::WithinWindow;
  {
    std::lock_guard<std::mutex> lock(impl->sfx_mu);
    admit = admit_sfx_sync(target_us, heard_us, impl->pending_syncs.size());
    if (admit == SfxSyncAdmit::WithinWindow) {
      slot = impl->acquire_slot_locked();
      if (slot == nullptr) {
        admit = SfxSyncAdmit::AtCapacity;
      } else {
        slot->claim.store(SfxSyncClaim::Pending, std::memory_order_release);
        slot->handle.store(0, std::memory_order_release);
        slot->clip = clip;
        slot->sync_detached.store(false, std::memory_order_release);
        slot->engine.store(this, std::memory_order_release);
        slot->in_use.store(true, std::memory_order_release);
        slot_gen = slot->generation.load(std::memory_order_acquire);
        impl->pending_syncs.push_back(slot);
      }
    }
  }
  if (admit == SfxSyncAdmit::TooFar || admit == SfxSyncAdmit::AtCapacity) {
    return false;
  }
  if (admit == SfxSyncAdmit::PastOrDue || slot == nullptr) {
    return play_sfx_internal(clip);
  }

  auto same_generation = [&]() noexcept {
    return slot->generation.load(std::memory_order_acquire) == slot_gen;
  };

  auto detach_and_recycle = [&](bool remove_sync, HSYNC sync_handle) {
    if (remove_sync && sync_handle != 0 && impl->music != 0) {
      BASS_ChannelRemoveSync(impl->music, sync_handle);
    }
    if (!same_generation()) {
      return;
    }
    slot->sync_detached.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(impl->sfx_mu);
      impl->unlink_pending_locked(slot);
      impl->try_recycle_locked(slot);
    }
  };

  if (shutting_down_.load(std::memory_order_seq_cst)) {
    (void)claim_sfx_sync_cancel(slot->claim);
    detach_and_recycle(false, 0);
    return false;
  }
  const HSYNC sync = BASS_ChannelSetSync(
      impl->music, BASS_SYNC_POS | BASS_SYNC_MIXTIME | BASS_SYNC_ONETIME, target, sfx_pos_sync_proc,
      slot);
  if (sync != 0 && same_generation()) {
    slot->handle.store(static_cast<unsigned long long>(sync), std::memory_order_release);
  }
  if (!same_generation()) {
    return true;
  }

  bool found_pending = false;
  SfxSyncClaim claim = SfxSyncClaim::Pending;
  {
    std::lock_guard<std::mutex> lock(impl->sfx_mu);
    claim = slot->claim.load(std::memory_order_acquire);
    for (SfxSyncSlot* pending : impl->pending_syncs) {
      if (pending == slot) {
        found_pending = true;
        break;
      }
    }
  }

  const bool due = target <= music_heard_bytes();
  const SfxSyncArmResult result = decide_arm_resolution(sync != 0, found_pending, claim, due);

  if (result == SfxSyncArmResult::CallbackConsumed) {
    return true;
  }
  if (result == SfxSyncArmResult::CancelledDuringArm) {
    // D: clear/shutdown cancelled during the handle==0 window. Remove the
    // just-created sync ourselves so it is never left armed with this slot.
    detach_and_recycle(sync != 0, sync);
    return false;
  }
  if (result == SfxSyncArmResult::RetryLater) {
    (void)claim_sfx_sync_cancel(slot->claim);
    detach_and_recycle(sync != 0, sync);
    if (sync == 0) {
      WDS_LOG("AudioEngine: ChannelSetSync failed code=%d\n", BASS_ErrorGetCode());
    }
    return false;
  }
  if (result == SfxSyncArmResult::PlayDue) {
    const SfxSyncPlayClaim play = finish_due_play_claim(slot->claim);
    if (play == SfxSyncPlayClaim::AlreadyFired) {
      return true;
    }
    if (play == SfxSyncPlayClaim::Cancelled) {
      detach_and_recycle(sync != 0, sync);
      return false;
    }
    detach_and_recycle(sync != 0, sync);
    return play_sfx_internal(clip);
  }

  return true;
}

void AudioEngine::clear_scheduled_sfx() {
  // Single-writer with schedule_sfx_at (audio control / main thread). The
  // MIXTIME SYNCPROC is the only concurrent mutator. Entries still in the
  // handle==0 window (SetSync not yet returned) stay in_use with claim
  // Cancelled; the arming code itself RemoveSyncs the handle when it appears.
  const std::shared_ptr<Impl> impl = std::atomic_load(&impl_);
  if (!impl) {
    return;
  }
  std::vector<SfxSyncSlot*> doomed;
  {
    std::lock_guard<std::mutex> lock(impl->sfx_mu);
    doomed.swap(impl->pending_syncs);
    for (SfxSyncSlot* slot : doomed) {
      if (slot != nullptr) {
        (void)claim_sfx_sync_cancel(slot->claim);
      }
    }
  }
  if (impl->music != 0) {
    for (SfxSyncSlot* slot : doomed) {
      if (slot == nullptr) {
        continue;
      }
      const unsigned long long handle = slot->handle.load(std::memory_order_acquire);
      if (handle != 0) {
        BASS_ChannelRemoveSync(impl->music, static_cast<HSYNC>(handle));
        slot->sync_detached.store(true, std::memory_order_release);
      }
    }
  } else {
    for (SfxSyncSlot* slot : doomed) {
      if (slot != nullptr) {
        slot->sync_detached.store(true, std::memory_order_release);
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(impl->sfx_mu);
    for (SfxSyncSlot* slot : doomed) {
      impl->try_recycle_locked(slot);
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
  if (impl_->mixer != 0) {
    if (enabled) {
      if (impl_->hold_ch == 0) {
        impl_->hold_ch = BASS_SampleGetChannel(impl_->samples[hold_idx],
                                               BASS_SAMCHAN_STREAM | BASS_STREAM_DECODE);
        if (impl_->hold_ch == 0) {
          WDS_LOG("AudioEngine: Hold decode stream failed code=%d\n", BASS_ErrorGetCode());
          return;
        }
        BASS_ChannelSetAttribute(impl_->hold_ch, BASS_ATTRIB_VOL, effective_sfx_volume());
        if (!BASS_Mixer_StreamAddChannel(impl_->mixer, impl_->hold_ch, BASS_MIXER_CHAN_NORAMPIN)) {
          WDS_LOG("AudioEngine: Mixer add Hold failed code=%d\n", BASS_ErrorGetCode());
          BASS_StreamFree(impl_->hold_ch);
          impl_->hold_ch = 0;
          return;
        }
      } else {
        BASS_ChannelSetAttribute(impl_->hold_ch, BASS_ATTRIB_VOL, effective_sfx_volume());
        BASS_ChannelSetPosition(impl_->hold_ch, 0, BASS_POS_BYTE);
        BASS_Mixer_ChannelFlags(impl_->hold_ch, 0, BASS_MIXER_CHAN_PAUSE);
      }
      impl_->hold_playing = true;
    } else {
      if (impl_->hold_ch != 0) {
        BASS_Mixer_ChannelFlags(impl_->hold_ch, BASS_MIXER_CHAN_PAUSE, BASS_MIXER_CHAN_PAUSE);
      }
      impl_->hold_playing = false;
    }
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
  if (impl_->mixer != 0) {
    remove_mixer_sfx_sources();
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
