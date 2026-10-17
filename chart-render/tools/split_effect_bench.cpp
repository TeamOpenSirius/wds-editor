#include <wds/chart_render/shuriken/evaluate.hpp>
#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/chart_render/split_effect_particles.hpp>
#include <wds/chart_render/stage_geometry.hpp>
#include "split_wall_profile.hpp"
#include "shuriken/internal.hpp"
#include "shuriken/split_eval_carry.hpp"
#include <wds/core/gimmick.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/preview_snapshot.hpp>
#include <wds/core/split_fade.hpp>

#if __has_include(<wds/common/perf_stages.hpp>)
#include <wds/common/perf_stages.hpp>
#else
#include <string>
namespace wds::common::perf {
inline void set_enabled(bool) noexcept {}
inline void end_frame() noexcept {}
inline std::string window_report() { return {}; }
}  // namespace wds::common::perf
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <poll.h>
#include <random>
#include <tuple>
#include <signal.h>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>
#include <fcntl.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {

using wds::chart_editor::PreviewSplitLaneInstance;
using wds::chart_editor::SplitLaneType;
using wds::chart_render::SplitEffectFrameCollect;
using wds::chart_render::SplitEffectLiveParticle;
using wds::chart_render::SplitEffectTrailChan;
using wds::chart_render::SplitEffectParticle;
using wds::chart_render::SplitEffectPlaybackState;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGoldenMagic = 0x50534457u;  // 'WDSP' little-endian
constexpr uint32_t kGoldenVersionFull = 1;
constexpr uint32_t kGoldenVersionHash = 2;
constexpr int kGoldenHashGroupCount = 8;
constexpr uint8_t kGoldenSampleOk = 0;
constexpr uint8_t kGoldenSampleTimeout = 1;
constexpr double kDefaultGroupTimeoutSec = 300.0;

enum class GoldenTier : uint32_t { Full = 0, Quick = 1 };

enum class SampleOrder : uint32_t { Normal = 0, Reverse = 1, Shuffle = 2 };

SampleOrder g_sample_order = SampleOrder::Normal;

constexpr int32_t kQuickAlwaysEffects[] = {10660, 10390, 11712, 3010, 11621, 10516};
constexpr double kScanTmaxSec = 5.0;
constexpr double kScanEffectTimeoutSec = 60.0;
constexpr int64_t kDefaultStartMs = 10000;
constexpr int kDefaultFrames = 120;
constexpr uint32_t kDefaultSeed = 1;
constexpr int kDefaultInstances = 4;
constexpr int kInstanceSetSingle = 0;
constexpr int kInstanceSetOverlap4Stagger0 = 1;
constexpr int kInstanceSetOverlap4Stagger02 = 2;
constexpr int kDefaultScenarioEffect = 10660;
constexpr int kOverlapInstanceCount = 4;
constexpr int kOverlapSampleCount = 20;
constexpr double kOverlapStaggers[] = {0.0, 0.2};

// Fixed single-instance times. Do not pick a heaviest effect at write time.
constexpr double kGoldenTimes[] = {0.05, 0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 4.5};

struct SampleKey {
  int32_t effect_id = 0;
  int32_t split_count = 0;
  int32_t instance_set = 0;
  int32_t instance_count = 1;
  double now_t = 0.0;
  int particle_count = 0;
};

const char* format_key(const SampleKey& k, char* buf, size_t n) {
  std::snprintf(buf, n, "effect=%d split_count=%d set=%d inst=%d now_t=%.5f n=%d", k.effect_id,
                k.split_count, k.instance_set, k.instance_count, k.now_t, k.particle_count);
  return buf;
}

void usage() {
  std::fprintf(stdout,
               "wds_split_effect_bench — collect-cost harness (does not change simulation)\n"
               "\n"
               "  scan [--quality 0] [--split-count 6] [--type full] [--dt 0.0333]\n"
               "       [--tmax SEC] [--effect-timeout SEC] [--only ID[,ID...]] [--from-index N]\n"
               "       [--jobs N] [--csv PATH]\n"
               "      Sweep now_t in [0, tmax] for official effect ids (stateless collect).\n"
               "\n"
               "  scenario --effect ID [--instances 4] [--stagger 0|SEC]\n"
               "           [--mode play|scrub|cold|dead|seek-play|all] [--quality 0]\n"
               "           [--frames N] [--seed S] [--now-t T] [--now-t-hi T] [--warm-from-t T]\n"
               "           [--warm-frames N] [--dt SEC]\n"
               "      Overlapping instances, peak-count window, per-mode timings + perf stages.\n"
               "      --warm-from-t plays untimed 1/60 frames from T up to --now-t (app seek warm).\n"
               "      --warm-frames plays N untimed frames in-range before the measured --frames.\n"
               "      --mode seek-play: editor seek (clear caches + dead latches) then play 1/60\n"
               "      advancing from T for N frames. Prints SEEKFRAME rows. T is play-relative.\n"
               "      --mode dead: full fixture (past crowd + live + future) after all live\n"
               "      particles died and before the next group starts. Default now_t 3.5–5.0.\n"
               "\n"
               "  golden-write --out FILE [--scenario-effect 10660] [--full] [--tier full|quick]\n"
               "               [--jobs N] [--group-timeout SEC]\n"
               "  golden-compare --baseline FILE [--scenario-effect 10660] [--tier full|quick]\n"
               "                 [--exact | --tol-pos 1e-3 --tol-color 0.004] [--max-report N]\n"
               "                 [--jobs N] [--group-timeout SEC]\n"
               "                 [--sample-order normal|reverse|shuffle]\n"
               "      Hash golden (v2, default) or full v1 with --full. Tolerance needs v1 baseline.\n"
               "      --sample-order shuffles or reverses times within a group process so the\n"
               "      integrator cache is exercised across unrelated ages; hashes must still match.\n"
               "\n"
               "  golden-convert --in V1 --out V2 [--tier full|quick]\n"
               "  golden-compare-files A B [--max-report N]\n"
               "  golden-dump --key \"E SC NT\" --out FILE [--scenario-effect 10660]\n"
               "  geom-dump --effect ID[,ID...] --out FILE.json [--now-t T] [--line 0]\n"
               "            [--w 1920] [--h 1080]\n"
               "      Drawn-geometry stats (StageGeometry + Ribbon Cross). geom-v1 hop.\n"
               "      Single-sample v1 dump; key is effect_id split_count now_t (optional set/inst).\n"
               "  raster --effect ID --now-t T --out FILE.ppm [--instances 4] [--stagger 0.2]\n"
               "         [--w 960] [--h 540] [--bounds xmin,xmax,ymin,ymax]\n"
               "      Software splat of live particles (world_x / main_y). Prints bounds.\n"
               "\n"
               "  noise-error [--scenario-effect 10660] [--tier full|quick]\n"
               "      Approx (analytic curl) vs exact FD. No-noise effects must be bit-identical.\n"
               "      Noise effects: count rel p99 < 2 percent, matched-particle pixel p99 < 0.5.\n"
               "      hop, not 2022.3.\n"
               "  noise-catalog\n"
               "      List official effects with active Noise / noise-driven sub-emitters.\n"
               "  noise-hold-plan\n"
               "      Per-system hold-4 travel, live estimate, chosen N (hop, not 2022.3).\n"
               "\n"
               "  carry-check [--only ID[,ID...]] [--quick]\n"
               "      Play sequences (dt 1/60 and random dt in [1/240, 0.25], plus backward\n"
               "      jumps and seeks) for catalog effects x split_count {1,6} x instances\n"
               "      {1, 4 stagger 0, 4 stagger 0.2}. Each frame hash must match stateless.\n"
               "\n"
               "  persist-check [--only ID[,ID...]] [--quick]\n"
               "      Persistent store vs remake, byte-identical. Sequences: 1/60, 1/30,\n"
               "      0.25, 0.5, backward, stop/restart, cross-fade-end, seek-fade-mid,\n"
               "      seek-into-fade. Full official set unless --quick.\n"
               "  persist-ab --effect ID [--quality 1]\n"
               "      Same-load interleaved persist on/off: play, jumps, scrub, backward.\n"
               "  persist-fade-frac [--effect ID[,ID...]] [--frames N]\n"
               "      Fade-in vs post-fade frame counts for t=0 play and the peak play window.\n"
               "  persist-phase [--effect ID[,ID...]] [--quality 1] [--frames N] [--persist -1|0|1]\n"
               "      Play 2xFull6 and print collect p50/p95/max for fade-in vs post-fade.\n"
               "      --persist -1 (default) runs on then off; 0 or 1 runs one side only.\n"
               "  persist-rss\n"
               "      3000-frame play+seek across several effects; prints peak RSS.\n"
               "\n"
               "  resume-gap [--quality 1] [--tk 0.7] [--samples 6]\n"
               "      Snapshot at T_k, one evaluate at T_k+g for g=0.25/0.5/1/2/4s.\n"
               "      11621/10380/10660/11711 ×4 stagger 0.2. Exact vs remake + cost.\n"
               "  kf-memory --chart PATH [--gap 0.25,0.5,1,2] [--quality 1]\n"
               "      Hypothetical whole-chart snapshot bytes in note windows (alive slots).\n"
               "\n"
               "  dead-skip-check [--tmax 8] [--dt 0.05] [--instances 1] [--split-count 2]\n"
               "                  [--only ID[,ID...]]\n"
               "      Bit-exact A/B of dead-system skip vs full evaluate over now_t.\n"
               "\n"
               "  schedule-compare [--mode trails|rod|all] [--effect ID[,ID...]] [--tier quick]\n"
               "                   [--split-count N] [--now-t T] [--analyze-outliers]\n"
               "      In-process A/B of WDS_SPLIT_INTEGRATOR (legacy vs grid). Trails: screen\n"
               "      length / Hausdorff / head px plus FD vs analytic |total|. RoD: count\n"
               "      deltas vs a 960 Hz grid reference. --analyze-outliers ranks the worst\n"
               "      trail pairs and compares each to a 960 Hz grid reference.\n");
}

PreviewSplitLaneInstance make_split(int32_t effect_id, int32_t split_count, SplitLaneType type,
                                    int64_t start_ms, int32_t source_note_id = 0) {
  PreviewSplitLaneInstance split;
  const int32_t src = source_note_id != 0 ? source_note_id : effect_id;
  split.apply_identity(src, split_count, type, effect_id, start_ms, start_ms + 30000);
  split.apply_state(true, false, split_count + 1);
  split.apply_animation(1.0f, 0.0f, 1.0f, 0.0f, 1);
  split.should_show = true;
  split.split_line_alpha = 1.0f;
  return split;
}

double play_origin_sec(int64_t start_ms) {
  return static_cast<double>(start_ms) / 1000.0 -
         static_cast<double>(wds::chart_editor::kOfficialSplitFadeInSec);
}

double now_sec_from_now_t(int64_t start_ms, double now_t) {
  return play_origin_sec(start_ms) + now_t;
}

SplitLaneType parse_type(const std::string& s) {
  std::string t = s;
  for (char& c : t) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  if (t == "full") {
    return SplitLaneType::Full;
  }
  if (t == "bothends" || t == "both" || t == "both_ends") {
    return SplitLaneType::BothEnds;
  }
  if (t == "light") {
    return SplitLaneType::Light;
  }
  if (t == "ignore") {
    return SplitLaneType::Ignore;
  }
  return SplitLaneType::Full;
}

const char* type_name(SplitLaneType t) {
  return wds::chart_editor::split_lane_type_name(t);
}

bool eq_i(const char* a, const char* b) {
  return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

void apply_sample_order(std::vector<double>& times, uint32_t seed) {
  if (g_sample_order == SampleOrder::Reverse) {
    std::reverse(times.begin(), times.end());
  } else if (g_sample_order == SampleOrder::Shuffle) {
    std::mt19937 rng(seed != 0 ? seed : 1u);
    std::shuffle(times.begin(), times.end(), rng);
  }
}

SampleOrder parse_sample_order(const char* s) {
  if (eq_i(s, "reverse")) {
    return SampleOrder::Reverse;
  }
  if (eq_i(s, "shuffle")) {
    return SampleOrder::Shuffle;
  }
  return SampleOrder::Normal;
}

std::vector<PreviewSplitLaneInstance> make_dead_fixture(int32_t effect_id, int instances) {
  std::vector<PreviewSplitLaneInstance> out;
  const int n = std::max(instances, 1);
  // Past notes: cheap 3010, started 10s before the live group (now_t >> keep).
  if (wds::chart_render::official_split_effect(3010) != nullptr) {
    for (int i = 0; i < n; ++i) {
      out.push_back(make_split(3010, 6, SplitLaneType::Full, 0, 30100 + i));
    }
  }
  for (int i = 0; i < n; ++i) {
    out.push_back(make_split(effect_id, 6, SplitLaneType::Full, kDefaultStartMs, effect_id * 10 + i + 1));
  }
  // Future notes: same effect 10s later, still now_t < 0 in the gap.
  for (int i = 0; i < n; ++i) {
    out.push_back(make_split(effect_id, 6, SplitLaneType::Full, kDefaultStartMs + 10000,
                            effect_id * 10 + 100 + i + 1));
  }
  return out;
}

std::vector<PreviewSplitLaneInstance> make_instances(int32_t effect_id, int instances,
                                                     double stagger_sec, int32_t split_count,
                                                     SplitLaneType type) {
  std::vector<PreviewSplitLaneInstance> out;
  out.reserve(static_cast<size_t>(std::max(instances, 0)));
  for (int i = 0; i < instances; ++i) {
    const int64_t start_ms =
        kDefaultStartMs + static_cast<int64_t>(std::llround(stagger_sec * 1000.0 * i));
    out.push_back(make_split(effect_id, split_count, type, start_ms, effect_id * 10 + i + 1));
  }
  return out;
}

struct CollectFaults {
  long minflt = 0;
  long majflt = 0;
  int n = 0;
};

CollectFaults g_collect_faults{};

void mark_collect_faults() {
  g_collect_faults = {};
}

double collect_ms(const std::vector<PreviewSplitLaneInstance>& splits, int quality, double now_sec,
                  bool playing, SplitEffectPlaybackState& state, SplitEffectFrameCollect& io) {
  rusage ru0{};
  rusage ru1{};
  getrusage(RUSAGE_SELF, &ru0);
  const auto t0 = Clock::now();
  wds::chart_render::collect_split_effect_frame(splits, quality, now_sec, playing, state, io);
  const auto t1 = Clock::now();
  getrusage(RUSAGE_SELF, &ru1);
  g_collect_faults.minflt += ru1.ru_minflt - ru0.ru_minflt;
  g_collect_faults.majflt += ru1.ru_majflt - ru0.ru_majflt;
  ++g_collect_faults.n;
  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  wds::chart_render::split_effect_note_prev_frame_ms(ms);
  return ms;
}

struct MsStats {
  std::vector<double> samples;

  void add(double ms) { samples.push_back(ms); }

  double avg() const {
    if (samples.empty()) {
      return 0.0;
    }
    double s = 0.0;
    for (double v : samples) {
      s += v;
    }
    return s / static_cast<double>(samples.size());
  }

  double max() const {
    if (samples.empty()) {
      return 0.0;
    }
    return *std::max_element(samples.begin(), samples.end());
  }

  double pct(double p) const {
    if (samples.empty()) {
      return 0.0;
    }
    std::vector<double> copy = samples;
    std::sort(copy.begin(), copy.end());
    const double u = std::clamp(p, 0.0, 1.0);
    const size_t i = std::min(copy.size() - 1,
                              static_cast<size_t>(u * static_cast<double>(copy.size() - 1)));
    return copy[i];
  }
};

struct ParticleStats {
  int64_t sum = 0;
  int max = 0;
  int frames = 0;

  void add(int n) {
    sum += n;
    max = std::max(max, n);
    ++frames;
  }

  double avg() const {
    return frames == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(frames);
  }
};

void choose_peak_range(const std::vector<PreviewSplitLaneInstance>& splits, int quality,
                       int frames, double& range_lo, double& range_hi) {
  if (splits.empty()) {
    range_lo = play_origin_sec(kDefaultStartMs);
    range_hi = range_lo + 2.0;
    return;
  }
  const double first = play_origin_sec(splits.front().start_ms);
  const double last = play_origin_sec(splits.back().start_ms);
  double overlap_lo = last;
  double overlap_hi = first + static_cast<double>(wds::chart_render::kSplitEffectLinePlayWindowSec);
  if (overlap_hi <= overlap_lo) {
    overlap_hi = overlap_lo + 2.0;
  }

  SplitEffectFrameCollect io;
  SplitEffectPlaybackState state;
  int best_n = -1;
  double best_now = overlap_lo;
  const double probe_dt = 0.05;
  for (double now_sec = overlap_lo; now_sec <= overlap_hi + 1e-12; now_sec += probe_dt) {
    wds::chart_render::collect_split_effect_frame(splits, quality, now_sec, false, state, io);
    if (io.total > best_n) {
      best_n = io.total;
      best_now = now_sec;
    }
  }

  const double need = std::max(0.0, static_cast<double>(std::max(frames, 1) - 1) / 60.0);
  double half = std::max(0.25, 0.5 * need);
  range_lo = std::max(overlap_lo, best_now - half);
  range_hi = std::min(overlap_hi, range_lo + std::max(need, 0.5));
  if (range_hi > overlap_hi) {
    range_hi = overlap_hi;
    range_lo = std::max(overlap_lo, range_hi - std::max(need, 0.5));
  }
  if (range_hi <= range_lo) {
    range_lo = overlap_lo;
    range_hi = std::min(overlap_hi, overlap_lo + std::max(need, 0.5));
  }
}

// --- binary particle dump ---------------------------------------------------

bool write_raw(FILE* f, const void* p, size_t n) {
  return std::fwrite(p, 1, n, f) == n;
}

bool read_raw(FILE* f, void* p, size_t n) {
  return std::fread(p, 1, n, f) == n;
}

bool write_u8(FILE* f, uint8_t v) { return write_raw(f, &v, sizeof(v)); }
bool write_u16(FILE* f, uint16_t v) { return write_raw(f, &v, sizeof(v)); }
bool write_u32(FILE* f, uint32_t v) { return write_raw(f, &v, sizeof(v)); }
bool write_i32(FILE* f, int32_t v) { return write_raw(f, &v, sizeof(v)); }
bool write_f32(FILE* f, float v) { return write_raw(f, &v, sizeof(v)); }
bool write_f64(FILE* f, double v) { return write_raw(f, &v, sizeof(v)); }
bool write_u64(FILE* f, uint64_t v) { return write_raw(f, &v, sizeof(v)); }

bool read_u8(FILE* f, uint8_t& v) { return read_raw(f, &v, sizeof(v)); }
bool read_u16(FILE* f, uint16_t& v) { return read_raw(f, &v, sizeof(v)); }
bool read_u32(FILE* f, uint32_t& v) { return read_raw(f, &v, sizeof(v)); }
bool read_i32(FILE* f, int32_t& v) { return read_raw(f, &v, sizeof(v)); }
bool read_f32(FILE* f, float& v) { return read_raw(f, &v, sizeof(v)); }
bool read_f64(FILE* f, double& v) { return read_raw(f, &v, sizeof(v)); }
bool read_u64(FILE* f, uint64_t& v) { return read_raw(f, &v, sizeof(v)); }

bool write_f32_vec(FILE* f, const std::vector<float>& v) {
  if (!write_u32(f, static_cast<uint32_t>(v.size()))) {
    return false;
  }
  if (v.empty()) {
    return true;
  }
  return write_raw(f, v.data(), v.size() * sizeof(float));
}

bool write_f32_vec(FILE* f, const SplitEffectTrailChan& v) {
  if (!write_u32(f, static_cast<uint32_t>(v.size()))) {
    return false;
  }
  if (v.empty()) {
    return true;
  }
  return write_raw(f, v.data, static_cast<size_t>(v.n) * sizeof(float));
}

[[maybe_unused]] bool read_f32_vec(FILE* f, std::vector<float>& v) {
  uint32_t n = 0;
  if (!read_u32(f, n)) {
    return false;
  }
  v.resize(n);
  if (n == 0) {
    return true;
  }
  return read_raw(f, v.data(), static_cast<size_t>(n) * sizeof(float));
}

wds::chart_render::SplitEffectTrailArena& bench_read_trails() {
  static wds::chart_render::SplitEffectTrailArena arena;
  return arena;
}

bool read_f32_vec(FILE* f, SplitEffectTrailChan& v) {
  uint32_t n = 0;
  if (!read_u32(f, n)) {
    return false;
  }
  if (n == 0) {
    v.clear();
    return true;
  }
  if (n > 0xffffu) {
    return false;
  }
  v.data = bench_read_trails().alloc(n);
  v.n = static_cast<uint16_t>(n);
  return read_raw(f, v.data, static_cast<size_t>(n) * sizeof(float));
}

struct FileWriter {
  FILE* f = nullptr;
  bool write_raw(const void* p, size_t n) const { return ::write_raw(f, p, n); }
  bool write_u8(uint8_t v) const { return ::write_u8(f, v); }
  bool write_u16(uint16_t v) const { return ::write_u16(f, v); }
  bool write_u32(uint32_t v) const { return ::write_u32(f, v); }
  bool write_i32(int32_t v) const { return ::write_i32(f, v); }
  bool write_f32(float v) const { return ::write_f32(f, v); }
  bool write_f64(double v) const { return ::write_f64(f, v); }
  bool write_f32_vec(const std::vector<float>& v) const { return ::write_f32_vec(f, v); }
  bool write_f32_vec(const SplitEffectTrailChan& v) const { return ::write_f32_vec(f, v); }
};

struct BufferWriter {
  std::vector<uint8_t>& b;
  bool write_raw(const void* p, size_t n) {
    const auto* bytes = static_cast<const uint8_t*>(p);
    b.insert(b.end(), bytes, bytes + n);
    return true;
  }
  bool write_u8(uint8_t v) { return write_raw(&v, sizeof(v)); }
  bool write_u16(uint16_t v) { return write_raw(&v, sizeof(v)); }
  bool write_u32(uint32_t v) { return write_raw(&v, sizeof(v)); }
  bool write_i32(int32_t v) { return write_raw(&v, sizeof(v)); }
  bool write_f32(float v) { return write_raw(&v, sizeof(v)); }
  bool write_f64(double v) { return write_raw(&v, sizeof(v)); }
  bool write_f32_vec(const std::vector<float>& v) {
    if (!write_u32(static_cast<uint32_t>(v.size()))) {
      return false;
    }
    if (v.empty()) {
      return true;
    }
    return write_raw(v.data(), v.size() * sizeof(float));
  }
  bool write_f32_vec(const SplitEffectTrailChan& v) {
    if (!write_u32(static_cast<uint32_t>(v.size()))) {
      return false;
    }
    if (v.empty()) {
      return true;
    }
    return write_raw(v.data, static_cast<size_t>(v.n) * sizeof(float));
  }
};

constexpr uint64_t kFnv1a64Offset = 14695981039346656037ull;
constexpr uint64_t kFnv1a64Prime = 1099511628211ull;

uint64_t fnv1a64(const void* data, size_t n, uint64_t h = kFnv1a64Offset) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<uint64_t>(p[i]);
    h *= kFnv1a64Prime;
  }
  return h;
}

uint64_t fnv1a64_buffer(const std::vector<uint8_t>& b) {
  return fnv1a64(b.data(), b.size());
}

template <typename W>
bool write_particle(W& w, const SplitEffectLiveParticle& p) {
  return w.write_f32(p.world_x) && w.write_f32(p.main_y) && w.write_f32(p.z) &&
         w.write_f32(p.birth_t) && w.write_i32(p.effect_id) && w.write_i32(p.line_index) &&
         w.write_u32(p.catalog_index) && w.write_u8(p.element_index) && w.write_u32(p.seed) &&
         w.write_f32(p.age) && w.write_f32(p.sampled_lifetime) &&
         w.write_f32(p.remaining_lifetime) && w.write_f32(p.current_size) &&
         w.write_f32(p.current_size_y) && w.write_f32(p.current_size_z) &&
         w.write_f32(p.start_size) && w.write_f32(p.start_size_y) &&
         w.write_f32(p.start_size_z) && w.write_f32(p.start_r) && w.write_f32(p.start_g) &&
         w.write_f32(p.start_b) && w.write_f32(p.start_a) && w.write_f32(p.current_r) &&
         w.write_f32(p.current_g) && w.write_f32(p.current_b) && w.write_f32(p.current_a) &&
         w.write_f32(p.ang_vx) && w.write_f32(p.ang_vy) && w.write_f32(p.ang_vz) &&
         w.write_f32(p.birth_fade) && w.write_f32(p.birth_parent_z) &&
         w.write_f32(p.birth_origin_y_delta) && w.write_f32(p.effective_distance) &&
         w.write_f32(p.size) && w.write_f32(p.max_particle_size) && w.write_f32(p.r) &&
         w.write_f32(p.g) && w.write_f32(p.b) && w.write_f32(p.a) &&
         w.write_u16(p.uv_name_index) && w.write_u8(p.blend) && w.write_u8(p.sorting_order) &&
         w.write_u16(p.flags) && w.write_u8(p.render_mode) && w.write_f32(p.vel_x) &&
         w.write_f32(p.vel_y) && w.write_f32(p.vel_z) && w.write_f32(p.stored_vx) &&
         w.write_f32(p.stored_vy) && w.write_f32(p.stored_vz) && w.write_f32(p.vol_vx) &&
         w.write_f32(p.vol_vy) && w.write_f32(p.vol_vz) && w.write_f32(p.noise_vx) &&
         w.write_f32(p.noise_vy) && w.write_f32(p.noise_vz) && w.write_f32(p.total_vx) &&
         w.write_f32(p.total_vy) && w.write_f32(p.total_vz) && w.write_f32(p.length_scale) &&
         w.write_f32(p.velocity_scale) && w.write_f32(p.pivot_x) && w.write_f32(p.pivot_y) &&
         w.write_f32(p.pivot_z) && w.write_f32(p.custom1_x) && w.write_f32(p.custom1_y) &&
         w.write_f32(p.emit_u) && w.write_u8(p.inherit_props) &&
         w.write_f32(p.butterfly_wing_x) && w.write_f32(p.sol_y) && w.write_f32(p.size_y) &&
         w.write_f32(p.size_z) && w.write_u8(p.size_3d) && w.write_f32(p.roll) &&
         w.write_f32(p.tilt_x) && w.write_f32(p.tilt_y) && w.write_u8(p.render_alignment) &&
         w.write_u16(p.trail_points) && w.write_f32_vec(p.trail_x) &&
         w.write_f32_vec(p.trail_y) && w.write_f32_vec(p.trail_z) &&
         w.write_f32_vec(p.trail_widths) && w.write_f32_vec(p.trail_color_r) &&
         w.write_f32_vec(p.trail_color_g) && w.write_f32_vec(p.trail_color_b) &&
         w.write_f32_vec(p.trail_color_a) && w.write_f32(p.trail_width) &&
         w.write_u32(p.trail_chain) && w.write_u16(p.trail_vertex) &&
         w.write_u16(p.trail_count) && w.write_f32(p.trail_r) && w.write_f32(p.trail_g) &&
         w.write_f32(p.trail_b) && w.write_f32(p.trail_a) && w.write_f32(p.trail_u0) &&
         w.write_f32(p.trail_u1) && w.write_u16(p.trail_over_alpha_begin) &&
         w.write_u8(p.trail_over_alpha_count) && w.write_u16(p.trail_over_color_begin) &&
         w.write_u8(p.trail_over_color_count) && w.write_u16(p.trail_over_min_alpha_begin) &&
         w.write_u8(p.trail_over_min_alpha_count) && w.write_u16(p.trail_over_min_color_begin) &&
         w.write_u8(p.trail_over_min_color_count) && w.write_u8(p.trail_over_state) &&
         w.write_u8(p.trail_over_mode) && w.write_u8(p.trail_over_min_mode) &&
         w.write_f32(p.trail_over_r) && w.write_f32(p.trail_over_g) &&
         w.write_f32(p.trail_over_b) && w.write_f32(p.trail_over_r_min) &&
         w.write_f32(p.trail_over_g_min) && w.write_f32(p.trail_over_b_min) &&
         w.write_f32(p.trail_color_u) && w.write_f32(p.trail_over_a0) &&
         w.write_f32(p.trail_over_a1) && w.write_f32(p.camera_velocity_scale) &&
         w.write_u8(p.freeform_stretching) && w.write_u8(p.rotate_with_stretch) &&
         w.write_f32(p.noise_rotation_amount) && w.write_f32(p.noise_size_amount) &&
         w.write_u8(p.flip_u) && w.write_u8(p.flip_v);
}

bool write_particle_file(FILE* f, const SplitEffectLiveParticle& p) {
  FileWriter w{f};
  return write_particle(w, p);
}

bool read_particle(FILE* f, SplitEffectLiveParticle& p) {
  return read_f32(f, p.world_x) && read_f32(f, p.main_y) && read_f32(f, p.z) &&
         read_f32(f, p.birth_t) && read_i32(f, p.effect_id) && read_i32(f, p.line_index) &&
         read_u32(f, p.catalog_index) && read_u8(f, p.element_index) && read_u32(f, p.seed) &&
         read_f32(f, p.age) && read_f32(f, p.sampled_lifetime) &&
         read_f32(f, p.remaining_lifetime) && read_f32(f, p.current_size) &&
         read_f32(f, p.current_size_y) && read_f32(f, p.current_size_z) &&
         read_f32(f, p.start_size) && read_f32(f, p.start_size_y) && read_f32(f, p.start_size_z) &&
         read_f32(f, p.start_r) && read_f32(f, p.start_g) && read_f32(f, p.start_b) &&
         read_f32(f, p.start_a) && read_f32(f, p.current_r) && read_f32(f, p.current_g) &&
         read_f32(f, p.current_b) && read_f32(f, p.current_a) && read_f32(f, p.ang_vx) &&
         read_f32(f, p.ang_vy) && read_f32(f, p.ang_vz) && read_f32(f, p.birth_fade) &&
         read_f32(f, p.birth_parent_z) && read_f32(f, p.birth_origin_y_delta) &&
         read_f32(f, p.effective_distance) && read_f32(f, p.size) &&
         read_f32(f, p.max_particle_size) && read_f32(f, p.r) && read_f32(f, p.g) &&
         read_f32(f, p.b) && read_f32(f, p.a) && read_u16(f, p.uv_name_index) &&
         read_u8(f, p.blend) && read_u8(f, p.sorting_order) && read_u16(f, p.flags) &&
         read_u8(f, p.render_mode) && read_f32(f, p.vel_x) && read_f32(f, p.vel_y) &&
         read_f32(f, p.vel_z) && read_f32(f, p.stored_vx) && read_f32(f, p.stored_vy) &&
         read_f32(f, p.stored_vz) && read_f32(f, p.vol_vx) && read_f32(f, p.vol_vy) &&
         read_f32(f, p.vol_vz) && read_f32(f, p.noise_vx) && read_f32(f, p.noise_vy) &&
         read_f32(f, p.noise_vz) && read_f32(f, p.total_vx) && read_f32(f, p.total_vy) &&
         read_f32(f, p.total_vz) && read_f32(f, p.length_scale) &&
         read_f32(f, p.velocity_scale) && read_f32(f, p.pivot_x) && read_f32(f, p.pivot_y) &&
         read_f32(f, p.pivot_z) && read_f32(f, p.custom1_x) && read_f32(f, p.custom1_y) &&
         read_f32(f, p.emit_u) && read_u8(f, p.inherit_props) &&
         read_f32(f, p.butterfly_wing_x) && read_f32(f, p.sol_y) && read_f32(f, p.size_y) &&
         read_f32(f, p.size_z) && read_u8(f, p.size_3d) && read_f32(f, p.roll) &&
         read_f32(f, p.tilt_x) && read_f32(f, p.tilt_y) && read_u8(f, p.render_alignment) &&
         read_u16(f, p.trail_points) && read_f32_vec(f, p.trail_x) && read_f32_vec(f, p.trail_y) &&
         read_f32_vec(f, p.trail_z) && read_f32_vec(f, p.trail_widths) &&
         read_f32_vec(f, p.trail_color_r) && read_f32_vec(f, p.trail_color_g) &&
         read_f32_vec(f, p.trail_color_b) && read_f32_vec(f, p.trail_color_a) &&
         read_f32(f, p.trail_width) && read_u32(f, p.trail_chain) &&
         read_u16(f, p.trail_vertex) && read_u16(f, p.trail_count) && read_f32(f, p.trail_r) &&
         read_f32(f, p.trail_g) && read_f32(f, p.trail_b) && read_f32(f, p.trail_a) &&
         read_f32(f, p.trail_u0) && read_f32(f, p.trail_u1) &&
         read_u16(f, p.trail_over_alpha_begin) && read_u8(f, p.trail_over_alpha_count) &&
         read_u16(f, p.trail_over_color_begin) && read_u8(f, p.trail_over_color_count) &&
         read_u16(f, p.trail_over_min_alpha_begin) && read_u8(f, p.trail_over_min_alpha_count) &&
         read_u16(f, p.trail_over_min_color_begin) && read_u8(f, p.trail_over_min_color_count) &&
         read_u8(f, p.trail_over_state) && read_u8(f, p.trail_over_mode) &&
         read_u8(f, p.trail_over_min_mode) && read_f32(f, p.trail_over_r) &&
         read_f32(f, p.trail_over_g) && read_f32(f, p.trail_over_b) &&
         read_f32(f, p.trail_over_r_min) && read_f32(f, p.trail_over_g_min) &&
         read_f32(f, p.trail_over_b_min) && read_f32(f, p.trail_color_u) &&
         read_f32(f, p.trail_over_a0) && read_f32(f, p.trail_over_a1) &&
         read_f32(f, p.camera_velocity_scale) && read_u8(f, p.freeform_stretching) &&
         read_u8(f, p.rotate_with_stretch) && read_f32(f, p.noise_rotation_amount) &&
         read_f32(f, p.noise_size_amount) && read_u8(f, p.flip_u) && read_u8(f, p.flip_v);
}

bool write_sample(FILE* f, const SampleKey& key, const SplitEffectFrameCollect& io) {
  if (!write_i32(f, key.effect_id) || !write_i32(f, key.split_count) ||
      !write_i32(f, key.instance_set) || !write_i32(f, key.instance_count) ||
      !write_f64(f, key.now_t) || !write_u32(f, static_cast<uint32_t>(key.particle_count))) {
    return false;
  }
  int written = 0;
  for (size_t s = 0; s < io.split_count.size(); ++s) {
    const int begin = io.split_begin[s];
    const int n = io.split_count[s];
    for (int i = 0; i < n; ++i) {
      if (!write_particle_file(f, io.pool[static_cast<size_t>(begin + i)])) {
        return false;
      }
      ++written;
    }
  }
  return written == key.particle_count;
}

bool read_sample_key(FILE* f, SampleKey& key) {
  uint32_t n = 0;
  if (!read_i32(f, key.effect_id) || !read_i32(f, key.split_count) ||
      !read_i32(f, key.instance_set) || !read_i32(f, key.instance_count) ||
      !read_f64(f, key.now_t) || !read_u32(f, n)) {
    return false;
  }
  key.particle_count = static_cast<int>(n);
  return true;
}

bool keys_same(const SampleKey& a, const SampleKey& b) {
  return a.effect_id == b.effect_id && a.split_count == b.split_count &&
         a.instance_set == b.instance_set && a.instance_count == b.instance_count &&
         std::fabs(a.now_t - b.now_t) <= 1e-9;
}

void overlap_now_times(std::vector<double>& times) {
  times.clear();
  times.reserve(static_cast<size_t>(kOverlapSampleCount));
  const double window = static_cast<double>(wds::chart_render::kSplitEffectLinePlayWindowSec);
  const double denom = static_cast<double>(std::max(kOverlapSampleCount - 1, 1));
  for (int i = 0; i < kOverlapSampleCount; ++i) {
    times.push_back(window * static_cast<double>(i) / denom);
  }
}

const char* golden_tier_name(GoldenTier t) {
  return t == GoldenTier::Quick ? "quick" : "full";
}

GoldenTier parse_golden_tier(const char* s) {
  if (s == nullptr) {
    return GoldenTier::Full;
  }
  if (eq_i(s, "quick")) {
    return GoldenTier::Quick;
  }
  if (eq_i(s, "full")) {
    return GoldenTier::Full;
  }
  return GoldenTier::Full;
}

using namespace wds::chart_render;

const SplitEffectParticle* effect_catalog_particles(int32_t effect_id, uint16_t* count) {
  const wds::chart_render::SplitEffectCatalogEntry* entry = official_split_effect(effect_id);
  if (entry == nullptr) {
    if (count != nullptr) {
      *count = 0;
    }
    return nullptr;
  }
  return official_split_effect_particles(*entry, count);
}

bool effect_any_particle(int32_t effect_id,
                         const std::function<bool(const SplitEffectParticle&)>& pred) {
  uint16_t n = 0;
  const SplitEffectParticle* ps = effect_catalog_particles(effect_id, &n);
  if (ps == nullptr) {
    return false;
  }
  for (uint16_t i = 0; i < n; ++i) {
    if (pred(ps[i])) {
      return true;
    }
  }
  return false;
}

bool effect_has_active_noise(int32_t effect_id) {
  return effect_any_particle(effect_id, [](const SplitEffectParticle& p) {
    return (p.flags & kSplitEffectFlagNoise) != 0 && std::fabs(p.noise_strength) >= 1e-8f &&
           official_split_effect_noise_position_curl_on(p.noise_position_amount);
  });
}

bool effect_has_noise_driven_subemit(int32_t effect_id) {
  uint16_t n = 0;
  const SplitEffectParticle* ps = effect_catalog_particles(effect_id, &n);
  if (ps == nullptr) {
    return false;
  }
  for (uint16_t pi = 0; pi < n; ++pi) {
    const SplitEffectParticle& p = ps[pi];
    if ((p.flags & kSplitEffectFlagNoise) == 0 || std::fabs(p.noise_strength) < 1e-8f ||
        !official_split_effect_noise_position_curl_on(p.noise_position_amount)) {
      continue;
    }
    const ptrdiff_t global_i = ps + pi - split_effect_catalog_detail::kSplitEffectParticles;
    if (global_i < 0) {
      continue;
    }
    uint16_t sub_n = 0;
    const SplitEffectSubEmitter* subs = official_split_effect_subemitters_for_parent(
        static_cast<uint16_t>(global_i), &sub_n);
    for (uint16_t si = 0; si < sub_n; ++si) {
      if (subs[si].type == kSplitEffectSubEmitBirth || subs[si].type == kSplitEffectSubEmitDeath) {
        return true;
      }
    }
  }
  return false;
}

float effect_noise_weight(int32_t effect_id) {
  uint16_t n = 0;
  const SplitEffectParticle* ps = effect_catalog_particles(effect_id, &n);
  if (ps == nullptr) {
    return 0.0f;
  }
  float w = 0.0f;
  for (uint16_t i = 0; i < n; ++i) {
    const SplitEffectParticle& p = ps[i];
    if ((p.flags & kSplitEffectFlagNoise) == 0 ||
        !official_split_effect_noise_position_curl_on(p.noise_position_amount)) {
      continue;
    }
    const float s = std::fabs(p.noise_strength) + std::fabs(p.noise_strength_y) +
                    std::fabs(p.noise_strength_z);
    w = std::max(w, s * std::max(1, static_cast<int>(p.noise_octaves)) *
                        std::fabs(p.noise_position_amount));
  }
  return w;
}

bool effect_has_subemitter(int32_t effect_id, uint8_t type) {
  uint16_t n = 0;
  const SplitEffectParticle* ps = effect_catalog_particles(effect_id, &n);
  if (ps == nullptr) {
    return false;
  }
  for (uint16_t pi = 0; pi < n; ++pi) {
    const ptrdiff_t global_i =
        ps + pi - split_effect_catalog_detail::kSplitEffectParticles;
    if (global_i < 0) {
      continue;
    }
    uint16_t sub_n = 0;
    const SplitEffectSubEmitter* subs = official_split_effect_subemitters_for_parent(
        static_cast<uint16_t>(global_i), &sub_n);
    for (uint16_t si = 0; si < sub_n; ++si) {
      if (subs[si].type == type) {
        return true;
      }
    }
  }
  return false;
}

std::unordered_set<int32_t> build_quick_tier_effect_ids(bool print_selection) {
  const std::vector<int32_t> ids = official_split_effect_ids();
  std::unordered_set<int32_t> chosen;
  auto add = [&](int32_t id, const char* reason) {
    if (chosen.insert(id).second && print_selection) {
      std::fprintf(stdout, "  quick-tier effect %d  (%s)\n", id, reason);
    }
  };
  for (int32_t id : kQuickAlwaysEffects) {
    add(id, "always");
  }
  auto pick_two = [&](const char* reason, const std::function<bool(int32_t)>& pred) {
    int got = 0;
    for (int32_t id : ids) {
      if (!pred(id)) {
        continue;
      }
      if (chosen.count(id) != 0) {
        continue;
      }
      add(id, reason);
      if (++got >= 2) {
        break;
      }
    }
  };
  pick_two("Birth sub-emitter",
           [&](int32_t id) { return effect_has_subemitter(id, kSplitEffectSubEmitBirth); });
  pick_two("Death sub-emitter",
           [&](int32_t id) { return effect_has_subemitter(id, kSplitEffectSubEmitDeath); });
  pick_two("Trails particle mode", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return (p.flags & kSplitEffectFlagTrails) != 0 && p.trail_mode == 0;
    });
  });
  pick_two("Trails ribbon mode", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return (p.flags & kSplitEffectFlagTrails) != 0 && p.trail_mode == 1;
    });
  });
  pick_two("RateOverDistance", [&](int32_t id) {
    return effect_any_particle(
        id, [](const SplitEffectParticle& p) { return p.rate_over_distance > 0.0f; });
  });
  pick_two("RateOverTime curve", [&](int32_t id) {
    return effect_any_particle(
        id, [](const SplitEffectParticle& p) { return p.rot_curve_count > 0; });
  });
  pick_two("bursts", [&](int32_t id) {
    return effect_any_particle(id,
                               [](const SplitEffectParticle& p) { return p.burst_count > 0; });
  });
  pick_two("Noise", [&](int32_t id) {
    return effect_any_particle(id,
                               [](const SplitEffectParticle& p) {
                                 return (p.flags & kSplitEffectFlagNoise) != 0;
                               });
  });
  pick_two("Limit velocity/drag", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return p.clamp_enabled != 0 && (p.clamp_drag > 0.0f || p.clamp_speed > 0.0f ||
                                      p.clamp_drag_count > 0 || p.clamp_speed_count > 0);
    });
  });
  pick_two("Stretch render mode", [&](int32_t id) {
    return effect_any_particle(
        id, [](const SplitEffectParticle& p) { return p.render_mode == kSplitEffectRenderStretch; });
  });
  pick_two("Ring mesh", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return (p.flags & kSplitEffectFlagRingMesh) != 0;
    });
  });
  pick_two("Cube mesh", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return (p.flags & kSplitEffectFlagCubeMesh) != 0;
    });
  });
  pick_two("Butterfly", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return (p.flags & kSplitEffectFlagButterfly) != 0;
    });
  });
  pick_two("Local simulation space", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return p.simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::Local);
    });
  });
  pick_two("World simulation space", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return p.simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::World);
    });
  });
  pick_two("Looping", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return (p.flags & kSplitEffectFlagLooping) != 0;
    });
  });
  pick_two("useAutoRandomSeed", [&](int32_t id) {
    return effect_any_particle(id,
                               [](const SplitEffectParticle& p) { return p.auto_random_seed != 0; });
  });
  pick_two("Inherit velocity", [&](int32_t id) {
    return effect_any_particle(id,
                               [](const SplitEffectParticle& p) { return p.inherit_enabled != 0; });
  });
  pick_two("PSR flip non-zero", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return p.flip_x > 0.0f || p.flip_y > 0.0f;
    });
  });
  pick_two("startDelay>0", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return official_split_effect_start_delay(p) > 0.0f;
    });
  });
  pick_two("simulationSpeed!=1", [&](int32_t id) {
    return effect_any_particle(id, [](const SplitEffectParticle& p) {
      return std::fabs(p.simulation_speed - 1.0f) > 1e-6f;
    });
  });
  if (print_selection) {
    std::fprintf(stdout, "quick-tier total effects=%zu\n", chosen.size());
  }
  return chosen;
}

bool quick_tier_includes_effect(GoldenTier tier, int32_t effect_id) {
  if (tier == GoldenTier::Full) {
    return true;
  }
  static const std::unordered_set<int32_t> kQuick = build_quick_tier_effect_ids(false);
  return kQuick.count(effect_id) != 0;
}

struct SampleHashes {
  uint64_t full = 0;
  uint64_t groups[kGoldenHashGroupCount] = {};
};

template <typename W>
bool write_particle_group(W& w, const SplitEffectLiveParticle& p, int group) {
  switch (group) {
    case 0:  // identity
      return w.write_i32(p.effect_id) && w.write_i32(p.line_index) && w.write_u32(p.catalog_index) &&
             w.write_u8(p.element_index) && w.write_u32(p.seed) && w.write_u16(p.uv_name_index) &&
             w.write_u8(p.blend) && w.write_u8(p.sorting_order) && w.write_u16(p.flags) &&
             w.write_u8(p.render_mode) && w.write_u16(p.trail_points) &&
             w.write_u32(p.trail_chain) && w.write_u16(p.trail_vertex) &&
             w.write_u16(p.trail_count) && w.write_u8(p.inherit_props) && w.write_u8(p.size_3d) &&
             w.write_u8(p.render_alignment) && w.write_u16(p.trail_over_alpha_begin) &&
             w.write_u8(p.trail_over_alpha_count) && w.write_u16(p.trail_over_color_begin) &&
             w.write_u8(p.trail_over_color_count) &&
             w.write_u16(p.trail_over_min_alpha_begin) &&
             w.write_u8(p.trail_over_min_alpha_count) &&
             w.write_u16(p.trail_over_min_color_begin) &&
             w.write_u8(p.trail_over_min_color_count) && w.write_u8(p.trail_over_state) &&
             w.write_u8(p.trail_over_mode) && w.write_u8(p.trail_over_min_mode) &&
             w.write_u8(p.freeform_stretching) && w.write_u8(p.rotate_with_stretch) &&
             w.write_u8(p.flip_u) && w.write_u8(p.flip_v);
    case 1:  // position
      return w.write_f32(p.world_x) && w.write_f32(p.main_y) && w.write_f32(p.z) &&
             w.write_f32(p.birth_t) && w.write_f32(p.birth_fade) && w.write_f32(p.birth_parent_z) &&
             w.write_f32(p.birth_origin_y_delta) && w.write_f32(p.effective_distance) &&
             w.write_f32(p.pivot_x) && w.write_f32(p.pivot_y) && w.write_f32(p.pivot_z) &&
             w.write_f32(p.roll) && w.write_f32(p.tilt_x) && w.write_f32(p.tilt_y) &&
             w.write_f32(p.butterfly_wing_x) && w.write_f32(p.custom1_x) && w.write_f32(p.custom1_y) &&
             w.write_f32(p.emit_u);
    case 2:  // size
      return w.write_f32(p.current_size) && w.write_f32(p.current_size_y) &&
             w.write_f32(p.current_size_z) && w.write_f32(p.start_size) &&
             w.write_f32(p.start_size_y) && w.write_f32(p.start_size_z) && w.write_f32(p.size) &&
             w.write_f32(p.max_particle_size) && w.write_f32(p.size_y) && w.write_f32(p.size_z) &&
             w.write_f32(p.sol_y) && w.write_f32(p.length_scale) && w.write_f32(p.velocity_scale) &&
             w.write_f32(p.noise_size_amount);
    case 3:  // color
      return w.write_f32(p.r) && w.write_f32(p.g) && w.write_f32(p.b) && w.write_f32(p.a) &&
             w.write_f32(p.start_r) && w.write_f32(p.start_g) && w.write_f32(p.start_b) &&
             w.write_f32(p.start_a) && w.write_f32(p.current_r) && w.write_f32(p.current_g) &&
             w.write_f32(p.current_b) && w.write_f32(p.current_a) && w.write_f32(p.trail_r) &&
             w.write_f32(p.trail_g) && w.write_f32(p.trail_b) && w.write_f32(p.trail_a) &&
             w.write_f32(p.trail_over_r) && w.write_f32(p.trail_over_g) &&
             w.write_f32(p.trail_over_b) && w.write_f32(p.trail_over_r_min) &&
             w.write_f32(p.trail_over_g_min) && w.write_f32(p.trail_over_b_min);
    case 4:  // velocity
      return w.write_f32(p.vel_x) && w.write_f32(p.vel_y) && w.write_f32(p.vel_z) &&
             w.write_f32(p.stored_vx) && w.write_f32(p.stored_vy) && w.write_f32(p.stored_vz) &&
             w.write_f32(p.vol_vx) && w.write_f32(p.vol_vy) && w.write_f32(p.vol_vz) &&
             w.write_f32(p.noise_vx) && w.write_f32(p.noise_vy) && w.write_f32(p.noise_vz) &&
             w.write_f32(p.total_vx) && w.write_f32(p.total_vy) && w.write_f32(p.total_vz) &&
             w.write_f32(p.ang_vx) && w.write_f32(p.ang_vy) && w.write_f32(p.ang_vz) &&
             w.write_f32(p.camera_velocity_scale) && w.write_f32(p.noise_rotation_amount);
    case 5:  // trail scalars
      return w.write_f32(p.trail_width) && w.write_f32(p.trail_u0) && w.write_f32(p.trail_u1) &&
             w.write_f32(p.trail_color_u) && w.write_f32(p.trail_over_a0) &&
             w.write_f32(p.trail_over_a1);
    case 6:  // trail vectors
      return w.write_f32_vec(p.trail_x) && w.write_f32_vec(p.trail_y) &&
             w.write_f32_vec(p.trail_z) && w.write_f32_vec(p.trail_widths) &&
             w.write_f32_vec(p.trail_color_r) && w.write_f32_vec(p.trail_color_g) &&
             w.write_f32_vec(p.trail_color_b) && w.write_f32_vec(p.trail_color_a);
    case 7:  // misc
      return w.write_f32(p.age) && w.write_f32(p.sampled_lifetime) &&
             w.write_f32(p.remaining_lifetime);
    default:
      return false;
  }
}

static const char* kGoldenHashGroupNames[kGoldenHashGroupCount] = {
    "identity", "position", "size", "color", "velocity", "trail_scalars", "trail_vectors", "misc"};

SampleHashes hash_collect(const SplitEffectFrameCollect& io, int particle_count) {
  SampleHashes out;
  std::vector<uint8_t> full_buf;
  std::vector<uint8_t> group_bufs[kGoldenHashGroupCount];
  BufferWriter full_w{full_buf};
  BufferWriter group_ws[kGoldenHashGroupCount] = {
      BufferWriter{group_bufs[0]}, BufferWriter{group_bufs[1]}, BufferWriter{group_bufs[2]},
      BufferWriter{group_bufs[3]}, BufferWriter{group_bufs[4]}, BufferWriter{group_bufs[5]},
      BufferWriter{group_bufs[6]}, BufferWriter{group_bufs[7]}};
  int written = 0;
  for (size_t s = 0; s < io.split_count.size(); ++s) {
    const int begin = io.split_begin[s];
    const int n = io.split_count[s];
    for (int i = 0; i < n; ++i) {
      const SplitEffectLiveParticle& p = io.pool[static_cast<size_t>(begin + i)];
      if (!write_particle(full_w, p)) {
        return {};
      }
      for (int g = 0; g < kGoldenHashGroupCount; ++g) {
        if (!write_particle_group(group_ws[g], p, g)) {
          return {};
        }
      }
      ++written;
    }
  }
  if (written != particle_count) {
    return {};
  }
  out.full = fnv1a64_buffer(full_buf);
  for (int g = 0; g < kGoldenHashGroupCount; ++g) {
    out.groups[g] = fnv1a64_buffer(group_bufs[g]);
  }
  return out;
}

struct HashSampleRecord {
  SampleKey key;
  uint8_t status = kGoldenSampleOk;
  SampleHashes hashes;
};

bool write_hash_header(FILE* f, GoldenTier tier, uint32_t count_placeholder) {
  return write_u32(f, kGoldenMagic) && write_u32(f, kGoldenVersionHash) &&
         write_u32(f, count_placeholder) && write_u32(f, static_cast<uint32_t>(tier));
}

bool write_hash_record(FILE* f, const HashSampleRecord& rec) {
  const SampleKey& key = rec.key;
  if (!write_i32(f, key.effect_id) || !write_i32(f, key.split_count) ||
      !write_i32(f, key.instance_set) || !write_i32(f, key.instance_count) ||
      !write_f64(f, key.now_t) || !write_u32(f, static_cast<uint32_t>(key.particle_count)) ||
      !write_u8(f, rec.status)) {
    return false;
  }
  if (!write_u64(f, rec.hashes.full)) {
    return false;
  }
  for (int g = 0; g < kGoldenHashGroupCount; ++g) {
    if (!write_u64(f, rec.hashes.groups[g])) {
      return false;
    }
  }
  return true;
}

struct GoldenGroupSpec {
  int group_index = 0;
  int32_t effect_id = 0;
  int32_t split_count = 0;
  int scenario_overlap_index = -1;  // -1 = single-instance sweep
};

struct GoldenGroupResult {
  int group_index = 0;
  bool timed_out = false;
  std::vector<HashSampleRecord> samples;
};

void collect_single_group_samples(const GoldenGroupSpec& spec, int32_t scenario_effect,
                                  std::vector<HashSampleRecord>& out) {
  out.clear();
  SplitEffectFrameCollect io;
  SplitEffectPlaybackState state;
  if (spec.scenario_overlap_index < 0) {
    const std::vector<PreviewSplitLaneInstance> splits{
        make_split(spec.effect_id, spec.split_count, SplitLaneType::Full, kDefaultStartMs)};
    std::vector<double> times(std::begin(kGoldenTimes), std::end(kGoldenTimes));
    apply_sample_order(times, static_cast<uint32_t>(spec.effect_id) * 17u +
                                  static_cast<uint32_t>(spec.split_count));
    for (double now_t : times) {
      // Remint gens so visit order cannot change seeds; keep integrator cache.
      wds::chart_render::reset_split_effect_auto_seed_session(false);
      const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
      wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
      HashSampleRecord rec;
      rec.key.effect_id = spec.effect_id;
      rec.key.split_count = spec.split_count;
      rec.key.instance_set = kInstanceSetSingle;
      rec.key.instance_count = 1;
      rec.key.now_t = now_t;
      rec.key.particle_count = io.total;
      rec.hashes = hash_collect(io, io.total);
      out.push_back(rec);
    }
    return;
  }
  const int overlap_sets[2] = {kInstanceSetOverlap4Stagger0, kInstanceSetOverlap4Stagger02};
  const int si = spec.scenario_overlap_index;
  std::vector<double> overlap_times;
  overlap_now_times(overlap_times);
  apply_sample_order(overlap_times, static_cast<uint32_t>(scenario_effect) * 31u +
                                       static_cast<uint32_t>(si + 1));
  const auto splits = make_instances(scenario_effect, kOverlapInstanceCount, kOverlapStaggers[si],
                                     6, SplitLaneType::Full);
  for (double now_t : overlap_times) {
    wds::chart_render::reset_split_effect_auto_seed_session(false);
    const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
    wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
    HashSampleRecord rec;
    rec.key.effect_id = scenario_effect;
    rec.key.split_count = 6;
    rec.key.instance_set = overlap_sets[si];
    rec.key.instance_count = kOverlapInstanceCount;
    rec.key.now_t = now_t;
    rec.key.particle_count = io.total;
    rec.hashes = hash_collect(io, io.total);
    out.push_back(rec);
  }
}

// Child writes: group_index, u32 sample_count, then hash records (binary via dup fd).
[[noreturn]] void golden_group_child_main2(int write_fd, GoldenGroupSpec spec,
                                           int32_t scenario_effect) {
  std::vector<HashSampleRecord> samples;
  collect_single_group_samples(spec, scenario_effect, samples);
  uint32_t count = static_cast<uint32_t>(samples.size());
  if (write(write_fd, &spec.group_index, sizeof(spec.group_index)) !=
          static_cast<ssize_t>(sizeof(spec.group_index)) ||
      write(write_fd, &count, sizeof(count)) != static_cast<ssize_t>(sizeof(count))) {
    _exit(1);
  }
  FILE* wf = fdopen(dup(write_fd), "wb");
  if (wf == nullptr) {
    _exit(1);
  }
  for (const HashSampleRecord& rec : samples) {
    if (!write_hash_record(wf, rec)) {
      std::fclose(wf);
      _exit(1);
    }
  }
  std::fclose(wf);
  close(write_fd);
  _exit(0);
}

bool run_group_with_timeout(const GoldenGroupSpec& spec, int32_t scenario_effect,
                            double timeout_sec, GoldenGroupResult& out) {
  out = GoldenGroupResult{};
  out.group_index = spec.group_index;
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    close(pipefd[0]);
    golden_group_child_main2(pipefd[1], spec, scenario_effect);
  }
  close(pipefd[1]);
  const auto wall_start = Clock::now();
  std::vector<uint8_t> payload;
  uint8_t chunk[65536];
  bool child_done = false;
  bool timed_out = false;
  while (!child_done) {
    const double elapsed =
        std::chrono::duration<double>(Clock::now() - wall_start).count();
    if (elapsed >= timeout_sec) {
      timed_out = true;
      kill(pid, SIGKILL);
      break;
    }
    struct pollfd pfd {};
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    const int wait_ms = static_cast<int>(std::min(100.0, (timeout_sec - elapsed) * 1000.0));
    const int pr = poll(&pfd, 1, wait_ms);
    if (pr > 0 && (pfd.revents & POLLIN)) {
      const ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
      if (n <= 0) {
        break;
      }
      payload.insert(payload.end(), chunk, chunk + n);
    }
    int status = 0;
    const pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      child_done = true;
      while (true) {
        struct pollfd drain {};
        drain.fd = pipefd[0];
        drain.events = POLLIN;
        if (poll(&drain, 1, 0) <= 0 || !(drain.revents & POLLIN)) {
          break;
        }
        const ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
        if (n <= 0) {
          break;
        }
        payload.insert(payload.end(), chunk, chunk + n);
      }
    }
  }
  if (!child_done) {
    int status = 0;
    waitpid(pid, &status, 0);
  }
  close(pipefd[0]);
  if (timed_out) {
    out.timed_out = true;
    return true;
  }
  if (payload.size() < sizeof(int) + sizeof(uint32_t)) {
    out.timed_out = true;
    return true;
  }
  size_t off = 0;
  int read_group = 0;
  std::memcpy(&read_group, payload.data() + off, sizeof(read_group));
  off += sizeof(read_group);
  uint32_t sample_count = 0;
  std::memcpy(&sample_count, payload.data() + off, sizeof(sample_count));
  off += sizeof(sample_count);
  FILE* rf = fmemopen(const_cast<uint8_t*>(payload.data() + off),
                      payload.size() - off, "rb");
  if (rf == nullptr) {
    out.timed_out = true;
    return true;
  }
  out.samples.resize(sample_count);
  for (uint32_t i = 0; i < sample_count; ++i) {
    HashSampleRecord& rec = out.samples[static_cast<size_t>(i)];
    uint32_t pc_u = 0;
    if (!read_i32(rf, rec.key.effect_id) || !read_i32(rf, rec.key.split_count) ||
        !read_i32(rf, rec.key.instance_set) || !read_i32(rf, rec.key.instance_count) ||
        !read_f64(rf, rec.key.now_t) || !read_u32(rf, pc_u) || !read_u8(rf, rec.status) ||
        !read_u64(rf, rec.hashes.full)) {
      std::fclose(rf);
      out.timed_out = true;
      return true;
    }
    rec.key.particle_count = static_cast<int>(pc_u);
    for (int g = 0; g < kGoldenHashGroupCount; ++g) {
      if (!read_u64(rf, rec.hashes.groups[g])) {
        std::fclose(rf);
        out.timed_out = true;
        return true;
      }
    }
  }
  std::fclose(rf);
  (void)read_group;
  return true;
}

void fill_timeout_samples(const GoldenGroupSpec& spec, int32_t scenario_effect,
                          std::vector<HashSampleRecord>& out) {
  out.clear();
  if (spec.scenario_overlap_index < 0) {
    for (double now_t : kGoldenTimes) {
      HashSampleRecord rec;
      rec.key.effect_id = spec.effect_id;
      rec.key.split_count = spec.split_count;
      rec.key.instance_set = kInstanceSetSingle;
      rec.key.instance_count = 1;
      rec.key.now_t = now_t;
      rec.key.particle_count = 0;
      rec.status = kGoldenSampleTimeout;
      out.push_back(rec);
    }
    return;
  }
  const int overlap_sets[2] = {kInstanceSetOverlap4Stagger0, kInstanceSetOverlap4Stagger02};
  std::vector<double> overlap_times;
  overlap_now_times(overlap_times);
  for (double now_t : overlap_times) {
    HashSampleRecord rec;
    rec.key.effect_id = scenario_effect;
    rec.key.split_count = 6;
    rec.key.instance_set = overlap_sets[spec.scenario_overlap_index];
    rec.key.instance_count = kOverlapInstanceCount;
    rec.key.now_t = now_t;
    rec.key.particle_count = 0;
    rec.status = kGoldenSampleTimeout;
    out.push_back(rec);
  }
}

std::vector<GoldenGroupSpec> build_golden_groups(GoldenTier tier, int32_t scenario_effect) {
  std::vector<GoldenGroupSpec> groups;
  const std::vector<int32_t> ids = official_split_effect_ids();
  const int32_t counts[2] = {1, 6};
  int gi = 0;
  for (int32_t id : ids) {
    if (!quick_tier_includes_effect(tier, id)) {
      continue;
    }
    for (int32_t sc : counts) {
      GoldenGroupSpec g;
      g.group_index = gi++;
      g.effect_id = id;
      g.split_count = sc;
      g.scenario_overlap_index = -1;
      groups.push_back(g);
    }
  }
  for (int si = 0; si < 2; ++si) {
    GoldenGroupSpec g;
    g.group_index = gi++;
    g.effect_id = scenario_effect;
    g.split_count = 6;
    g.scenario_overlap_index = si;
    groups.push_back(g);
  }
  return groups;
}

// Controller::initialize draws unity::Random::Range(0, 1000) per line into a
// process-global LCG (unity_api.cpp). Lazy creation order must match the full
// serial golden walk before any fork (parallel workers or per-group timeout).
void prehydrate_golden_controllers(int32_t scenario_effect) {
  for (int32_t id : wds::chart_render::official_split_effect_ids()) {
    (void)wds::chart_render::official_split_effect_controller(id);
  }
  (void)wds::chart_render::official_split_effect_controller(scenario_effect);
}

int default_golden_jobs() {
  const unsigned n = std::thread::hardware_concurrency();
  return n > 0 ? static_cast<int>(n) : 1;
}

bool read_hash_record(FILE* f, HashSampleRecord& rec) {
  uint32_t pc_u = 0;
  if (!read_i32(f, rec.key.effect_id) || !read_i32(f, rec.key.split_count) ||
      !read_i32(f, rec.key.instance_set) || !read_i32(f, rec.key.instance_count) ||
      !read_f64(f, rec.key.now_t) || !read_u32(f, pc_u) || !read_u8(f, rec.status) ||
      !read_u64(f, rec.hashes.full)) {
    return false;
  }
  rec.key.particle_count = static_cast<int>(pc_u);
  for (int g = 0; g < kGoldenHashGroupCount; ++g) {
    if (!read_u64(f, rec.hashes.groups[g])) {
      return false;
    }
  }
  return true;
}

struct GoldenRunConfig {
  GoldenTier tier = GoldenTier::Full;
  int jobs = 1;
  double group_timeout = kDefaultGroupTimeoutSec;
  int32_t scenario_effect = kDefaultScenarioEffect;
  bool full_format = false;
  SampleOrder sample_order = SampleOrder::Normal;
};

[[noreturn]] void golden_worker_process(int worker_id, int jobs, GoldenTier tier,
                                        int32_t scenario_effect, double group_timeout,
                                        const char* tmp_path) {
  const std::vector<GoldenGroupSpec> groups = build_golden_groups(tier, scenario_effect);
  FILE* out = std::fopen(tmp_path, "wb");
  if (out == nullptr) {
    _exit(1);
  }
  for (const GoldenGroupSpec& g : groups) {
    if (g.group_index % jobs != worker_id) {
      continue;
    }
    GoldenGroupResult gr;
    run_group_with_timeout(g, scenario_effect, group_timeout, gr);
    if (gr.timed_out) {
      fill_timeout_samples(g, scenario_effect, gr.samples);
    }
    if (!write_i32(out, g.group_index) ||
        !write_u32(out, static_cast<uint32_t>(gr.samples.size()))) {
      std::fclose(out);
      _exit(1);
    }
    for (const HashSampleRecord& rec : gr.samples) {
      if (!write_hash_record(out, rec)) {
        std::fclose(out);
        _exit(1);
      }
    }
  }
  std::fclose(out);
  _exit(0);
}

bool merge_worker_files(const std::vector<GoldenGroupSpec>& groups,
                        const std::vector<std::string>& worker_paths,
                        std::vector<HashSampleRecord>& ordered_out) {
  std::vector<GoldenGroupResult> group_results(groups.size());
  for (const std::string& path : worker_paths) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
      continue;
    }
    while (true) {
      int32_t group_index = -1;
      uint32_t n = 0;
      if (!read_i32(f, group_index) || !read_u32(f, n)) {
        break;
      }
      if (group_index < 0 || static_cast<size_t>(group_index) >= group_results.size()) {
        for (uint32_t i = 0; i < n; ++i) {
          HashSampleRecord skip;
          if (!read_hash_record(f, skip)) {
            break;
          }
        }
        continue;
      }
      GoldenGroupResult& gr = group_results[static_cast<size_t>(group_index)];
      gr.group_index = group_index;
      gr.samples.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        if (!read_hash_record(f, gr.samples[static_cast<size_t>(i)])) {
          std::fclose(f);
          return false;
        }
      }
    }
    std::fclose(f);
  }
  ordered_out.clear();
  for (const GoldenGroupSpec& g : groups) {
    const GoldenGroupResult& gr = group_results[static_cast<size_t>(g.group_index)];
    for (const HashSampleRecord& rec : gr.samples) {
      ordered_out.push_back(rec);
    }
  }
  return true;
}

bool collect_hash_samples(const GoldenRunConfig& cfg, std::vector<HashSampleRecord>& ordered_out) {
  if (cfg.tier == GoldenTier::Quick) {
    std::fprintf(stdout, "quick-tier selection:\n");
    build_quick_tier_effect_ids(true);
  }
  prehydrate_golden_controllers(cfg.scenario_effect);
  g_sample_order = cfg.sample_order;
  const std::vector<GoldenGroupSpec> groups = build_golden_groups(cfg.tier, cfg.scenario_effect);
  if (cfg.jobs <= 1) {
    std::vector<GoldenGroupResult> group_results(groups.size());
    for (const GoldenGroupSpec& g : groups) {
      GoldenGroupResult gr;
      run_group_with_timeout(g, cfg.scenario_effect, cfg.group_timeout, gr);
      if (gr.timed_out) {
        fill_timeout_samples(g, cfg.scenario_effect, gr.samples);
      } else {
        gr.group_index = g.group_index;
      }
      group_results[static_cast<size_t>(g.group_index)] = std::move(gr);
    }
    ordered_out.clear();
    for (const GoldenGroupSpec& g : groups) {
      for (const HashSampleRecord& rec :
           group_results[static_cast<size_t>(g.group_index)].samples) {
        ordered_out.push_back(rec);
      }
    }
    return true;
  }
  const int jobs = std::max(1, cfg.jobs);
  std::vector<std::string> tmps(static_cast<size_t>(jobs));
  std::vector<pid_t> pids(static_cast<size_t>(jobs));
  for (int w = 0; w < jobs; ++w) {
    char path[512];
    std::snprintf(path, sizeof(path), "/tmp/wds_golden_%d_%d.bin", static_cast<int>(getpid()), w);
    tmps[static_cast<size_t>(w)] = path;
    const pid_t pid = fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      golden_worker_process(w, jobs, cfg.tier, cfg.scenario_effect, cfg.group_timeout, path);
    }
    pids[static_cast<size_t>(w)] = pid;
  }
  for (pid_t pid : pids) {
    int status = 0;
    waitpid(pid, &status, 0);
  }
  const bool ok = merge_worker_files(groups, tmps, ordered_out);
  for (const std::string& path : tmps) {
    std::remove(path.c_str());
  }
  return ok;
}

bool read_golden_header(FILE* f, uint32_t& version, uint32_t& count, GoldenTier& tier) {
  uint32_t magic = 0;
  tier = GoldenTier::Full;
  if (!read_u32(f, magic) || !read_u32(f, version) || !read_u32(f, count)) {
    return false;
  }
  if (magic != kGoldenMagic) {
    return false;
  }
  if (version == kGoldenVersionHash) {
    uint32_t t = 0;
    if (!read_u32(f, t)) {
      return false;
    }
    tier = static_cast<GoldenTier>(t);
  }
  return true;
}

template <typename Fn>
bool for_each_golden_sample(Fn&& fn, int32_t scenario_effect, GoldenTier tier = GoldenTier::Full) {
  prehydrate_golden_controllers(scenario_effect);
  const std::vector<int32_t> ids = wds::chart_render::official_split_effect_ids();
  SplitEffectFrameCollect io;
  SplitEffectPlaybackState state;
  const int32_t counts[2] = {1, 6};
  for (int32_t id : ids) {
    if (!quick_tier_includes_effect(tier, id)) {
      continue;
    }
    for (int32_t sc : counts) {
      wds::chart_render::reset_split_effect_auto_seed_session();
      const std::vector<PreviewSplitLaneInstance> splits{
          make_split(id, sc, SplitLaneType::Full, kDefaultStartMs)};
      for (double now_t : kGoldenTimes) {
        const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
        wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
        SampleKey key;
        key.effect_id = id;
        key.split_count = sc;
        key.instance_set = kInstanceSetSingle;
        key.instance_count = 1;
        key.now_t = now_t;
        key.particle_count = io.total;
        if (!fn(key, io)) {
          return false;
        }
      }
    }
  }

  const int overlap_sets[2] = {kInstanceSetOverlap4Stagger0, kInstanceSetOverlap4Stagger02};
  std::vector<double> overlap_times;
  overlap_now_times(overlap_times);
  for (int si = 0; si < 2; ++si) {
    wds::chart_render::reset_split_effect_auto_seed_session();
    const auto splits =
        make_instances(scenario_effect, kOverlapInstanceCount, kOverlapStaggers[si], 6,
                       SplitLaneType::Full);
    for (double now_t : overlap_times) {
      const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
      wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
      SampleKey key;
      key.effect_id = scenario_effect;
      key.split_count = 6;
      key.instance_set = overlap_sets[si];
      key.instance_count = kOverlapInstanceCount;
      key.now_t = now_t;
      key.particle_count = io.total;
      if (!fn(key, io)) {
        return false;
      }
    }
  }
  return true;
}

bool bits_equal_f32(float a, float b) {
  uint32_t ua = 0;
  uint32_t ub = 0;
  std::memcpy(&ua, &a, sizeof(ua));
  std::memcpy(&ub, &b, sizeof(ub));
  return ua == ub;
}

struct WorstDiff {
  std::string field;
  SampleKey key;
  double abs_diff = 0.0;
  double cand = 0.0;
  double base = 0.0;
};

struct FieldDist {
  std::vector<double> abs;
  void add(double d) { abs.push_back(d); }
  double max() const {
    double m = 0.0;
    for (double v : abs) m = std::max(m, v);
    return m;
  }
  double pct(double p) const {
    if (abs.empty()) return 0.0;
    std::vector<double> s = abs;
    std::sort(s.begin(), s.end());
    const double idx = p * static_cast<double>(s.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, s.size() - 1);
    const double t = idx - static_cast<double>(lo);
    return s[lo] * (1.0 - t) + s[hi] * t;
  }
};

struct CompareAcc {
  bool exact = false;
  double tol_pos = 1e-3;
  double tol_color = 0.004;
  double tol_rel = 1e-3;
  int samples = 0;
  int mismatching = 0;
  int count_mismatch = 0;
  int matched_samples = 0;
  int max_report = 20;
  int reported = 0;
  std::unordered_map<std::string, WorstDiff> worst;
  std::unordered_map<int32_t, int> count_mismatch_effects;
  std::unordered_map<int32_t, double> effect_max_pos;
  std::unordered_map<int32_t, double> effect_max_pixel;
  std::unordered_map<int32_t, double> effect_max_pixel_trail;
  FieldDist world_x;
  FieldDist main_y;
  FieldDist world_z;
  FieldDist trail;
  FieldDist color;
  FieldDist size;
  FieldDist pixel;
  FieldDist pixel_raw;
  FieldDist pixel_trail;
  int clip_flip = 0;
  int trail_count_mismatch = 0;
  int trail_clip_flip = 0;
  struct PixHit {
    int32_t effect_id = 0;
    double pix = 0.0;
    double cz = 0.0;
    double dy = 0.0;
  };
  std::vector<PixHit> pix_hits;
  bool sample_failed = false;

  void note(const char* field, const SampleKey& key, double cand, double base, bool ok) {
    const double d = std::fabs(cand - base);
    auto it = worst.find(field);
    if (it == worst.end() || d > it->second.abs_diff) {
      WorstDiff w;
      w.field = field;
      w.key = key;
      w.abs_diff = d;
      w.cand = cand;
      w.base = base;
      worst[field] = w;
    }
    if (!ok) {
      sample_failed = true;
    }
  }
};

bool int_ok(CompareAcc& acc, const char* field, const SampleKey& key, long long cand,
            long long base) {
  const bool ok = cand == base;
  acc.note(field, key, static_cast<double>(cand), static_cast<double>(base), ok);
  return ok;
}

bool f32_exact(CompareAcc& acc, const char* field, const SampleKey& key, float cand, float base) {
  const bool ok = bits_equal_f32(cand, base);
  acc.note(field, key, static_cast<double>(cand), static_cast<double>(base), ok);
  return ok;
}

bool f32_tol(CompareAcc& acc, const char* field, const SampleKey& key, float cand, float base,
             double tol, bool relative) {
  if (acc.exact) {
    return f32_exact(acc, field, key, cand, base);
  }
  const double c = static_cast<double>(cand);
  const double b = static_cast<double>(base);
  const double d = std::fabs(c - b);
  const double limit = relative ? tol * std::max(1.0, std::fabs(b)) : tol;
  const bool ok = d <= limit;
  acc.note(field, key, c, b, ok);
  return ok;
}

template <typename Vec>
bool vec_tol(CompareAcc& acc, const char* field, const SampleKey& key, const Vec& cand,
             const Vec& base, double tol, bool relative) {
  if (!int_ok(acc, field, key, static_cast<long long>(cand.size()),
              static_cast<long long>(base.size()))) {
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < base.size(); ++i) {
    char name[96];
    std::snprintf(name, sizeof(name), "%s[%zu]", field, i);
    if (acc.exact) {
      ok = f32_exact(acc, name, key, cand[i], base[i]) && ok;
    } else {
      ok = f32_tol(acc, name, key, cand[i], base[i], tol, relative) && ok;
    }
  }
  return ok;
}

bool compare_particle(CompareAcc& acc, const SampleKey& key, const SplitEffectLiveParticle& c,
                      const SplitEffectLiveParticle& b) {
  bool ok = true;
  auto id = [&](const char* f, long long cv, long long bv) { ok = int_ok(acc, f, key, cv, bv) && ok; };
  auto pos = [&](const char* f, float cv, float bv) {
    ok = f32_tol(acc, f, key, cv, bv, acc.tol_pos, true) && ok;
  };
  auto col = [&](const char* f, float cv, float bv) {
    ok = f32_tol(acc, f, key, cv, bv, acc.tol_color, false) && ok;
  };
  auto rel = [&](const char* f, float cv, float bv) {
    ok = f32_tol(acc, f, key, cv, bv, acc.tol_rel, true) && ok;
  };

  id("effect_id", c.effect_id, b.effect_id);
  id("line_index", c.line_index, b.line_index);
  id("catalog_index", static_cast<long long>(c.catalog_index),
     static_cast<long long>(b.catalog_index));
  id("element_index", c.element_index, b.element_index);
  id("seed", static_cast<long long>(c.seed), static_cast<long long>(b.seed));
  id("uv_name_index", c.uv_name_index, b.uv_name_index);
  id("flags", c.flags, b.flags);
  id("render_mode", c.render_mode, b.render_mode);
  id("trail_points", c.trail_points, b.trail_points);
  id("trail_chain", static_cast<long long>(c.trail_chain), static_cast<long long>(b.trail_chain));
  id("trail_vertex", c.trail_vertex, b.trail_vertex);
  id("trail_count", c.trail_count, b.trail_count);

  pos("world_x", c.world_x, b.world_x);
  pos("main_y", c.main_y, b.main_y);
  pos("z", c.z, b.z);
  acc.world_x.add(std::fabs(static_cast<double>(c.world_x) - static_cast<double>(b.world_x)));
  acc.main_y.add(std::fabs(static_cast<double>(c.main_y) - static_cast<double>(b.main_y)));
  acc.world_z.add(std::fabs(static_cast<double>(c.z) - static_cast<double>(b.z)));
  {
    const auto nc = wds::chart_editor::project_main_xyz(c.world_x, c.main_y, c.z);
    const auto nb = wds::chart_editor::project_main_xyz(b.world_x, b.main_y, b.z);
    const double dpx = (static_cast<double>(nc.x) - static_cast<double>(nb.x)) * 0.5 * 1920.0;
    const double dpy = (static_cast<double>(nc.y) - static_cast<double>(nb.y)) * 0.5 * 1080.0;
    const double pix = std::sqrt(dpx * dpx + dpy * dpy);
    acc.pixel_raw.add(pix);
    const bool front_c = wds::chart_editor::official_main_in_front_of_camera(c.main_y, c.z);
    const bool front_b = wds::chart_editor::official_main_in_front_of_camera(b.main_y, b.z);
    if (front_c && front_b) {
      acc.pixel.add(pix);
      acc.effect_max_pixel[key.effect_id] = std::max(acc.effect_max_pixel[key.effect_id], pix);
      if (pix >= 0.4) {
        CompareAcc::PixHit h;
        h.effect_id = key.effect_id;
        h.pix = pix;
        h.cz = std::min(static_cast<double>(wds::chart_editor::official_main_camera_z(c.main_y, c.z)),
                        static_cast<double>(wds::chart_editor::official_main_camera_z(b.main_y, b.z)));
        h.dy = std::fabs(static_cast<double>(c.main_y) - static_cast<double>(b.main_y));
        acc.pix_hits.push_back(h);
      }
    } else if (front_c != front_b) {
      ++acc.clip_flip;
    }
  }
  acc.effect_max_pos[key.effect_id] =
      std::max(acc.effect_max_pos[key.effect_id],
               std::max({std::fabs(static_cast<double>(c.world_x) - static_cast<double>(b.world_x)),
                         std::fabs(static_cast<double>(c.main_y) - static_cast<double>(b.main_y)),
                         std::fabs(static_cast<double>(c.z) - static_cast<double>(b.z))}));
  pos("vel_x", c.vel_x, b.vel_x);
  pos("vel_y", c.vel_y, b.vel_y);
  pos("vel_z", c.vel_z, b.vel_z);
  ok = vec_tol(acc, "trail_x", key, c.trail_x, b.trail_x, acc.tol_pos, true) && ok;
  ok = vec_tol(acc, "trail_y", key, c.trail_y, b.trail_y, acc.tol_pos, true) && ok;
  ok = vec_tol(acc, "trail_z", key, c.trail_z, b.trail_z, acc.tol_pos, true) && ok;
  if (c.trail_x.size() != b.trail_x.size() || c.trail_y.size() != b.trail_y.size() ||
      c.trail_z.size() != b.trail_z.size() || c.trail_points != b.trail_points) {
    ++acc.trail_count_mismatch;
  } else if (c.trail_x.size() == b.trail_x.size()) {
    for (size_t i = 0; i < c.trail_x.size(); ++i) {
      const double dx = std::fabs(static_cast<double>(c.trail_x[i]) - static_cast<double>(b.trail_x[i]));
      const double dy = std::fabs(static_cast<double>(c.trail_y[i]) - static_cast<double>(b.trail_y[i]));
      const double dz = std::fabs(static_cast<double>(c.trail_z[i]) - static_cast<double>(b.trail_z[i]));
      acc.trail.add(std::max({dx, dy, dz}));
      const auto nc = wds::chart_editor::project_main_xyz(c.trail_x[i], c.trail_y[i], c.trail_z[i]);
      const auto nb = wds::chart_editor::project_main_xyz(b.trail_x[i], b.trail_y[i], b.trail_z[i]);
      const double dpx = (static_cast<double>(nc.x) - static_cast<double>(nb.x)) * 0.5 * 1920.0;
      const double dpy = (static_cast<double>(nc.y) - static_cast<double>(nb.y)) * 0.5 * 1080.0;
      const double pix = std::sqrt(dpx * dpx + dpy * dpy);
      const bool front_c =
          wds::chart_editor::official_main_in_front_of_camera(c.trail_y[i], c.trail_z[i]);
      const bool front_b =
          wds::chart_editor::official_main_in_front_of_camera(b.trail_y[i], b.trail_z[i]);
      if (front_c && front_b) {
        acc.pixel_trail.add(pix);
        acc.effect_max_pixel_trail[key.effect_id] =
            std::max(acc.effect_max_pixel_trail[key.effect_id], pix);
      } else if (front_c != front_b) {
        ++acc.trail_clip_flip;
      }
    }
  }

  col("r", c.r, b.r);
  col("g", c.g, b.g);
  col("b", c.b, b.b);
  col("a", c.a, b.a);
  acc.color.add(std::max({std::fabs(static_cast<double>(c.r) - static_cast<double>(b.r)),
                          std::fabs(static_cast<double>(c.g) - static_cast<double>(b.g)),
                          std::fabs(static_cast<double>(c.b) - static_cast<double>(b.b)),
                          std::fabs(static_cast<double>(c.a) - static_cast<double>(b.a))}));
  col("start_r", c.start_r, b.start_r);
  col("start_g", c.start_g, b.start_g);
  col("start_b", c.start_b, b.start_b);
  col("start_a", c.start_a, b.start_a);
  col("current_r", c.current_r, b.current_r);
  col("current_g", c.current_g, b.current_g);
  col("current_b", c.current_b, b.current_b);
  col("current_a", c.current_a, b.current_a);
  col("trail_r", c.trail_r, b.trail_r);
  col("trail_g", c.trail_g, b.trail_g);
  col("trail_b", c.trail_b, b.trail_b);
  col("trail_a", c.trail_a, b.trail_a);
  col("trail_over_r", c.trail_over_r, b.trail_over_r);
  col("trail_over_g", c.trail_over_g, b.trail_over_g);
  col("trail_over_b", c.trail_over_b, b.trail_over_b);
  col("trail_over_r_min", c.trail_over_r_min, b.trail_over_r_min);
  col("trail_over_g_min", c.trail_over_g_min, b.trail_over_g_min);
  col("trail_over_b_min", c.trail_over_b_min, b.trail_over_b_min);
  col("trail_over_a0", c.trail_over_a0, b.trail_over_a0);
  col("trail_over_a1", c.trail_over_a1, b.trail_over_a1);
  ok = vec_tol(acc, "trail_color_r", key, c.trail_color_r, b.trail_color_r, acc.tol_color, false) &&
       ok;
  ok = vec_tol(acc, "trail_color_g", key, c.trail_color_g, b.trail_color_g, acc.tol_color, false) &&
       ok;
  ok = vec_tol(acc, "trail_color_b", key, c.trail_color_b, b.trail_color_b, acc.tol_color, false) &&
       ok;
  ok = vec_tol(acc, "trail_color_a", key, c.trail_color_a, b.trail_color_a, acc.tol_color, false) &&
       ok;

  rel("birth_t", c.birth_t, b.birth_t);
  rel("age", c.age, b.age);
  rel("sampled_lifetime", c.sampled_lifetime, b.sampled_lifetime);
  rel("remaining_lifetime", c.remaining_lifetime, b.remaining_lifetime);
  rel("current_size", c.current_size, b.current_size);
  rel("current_size_y", c.current_size_y, b.current_size_y);
  rel("current_size_z", c.current_size_z, b.current_size_z);
  acc.size.add(std::max({std::fabs(static_cast<double>(c.current_size) - static_cast<double>(b.current_size)),
                         std::fabs(static_cast<double>(c.current_size_y) - static_cast<double>(b.current_size_y)),
                         std::fabs(static_cast<double>(c.current_size_z) - static_cast<double>(b.current_size_z)),
                         std::fabs(static_cast<double>(c.size) - static_cast<double>(b.size))}));
  rel("start_size", c.start_size, b.start_size);
  rel("start_size_y", c.start_size_y, b.start_size_y);
  rel("start_size_z", c.start_size_z, b.start_size_z);
  rel("ang_vx", c.ang_vx, b.ang_vx);
  rel("ang_vy", c.ang_vy, b.ang_vy);
  rel("ang_vz", c.ang_vz, b.ang_vz);
  rel("birth_fade", c.birth_fade, b.birth_fade);
  rel("birth_parent_z", c.birth_parent_z, b.birth_parent_z);
  rel("birth_origin_y_delta", c.birth_origin_y_delta, b.birth_origin_y_delta);
  rel("effective_distance", c.effective_distance, b.effective_distance);
  rel("size", c.size, b.size);
  rel("max_particle_size", c.max_particle_size, b.max_particle_size);
  rel("stored_vx", c.stored_vx, b.stored_vx);
  rel("stored_vy", c.stored_vy, b.stored_vy);
  rel("stored_vz", c.stored_vz, b.stored_vz);
  rel("vol_vx", c.vol_vx, b.vol_vx);
  rel("vol_vy", c.vol_vy, b.vol_vy);
  rel("vol_vz", c.vol_vz, b.vol_vz);
  rel("noise_vx", c.noise_vx, b.noise_vx);
  rel("noise_vy", c.noise_vy, b.noise_vy);
  rel("noise_vz", c.noise_vz, b.noise_vz);
  rel("total_vx", c.total_vx, b.total_vx);
  rel("total_vy", c.total_vy, b.total_vy);
  rel("total_vz", c.total_vz, b.total_vz);
  rel("length_scale", c.length_scale, b.length_scale);
  rel("velocity_scale", c.velocity_scale, b.velocity_scale);
  rel("pivot_x", c.pivot_x, b.pivot_x);
  rel("pivot_y", c.pivot_y, b.pivot_y);
  rel("pivot_z", c.pivot_z, b.pivot_z);
  rel("custom1_x", c.custom1_x, b.custom1_x);
  rel("custom1_y", c.custom1_y, b.custom1_y);
  rel("emit_u", c.emit_u, b.emit_u);
  rel("butterfly_wing_x", c.butterfly_wing_x, b.butterfly_wing_x);
  rel("sol_y", c.sol_y, b.sol_y);
  rel("size_y", c.size_y, b.size_y);
  rel("size_z", c.size_z, b.size_z);
  rel("roll", c.roll, b.roll);
  rel("tilt_x", c.tilt_x, b.tilt_x);
  rel("tilt_y", c.tilt_y, b.tilt_y);
  rel("trail_width", c.trail_width, b.trail_width);
  rel("trail_u0", c.trail_u0, b.trail_u0);
  rel("trail_u1", c.trail_u1, b.trail_u1);
  rel("trail_color_u", c.trail_color_u, b.trail_color_u);
  rel("camera_velocity_scale", c.camera_velocity_scale, b.camera_velocity_scale);
  rel("noise_rotation_amount", c.noise_rotation_amount, b.noise_rotation_amount);
  rel("noise_size_amount", c.noise_size_amount, b.noise_size_amount);
  ok = vec_tol(acc, "trail_widths", key, c.trail_widths, b.trail_widths, acc.tol_rel, true) && ok;

  id("blend", c.blend, b.blend);
  id("sorting_order", c.sorting_order, b.sorting_order);
  id("inherit_props", c.inherit_props, b.inherit_props);
  id("size_3d", c.size_3d, b.size_3d);
  id("render_alignment", c.render_alignment, b.render_alignment);
  id("trail_over_alpha_begin", c.trail_over_alpha_begin, b.trail_over_alpha_begin);
  id("trail_over_alpha_count", c.trail_over_alpha_count, b.trail_over_alpha_count);
  id("trail_over_color_begin", c.trail_over_color_begin, b.trail_over_color_begin);
  id("trail_over_color_count", c.trail_over_color_count, b.trail_over_color_count);
  id("trail_over_min_alpha_begin", c.trail_over_min_alpha_begin, b.trail_over_min_alpha_begin);
  id("trail_over_min_alpha_count", c.trail_over_min_alpha_count, b.trail_over_min_alpha_count);
  id("trail_over_min_color_begin", c.trail_over_min_color_begin, b.trail_over_min_color_begin);
  id("trail_over_min_color_count", c.trail_over_min_color_count, b.trail_over_min_color_count);
  id("trail_over_state", c.trail_over_state, b.trail_over_state);
  id("trail_over_mode", c.trail_over_mode, b.trail_over_mode);
  id("trail_over_min_mode", c.trail_over_min_mode, b.trail_over_min_mode);
  id("freeform_stretching", c.freeform_stretching, b.freeform_stretching);
  id("rotate_with_stretch", c.rotate_with_stretch, b.rotate_with_stretch);
  id("flip_u", c.flip_u, b.flip_u);
  id("flip_v", c.flip_v, b.flip_v);
  return ok;
}

std::vector<SplitEffectLiveParticle> flatten(const SplitEffectFrameCollect& io) {
  std::vector<SplitEffectLiveParticle> out;
  out.reserve(static_cast<size_t>(io.total));
  for (size_t s = 0; s < io.split_count.size(); ++s) {
    const int begin = io.split_begin[s];
    const int n = io.split_count[s];
    for (int i = 0; i < n; ++i) {
      out.push_back(io.pool[static_cast<size_t>(begin + i)]);
    }
  }
  return out;
}

// --- subcommands ------------------------------------------------------------

struct ScanRow {
  int32_t id = 0;
  int max_n = 0;
  double max_ms = 0.0;
  double mean_ms = 0.0;
  int frames = 0;
  bool timed_out = false;
  double timeout_at_now_t = 0.0;
};

enum : uint32_t {
  kScanPipeMagic = 0x53434E50u,
  kScanPipeProgress = 0u,
  kScanPipeComplete = 1u,
};

struct ScanPipeMsg {
  uint32_t magic = kScanPipeMagic;
  uint32_t kind = kScanPipeProgress;
  int32_t effect_id = 0;
  double now_t = 0.0;
  int32_t max_n = 0;
  double max_ms = 0.0;
  double mean_ms = 0.0;
  int32_t frames = 0;
};

static_assert(sizeof(ScanPipeMsg) <= 512, "ScanPipeMsg must fit in PIPE_BUF for atomic writes");

void print_ms_cell(double ms) {
  if (std::isinf(ms)) {
    std::fputs("       inf", stdout);
  } else {
    std::fprintf(stdout, "%10.3f", ms);
  }
}

void print_ms_csv(FILE* out, double ms) {
  if (out == nullptr) {
    return;
  }
  if (std::isinf(ms)) {
    std::fputs("inf", out);
  } else {
    std::fprintf(out, "%.6f", ms);
  }
}

bool write_scan_pipe_msg(int fd, const ScanPipeMsg& msg) {
  const ssize_t n = write(fd, &msg, sizeof(msg));
  return n == static_cast<ssize_t>(sizeof(msg));
}

bool read_scan_pipe_msg(int fd, ScanPipeMsg& out) {
  uint8_t buf[sizeof(ScanPipeMsg)];
  size_t got = 0;
  while (got < sizeof(buf)) {
    const ssize_t n = read(fd, buf + got, sizeof(buf) - got);
    if (n <= 0) {
      return false;
    }
    got += static_cast<size_t>(n);
  }
  std::memcpy(&out, buf, sizeof(out));
  return out.magic == kScanPipeMagic;
}

[[noreturn]] void scan_effect_child_main(int write_fd, int32_t effect_id, int quality,
                                         int32_t split_count, SplitLaneType type, double dt,
                                         double tmax) {
  wds::chart_render::reset_split_effect_auto_seed_session();
  const std::vector<PreviewSplitLaneInstance> splits{
      make_split(effect_id, split_count, type, kDefaultStartMs)};
  SplitEffectFrameCollect io;
  SplitEffectPlaybackState state;
  int max_n = 0;
  double max_ms = 0.0;
  double sum_ms = 0.0;
  int frames = 0;
  for (double now_t = 0.0; now_t <= tmax + 1e-12; now_t += dt) {
    ScanPipeMsg progress;
    progress.kind = kScanPipeProgress;
    progress.effect_id = effect_id;
    progress.now_t = now_t;
    if (!write_scan_pipe_msg(write_fd, progress)) {
      _exit(1);
    }
    const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
    const double ms = collect_ms(splits, quality, now_sec, false, state, io);
    max_n = std::max(max_n, io.total);
    max_ms = std::max(max_ms, ms);
    sum_ms += ms;
    ++frames;
  }
  ScanPipeMsg done;
  done.kind = kScanPipeComplete;
  done.effect_id = effect_id;
  done.max_n = max_n;
  done.max_ms = max_ms;
  done.mean_ms = frames > 0 ? sum_ms / static_cast<double>(frames) : 0.0;
  done.frames = frames;
  write_scan_pipe_msg(write_fd, done);
  close(write_fd);
  _exit(0);
}

std::vector<int32_t> parse_effect_id_list(const char* s) {
  std::vector<int32_t> out;
  if (s == nullptr || *s == '\0') {
    return out;
  }
  const char* p = s;
  while (*p != '\0') {
    char* end = nullptr;
    const long v = std::strtol(p, &end, 10);
    if (end == p) {
      break;
    }
    out.push_back(static_cast<int32_t>(v));
    p = end;
    if (*p == ',') {
      ++p;
    }
  }
  return out;
}

std::vector<int32_t> scan_effect_id_list(const std::vector<int32_t>& only_ids, size_t from_index) {
  if (!only_ids.empty()) {
    return only_ids;
  }
  std::vector<int32_t> ids = wds::chart_render::official_split_effect_ids();
  if (from_index >= ids.size()) {
    ids.clear();
  } else if (from_index > 0) {
    ids.erase(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(from_index));
  }
  return ids;
}

bool scan_one_effect_with_timeout(int32_t effect_id, int quality, int32_t split_count,
                                  SplitLaneType type, double dt, double tmax,
                                  double effect_timeout_sec, ScanRow& row_out) {
  row_out = ScanRow{};
  row_out.id = effect_id;
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    std::fprintf(stderr, "pipe failed for effect %d\n", effect_id);
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    std::fprintf(stderr, "fork failed for effect %d\n", effect_id);
    return false;
  }
  if (pid == 0) {
    close(pipefd[0]);
    scan_effect_child_main(pipefd[1], effect_id, quality, split_count, type, dt, tmax);
  }
  close(pipefd[1]);

  const auto wall_start = Clock::now();
  const double budget_sec = effect_timeout_sec > 0.0 ? effect_timeout_sec : kScanEffectTimeoutSec;
  double last_now_t = 0.0;
  bool got_complete = false;
  ScanPipeMsg complete_msg{};

  auto budget_elapsed_sec = [&]() {
    return std::chrono::duration<double>(Clock::now() - wall_start).count();
  };

  while (true) {
    const double elapsed = budget_elapsed_sec();
    if (elapsed >= budget_sec) {
      break;
    }
    const int wait_ms = static_cast<int>(
        std::min(100.0, (budget_sec - elapsed) * 1000.0));
    struct pollfd pfd {};
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    const int pr = poll(&pfd, 1, wait_ms);
    if (pr > 0 && (pfd.revents & POLLIN)) {
      ScanPipeMsg msg;
      if (!read_scan_pipe_msg(pipefd[0], msg)) {
        break;
      }
      if (msg.kind == kScanPipeProgress) {
        last_now_t = msg.now_t;
      } else if (msg.kind == kScanPipeComplete) {
        complete_msg = msg;
        got_complete = true;
      }
    }
    int status = 0;
    const pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      while (true) {
        struct pollfd drain {};
        drain.fd = pipefd[0];
        drain.events = POLLIN;
        if (poll(&drain, 1, 0) <= 0 || !(drain.revents & POLLIN)) {
          break;
        }
        ScanPipeMsg msg;
        if (!read_scan_pipe_msg(pipefd[0], msg)) {
          break;
        }
        if (msg.kind == kScanPipeProgress) {
          last_now_t = msg.now_t;
        } else if (msg.kind == kScanPipeComplete) {
          complete_msg = msg;
          got_complete = true;
        }
      }
      break;
    }
  }

  if (!got_complete) {
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    row_out.timed_out = true;
    row_out.timeout_at_now_t = last_now_t;
    row_out.max_ms = std::numeric_limits<double>::infinity();
    row_out.mean_ms = std::numeric_limits<double>::infinity();
  } else {
    row_out.max_n = complete_msg.max_n;
    row_out.max_ms = complete_msg.max_ms;
    row_out.mean_ms = complete_msg.mean_ms;
    row_out.frames = complete_msg.frames;
  }
  close(pipefd[0]);
  return true;
}

int cmd_scan(int quality, int32_t split_count, SplitLaneType type, double dt, double tmax,
             double effect_timeout_sec, const std::vector<int32_t>& only_ids, size_t from_index,
             const std::string& csv_path, int jobs) {
  const std::vector<int32_t> ids = scan_effect_id_list(only_ids, from_index);
  const int job_count = std::max(1, jobs);
  std::fprintf(stdout,
               "scan quality=%d split_count=%d type=%s dt=%.4f tmax=%.1f effect_timeout=%.1f "
               "jobs=%d effects=%zu\n",
               quality, split_count, type_name(type), dt, tmax, effect_timeout_sec, job_count,
               ids.size());
  std::vector<ScanRow> rows(ids.size());
  std::atomic<size_t> next_index{0};
  std::atomic<int> done_count{0};
  std::atomic<bool> scan_failed{false};
  std::mutex progress_mu;

  auto report_row = [&](size_t index, const ScanRow& row) {
    const int done = done_count.fetch_add(1) + 1;
    const int32_t id = ids[index];
    std::lock_guard<std::mutex> lock(progress_mu);
    if (row.timed_out) {
      std::fprintf(stdout,
                   "  [%3d/%zu] %d  max_n=%d  max_ms=inf  mean_ms=inf  TIMEOUT at now_t=%.4f\n",
                   done, ids.size(), id, row.max_n, row.timeout_at_now_t);
    } else {
      std::fprintf(stdout, "  [%3d/%zu] %d  max_n=%d  max_ms=%.3f  mean_ms=%.3f\n", done,
                  ids.size(), id, row.max_n, row.max_ms, row.mean_ms);
    }
    std::fflush(stdout);
  };

  auto worker = [&]() {
    for (;;) {
      const size_t index = next_index.fetch_add(1);
      if (index >= ids.size()) {
        break;
      }
      ScanRow row;
      if (!scan_one_effect_with_timeout(ids[index], quality, split_count, type, dt, tmax,
                                        effect_timeout_sec, row)) {
        scan_failed.store(true);
        return;
      }
      rows[index] = row;
      report_row(index, row);
    }
  };

  const int workers = std::min(job_count, static_cast<int>(ids.size()));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(workers));
  for (int w = 0; w < workers; ++w) {
    threads.emplace_back(worker);
  }
  for (std::thread& t : threads) {
    t.join();
  }
  if (scan_failed.load()) {
    return 1;
  }

  std::vector<ScanRow> by_time = rows;
  std::sort(by_time.begin(), by_time.end(), [](const ScanRow& a, const ScanRow& b) {
    if (a.timed_out != b.timed_out) {
      return a.timed_out && !b.timed_out;
    }
    return a.max_ms > b.max_ms;
  });
  std::fprintf(stdout, "\n%-8s %8s %10s %10s %8s %9s %16s\n", "id", "max_n", "max_ms", "mean_ms",
               "frames", "timed_out", "timeout_at_now_t");
  for (const ScanRow& r : by_time) {
    std::fprintf(stdout, "%-8d %8d", r.id, r.max_n);
    print_ms_cell(r.max_ms);
    print_ms_cell(r.mean_ms);
    std::fprintf(stdout, " %8d %9s", r.frames, r.timed_out ? "yes" : "no");
    if (r.timed_out) {
      std::fprintf(stdout, " %16.4f", r.timeout_at_now_t);
    } else {
      std::fprintf(stdout, " %16s", "-");
    }
    std::fputc('\n', stdout);
  }
  std::fprintf(stdout, "\n");
  for (size_t i = 0, printed = 0; i < by_time.size() && printed < 5; ++i) {
    if (by_time[i].timed_out) {
      continue;
    }
    std::fprintf(stdout, "HEAVIEST %d\n", by_time[i].id);
    ++printed;
  }
  std::vector<ScanRow> by_n = rows;
  std::sort(by_n.begin(), by_n.end(),
            [](const ScanRow& a, const ScanRow& b) { return a.max_n > b.max_n; });
  size_t heaviest_n = 0;
  for (const ScanRow& r : by_n) {
    if (heaviest_n >= 5) {
      break;
    }
    if (r.timed_out) {
      continue;
    }
    std::fprintf(stdout, "HEAVIEST %d\n", r.id);
    ++heaviest_n;
  }

  if (!csv_path.empty()) {
    FILE* csv = std::fopen(csv_path.c_str(), "w");
    if (csv == nullptr) {
      std::fprintf(stderr, "failed to write csv: %s\n", csv_path.c_str());
      return 1;
    }
    std::fprintf(csv, "effect_id,max_particles,max_ms,mean_ms,frames,timed_out,timeout_at_now_t\n");
    for (const ScanRow& r : by_time) {
      std::fprintf(csv, "%d,%d,", r.id, r.max_n);
      print_ms_csv(csv, r.max_ms);
      std::fputc(',', csv);
      print_ms_csv(csv, r.mean_ms);
      std::fprintf(csv, ",%d,%d", r.frames, r.timed_out ? 1 : 0);
      if (r.timed_out) {
        std::fprintf(csv, ",%.6f", r.timeout_at_now_t);
      } else {
        std::fputs(",", csv);
      }
      std::fputc('\n', csv);
    }
    std::fclose(csv);
    std::fprintf(stdout, "wrote %s\n", csv_path.c_str());
  }
  return 0;
}

void print_mode_stats(const char* mode, const MsStats& ms, const ParticleStats& ps) {
  std::fprintf(stdout,
               "%s frames=%d particles avg=%.1f max=%d  collect_ms avg=%.3f p50=%.3f p95=%.3f "
               "max=%.3f\n",
               mode, ps.frames, ps.avg(), ps.max, ms.avg(), ms.pct(0.50), ms.pct(0.95), ms.max());
  const std::string report = wds::common::perf::window_report();
  if (!report.empty()) {
    std::fputs(report.c_str(), stdout);
  }
  const std::string wall = wds::chart_render::split_wall_report();
  if (!wall.empty()) {
    std::fputs(wall.c_str(), stdout);
  }
  {
    const int fn = g_collect_faults.n;
    const double minflt_pf =
        fn > 0 ? static_cast<double>(g_collect_faults.minflt) / static_cast<double>(fn) : 0.0;
    const double majflt_pf =
        fn > 0 ? static_cast<double>(g_collect_faults.majflt) / static_cast<double>(fn) : 0.0;
    std::fprintf(stdout, "  rusage minflt/frame=%.1f majflt/frame=%.3f n=%d\n", minflt_pf,
                 majflt_pf, fn);
  }
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

int run_scenario_mode(const char* mode_name, const std::vector<PreviewSplitLaneInstance>& splits,
                      int frames, uint32_t seed, double range_lo, double range_hi,
                      const std::string& kind, double warm_from_t, int warm_frames, double play_dt,
                      int quality) {
  wds::chart_render::reset_split_effect_auto_seed_session();
  (void)wds::common::perf::window_report();
  wds::chart_render::split_wall_reset();
  SplitEffectFrameCollect io;
  SplitEffectPlaybackState state;
  MsStats ms;
  ParticleStats ps;
  std::mt19937 rng(seed);
  if (kind == "play") {
    state.reset();
    if (warm_from_t >= 0.0) {
      double warm_sec = now_sec_from_now_t(kDefaultStartMs, warm_from_t);
      int from_t_frames = 0;
      while (warm_sec + 1e-12 < range_lo) {
        (void)collect_ms(splits, quality, warm_sec, true, state, io);
        warm_sec = std::min(range_lo, warm_sec + 1.0 / 60.0);
        ++from_t_frames;
      }
      std::fprintf(stdout, "  warm-from-t=%.4f frames=%d now_sec=%.4f n=%d\n", warm_from_t,
                   from_t_frames, warm_sec, io.total);
    }
    double now_sec = range_lo;
    if (warm_frames > 0) {
      int wf = 0;
      for (; wf < warm_frames; ++wf) {
        (void)collect_ms(splits, quality, now_sec, true, state, io);
        now_sec = std::min(range_hi, now_sec + (play_dt > 1e-9 ? play_dt : 1.0 / 60.0));
      }
      std::fprintf(stdout, "  warm-frames=%d now_sec=%.4f n=%d\n", wf, now_sec, io.total);
    }
    const bool frame_log = [] {
      const char* raw = std::getenv("WDS_SPLIT_FRAME_LOG");
      return raw != nullptr && raw[0] == '1' && raw[1] == '\0';
    }();
    mark_collect_faults();
    const bool wall_on = wds::chart_render::split_wall_enabled();
    auto prev_cache = wds::chart_render::split_effect_eval_cache_stats();
    const double origin = play_origin_sec(kDefaultStartMs);
    for (int i = 0; i < frames; ++i) {
      rusage ru0{};
      rusage ru1{};
      if (wall_on) {
        getrusage(RUSAGE_SELF, &ru0);
      }
      const double t = collect_ms(splits, quality, now_sec, true, state, io);
      if (wall_on) {
        getrusage(RUSAGE_SELF, &ru1);
      }
      ms.add(t);
      ps.add(io.total);
      if (frame_log) {
        std::fprintf(stdout, "FRAME i=%d collect_ms=%.3f n=%d\n", i, t, io.total);
      }
      if (wall_on) {
        wds::chart_render::SplitWallFrameSnap wall{};
        const bool have_wall = wds::chart_render::split_wall_copy_last_frame(wall);
        const auto cur = wds::chart_render::split_effect_eval_cache_stats();
        std::fprintf(stdout,
                     "PLAYFRAME i=%d now_t=%.4f now_sec=%.4f collect=%.3f n=%d "
                     "A=%.3f B=%.3f C=%.3f Bwall=%.3f task_sum=%.3f unscoped=%.3f "
                     "ntasks=%d eval=%d kept=%d realized=%d minflt=%ld majflt=%ld "
                     "cache hit/op/store=%llu/%llu/%llu carry hit/miss=%llu/%llu "
                     "walk hit/miss=%llu/%llu",
                     i, now_sec - origin, now_sec, t, io.total,
                     have_wall ? wall.sec_ms[static_cast<int>(wds::chart_render::SplitWallSec::PhaseA)]
                               : 0.0,
                     have_wall ? wall.sec_ms[static_cast<int>(wds::chart_render::SplitWallSec::PhaseB)]
                               : 0.0,
                     have_wall ? wall.sec_ms[static_cast<int>(wds::chart_render::SplitWallSec::PhaseC)]
                               : 0.0,
                     have_wall ? wall.phase_b_ms : 0.0, have_wall ? wall.task_sum_ms : 0.0,
                     have_wall ? wall.unscoped_ms : 0.0, have_wall ? wall.ntasks : 0,
                     have_wall ? wall.evaluated : 0, have_wall ? wall.kept : 0,
                     have_wall ? wall.realized : 0, ru1.ru_minflt - ru0.ru_minflt,
                     ru1.ru_majflt - ru0.ru_majflt, cur.lookup_hits - prev_cache.lookup_hits,
                     cur.lookup_ops - prev_cache.lookup_ops, cur.store_ops - prev_cache.store_ops,
                     cur.carry_hits - prev_cache.carry_hits, cur.carry_misses - prev_cache.carry_misses,
                     cur.carry_walk_hits - prev_cache.carry_walk_hits,
                     cur.carry_walk_misses - prev_cache.carry_walk_misses);
        prev_cache = cur;
        if (have_wall) {
          for (int s = 0; s < static_cast<int>(wds::chart_render::SplitWallSec::Count); ++s) {
            if (wall.sec_ms[s] < 0.0005) {
              continue;
            }
            std::fprintf(stdout, " %s=%.3f", wds::chart_render::split_wall_sec_name(
                                                 static_cast<wds::chart_render::SplitWallSec>(s)),
                         wall.sec_ms[s]);
          }
        }
        std::fprintf(stdout, "\n");
      }
      wds::common::perf::end_frame();
      now_sec = std::min(range_hi, now_sec + (play_dt > 1e-9 ? play_dt : 1.0 / 60.0));
    }
  } else if (kind == "dead") {
    state.reset();
    double warm_sec = now_sec_from_now_t(kDefaultStartMs, 0.0);
    int warm_frames = 0;
    int warm_max_n = 0;
    while (warm_sec + 1e-12 < range_lo) {
      (void)collect_ms(splits, quality, warm_sec, true, state, io);
      warm_max_n = std::max(warm_max_n, io.total);
      warm_sec = std::min(range_lo, warm_sec + (play_dt > 1e-9 ? play_dt : 1.0 / 60.0));
      ++warm_frames;
    }
    std::fprintf(stdout, "  dead-warm frames=%d now_sec=%.4f last_n=%d peak_n=%d splits=%zu\n",
                 warm_frames, warm_sec, io.total, warm_max_n, splits.size());
    double now_sec = range_lo;
    mark_collect_faults();
    for (int i = 0; i < frames; ++i) {
      const double t = collect_ms(splits, quality, now_sec, true, state, io);
      ms.add(t);
      ps.add(io.total);
      wds::common::perf::end_frame();
      now_sec = std::min(range_hi, now_sec + (play_dt > 1e-9 ? play_dt : 1.0 / 60.0));
    }
  } else if (kind == "scrub") {
    std::uniform_real_distribution<double> mag(0.001, 0.050);
    std::bernoulli_distribution sign(0.5);
    double now_sec = 0.5 * (range_lo + range_hi);
    mark_collect_faults();
    for (int i = 0; i < frames; ++i) {
      const double t = collect_ms(splits, quality, now_sec, false, state, io);
      ms.add(t);
      ps.add(io.total);
      wds::common::perf::end_frame();
      const double d = mag(rng) * (sign(rng) ? 1.0 : -1.0);
      now_sec = std::clamp(now_sec + d, range_lo, range_hi);
    }
  } else if (kind == "cold-keep-cache") {
    std::uniform_real_distribution<double> uni(range_lo, range_hi);
    mark_collect_faults();
    for (int i = 0; i < frames; ++i) {
      const double now_sec = uni(rng);
      const double t = collect_ms(splits, quality, now_sec, false, state, io);
      ms.add(t);
      ps.add(io.total);
      wds::common::perf::end_frame();
    }
  } else if (kind == "seek-play") {
    if (warm_frames > 0) {
      double warm_sec = range_lo;
      int wf = 0;
      for (; wf < warm_frames; ++wf) {
        (void)collect_ms(splits, quality, warm_sec, true, state, io);
        warm_sec = std::min(range_hi, warm_sec + (play_dt > 1e-9 ? play_dt : 1.0 / 60.0));
      }
      std::fprintf(stdout, "  seek-play warm-frames=%d now_sec=%.4f n=%d\n", wf, warm_sec, io.total);
    }
    state.reset();
    wds::chart_render::split_effect_clear_caches();
    const double origin = play_origin_sec(kDefaultStartMs);
    const double dt = play_dt > 1e-9 ? play_dt : 1.0 / 60.0;
    double now_sec = range_lo;
    mark_collect_faults();
    auto prev = wds::chart_render::split_effect_eval_cache_stats();
    for (int i = 0; i < frames; ++i) {
      rusage ru0{};
      rusage ru1{};
      getrusage(RUSAGE_SELF, &ru0);
      const double t = collect_ms(splits, quality, now_sec, true, state, io);
      getrusage(RUSAGE_SELF, &ru1);
      const long minflt = ru1.ru_minflt - ru0.ru_minflt;
      const long majflt = ru1.ru_majflt - ru0.ru_majflt;
      ms.add(t);
      ps.add(io.total);
      wds::chart_render::SplitWallFrameSnap wall{};
      const bool have_wall = wds::chart_render::split_wall_copy_last_frame(wall);
      const auto cur = wds::chart_render::split_effect_eval_cache_stats();
      const unsigned long long d_lu_hit = cur.lookup_hits - prev.lookup_hits;
      const unsigned long long d_lu_op = cur.lookup_ops - prev.lookup_ops;
      const unsigned long long d_st_op = cur.store_ops - prev.store_ops;
      const unsigned long long d_c_hit = cur.carry_hits - prev.carry_hits;
      const unsigned long long d_c_miss = cur.carry_misses - prev.carry_misses;
      const unsigned long long d_w_hit = cur.carry_walk_hits - prev.carry_walk_hits;
      const unsigned long long d_w_miss = cur.carry_walk_misses - prev.carry_walk_misses;
      prev = cur;
      std::fprintf(stdout,
                   "SEEKFRAME i=%d now_t=%.4f now_sec=%.4f collect=%.3f n=%d "
                   "A=%.3f B=%.3f C=%.3f Bwall=%.3f task_sum=%.3f unscoped=%.3f "
                   "ntasks=%d nthreads=%d eval=%d kept=%d realized=%d "
                   "minflt=%ld majflt=%ld "
                   "cache hit/op/store=%llu/%llu/%llu carry hit/miss=%llu/%llu "
                   "walk hit/miss=%llu/%llu",
                   i, now_sec - origin, now_sec, t, io.total,
                   have_wall ? wall.sec_ms[static_cast<int>(wds::chart_render::SplitWallSec::PhaseA)]
                             : 0.0,
                   have_wall ? wall.sec_ms[static_cast<int>(wds::chart_render::SplitWallSec::PhaseB)]
                             : 0.0,
                   have_wall ? wall.sec_ms[static_cast<int>(wds::chart_render::SplitWallSec::PhaseC)]
                             : 0.0,
                   have_wall ? wall.phase_b_ms : 0.0, have_wall ? wall.task_sum_ms : 0.0,
                   have_wall ? wall.unscoped_ms : 0.0, have_wall ? wall.ntasks : 0,
                   have_wall ? wall.nthreads : 0, have_wall ? wall.evaluated : 0,
                   have_wall ? wall.kept : 0, have_wall ? wall.realized : 0, minflt, majflt, d_lu_hit,
                   d_lu_op, d_st_op, d_c_hit, d_c_miss, d_w_hit, d_w_miss);
      if (have_wall) {
        for (int s = 0; s < static_cast<int>(wds::chart_render::SplitWallSec::Count); ++s) {
          if (wall.sec_ms[s] < 0.0005) {
            continue;
          }
          std::fprintf(stdout, " %s=%.3f",
                       wds::chart_render::split_wall_sec_name(
                           static_cast<wds::chart_render::SplitWallSec>(s)),
                       wall.sec_ms[s]);
        }
      }
      std::fputc('\n', stdout);
      wds::common::perf::end_frame();
      now_sec += dt;
    }
  } else {
    std::uniform_real_distribution<double> uni(range_lo, range_hi);
    mark_collect_faults();
    for (int i = 0; i < frames; ++i) {
      wds::chart_render::split_effect_clear_caches();
      const double now_sec = uni(rng);
      const double t = collect_ms(splits, quality, now_sec, false, state, io);
      ms.add(t);
      ps.add(io.total);
      wds::common::perf::end_frame();
    }
  }
  print_mode_stats(mode_name, ms, ps);
  const auto cache = wds::chart_render::split_effect_eval_cache_stats();
  std::fprintf(stdout,
               "  cache enabled=%d epoch=%u entries=%zu checkpoints=%zu used=%.2fMB budget=%.2fMB "
               "lookup hit/op/contend=%llu/%llu/%llu store op/contend=%llu/%llu\n",
               cache.enabled ? 1 : 0, cache.epoch, cache.particle_entries, cache.checkpoints,
               static_cast<double>(cache.bytes_used) / (1024.0 * 1024.0),
               static_cast<double>(cache.budget_bytes) / (1024.0 * 1024.0),
               static_cast<unsigned long long>(cache.lookup_hits),
               static_cast<unsigned long long>(cache.lookup_ops),
               static_cast<unsigned long long>(cache.lookup_contend),
               static_cast<unsigned long long>(cache.store_ops),
               static_cast<unsigned long long>(cache.store_contend));
  std::fprintf(stdout,
               "  carry enabled=%d systems=%zu particles=%zu used=%.2fMB budget=%.2fMB gap=%.3f "
               "hit/miss/fallback/commit=%llu/%llu/%llu/%llu walk hit/miss/commit=%llu/%llu/%llu\n",
               cache.carry_enabled ? 1 : 0, cache.carry_systems, cache.carry_particles,
               static_cast<double>(cache.carry_bytes_used) / (1024.0 * 1024.0),
               static_cast<double>(cache.carry_budget_bytes) / (1024.0 * 1024.0),
               cache.carry_max_gap, static_cast<unsigned long long>(cache.carry_hits),
               static_cast<unsigned long long>(cache.carry_misses),
               static_cast<unsigned long long>(cache.carry_fallbacks),
               static_cast<unsigned long long>(cache.carry_commits),
               static_cast<unsigned long long>(cache.carry_walk_hits),
               static_cast<unsigned long long>(cache.carry_walk_misses),
               static_cast<unsigned long long>(cache.carry_walk_commits));
  return 0;
}

int cmd_scenario(int32_t effect_id, int instances, double stagger, const std::string& mode,
                 int frames, uint32_t seed, double now_t, double now_t_hi, double warm_from_t,
                 int warm_frames, double play_dt, int quality) {
  if (wds::chart_render::official_split_effect(effect_id) == nullptr) {
    std::fprintf(stderr, "unknown effect id %d\n", effect_id);
    return 1;
  }
  const bool dead = mode == "dead";
  auto splits = dead ? make_dead_fixture(effect_id, instances)
                     : make_instances(effect_id, instances, stagger, 6, SplitLaneType::Full);
  double range_lo = 0.0;
  double range_hi = 0.0;
  bool pin_now_t = now_t >= 0.0;
  if (dead && !pin_now_t) {
    now_t = 3.5;
    now_t_hi = 5.0;
    pin_now_t = true;
  }
  if (pin_now_t) {
    const double hi_t = now_t_hi >= now_t ? now_t_hi : now_t;
    range_lo = now_sec_from_now_t(kDefaultStartMs, now_t);
    range_hi = now_sec_from_now_t(kDefaultStartMs, hi_t);
    if (mode == "seek-play" && now_t_hi < now_t) {
      const double dt = play_dt > 1e-9 ? play_dt : 1.0 / 60.0;
      range_hi = range_lo + dt * static_cast<double>(std::max(frames, 1) - 1);
    }
  } else {
    choose_peak_range(splits, quality, frames, range_lo, range_hi);
  }
  std::fprintf(stdout,
               "scenario effect=%d instances=%d stagger=%.4f mode=%s quality=%d frames=%d seed=%u "
               "splits=%zu\n"
               "  now_sec range [%.4f, %.4f] %s warm_from_t=%.4f play_dt=%.5f\n",
               effect_id, instances, stagger, mode.c_str(), quality, frames, seed, splits.size(),
               range_lo, range_hi,
               pin_now_t ? (dead ? "(dead-gap now_t)" : "(pinned now_t)")
                         : "(alive + near max particle count)",
               warm_from_t, play_dt);
  const char* bench_perf = std::getenv("WDS_SPLIT_BENCH_PERF");
  if (bench_perf != nullptr && bench_perf[0] != '\0' && bench_perf[0] != '0') {
    wds::common::perf::set_enabled(true);
  }

  auto run = [&](const char* name, const std::string& kind) {
    return run_scenario_mode(name, splits, frames, seed, range_lo, range_hi, kind, warm_from_t,
                             warm_frames, play_dt, quality);
  };
  if (mode == "all") {
    if (run("play", "play") != 0 || run("scrub", "scrub") != 0 || run("cold", "cold") != 0) {
      return 1;
    }
    return 0;
  }
  if (mode == "play" || mode == "scrub" || mode == "cold" || mode == "cold-keep-cache" ||
      mode == "dead" || mode == "seek-play") {
    return run(mode.c_str(), mode);
  }
  std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
  return 1;
}

int cmd_golden_write(const GoldenRunConfig& cfg, const std::string& path) {
  const auto t0 = Clock::now();
  if (cfg.full_format) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
      std::fprintf(stderr, "failed to open %s\n", path.c_str());
      return 1;
    }
    if (!write_u32(f, kGoldenMagic) || !write_u32(f, kGoldenVersionFull) || !write_u32(f, 0)) {
      std::fclose(f);
      return 1;
    }
    uint32_t count = 0;
    const bool ok = for_each_golden_sample(
        [&](const SampleKey& key, const SplitEffectFrameCollect& io) {
          if ((count % 200u) == 0u) {
            char buf[160];
            std::fprintf(stdout, "  golden-write %u  %s\n", count, format_key(key, buf, sizeof(buf)));
            std::fflush(stdout);
          }
          if (!write_sample(f, key, io)) {
            return false;
          }
          ++count;
          return true;
        },
        cfg.scenario_effect, cfg.tier);
    if (!ok) {
      std::fclose(f);
      std::fprintf(stderr, "golden-write failed after %u samples\n", count);
      return 1;
    }
    if (std::fseek(f, 8, SEEK_SET) != 0 || !write_u32(f, count)) {
      std::fclose(f);
      return 1;
    }
    std::fclose(f);
    const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
    std::fprintf(stdout, "golden-write v1 %s tier=%s samples=%u wall=%.2fs\n", path.c_str(),
                 golden_tier_name(cfg.tier), count, sec);
    return 0;
  }
  std::vector<HashSampleRecord> records;
  if (!collect_hash_samples(cfg, records)) {
    std::fprintf(stderr, "golden-write hash collection failed\n");
    return 1;
  }
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "failed to open %s\n", path.c_str());
    return 1;
  }
  if (!write_hash_header(f, cfg.tier, 0)) {
    std::fclose(f);
    return 1;
  }
  uint32_t count = 0;
  for (const HashSampleRecord& rec : records) {
    if ((count % 200u) == 0u) {
      char buf[160];
      std::fprintf(stdout, "  golden-write %u  %s\n", count,
                   format_key(rec.key, buf, sizeof(buf)));
      std::fflush(stdout);
    }
    if (!write_hash_record(f, rec)) {
      std::fclose(f);
      return 1;
    }
    ++count;
  }
  if (std::fseek(f, 8, SEEK_SET) != 0 || !write_u32(f, count)) {
    std::fclose(f);
    return 1;
  }
  std::fclose(f);
  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  std::fprintf(stdout, "golden-write v2 %s tier=%s jobs=%d samples=%u wall=%.2fs\n", path.c_str(),
               golden_tier_name(cfg.tier), cfg.jobs, count, sec);
  return 0;
}

struct SampleKeyEq {
  bool operator()(const SampleKey& a, const SampleKey& b) const { return keys_same(a, b); }
};

struct SampleKeyHashFn {
  size_t operator()(const SampleKey& k) const {
    size_t h = static_cast<size_t>(k.effect_id);
    h = h * 1315423911u + static_cast<size_t>(k.split_count);
    h = h * 1315423911u + static_cast<size_t>(k.instance_set);
    h = h * 1315423911u + static_cast<size_t>(k.instance_count);
    const uint64_t t = static_cast<uint64_t>(std::llround(k.now_t * 1e9));
    h = h * 1315423911u + static_cast<size_t>(t);
    return h;
  }
};

bool load_v2_baseline(const std::string& path, GoldenTier expected_tier,
                      std::unordered_map<SampleKey, HashSampleRecord, SampleKeyHashFn, SampleKeyEq>&
                          out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  uint32_t version = 0;
  uint32_t count = 0;
  GoldenTier tier = GoldenTier::Full;
  if (!read_golden_header(f, version, count, tier)) {
    std::fclose(f);
    return false;
  }
  if (version != kGoldenVersionHash) {
    std::fclose(f);
    return false;
  }
  if (tier != expected_tier) {
    const bool quick_subset =
        expected_tier == GoldenTier::Quick && tier == GoldenTier::Full;
    if (!quick_subset) {
      std::fprintf(stderr, "baseline tier=%s but compare tier=%s\n", golden_tier_name(tier),
                   golden_tier_name(expected_tier));
      std::fclose(f);
      return false;
    }
  }
  out.clear();
  for (uint32_t i = 0; i < count; ++i) {
    HashSampleRecord rec;
    if (!read_hash_record(f, rec)) {
      std::fclose(f);
      return false;
    }
    out[rec.key] = rec;
  }
  std::fclose(f);
  return true;
}

void print_hash_mismatch(const SampleKey& key, const HashSampleRecord& base,
                         const HashSampleRecord& cand, int max_report, int& reported) {
  if (reported >= max_report) {
    return;
  }
  char buf[160];
  if (cand.status == kGoldenSampleTimeout || base.status == kGoldenSampleTimeout) {
    std::fprintf(stdout, "MISMATCH TIMEOUT %s baseline_status=%u candidate_status=%u\n",
                 format_key(key, buf, sizeof(buf)), base.status, cand.status);
    ++reported;
    return;
  }
  if (base.key.particle_count != cand.key.particle_count) {
    std::fprintf(stdout, "MISMATCH count %s baseline_n=%d candidate_n=%d\n",
                 format_key(key, buf, sizeof(buf)), base.key.particle_count,
                 cand.key.particle_count);
    ++reported;
    return;
  }
  std::fprintf(stdout, "MISMATCH hash %s", format_key(key, buf, sizeof(buf)));
  if (base.hashes.full != cand.hashes.full) {
    std::fprintf(stdout, " full");
  }
  for (int g = 0; g < kGoldenHashGroupCount; ++g) {
    if (base.hashes.groups[g] != cand.hashes.groups[g]) {
      std::fprintf(stdout, " %s", kGoldenHashGroupNames[g]);
    }
  }
  std::fputc('\n', stdout);
  ++reported;
}

int cmd_golden_compare_hash(const GoldenRunConfig& cfg, const std::string& baseline_path,
                            int max_report) {
  const auto t0 = Clock::now();
  std::unordered_map<SampleKey, HashSampleRecord, SampleKeyHashFn, SampleKeyEq> base_map;
  if (!load_v2_baseline(baseline_path, cfg.tier, base_map)) {
    return 1;
  }
  std::vector<HashSampleRecord> live;
  if (!collect_hash_samples(cfg, live)) {
    return 1;
  }
  int mismatching = 0;
  int reported = 0;
  int samples = 0;
  for (const HashSampleRecord& cand : live) {
    ++samples;
    const auto it = base_map.find(cand.key);
    if (it == base_map.end()) {
      ++mismatching;
      if (reported < max_report) {
        char buf[160];
        std::fprintf(stdout, "MISMATCH missing baseline key %s\n",
                     format_key(cand.key, buf, sizeof(buf)));
        ++reported;
      }
      continue;
    }
    const HashSampleRecord& base = it->second;
    bool bad = cand.status != base.status || base.key.particle_count != cand.key.particle_count ||
               base.hashes.full != cand.hashes.full;
    if (!bad) {
      for (int g = 0; g < kGoldenHashGroupCount; ++g) {
        if (base.hashes.groups[g] != cand.hashes.groups[g]) {
          bad = true;
          break;
        }
      }
    }
    if (bad) {
      ++mismatching;
      print_hash_mismatch(cand.key, base, cand, max_report, reported);
    }
  }
  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  std::fprintf(stdout, "golden-compare hash tier=%s jobs=%d samples=%d mismatching=%d wall=%.2fs\n",
               golden_tier_name(cfg.tier), cfg.jobs, samples, mismatching, sec);
  return mismatching == 0 ? 0 : 1;
}

int cmd_golden_compare_v1(const GoldenRunConfig& cfg, const std::string& path, bool exact,
                          double tol_pos, double tol_color, int max_report) {
  const auto t0 = Clock::now();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "failed to open %s\n", path.c_str());
    return 1;
  }
  uint32_t version = 0;
  uint32_t file_count = 0;
  GoldenTier file_tier = GoldenTier::Full;
  if (!read_golden_header(f, version, file_count, file_tier)) {
    std::fclose(f);
    std::fprintf(stderr, "golden header read failed\n");
    return 1;
  }
  if (version != kGoldenVersionFull) {
    std::fclose(f);
    std::fprintf(stderr, "golden v1 compare requires version=1 (got %u)\n", version);
    return 1;
  }

  std::unordered_map<SampleKey, std::vector<SplitEffectLiveParticle>, SampleKeyHashFn, SampleKeyEq>
      base_map;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < file_count; ++i) {
    SampleKey file_key;
    if (!read_sample_key(f, file_key)) {
      std::fclose(f);
      return 1;
    }
    const bool keep = cfg.tier == GoldenTier::Full ||
                      quick_tier_includes_effect(cfg.tier, file_key.effect_id) ||
                      file_key.effect_id == cfg.scenario_effect;
    std::vector<SplitEffectLiveParticle> base;
    if (keep) {
      base.resize(static_cast<size_t>(file_key.particle_count));
    }
    for (int pi = 0; pi < file_key.particle_count; ++pi) {
      SplitEffectLiveParticle tmp;
      if (!read_particle(f, keep ? base[static_cast<size_t>(pi)] : tmp)) {
        std::fclose(f);
        return 1;
      }
    }
    if (keep) {
      base_map[file_key] = std::move(base);
      ++kept;
    }
  }
  std::fclose(f);
  std::fprintf(stdout, "golden-compare loaded %u/%u baseline samples (tier=%s)\n", kept, file_count,
               golden_tier_name(cfg.tier));
  std::fflush(stdout);

  CompareAcc acc;
  acc.exact = exact;
  acc.tol_pos = tol_pos;
  acc.tol_color = tol_color;
  acc.max_report = max_report;

  const bool walk_ok = for_each_golden_sample(
      [&](const SampleKey& live_key, const SplitEffectFrameCollect& io) {
        const auto it = base_map.find(live_key);
        if (it == base_map.end()) {
          return true;
        }
        const SampleKey& file_key = it->first;
        const std::vector<SplitEffectLiveParticle>& base = it->second;
        ++acc.samples;
        acc.sample_failed = false;
        char live_buf[160];
        if (live_key.particle_count != file_key.particle_count) {
          acc.sample_failed = true;
          ++acc.count_mismatch;
          acc.count_mismatch_effects[live_key.effect_id] += 1;
          acc.note("particle_count", live_key, live_key.particle_count, file_key.particle_count,
                   false);
          if (acc.reported < acc.max_report) {
            std::fprintf(stdout, "MISMATCH count live=%s file_n=%d\n",
                         format_key(live_key, live_buf, sizeof(live_buf)), file_key.particle_count);
            ++acc.reported;
          }
        } else {
          ++acc.matched_samples;
          const auto live = flatten(io);
          for (int i = 0; i < live_key.particle_count; ++i) {
            compare_particle(acc, live_key, live[static_cast<size_t>(i)],
                             base[static_cast<size_t>(i)]);
          }
          if (acc.sample_failed && acc.reported < acc.max_report) {
            std::fprintf(stdout, "MISMATCH sample %s\n",
                         format_key(live_key, live_buf, sizeof(live_buf)));
            ++acc.reported;
          }
        }
        if (acc.sample_failed) {
          ++acc.mismatching;
        }
        return true;
      },
      cfg.scenario_effect, cfg.tier);
  if (!walk_ok) {
    return 1;
  }

  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  std::fprintf(stdout,
               "golden-compare %s samples=%d mismatching=%d count_mismatch=%d matched=%d wall=%.2fs\n",
               exact ? "exact" : "tol", acc.samples, acc.mismatching, acc.count_mismatch,
               acc.matched_samples, sec);
  auto print_dist = [&](const char* name, const FieldDist& d) {
    std::fprintf(stdout, "  %s n=%zu max=%.6g p99=%.6g p50=%.6g\n", name, d.abs.size(), d.max(),
                 d.pct(0.99), d.pct(0.50));
  };
  print_dist("world_x", acc.world_x);
  print_dist("main_y", acc.main_y);
  print_dist("z", acc.world_z);
  print_dist("trail", acc.trail);
  print_dist("color", acc.color);
  print_dist("size", acc.size);
  print_dist("pixel_1920x1080", acc.pixel);
  if (!acc.count_mismatch_effects.empty()) {
    std::fprintf(stdout, "  count-mismatch effects:");
    std::vector<std::pair<int32_t, int>> rows(acc.count_mismatch_effects.begin(),
                                              acc.count_mismatch_effects.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& r : rows) {
      std::fprintf(stdout, " %d×%d", r.first, r.second);
    }
    std::fputc('\n', stdout);
  }
  if (!acc.effect_max_pos.empty()) {
    std::vector<std::pair<int32_t, double>> rows(acc.effect_max_pos.begin(),
                                                 acc.effect_max_pos.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stdout, "  top effects by max abs pos:");
    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
      std::fprintf(stdout, " %d=%.6g", rows[i].first, rows[i].second);
    }
    std::fputc('\n', stdout);
  }
  if (!acc.effect_max_pixel.empty()) {
    std::vector<std::pair<int32_t, double>> rows(acc.effect_max_pixel.begin(),
                                                 acc.effect_max_pixel.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stdout, "  top effects by max pixel_1920x1080:");
    for (size_t i = 0; i < rows.size() && i < 12; ++i) {
      std::fprintf(stdout, " %d=%.6g", rows[i].first, rows[i].second);
    }
    std::fputc('\n', stdout);
  }
  std::vector<WorstDiff> worst;
  worst.reserve(acc.worst.size());
  for (const auto& kv : acc.worst) {
    worst.push_back(kv.second);
  }
  std::sort(worst.begin(), worst.end(),
            [](const WorstDiff& a, const WorstDiff& b) { return a.abs_diff > b.abs_diff; });
  for (const WorstDiff& w : worst) {
    if (w.abs_diff <= 0.0) {
      continue;
    }
    char kbuf[160];
    std::fprintf(stdout, "  worst %s abs=%.6g cand=%.6g base=%.6g  %s\n", w.field.c_str(),
                 w.abs_diff, w.cand, w.base, format_key(w.key, kbuf, sizeof(kbuf)));
  }
  return acc.mismatching == 0 ? 0 : 1;
}

int cmd_noise_hold_plan() {
  using wds::chart_render::shuriken::split_noise_coord_rate;
  using wds::chart_render::shuriken::split_noise_est_frame_work;
  using wds::chart_render::shuriken::split_noise_est_live;
  using wds::chart_render::shuriken::split_noise_hold_for;
  using wds::chart_render::shuriken::split_noise_hold_target_travel;
  using wds::chart_render::shuriken::split_noise_hold_tvis;
  const float target = split_noise_hold_target_travel();
  const float tvis = split_noise_hold_tvis();
  std::fprintf(stdout, "noise-hold-plan target=%.6g tvis=%.6g budget=0.03 cost_hop=8\n",
               static_cast<double>(target), static_cast<double>(tvis));
  const auto ids = official_split_effect_ids();
  int densify_n = 0;
  for (int32_t id : ids) {
    uint16_t n = 0;
    const SplitEffectParticle* ps = effect_catalog_particles(id, &n);
    if (ps == nullptr || n == 0) {
      continue;
    }
    float work4 = 0.0f;
    for (uint16_t i = 0; i < n; ++i) {
      const float rate = split_noise_coord_rate(ps[i]);
      const int n4 = rate <= 1e-8f ? 16 : std::clamp(static_cast<int>(std::floor(target * 60.0f / rate)), 4, 16);
      work4 += split_noise_est_frame_work(ps[i], n4, id);
    }
    for (uint16_t i = 0; i < n; ++i) {
      const float rate = split_noise_coord_rate(ps[i]);
      if (rate <= 1e-8f && (ps[i].flags & kSplitEffectFlagNoise) == 0) {
        continue;
      }
      const float travel4 = rate * 4.0f / 60.0f;
      const int n_rate =
          rate <= 1e-8f ? 16 : std::clamp(static_cast<int>(std::floor(target * 60.0f / rate)), 1, 16);
      const int n4 = std::max(n_rate, 4);
      const int chosen = split_noise_hold_for(ps[i], id);
      const float live = split_noise_est_live(ps[i], id);
      const float extra =
          live * (60.0f / static_cast<float>(std::max(chosen, 1)) - 60.0f / 4.0f) * 8.0f / 60.0f;
      const float frac = work4 > 1e-8f ? extra / work4 : 0.0f;
      if (chosen < 4) {
        ++densify_n;
      }
      std::fprintf(stdout,
                   "  id=%d ei=%u i=%u rate=%.5g t4=%.5g n_rate=%d n4=%d N=%d live=%.3g extra=%.4g\n",
                   id, static_cast<unsigned>(ps[i].element_index), static_cast<unsigned>(i),
                   static_cast<double>(rate), static_cast<double>(travel4), n_rate, n4, chosen,
                   static_cast<double>(live), static_cast<double>(frac));
    }
  }
  std::fprintf(stdout, "noise-hold-plan densified_systems=%d\n", densify_n);
  return 0;
}

int cmd_noise_catalog() {
  const auto ids = official_split_effect_ids();
  int noise_n = 0;
  int quiet_n = 0;
  int sub_n = 0;
  std::vector<std::pair<float, int32_t>> heavy;
  std::fprintf(stdout, "noise-catalog effects=%zu\n", ids.size());
  for (int32_t id : ids) {
    if (!effect_has_active_noise(id)) {
      ++quiet_n;
      continue;
    }
    ++noise_n;
    const bool sub = effect_has_noise_driven_subemit(id);
    if (sub) {
      ++sub_n;
    }
    const float w = effect_noise_weight(id);
    heavy.push_back({w, id});
    uint16_t n = 0;
    const SplitEffectParticle* ps = effect_catalog_particles(id, &n);
    int rod = 0;
    int oct = 0;
    for (uint16_t i = 0; i < n; ++i) {
      if ((ps[i].flags & kSplitEffectFlagNoise) != 0) {
        oct = std::max(oct, static_cast<int>(ps[i].noise_octaves));
      }
      if (ps[i].rate_over_distance > 1e-6f) {
        ++rod;
      }
    }
    std::fprintf(stdout, "  noise %d systems=%u oct_max=%d rod=%d subemit=%d weight=%.4g\n", id, n,
                 oct, rod, sub ? 1 : 0, static_cast<double>(w));
  }
  std::sort(heavy.begin(), heavy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::fprintf(stdout, "noise-catalog active=%d quiet=%d noise_subemit=%d\n  heaviest:", noise_n,
               quiet_n, sub_n);
  for (size_t i = 0; i < heavy.size() && i < 8; ++i) {
    std::fprintf(stdout, " %d=%.4g", heavy[i].second, static_cast<double>(heavy[i].first));
  }
  std::fputc('\n', stdout);
  return 0;
}

int cmd_noise_error(int32_t scenario_effect, GoldenTier tier) {
  using wds::chart_render::shuriken::reset_split_eval_persistence_for_tests;
  using wds::chart_render::shuriken::set_split_noise_exact_for_tests;
  const auto t0 = Clock::now();
  prehydrate_golden_controllers(scenario_effect);
  CompareAcc acc;
  acc.exact = false;
  acc.tol_pos = 1.0e9;
  acc.tol_color = 1.0e9;
  acc.tol_rel = 1.0e9;
  acc.max_report = 12;
  FieldDist count_rel;
  FieldDist pixel_nosub;
  FieldDist pixel_sub;
  int quiet_samples = 0;
  int quiet_mismatch = 0;
  int noise_samples = 0;
  int matched_particles = 0;
  int unmatched_exact = 0;
  int unmatched_approx = 0;
  int color_nonzero = 0;
  int size_nonzero = 0;

  auto particle_pixel = [](const SplitEffectLiveParticle& a,
                           const SplitEffectLiveParticle& b) -> double {
    const bool front_a = wds::chart_editor::official_main_in_front_of_camera(a.main_y, a.z);
    const bool front_b = wds::chart_editor::official_main_in_front_of_camera(b.main_y, b.z);
    if (!front_a || !front_b) {
      return -1.0;
    }
    const auto na = wds::chart_editor::project_main_xyz(a.world_x, a.main_y, a.z);
    const auto nb = wds::chart_editor::project_main_xyz(b.world_x, b.main_y, b.z);
    const double dpx = (static_cast<double>(na.x) - static_cast<double>(nb.x)) * 0.5 * 1920.0;
    const double dpy = (static_cast<double>(na.y) - static_cast<double>(nb.y)) * 0.5 * 1080.0;
    return std::sqrt(dpx * dpx + dpy * dpy);
  };

  using PKey = std::tuple<int32_t, uint32_t, uint32_t>;
  auto eval_pair = [&](const std::vector<PreviewSplitLaneInstance>& splits, SampleKey& key) {
    const double now_sec = now_sec_from_now_t(kDefaultStartMs, key.now_t);
    SplitEffectFrameCollect exact_io;
    SplitEffectFrameCollect approx_io;
    SplitEffectPlaybackState st_a;
    SplitEffectPlaybackState st_b;
    reset_split_eval_persistence_for_tests();
    set_split_noise_exact_for_tests(1);
    wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, st_a, exact_io);
    reset_split_eval_persistence_for_tests();
    set_split_noise_exact_for_tests(0);
    wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, st_b, approx_io);
    set_split_noise_exact_for_tests(-1);
    key.particle_count = exact_io.total;
    ++acc.samples;
    acc.sample_failed = false;
    const bool noisy = effect_has_active_noise(key.effect_id);
    const auto exact = flatten(exact_io);
    const auto approx = flatten(approx_io);
    if (exact_io.total != approx_io.total) {
      ++acc.count_mismatch;
      acc.count_mismatch_effects[key.effect_id] += 1;
    }
    const double denom = static_cast<double>(std::max(exact_io.total, 1));
    count_rel.add(std::fabs(static_cast<double>(approx_io.total - exact_io.total)) / denom);
    if (!noisy) {
      ++quiet_samples;
      std::vector<uint8_t> ha;
      std::vector<uint8_t> hb;
      BufferWriter wa{ha};
      BufferWriter wb{hb};
      for (const auto& p : exact) {
        write_particle(wa, p);
      }
      for (const auto& p : approx) {
        write_particle(wb, p);
      }
      if (exact_io.total != approx_io.total || fnv1a64_buffer(ha) != fnv1a64_buffer(hb)) {
        ++quiet_mismatch;
        acc.sample_failed = true;
        ++acc.mismatching;
        if (acc.reported < acc.max_report) {
          ++acc.reported;
          std::fprintf(stderr, "noise-error quiet mismatch effect=%d now_t=%.3f n=%d/%d\n",
                       key.effect_id, key.now_t, approx_io.total, exact_io.total);
        }
      }
      return;
    }
    ++noise_samples;
    ++acc.matched_samples;
    const bool sub = effect_has_noise_driven_subemit(key.effect_id);
    std::map<PKey, std::vector<size_t>> ix;
    for (size_t i = 0; i < exact.size(); ++i) {
      const auto& p = exact[i];
      ix[PKey{p.line_index, p.catalog_index, p.seed}].push_back(i);
    }
    std::vector<uint8_t> used(exact.size(), 0);
    for (const auto& a : approx) {
      const PKey k{a.line_index, a.catalog_index, a.seed};
      auto it = ix.find(k);
      bool hit = false;
      if (it != ix.end()) {
        for (size_t ei : it->second) {
          if (used[ei] != 0) {
            continue;
          }
          used[ei] = 1;
          const auto& b = exact[ei];
          compare_particle(acc, key, a, b);
          ++matched_particles;
          const double pix = particle_pixel(a, b);
          if (pix >= 0.0) {
            (sub ? pixel_sub : pixel_nosub).add(pix);
          }
          const double cd =
              std::max({std::fabs(static_cast<double>(a.r) - static_cast<double>(b.r)),
                        std::fabs(static_cast<double>(a.g) - static_cast<double>(b.g)),
                        std::fabs(static_cast<double>(a.b) - static_cast<double>(b.b)),
                        std::fabs(static_cast<double>(a.a) - static_cast<double>(b.a))});
          const double sd = std::fabs(static_cast<double>(a.size) - static_cast<double>(b.size));
          if (cd > 1e-6) {
            ++color_nonzero;
          }
          if (sd > 1e-6) {
            ++size_nonzero;
          }
          hit = true;
          break;
        }
      }
      if (!hit) {
        ++unmatched_approx;
      }
    }
    for (size_t i = 0; i < used.size(); ++i) {
      if (used[i] == 0) {
        ++unmatched_exact;
      }
    }
  };

  const std::vector<int32_t> ids = wds::chart_render::official_split_effect_ids();
  const int32_t counts[2] = {1, 6};
  for (int32_t id : ids) {
    if (!quick_tier_includes_effect(tier, id)) {
      continue;
    }
    for (int32_t sc : counts) {
      wds::chart_render::reset_split_effect_auto_seed_session();
      const std::vector<PreviewSplitLaneInstance> splits{
          make_split(id, sc, SplitLaneType::Full, kDefaultStartMs)};
      for (double now_t : kGoldenTimes) {
        SampleKey key;
        key.effect_id = id;
        key.split_count = sc;
        key.instance_set = kInstanceSetSingle;
        key.instance_count = 1;
        key.now_t = now_t;
        eval_pair(splits, key);
      }
    }
  }
  const int overlap_sets[2] = {kInstanceSetOverlap4Stagger0, kInstanceSetOverlap4Stagger02};
  std::vector<double> overlap_times;
  overlap_now_times(overlap_times);
  for (int si = 0; si < 2; ++si) {
    wds::chart_render::reset_split_effect_auto_seed_session();
    const auto splits = make_instances(scenario_effect, kOverlapInstanceCount, kOverlapStaggers[si],
                                       6, SplitLaneType::Full);
    for (double now_t : overlap_times) {
      SampleKey key;
      key.effect_id = scenario_effect;
      key.split_count = 6;
      key.instance_set = overlap_sets[si];
      key.instance_count = kOverlapInstanceCount;
      key.now_t = now_t;
      eval_pair(splits, key);
    }
  }

  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  auto print_dist = [&](const char* name, const FieldDist& d) {
    std::fprintf(stdout, "  %s n=%zu max=%.6g p99=%.6g p50=%.6g\n", name, d.abs.size(), d.max(),
                 d.pct(0.99), d.pct(0.50));
  };
  std::fprintf(stdout,
               "noise-error samples=%d noise_samples=%d quiet_samples=%d quiet_mismatch=%d "
               "count_mismatch=%d matched_particles=%d unmatched_exact=%d unmatched_approx=%d "
               "color_max=%.6g size_max=%.6g wall=%.2fs\n",
               acc.samples, noise_samples, quiet_samples, quiet_mismatch, acc.count_mismatch,
               matched_particles, unmatched_exact, unmatched_approx, acc.color.max(),
               acc.size.max(), sec);
  print_dist("count_rel", count_rel);
  print_dist("pixel_1920x1080", acc.pixel);
  print_dist("pixel_no_subemit", pixel_nosub);
  print_dist("pixel_subemit", pixel_sub);
  print_dist("pixel_trail", acc.pixel_trail);
  print_dist("world_x", acc.world_x);
  print_dist("main_y", acc.main_y);
  print_dist("z", acc.world_z);
  print_dist("trail", acc.trail);
  print_dist("color", acc.color);
  print_dist("size", acc.size);
  if (!acc.effect_max_pixel.empty()) {
    std::vector<std::pair<int32_t, double>> rows(acc.effect_max_pixel.begin(),
                                                 acc.effect_max_pixel.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stdout, "  worst effects by pixel:");
    for (size_t i = 0; i < rows.size() && i < 12; ++i) {
      std::fprintf(stdout, " %d=%.6g", rows[i].first, rows[i].second);
    }
    std::fputc('\n', stdout);
  }
  if (!acc.effect_max_pixel_trail.empty()) {
    std::vector<std::pair<int32_t, double>> rows(acc.effect_max_pixel_trail.begin(),
                                                 acc.effect_max_pixel_trail.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stdout, "  worst effects by trail pixel:");
    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
      std::fprintf(stdout, " %d=%.6g", rows[i].first, rows[i].second);
    }
    std::fputc('\n', stdout);
  }
  const double p99 = std::max(acc.pixel.pct(0.99), acc.pixel_trail.pct(0.99));
  const double count_p99 = count_rel.pct(0.99);
  const bool quiet_ok = quiet_mismatch == 0;
  const bool count_ok = count_p99 < 0.02;
  const bool pix_ok = p99 < 0.5;
  const bool gate = quiet_ok && count_ok && pix_ok;
  if (!acc.count_mismatch_effects.empty()) {
    std::vector<std::pair<int32_t, int>> rows(acc.count_mismatch_effects.begin(),
                                              acc.count_mismatch_effects.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stdout, "  count_mismatch effects:");
    for (size_t i = 0; i < rows.size() && i < 12; ++i) {
      std::fprintf(stdout, " %d=%d", rows[i].first, rows[i].second);
    }
    std::fputc('\n', stdout);
  }
  std::fprintf(stdout,
               "  color/size diffs: nonzero_color=%d nonzero_size=%d of matched=%d "
               "(p99 color=%.6g size=%.6g). Sources: RoD Sigma|dp| birth time "
               "(emit_u startColor/startSize, COL/SOL age u), inherit from "
               "noise-moved parents, max_particles cap. ColorBySpeed/SizeBySpeed "
               "are hydrated but not applied. Quiet systems must stay identical.\n",
               color_nonzero, size_nonzero, matched_particles, acc.color.pct(0.99),
               acc.size.pct(0.99));
  std::fprintf(stdout,
               "noise-error %s quiet=%s count_rel_p99=%.6g pixel_p99=%.6g "
               "pixel_nosub_p99=%.6g pixel_sub_p99=%.6g "
               "(color/size diffs allowed when they follow noise motion)\n",
               gate ? "GATE_OK" : "GATE_FAIL", quiet_ok ? "identical" : "DIFF", count_p99, p99,
               pixel_nosub.pct(0.99), pixel_sub.pct(0.99));
  return gate ? 0 : 1;
}

int cmd_golden_compare(const GoldenRunConfig& cfg, const std::string& path, bool v1_tolerance,
                       double tol_pos, double tol_color, int max_report, bool explicit_tol) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "failed to open %s\n", path.c_str());
    return 1;
  }
  uint32_t magic = 0;
  if (!read_u32(f, magic) || magic != kGoldenMagic) {
    std::fclose(f);
    std::fprintf(stderr, "golden header mismatch magic=0x%x\n", magic);
    return 1;
  }
  uint32_t version = 0;
  uint32_t count = 0;
  GoldenTier base_tier = GoldenTier::Full;
  if (!read_u32(f, version) || !read_u32(f, count)) {
    std::fclose(f);
    std::fprintf(stderr, "golden header read failed\n");
    return 1;
  }
  if (version == kGoldenVersionHash) {
    uint32_t t = 0;
    if (!read_u32(f, t)) {
      std::fclose(f);
      return 1;
    }
    base_tier = static_cast<GoldenTier>(t);
  }
  std::fclose(f);
  if (version == kGoldenVersionHash) {
    if (explicit_tol) {
      std::fprintf(stderr, "tolerance compare requires v1 full baseline\n");
      return 1;
    }
    if (base_tier == GoldenTier::Full && cfg.tier == GoldenTier::Quick) {
      // allowed: subset compare
    } else if (base_tier != cfg.tier) {
      std::fprintf(stderr, "baseline tier=%s compare tier=%s\n", golden_tier_name(base_tier),
                   golden_tier_name(cfg.tier));
      return 1;
    }
    return cmd_golden_compare_hash(cfg, path, max_report);
  }
  if (version != kGoldenVersionFull) {
    std::fprintf(stderr, "unsupported golden version %u\n", version);
    return 1;
  }
  return cmd_golden_compare_v1(cfg, path, !v1_tolerance, tol_pos, tol_color, max_report);
}

int cmd_golden_convert(const std::string& in_path, const std::string& out_path, GoldenTier tier) {
  const auto t0 = Clock::now();
  FILE* in = std::fopen(in_path.c_str(), "rb");
  if (in == nullptr) {
    std::fprintf(stderr, "failed to open %s\n", in_path.c_str());
    return 1;
  }
  uint32_t version = 0;
  uint32_t count = 0;
  GoldenTier file_tier = GoldenTier::Full;
  if (!read_golden_header(in, version, count, file_tier)) {
    std::fclose(in);
    return 1;
  }
  if (version != kGoldenVersionFull) {
    std::fclose(in);
    std::fprintf(stderr, "golden-convert requires v1 input (version=%u)\n", version);
    return 1;
  }
  FILE* out = std::fopen(out_path.c_str(), "wb");
  if (out == nullptr) {
    std::fclose(in);
    return 1;
  }
  if (!write_hash_header(out, tier, 0)) {
    std::fclose(in);
    std::fclose(out);
    return 1;
  }
  uint32_t written = 0;
  auto convert_one = [&](SampleKey& key) -> bool {
    std::vector<SplitEffectLiveParticle> parts(static_cast<size_t>(key.particle_count));
    for (int pi = 0; pi < key.particle_count; ++pi) {
      if (!read_particle(in, parts[static_cast<size_t>(pi)])) {
        return false;
      }
    }
    SplitEffectFrameCollect io;
    io.total = key.particle_count;
    io.split_count.push_back(key.particle_count);
    io.split_begin.push_back(0);
    io.pool = std::move(parts);
    HashSampleRecord rec;
    rec.key = key;
    rec.status = kGoldenSampleOk;
    rec.hashes = hash_collect(io, key.particle_count);
    if (!write_hash_record(out, rec)) {
      return false;
    }
    ++written;
    return true;
  };
  if (count == 0) {
    SampleKey key;
    while (read_sample_key(in, key)) {
      if (!convert_one(key)) {
        std::fclose(in);
        std::fclose(out);
        return 1;
      }
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      SampleKey key;
      if (!read_sample_key(in, key)) {
        std::fclose(in);
        std::fclose(out);
        return 1;
      }
      if (!convert_one(key)) {
        std::fclose(in);
        std::fclose(out);
        return 1;
      }
    }
  }
  if (std::fseek(out, 8, SEEK_SET) != 0 || !write_u32(out, written)) {
    std::fclose(in);
    std::fclose(out);
    return 1;
  }
  std::fclose(in);
  std::fclose(out);
  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  std::fprintf(stdout, "golden-convert %s -> %s tier=%s samples=%u wall=%.2fs\n", in_path.c_str(),
               out_path.c_str(), golden_tier_name(tier), written, sec);
  return 0;
}

bool load_v2_file_any_tier(const std::string& path,
                           std::unordered_map<SampleKey, HashSampleRecord, SampleKeyHashFn,
                                              SampleKeyEq>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  uint32_t version = 0;
  uint32_t count = 0;
  GoldenTier tier = GoldenTier::Full;
  if (!read_golden_header(f, version, count, tier) || version != kGoldenVersionHash) {
    std::fclose(f);
    return false;
  }
  out.clear();
  if (count == 0) {
    HashSampleRecord rec;
    while (read_hash_record(f, rec)) {
      out[rec.key] = rec;
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      HashSampleRecord rec;
      if (!read_hash_record(f, rec)) {
        std::fclose(f);
        return false;
      }
      out[rec.key] = rec;
    }
  }
  std::fclose(f);
  return true;
}

int cmd_golden_compare_files(const std::string& a_path, const std::string& b_path, int max_report) {
  const auto t0 = Clock::now();
  std::unordered_map<SampleKey, HashSampleRecord, SampleKeyHashFn, SampleKeyEq> a_map;
  std::unordered_map<SampleKey, HashSampleRecord, SampleKeyHashFn, SampleKeyEq> b_map;
  if (!load_v2_file_any_tier(a_path, a_map)) {
    std::fprintf(stderr, "failed to read v2 file A\n");
    return 1;
  }
  if (!load_v2_file_any_tier(b_path, b_map)) {
    std::fprintf(stderr, "failed to read v2 file B\n");
    return 1;
  }
  int mismatching = 0;
  int reported = 0;
  for (const auto& kv : a_map) {
    const auto it = b_map.find(kv.first);
    if (it == b_map.end()) {
      ++mismatching;
      continue;
    }
    const HashSampleRecord& base = kv.second;
    const HashSampleRecord& cand = it->second;
    bool bad = base.status != cand.status || base.key.particle_count != cand.key.particle_count ||
               base.hashes.full != cand.hashes.full;
    if (!bad) {
      for (int g = 0; g < kGoldenHashGroupCount; ++g) {
        if (base.hashes.groups[g] != cand.hashes.groups[g]) {
          bad = true;
          break;
        }
      }
    }
    if (bad) {
      ++mismatching;
      print_hash_mismatch(kv.first, base, cand, max_report, reported);
    }
  }
  for (const auto& kv : b_map) {
    if (a_map.find(kv.first) == a_map.end()) {
      ++mismatching;
    }
  }
  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  std::fprintf(stdout, "golden-compare-files A=%zu B=%zu mismatching=%d wall=%.2fs\n", a_map.size(),
               b_map.size(), mismatching, sec);
  return mismatching == 0 ? 0 : 1;
}

bool parse_golden_dump_key(const std::string& key_str, SampleKey& key, int32_t scenario_effect) {
  int effect = 0;
  int split = 0;
  double now_t = 0.0;
  int set = kInstanceSetSingle;
  int inst = 1;
  const int n = std::sscanf(key_str.c_str(), "%d %d %lf %d %d", &effect, &split, &now_t, &set,
                            &inst);
  if (n < 3) {
    return false;
  }
  key.effect_id = effect;
  key.split_count = split;
  key.now_t = now_t;
  key.instance_set = set;
  key.instance_count = inst;
  if (n < 4) {
    key.instance_set = kInstanceSetSingle;
    key.instance_count = 1;
  }
  if (key.instance_set != kInstanceSetSingle) {
    key.effect_id = scenario_effect;
    key.split_count = 6;
  }
  return true;
}

int cmd_golden_dump(const std::string& key_str, const std::string& out_path,
                    int32_t scenario_effect) {
  SampleKey key;
  if (!parse_golden_dump_key(key_str, key, scenario_effect)) {
    std::fprintf(stderr, "golden-dump key parse failed (expected: EFFECT SPLIT NOW_T [SET INST])\n");
    return 1;
  }
  prehydrate_golden_controllers(scenario_effect);
  wds::chart_render::reset_split_effect_auto_seed_session();
  SplitEffectFrameCollect io;
  SplitEffectPlaybackState state;
  if (key.instance_set == kInstanceSetSingle) {
    const std::vector<PreviewSplitLaneInstance> splits{
        make_split(key.effect_id, key.split_count, SplitLaneType::Full, kDefaultStartMs)};
    const double now_sec = now_sec_from_now_t(kDefaultStartMs, key.now_t);
    wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
  } else {
    const int si = key.instance_set == kInstanceSetOverlap4Stagger02 ? 1 : 0;
    const auto splits = make_instances(scenario_effect, kOverlapInstanceCount, kOverlapStaggers[si],
                                       6, SplitLaneType::Full);
    const double now_sec = now_sec_from_now_t(kDefaultStartMs, key.now_t);
    wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
  }
  key.particle_count = io.total;
  FILE* f = std::fopen(out_path.c_str(), "wb");
  if (f == nullptr) {
    return 1;
  }
  if (!write_u32(f, kGoldenMagic) || !write_u32(f, kGoldenVersionFull) || !write_u32(f, 1)) {
    std::fclose(f);
    return 1;
  }
  if (!write_sample(f, key, io)) {
    std::fclose(f);
    return 1;
  }
  std::fclose(f);
  char buf[160];
  std::fprintf(stdout, "golden-dump %s %s\n", out_path.c_str(), format_key(key, buf, sizeof(buf)));
  return 0;
}

#include "geom_dump_cmd.inc"

int cmd_raster(int32_t effect_id, double now_t, const std::string& out_path, int instances,
               double stagger, int width, int height, bool have_bounds, float bx0, float bx1,
               float by0, float by1) {
  if (wds::chart_render::official_split_effect(effect_id) == nullptr) {
    std::fprintf(stderr, "unknown effect id %d\n", effect_id);
    return 1;
  }
  if (width < 8 || height < 8 || width > 4096 || height > 4096) {
    std::fprintf(stderr, "bad raster size\n");
    return 1;
  }
  wds::chart_render::reset_split_effect_auto_seed_session();
  SplitEffectFrameCollect io;
  SplitEffectPlaybackState state;
  const auto splits = make_instances(effect_id, instances, stagger, 6, SplitLaneType::Full);
  const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
  wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
  float xmin = 1.0e30f, xmax = -1.0e30f, ymin = 1.0e30f, ymax = -1.0e30f;
  int n = 0;
  int trail_n = 0;
  double trail_len_sum = 0.0;
  for (size_t s = 0; s < io.split_count.size(); ++s) {
    const int begin = io.split_begin[s];
    const int count = io.split_count[s];
    for (int i = 0; i < count; ++i) {
      const auto& p = io.pool[static_cast<size_t>(begin + i)];
      xmin = std::min(xmin, p.world_x);
      xmax = std::max(xmax, p.world_x);
      ymin = std::min(ymin, p.main_y);
      ymax = std::max(ymax, p.main_y);
      const int tn = static_cast<int>(p.trail_points);
      if (tn >= 2 && !p.trail_x.empty() && !p.trail_y.empty()) {
        ++trail_n;
        float px = p.trail_x[0];
        float py = p.trail_y[0];
        xmin = std::min(xmin, px);
        xmax = std::max(xmax, px);
        ymin = std::min(ymin, py);
        ymax = std::max(ymax, py);
        for (int ti = 1; ti < tn && ti < static_cast<int>(p.trail_x.size()); ++ti) {
          const float qx = p.trail_x[static_cast<size_t>(ti)];
          const float qy = p.trail_y[static_cast<size_t>(ti)];
          xmin = std::min(xmin, qx);
          xmax = std::max(xmax, qx);
          ymin = std::min(ymin, qy);
          ymax = std::max(ymax, qy);
          const float dx = qx - px;
          const float dy = qy - py;
          trail_len_sum += std::sqrt(dx * dx + dy * dy);
          px = qx;
          py = qy;
        }
      }
      ++n;
    }
  }
  if (n == 0) {
    xmin = -1.0f;
    xmax = 1.0f;
    ymin = -1.0f;
    ymax = 1.0f;
  }
  if (have_bounds) {
    xmin = bx0;
    xmax = bx1;
    ymin = by0;
    ymax = by1;
  } else {
    const float dx = std::max(xmax - xmin, 1e-3f);
    const float dy = std::max(ymax - ymin, 1e-3f);
    xmin -= dx * 0.08f;
    xmax += dx * 0.08f;
    ymin -= dy * 0.08f;
    ymax += dy * 0.08f;
  }
  std::vector<float> rgb(static_cast<size_t>(width * height * 3), 0.08f);
  const float sx = static_cast<float>(width - 1) / std::max(xmax - xmin, 1e-3f);
  const float sy = static_cast<float>(height - 1) / std::max(ymax - ymin, 1e-3f);
  auto plot = [&](int px, int py, float r, float g, float b, float a) {
    if (px < 0 || py < 0 || px >= width || py >= height) {
      return;
    }
    const size_t i = static_cast<size_t>((height - 1 - py) * width + px) * 3;
    const float w = std::clamp(a, 0.0f, 1.0f);
    rgb[i + 0] = rgb[i + 0] * (1.0f - w) + r * w;
    rgb[i + 1] = rgb[i + 1] * (1.0f - w) + g * w;
    rgb[i + 2] = rgb[i + 2] * (1.0f - w) + b * w;
  };
  for (size_t s = 0; s < io.split_count.size(); ++s) {
    const int begin = io.split_begin[s];
    const int count = io.split_count[s];
    for (int i = 0; i < count; ++i) {
      const auto& p = io.pool[static_cast<size_t>(begin + i)];
      const float u = (p.world_x - xmin) * sx;
      const float v = (p.main_y - ymin) * sy;
      const int rad = std::max(1, static_cast<int>(std::lround(std::max(p.current_size, p.size) * sx * 0.15f)));
      const float a = std::clamp(p.a > 0.0f ? p.a : p.current_a, 0.05f, 1.0f);
      const float cr = std::clamp(p.r > 0.0f ? p.r : p.current_r, 0.0f, 1.0f);
      const float cg = std::clamp(p.g > 0.0f ? p.g : p.current_g, 0.0f, 1.0f);
      const float cb = std::clamp(p.b > 0.0f ? p.b : p.current_b, 0.0f, 1.0f);
      const int cx = static_cast<int>(std::lround(u));
      const int cy = static_cast<int>(std::lround(v));
      const bool skip_dead_body =
          p.remaining_lifetime <= 1e-5f &&
          (p.flags & wds::chart_render::kSplitEffectFlagTrails) != 0 &&
          p.trail_count < 2 && p.trail_points >= 2;
      if (!skip_dead_body) {
        for (int dy = -rad; dy <= rad; ++dy) {
          for (int dx = -rad; dx <= rad; ++dx) {
            if (dx * dx + dy * dy > rad * rad) {
              continue;
            }
            plot(cx + dx, cy + dy, cr, cg, cb, a);
          }
        }
      }
      const int tn = static_cast<int>(p.trail_points);
      if (tn >= 2 && !p.trail_x.empty() && !p.trail_y.empty()) {
        auto line = [&](int x0, int y0, int x1, int y1, float lr, float lg, float lb, float la) {
          const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
          if (steps <= 0) {
            plot(x0, y0, lr, lg, lb, la);
            return;
          }
          for (int s = 0; s <= steps; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            plot(x0 + static_cast<int>(std::lround((x1 - x0) * t)),
                 y0 + static_cast<int>(std::lround((y1 - y0) * t)), lr, lg, lb, la);
          }
        };
        int px = static_cast<int>(std::lround((p.trail_x[0] - xmin) * sx));
        int py = static_cast<int>(std::lround((p.trail_y[0] - ymin) * sy));
        for (int ti = 1; ti < tn && ti < static_cast<int>(p.trail_x.size()); ++ti) {
          const int qx = static_cast<int>(std::lround((p.trail_x[static_cast<size_t>(ti)] - xmin) * sx));
          const int qy = static_cast<int>(std::lround((p.trail_y[static_cast<size_t>(ti)] - ymin) * sy));
          const float u = static_cast<float>(ti) / static_cast<float>(tn - 1);
          const float ta = std::clamp(a * (0.35f + 0.55f * (1.0f - u)), 0.12f, 1.0f);
          line(px, py, qx, qy, cr, cg, cb, ta);
          px = qx;
          py = qy;
        }
      }
    }
  }
  FILE* f = std::fopen(out_path.c_str(), "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "failed to open %s\n", out_path.c_str());
    return 1;
  }
  std::fprintf(f, "P6\n%d %d\n255\n", width, height);
  std::vector<unsigned char> bytes(static_cast<size_t>(width * height * 3));
  for (size_t i = 0; i < bytes.size(); ++i) {
    const float v = std::clamp(rgb[i], 0.0f, 1.0f);
    bytes[i] = static_cast<unsigned char>(v * 255.0f + 0.5f);
  }
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  std::fclose(f);
  std::fprintf(stdout,
               "raster %s effect=%d n=%d trails=%d trail_len_sum=%.6f now_t=%.5f "
               "bounds=%.6f,%.6f,%.6f,%.6f\n",
               out_path.c_str(), effect_id, n, trail_n, trail_len_sum, now_t, xmin, xmax, ymin,
               ymax);
  return ok ? 0 : 1;
}

struct ParticleId {
  int32_t line_index = 0;
  uint32_t catalog_index = 0;
  uint32_t seed = 0;
  uint8_t element_index = 0;
  bool operator==(const ParticleId& o) const {
    return line_index == o.line_index && catalog_index == o.catalog_index && seed == o.seed &&
           element_index == o.element_index;
  }
};

struct ParticleIdHash {
  size_t operator()(const ParticleId& k) const {
    size_t h = std::hash<int32_t>{}(k.line_index);
    h ^= std::hash<uint32_t>{}(k.catalog_index) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(k.seed) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= std::hash<uint8_t>{}(k.element_index) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
  }
};

ParticleId particle_id_of(const SplitEffectLiveParticle& p) {
  ParticleId id;
  id.line_index = p.line_index;
  id.catalog_index = p.catalog_index;
  id.seed = p.seed;
  id.element_index = p.element_index;
  return id;
}

std::vector<SplitEffectLiveParticle> collect_schedule_live(int32_t effect_id, int32_t split_count,
                                                           double now_t, bool legacy, float hz) {
  wds::chart_render::shuriken::set_split_integrator_legacy_for_tests(legacy);
  wds::chart_render::shuriken::set_integration_hz_for_tests(hz);
  wds::chart_render::reset_split_effect_auto_seed_session();
  SplitEffectFrameCollect io;
  SplitEffectPlaybackState state;
  const std::vector<PreviewSplitLaneInstance> splits{
      make_split(effect_id, split_count, SplitLaneType::Full, kDefaultStartMs)};
  const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
  wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
  return flatten(io);
}

void restore_schedule_defaults() {
  wds::chart_render::shuriken::set_split_integrator_legacy_for_tests(false);
  wds::chart_render::shuriken::set_integration_hz_for_tests(0.0f);
}

struct ScreenPt {
  float x = 0.0f;
  float y = 0.0f;
};

ScreenPt trail_to_screen(float wx, float my, float z) {
  const auto ndc = wds::chart_editor::project_main_xyz(wx, my, z);
  ScreenPt p;
  p.x = (ndc.x * 0.5f + 0.5f) * 1920.0f;
  p.y = (ndc.y * 0.5f + 0.5f) * 1080.0f;
  return p;
}

std::vector<ScreenPt> project_particle_trail(const SplitEffectLiveParticle& p) {
  std::vector<ScreenPt> out;
  const int n = static_cast<int>(p.trail_points);
  out.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const size_t ii = static_cast<size_t>(i);
    if (ii >= p.trail_x.size() || ii >= p.trail_y.size() || ii >= p.trail_z.size()) {
      break;
    }
    out.push_back(trail_to_screen(p.trail_x[ii], p.trail_y[ii], p.trail_z[ii]));
  }
  return out;
}

float screen_seg_dist(const ScreenPt& p, const ScreenPt& a, const ScreenPt& b) {
  const float vx = b.x - a.x;
  const float vy = b.y - a.y;
  const float len2 = vx * vx + vy * vy;
  if (len2 <= 1e-12f) {
    return std::hypot(p.x - a.x, p.y - a.y);
  }
  float t = ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2;
  t = std::clamp(t, 0.0f, 1.0f);
  return std::hypot(p.x - (a.x + t * vx), p.y - (a.y + t * vy));
}

float directed_hausdorff(const std::vector<ScreenPt>& a, const std::vector<ScreenPt>& b) {
  if (a.empty() || b.empty()) {
    return a.empty() && b.empty() ? 0.0f : 1.0e9f;
  }
  float mx = 0.0f;
  for (const ScreenPt& p : a) {
    float best = std::hypot(p.x - b[0].x, p.y - b[0].y);
    for (size_t i = 0; i + 1 < b.size(); ++i) {
      best = std::min(best, screen_seg_dist(p, b[i], b[i + 1]));
    }
    mx = std::max(mx, best);
  }
  return mx;
}

float symmetric_hausdorff(const std::vector<ScreenPt>& a, const std::vector<ScreenPt>& b) {
  return std::max(directed_hausdorff(a, b), directed_hausdorff(b, a));
}

float polyline_length(const std::vector<ScreenPt>& pts) {
  float s = 0.0f;
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    s += std::hypot(pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y);
  }
  return s;
}

bool is_particle_trail(const SplitEffectLiveParticle& p) {
  return p.trail_points >= 2 && p.trail_count == 0;
}

float hypot3(float x, float y, float z) {
  return std::sqrt(x * x + y * y + z * z);
}

float polyline_length_3d(const SplitEffectLiveParticle& p) {
  float s = 0.0f;
  const int n = static_cast<int>(p.trail_points);
  for (int i = 0; i + 1 < n; ++i) {
    const size_t a = static_cast<size_t>(i);
    const size_t b = static_cast<size_t>(i + 1);
    if (b >= p.trail_x.size() || b >= p.trail_y.size() || b >= p.trail_z.size()) {
      break;
    }
    s += hypot3(p.trail_x[b] - p.trail_x[a], p.trail_y[b] - p.trail_y[a],
                p.trail_z[b] - p.trail_z[a]);
  }
  return s;
}

const SplitEffectLiveParticle* find_trail_id(const std::vector<SplitEffectLiveParticle>& ps,
                                             const ParticleId& id) {
  for (const auto& p : ps) {
    if (is_particle_trail(p) && particle_id_of(p) == id) {
      return &p;
    }
  }
  return nullptr;
}

float catalog_trail_min_d(int32_t effect_id, uint8_t element_index) {
  uint16_t n = 0;
  const SplitEffectParticle* ps = effect_catalog_particles(effect_id, &n);
  for (uint16_t i = 0; ps != nullptr && i < n; ++i) {
    if (ps[i].element_index == element_index) {
      return ps[i].trail_min_vertex;
    }
  }
  return 0.0f;
}

struct TrailOutlier {
  ParticleId id;
  int32_t effect = 0;
  int32_t split = 0;
  double now_t = 0.0;
  float haus = 0.0f;
  float len_rel = 0.0f;
  float head = 0.0f;
  float old_len = 0.0f;
  float new_len = 0.0f;
  uint16_t verts_l = 0;
  uint16_t verts_g = 0;
  float fd_l = 0.0f;
  float fd_g = 0.0f;
  float age = 0.0f;
};

std::vector<TrailOutlier> pick_worst_trail_outliers(const std::vector<TrailOutlier>& src) {
  std::vector<TrailOutlier> by_h = src;
  std::vector<TrailOutlier> by_l = src;
  std::sort(by_h.begin(), by_h.end(),
            [](const TrailOutlier& a, const TrailOutlier& b) { return a.haus > b.haus; });
  std::sort(by_l.begin(), by_l.end(),
            [](const TrailOutlier& a, const TrailOutlier& b) { return a.len_rel > b.len_rel; });
  std::vector<TrailOutlier> out;
  auto add = [&](const TrailOutlier& o) {
    for (const auto& e : out) {
      if (e.effect == o.effect && e.split == o.split && std::fabs(e.now_t - o.now_t) < 1e-9 &&
          e.id == o.id) {
        return;
      }
    }
    out.push_back(o);
  };
  for (size_t i = 0; i < by_h.size() && i < 10; ++i) {
    add(by_h[i]);
  }
  for (size_t i = 0; i < by_l.size() && i < 10; ++i) {
    add(by_l[i]);
  }
  return out;
}

void report_trail_outlier_refs(const std::vector<TrailOutlier>& all) {
  std::vector<int32_t> effects;
  for (const auto& o : all) {
    if (std::find(effects.begin(), effects.end(), o.effect) == effects.end()) {
      effects.push_back(o.effect);
    }
  }
  int n_a = 0;
  int n_b = 0;
  int n_c = 0;
  constexpr double kDt = 1.0 / 60.0;
  for (int32_t effect : effects) {
    std::vector<TrailOutlier> subset;
    for (const auto& o : all) {
      if (o.effect == effect) {
        subset.push_back(o);
      }
    }
    const auto worst = pick_worst_trail_outliers(subset);
    std::fprintf(stdout, "trail outliers effect=%d visible=%zu ranked=%zu\n", effect, subset.size(),
                 worst.size());
    struct FrameKey {
      int32_t effect = 0;
      int32_t split = 0;
      double now_t = 0.0;
      bool operator==(const FrameKey& o) const {
        return effect == o.effect && split == o.split && std::fabs(now_t - o.now_t) < 1e-9;
      }
    };
    struct FramePack {
      FrameKey key;
      std::vector<SplitEffectLiveParticle> legacy;
      std::vector<SplitEffectLiveParticle> grid;
      std::vector<SplitEffectLiveParticle> ref;
      std::vector<SplitEffectLiveParticle> legacy_m;
      std::vector<SplitEffectLiveParticle> legacy_p;
    };
    std::vector<FramePack> frames;
    auto load = [&](const FrameKey& key) -> FramePack& {
      for (auto& f : frames) {
        if (f.key == key) {
          return f;
        }
      }
      FramePack f;
      f.key = key;
      f.legacy = collect_schedule_live(key.effect, key.split, key.now_t, true, 0.0f);
      f.grid = collect_schedule_live(key.effect, key.split, key.now_t, false, 0.0f);
      f.ref = collect_schedule_live(key.effect, key.split, key.now_t, false, 960.0f);
      const double tm = std::max(0.0, key.now_t - kDt);
      const double tp = key.now_t + kDt;
      f.legacy_m = collect_schedule_live(key.effect, key.split, tm, true, 0.0f);
      f.legacy_p = collect_schedule_live(key.effect, key.split, tp, true, 0.0f);
      frames.push_back(std::move(f));
      return frames.back();
    };
    for (const auto& o : worst) {
      FrameKey key{o.effect, o.split, o.now_t};
      const FramePack& f = load(key);
      const auto* lp = find_trail_id(f.legacy, o.id);
      const auto* gp = find_trail_id(f.grid, o.id);
      const auto* rp = find_trail_id(f.ref, o.id);
      const auto* lm = find_trail_id(f.legacy_m, o.id);
      const auto* lpp = find_trail_id(f.legacy_p, o.id);
      if (lp == nullptr || gp == nullptr || rp == nullptr) {
        std::fprintf(stdout,
                     "  outl effect=%d split=%d now_t=%.2f seed=%u missing L/G/R=%d/%d/%d\n",
                     o.effect, o.split, o.now_t, o.id.seed, lp != nullptr, gp != nullptr,
                     rp != nullptr);
        continue;
      }
      const auto ls = project_particle_trail(*lp);
      const auto gs = project_particle_trail(*gp);
      const auto rs = project_particle_trail(*rp);
      const float haus_lref = symmetric_hausdorff(ls, rs);
      const float haus_gref = symmetric_hausdorff(gs, rs);
      const float len_l = polyline_length(ls);
      const float len_g = polyline_length(gs);
      const float len_r = polyline_length(rs);
      const float len_lref = len_r > 1e-3f ? std::fabs(len_l - len_r) / len_r : 0.0f;
      const float len_gref = len_r > 1e-3f ? std::fabs(len_g - len_r) / len_r : 0.0f;
      const float len3_l = polyline_length_3d(*lp);
      const float len3_g = polyline_length_3d(*gp);
      const float len3_r = polyline_length_3d(*rp);
      const float fd_r = hypot3(rp->vel_x, rp->vel_y, rp->vel_z);
      const float min_d = catalog_trail_min_d(o.effect, o.id.element_index);
      auto pred_n = [&](float fd) {
        if (min_d <= 1e-6f || fd <= 1e-4f) {
          return 3;
        }
        const float step = min_d / fd;
        const int n = static_cast<int>(std::floor(o.age / step)) + 1;
        return std::clamp(n, 3, 512);
      };
      const int pred_l = pred_n(o.fd_l);
      const int pred_g = pred_n(o.fd_g);
      const int pred_r = pred_n(fd_r);
      const uint16_t vm = lm != nullptr ? lm->trail_points : 0;
      const uint16_t vp = lpp != nullptr ? lpp->trail_points : 0;
      const bool flicker = (lm != nullptr && vm != lp->trail_points) ||
                           (lpp != nullptr && vp != lp->trail_points);
      char cls = 'c';
      if (haus_gref + 0.5f < haus_lref) {
        cls = 'a';
      } else if (haus_lref + 0.5f < haus_gref) {
        cls = 'b';
      } else {
        cls = 'c';
      }
      if (cls == 'a') {
        ++n_a;
      } else if (cls == 'b') {
        ++n_b;
      } else {
        ++n_c;
      }
      std::fprintf(stdout,
                   "  outl effect=%d split=%d now_t=%.2f seed=%u el=%u cls=%c "
                   "verts=L/G/R=%u/%u/%u pred=%d/%d/%d min_d=%.3f "
                   "haus_lg=%.3f haus_lref=%.3f haus_gref=%.3f "
                   "len_lg=%.3f/%.3f rel=%.3f len_lref=%.3f len_gref=%.3f "
                   "len3=%.3f/%.3f/%.3f fd=%.3f/%.3f/%.3f "
                   "flicker=%d verts_t= %u/%u/%u\n",
                   o.effect, o.split, o.now_t, o.id.seed, static_cast<unsigned>(o.id.element_index),
                   cls, lp->trail_points, gp->trail_points, rp->trail_points, pred_l, pred_g, pred_r,
                   static_cast<double>(min_d), static_cast<double>(o.haus),
                   static_cast<double>(haus_lref), static_cast<double>(haus_gref),
                   static_cast<double>(len_l), static_cast<double>(len_g),
                   static_cast<double>(o.len_rel), static_cast<double>(len_lref),
                   static_cast<double>(len_gref), static_cast<double>(len3_l),
                   static_cast<double>(len3_g), static_cast<double>(len3_r),
                   static_cast<double>(o.fd_l), static_cast<double>(o.fd_g),
                   static_cast<double>(fd_r), flicker ? 1 : 0, static_cast<unsigned>(vm),
                   static_cast<unsigned>(lp->trail_points), static_cast<unsigned>(vp));
    }
  }
  std::fprintf(stdout, "trail outlier class a/b/c=%d/%d/%d (a=grid closer to 960Hz, b=legacy closer)\n",
               n_a, n_b, n_c);
}

int cmd_schedule_compare(const std::string& mode, const std::vector<int32_t>& only_ids,
                         GoldenTier tier, int32_t split_count_filter, double now_t_filter,
                         bool analyze_outliers) {
  prehydrate_golden_controllers(kDefaultScenarioEffect);
  const bool want_trails = mode == "trails" || mode == "all";
  const bool want_rod = mode == "rod" || mode == "all";
  if (!want_trails && !want_rod) {
    std::fprintf(stderr, "schedule-compare --mode must be trails|rod|all\n");
    return 1;
  }

  std::vector<int32_t> trail_ids;
  std::vector<int32_t> rod_ids;
  const std::vector<int32_t> catalog_ids = official_split_effect_ids();
  auto keep_id = [&](int32_t id) {
    if (!only_ids.empty()) {
      return std::find(only_ids.begin(), only_ids.end(), id) != only_ids.end();
    }
    return quick_tier_includes_effect(tier, id);
  };
  for (int32_t id : catalog_ids) {
    if (!keep_id(id)) {
      continue;
    }
    if (want_trails && effect_any_particle(id, [](const SplitEffectParticle& p) {
          return (p.flags & kSplitEffectFlagTrails) != 0 && p.trail_mode == 0;
        })) {
      trail_ids.push_back(id);
    }
    if (want_rod) {
      rod_ids.push_back(id);
    }
  }
  if (want_trails && only_ids.empty() &&
      std::find(trail_ids.begin(), trail_ids.end(), 10660) == trail_ids.end()) {
    trail_ids.insert(trail_ids.begin(), 10660);
  }

  std::vector<int32_t> splits;
  if (split_count_filter > 0) {
    splits.push_back(split_count_filter);
  } else {
    splits.push_back(1);
    splits.push_back(6);
  }
  std::vector<double> times;
  if (now_t_filter >= 0.0) {
    times.push_back(now_t_filter);
  } else {
    times.assign(std::begin(kGoldenTimes), std::end(kGoldenTimes));
  }

  int trail_pairs = 0;
  int trail_visible = 0;
  int trail_len_fail = 0;
  double max_h = 0.0;
  double max_head = 0.0;
  double max_len_rel = 0.0;
  double sum_h = 0.0;
  double sum_fd_rel_legacy = 0.0;
  double sum_fd_rel_grid = 0.0;
  int fd_n = 0;
  std::vector<TrailOutlier> trail_outliers;

  if (want_trails) {
    std::fprintf(stdout, "schedule-compare trails effects=%zu splits=%zu times=%zu\n",
                 trail_ids.size(), splits.size(), times.size());
    for (int32_t id : trail_ids) {
      uint16_t pn = 0;
      const SplitEffectParticle* ps = effect_catalog_particles(id, &pn);
      for (uint16_t i = 0; ps != nullptr && i < pn; ++i) {
        const auto& p = ps[i];
        if ((p.flags & kSplitEffectFlagTrails) == 0 || p.trail_mode != 0) {
          continue;
        }
        std::fprintf(stdout,
                     "  catalog effect=%d el=%u flags=0x%x noise=%d min_d=%.3f life=%.3f "
                     "speed=%.3f vol=(%.2f,%.2f,%.2f) orb=(%.2f,%.2f,%.2f)\n",
                     id, static_cast<unsigned>(p.element_index), static_cast<unsigned>(p.flags),
                     (p.flags & kSplitEffectFlagNoise) != 0 ? 1 : 0, static_cast<double>(p.trail_min_vertex),
                     static_cast<double>(p.trail_lifetime), static_cast<double>(p.start_speed),
                     static_cast<double>(p.vel_x), static_cast<double>(p.vel_y),
                     static_cast<double>(p.vel_z), static_cast<double>(p.orbital_x),
                     static_cast<double>(p.orbital_y), static_cast<double>(p.orbital_z));
      }
    }
    for (int32_t id : trail_ids) {
      for (int32_t sc : splits) {
        for (double now_t : times) {
          const auto legacy = collect_schedule_live(id, sc, now_t, true, 0.0f);
          const auto grid = collect_schedule_live(id, sc, now_t, false, 0.0f);
          std::unordered_map<ParticleId, const SplitEffectLiveParticle*, ParticleIdHash> gmap;
          for (const auto& p : grid) {
            if (is_particle_trail(p)) {
              gmap[particle_id_of(p)] = &p;
            }
          }
          int sample_pairs = 0;
          double sample_max_h = 0.0;
          for (const auto& oldp : legacy) {
            if (!is_particle_trail(oldp)) {
              continue;
            }
            const auto it = gmap.find(particle_id_of(oldp));
            if (it == gmap.end()) {
              continue;
            }
            const auto& newp = *it->second;
            const auto old_s = project_particle_trail(oldp);
            const auto new_s = project_particle_trail(newp);
            const float h = symmetric_hausdorff(old_s, new_s);
            const float old_len = polyline_length(old_s);
            const float new_len = polyline_length(new_s);
            const float head = (old_s.empty() || new_s.empty())
                                   ? 0.0f
                                   : std::hypot(old_s[0].x - new_s[0].x, old_s[0].y - new_s[0].y);
            const float len_rel =
                old_len > 1e-3f ? std::fabs(new_len - old_len) / old_len : 0.0f;
            const float fd_old = hypot3(oldp.vel_x, oldp.vel_y, oldp.vel_z);
            const float fd_new = hypot3(newp.vel_x, newp.vel_y, newp.vel_z);
            const float an_old = hypot3(oldp.total_vx, oldp.total_vy, oldp.total_vz);
            const float an_new = hypot3(newp.total_vx, newp.total_vy, newp.total_vz);
            const float rel_old = std::fabs(fd_old - an_old) / std::max(an_old, 1e-3f);
            const float rel_new = std::fabs(fd_new - an_new) / std::max(an_new, 1e-3f);
            ++trail_pairs;
            ++sample_pairs;
            ++fd_n;
            sum_h += static_cast<double>(h);
            sum_fd_rel_legacy += static_cast<double>(rel_old);
            sum_fd_rel_grid += static_cast<double>(rel_new);
            max_h = std::max(max_h, static_cast<double>(h));
            max_head = std::max(max_head, static_cast<double>(head));
            max_len_rel = std::max(max_len_rel, static_cast<double>(len_rel));
            sample_max_h = std::max(sample_max_h, static_cast<double>(h));
            if (h > 1.0f || len_rel > 0.02f) {
              ++trail_visible;
              TrailOutlier o;
              o.id = particle_id_of(oldp);
              o.effect = id;
              o.split = sc;
              o.now_t = now_t;
              o.haus = h;
              o.len_rel = len_rel;
              o.head = head;
              o.old_len = old_len;
              o.new_len = new_len;
              o.verts_l = oldp.trail_points;
              o.verts_g = newp.trail_points;
              o.fd_l = fd_old;
              o.fd_g = fd_new;
              o.age = oldp.age;
              trail_outliers.push_back(o);
            }
            if (len_rel > 0.02f) {
              ++trail_len_fail;
            }
            if (!analyze_outliers &&
                (h > 1.0f || len_rel > 0.02f || (id == 10660 && sample_pairs <= 3))) {
              const float pose = std::hypot(
                  oldp.world_x - newp.world_x,
                  std::hypot(oldp.main_y - newp.main_y, oldp.z - newp.z));
              const auto mid_delta = [&](const std::vector<ScreenPt>& a,
                                         const std::vector<ScreenPt>& b, float u) {
                if (a.empty() || b.empty()) {
                  return 0.0f;
                }
                const size_t ia = static_cast<size_t>(
                    std::clamp(u, 0.0f, 1.0f) * static_cast<float>(a.size() - 1));
                const size_t ib = static_cast<size_t>(
                    std::clamp(u, 0.0f, 1.0f) * static_cast<float>(b.size() - 1));
                return std::hypot(a[ia].x - b[ib].x, a[ia].y - b[ib].y);
              };
              const float mid = mid_delta(old_s, new_s, 0.5f);
              const float tail = mid_delta(old_s, new_s, 1.0f);
              const float step_old =
                  oldp.trail_points > 1
                      ? oldp.age / static_cast<float>(oldp.trail_points - 1)
                      : 0.0f;
              const float step_new =
                  newp.trail_points > 1
                      ? newp.age / static_cast<float>(newp.trail_points - 1)
                      : 0.0f;
              std::fprintf(stdout,
                           "  trail effect=%d split=%d now_t=%.2f seed=%u verts=%u/%u "
                           "len=%.2f/%.2f haus=%.3f head=%.3f fd=%.3f/%.3f an=%.3f/%.3f "
                           "fd_rel=%.3f/%.3f age=%.3f/%.3f birth=%.3f/%.3f pose=%.5f "
                           "step=%.4f/%.4f mid=%.2f tail=%.2f\n",
                           id, sc, now_t, oldp.seed, oldp.trail_points, newp.trail_points,
                           static_cast<double>(old_len), static_cast<double>(new_len),
                           static_cast<double>(h), static_cast<double>(head),
                           static_cast<double>(fd_old), static_cast<double>(fd_new),
                           static_cast<double>(an_old), static_cast<double>(an_new),
                           static_cast<double>(rel_old), static_cast<double>(rel_new),
                           static_cast<double>(oldp.age), static_cast<double>(newp.age),
                           static_cast<double>(oldp.birth_t), static_cast<double>(newp.birth_t),
                           static_cast<double>(pose), static_cast<double>(step_old),
                           static_cast<double>(step_new), static_cast<double>(mid),
                           static_cast<double>(tail));
              if (h > 15.0f && !oldp.trail_x.empty() && !newp.trail_x.empty()) {
                const size_t ot = oldp.trail_x.size() - 1;
                const size_t nt = newp.trail_x.size() - 1;
                std::fprintf(stdout,
                             "    ends head3d=%.3f,%.3f,%.3f / %.3f,%.3f,%.3f "
                             "tail3d=%.3f,%.3f,%.3f / %.3f,%.3f,%.3f\n",
                             static_cast<double>(oldp.trail_x[0]), static_cast<double>(oldp.trail_y[0]),
                             static_cast<double>(oldp.trail_z[0]), static_cast<double>(newp.trail_x[0]),
                             static_cast<double>(newp.trail_y[0]), static_cast<double>(newp.trail_z[0]),
                             static_cast<double>(oldp.trail_x[ot]), static_cast<double>(oldp.trail_y[ot]),
                             static_cast<double>(oldp.trail_z[ot]), static_cast<double>(newp.trail_x[nt]),
                             static_cast<double>(newp.trail_y[nt]), static_cast<double>(newp.trail_z[nt]));
              }
            }
          }
          if (sample_pairs == 0 && id == 10660) {
            std::fprintf(stdout, "  trail effect=%d split=%d now_t=%.2f pairs=0 legacy_n=%zu grid_n=%zu\n",
                         id, sc, now_t, legacy.size(), grid.size());
          }
          (void)sample_max_h;
        }
      }
    }
    const double mean_h = trail_pairs > 0 ? sum_h / static_cast<double>(trail_pairs) : 0.0;
    const double mean_fd_old = fd_n > 0 ? sum_fd_rel_legacy / static_cast<double>(fd_n) : 0.0;
    const double mean_fd_new = fd_n > 0 ? sum_fd_rel_grid / static_cast<double>(fd_n) : 0.0;
    std::fprintf(stdout,
                 "trails summary pairs=%d visible_gt_1px_or_2pct=%d len_gt_2pct=%d "
                 "max_haus_px=%.3f mean_haus_px=%.3f max_head_px=%.3f max_len_rel=%.4f "
                 "mean_fd_rel_legacy=%.4f mean_fd_rel_grid=%.4f\n",
                 trail_pairs, trail_visible, trail_len_fail, max_h, mean_h, max_head, max_len_rel,
                 mean_fd_old, mean_fd_new);
    if (analyze_outliers && !trail_outliers.empty()) {
      report_trail_outlier_refs(trail_outliers);
    }
  }

  int rod_samples = 0;
  int rod_mismatch = 0;
  int closer_grid = 0;
  int closer_legacy = 0;
  int tie_ref = 0;
  if (want_rod) {
    std::fprintf(stdout, "schedule-compare rod effects=%zu splits=%zu times=%zu\n", rod_ids.size(),
                 splits.size(), times.size());
    for (int32_t id : rod_ids) {
      for (int32_t sc : splits) {
        for (double now_t : times) {
          const auto legacy = collect_schedule_live(id, sc, now_t, true, 0.0f);
          const auto grid = collect_schedule_live(id, sc, now_t, false, 0.0f);
          ++rod_samples;
          if (static_cast<int>(legacy.size()) == static_cast<int>(grid.size())) {
            continue;
          }
          ++rod_mismatch;
          const auto ref = collect_schedule_live(id, sc, now_t, false, 960.0f);
          std::unordered_set<ParticleId, ParticleIdHash> lset;
          std::unordered_set<ParticleId, ParticleIdHash> gset;
          std::unordered_map<ParticleId, const SplitEffectLiveParticle*, ParticleIdHash> lmap;
          std::unordered_map<ParticleId, const SplitEffectLiveParticle*, ParticleIdHash> gmap;
          std::unordered_map<ParticleId, const SplitEffectLiveParticle*, ParticleIdHash> rmap;
          for (const auto& p : legacy) {
            lset.insert(particle_id_of(p));
            lmap[particle_id_of(p)] = &p;
          }
          for (const auto& p : grid) {
            gset.insert(particle_id_of(p));
            gmap[particle_id_of(p)] = &p;
          }
          for (const auto& p : ref) {
            rmap[particle_id_of(p)] = &p;
          }
          int only_legacy = 0;
          int only_grid = 0;
          double only_size = 0.0;
          double only_alpha = 0.0;
          int only_add = 0;
          int only_n = 0;
          for (const auto& p : legacy) {
            if (gset.count(particle_id_of(p)) == 0) {
              ++only_legacy;
              only_size += static_cast<double>(p.size);
              only_alpha += static_cast<double>(p.a);
              only_add += (p.blend == static_cast<uint8_t>(SplitEffectBlend::Additive)) ? 1 : 0;
              ++only_n;
            }
          }
          for (const auto& p : grid) {
            if (lset.count(particle_id_of(p)) == 0) {
              ++only_grid;
              only_size += static_cast<double>(p.size);
              only_alpha += static_cast<double>(p.a);
              only_add += (p.blend == static_cast<uint8_t>(SplitEffectBlend::Additive)) ? 1 : 0;
              ++only_n;
            }
          }
          const int d_legacy = std::abs(static_cast<int>(legacy.size()) - static_cast<int>(ref.size()));
          const int d_grid = std::abs(static_cast<int>(grid.size()) - static_cast<int>(ref.size()));
          if (d_grid < d_legacy) {
            ++closer_grid;
          } else if (d_legacy < d_grid) {
            ++closer_legacy;
          } else {
            ++tie_ref;
          }
          double pose_legacy = 0.0;
          double pose_grid = 0.0;
          int pose_n = 0;
          for (const auto& kv : rmap) {
            const auto li = lmap.find(kv.first);
            const auto gi = gmap.find(kv.first);
            if (li == lmap.end() || gi == gmap.end()) {
              continue;
            }
            const auto& rp = *kv.second;
            const auto& lp = *li->second;
            const auto& gp = *gi->second;
            pose_legacy += std::hypot(lp.world_x - rp.world_x,
                                      std::hypot(lp.main_y - rp.main_y, lp.z - rp.z));
            pose_grid += std::hypot(gp.world_x - rp.world_x,
                                    std::hypot(gp.main_y - rp.main_y, gp.z - rp.z));
            ++pose_n;
          }
          const double mean_only_size = only_n > 0 ? only_size / static_cast<double>(only_n) : 0.0;
          const double mean_only_a = only_n > 0 ? only_alpha / static_cast<double>(only_n) : 0.0;
          const double add_frac = only_n > 0 ? static_cast<double>(only_add) / static_cast<double>(only_n)
                                             : 0.0;
          auto child_n = [](const std::vector<SplitEffectLiveParticle>& ps) {
            int n = 0;
            for (const auto& p : ps) {
              if (p.inherit_props != 0) {
                ++n;
              }
            }
            return n;
          };
          const int ch_l = child_n(legacy);
          const int ch_g = child_n(grid);
          const int ch_r = child_n(ref);
          const int ch_max = std::max(ch_l, ch_g);
          const double child_frac =
              ch_max > 0 ? static_cast<double>(std::abs(ch_g - ch_l)) / static_cast<double>(ch_max)
                         : 0.0;
          std::fprintf(stdout,
                       "  rod effect=%d split=%d now_t=%.2f n_legacy/grid/ref=%zu/%zu/%zu "
                       "delta=%+d child_l/g/r=%d/%d/%d child_frac=%.4f only_l/g=%d/%d "
                       "d_ref l/g=%d/%d "
                       "pose_err l/g=%.5f/%.5f only_size=%.3f only_a=%.3f additive=%.2f\n",
                       id, sc, now_t, legacy.size(), grid.size(), ref.size(),
                       static_cast<int>(grid.size()) - static_cast<int>(legacy.size()), ch_l, ch_g,
                       ch_r, child_frac, only_legacy, only_grid, d_legacy, d_grid,
                       pose_n > 0 ? pose_legacy / static_cast<double>(pose_n) : 0.0,
                       pose_n > 0 ? pose_grid / static_cast<double>(pose_n) : 0.0, mean_only_size,
                       mean_only_a, add_frac);
        }
      }
    }
    std::fprintf(stdout,
                 "rod summary samples=%d count_mismatch=%d closer_to_960hz grid/legacy/tie=%d/%d/%d\n",
                 rod_samples, rod_mismatch, closer_grid, closer_legacy, tie_ref);
  }

  restore_schedule_defaults();
  return 0;
}

std::vector<double> make_carry_check_times(uint32_t seed) {
  std::vector<double> times;
  times.reserve(28);
  double now = 0.10;
  for (int i = 0; i < 12; ++i) {
    times.push_back(now);
    now += 1.0 / 60.0;
  }
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dt(1.0 / 240.0, 0.25);
  for (int i = 0; i < 8; ++i) {
    now += dt(rng);
    if (now > 4.5) {
      now = 0.35 + dt(rng) * 0.15;
    }
    times.push_back(now);
  }
  times.push_back(std::max(0.05, times.back() - 0.12));
  times.push_back(std::max(0.02, times.back() - 0.04));
  times.push_back(2.0);
  times.push_back(0.35);
  return times;
}

uint64_t hash_frame_collect(const SplitEffectFrameCollect& io) {
  std::vector<uint8_t> buf;
  BufferWriter w{buf};
  w.write_i32(io.total);
  for (int i = 0; i < io.total; ++i) {
    if (!write_particle(w, io.pool[static_cast<size_t>(i)])) {
      return 0;
    }
  }
  return fnv1a64_buffer(buf);
}

int cmd_carry_check(const std::vector<int32_t>& only_ids, bool quick) {
  std::vector<int32_t> ids = only_ids.empty() ? wds::chart_render::official_split_effect_ids()
                                              : only_ids;
  if (quick && only_ids.empty()) {
    ids.assign(std::begin(kQuickAlwaysEffects), std::end(kQuickAlwaysEffects));
  }
  const int32_t split_counts[] = {1, 6};
  struct InstSpec {
    int instances;
    double stagger;
  };
  const InstSpec insts[] = {{1, 0.0}, {4, 0.0}, {4, 0.2}};
  int sequences = 0;
  int frames = 0;
  int mismatches = 0;
  std::unordered_set<int32_t> mismatch_ids;
  const auto t0 = Clock::now();
  for (int32_t id : ids) {
    if (wds::chart_render::official_split_effect(id) == nullptr) {
      continue;
    }
    for (int32_t sc : split_counts) {
      for (const InstSpec& spec : insts) {
        const auto splits = make_instances(id, spec.instances, spec.stagger, sc, SplitLaneType::Full);
        const uint32_t seed = static_cast<uint32_t>(id) * 1009u + static_cast<uint32_t>(sc) * 17u +
                              static_cast<uint32_t>(spec.instances) * 13u +
                              static_cast<uint32_t>(spec.stagger * 10.0);
        const std::vector<double> times = make_carry_check_times(seed);
        std::vector<uint64_t> stateless;
        std::vector<int> stateless_n;
        stateless.reserve(times.size());
        wds::chart_render::split_effect_set_eval_persist_enabled(0);
        wds::chart_render::split_effect_set_eval_carry_enabled(0);
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        SplitEffectFrameCollect io;
        SplitEffectFrameCollect io_stateless;
        SplitEffectPlaybackState state;
        for (double now_t : times) {
          const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
          wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
          stateless.push_back(hash_frame_collect(io));
          stateless_n.push_back(io.total);
        }
        wds::chart_render::split_effect_set_eval_persist_enabled(0);
        wds::chart_render::split_effect_set_eval_carry_enabled(1);
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        state.reset();
        for (size_t i = 0; i < times.size(); ++i) {
          const double now_sec = now_sec_from_now_t(kDefaultStartMs, times[i]);
          wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
          const uint64_t h = hash_frame_collect(io);
          ++frames;
          if (h != stateless[i]) {
            ++mismatches;
            if (mismatch_ids.insert(id).second) {
              std::fprintf(stderr,
                           "carry-check first-fail effect=%d split=%d inst=%d stagger=%.2f i=%zu "
                           "now_t=%.5f n=%d/%d unique=%zu\n",
                           id, sc, spec.instances, spec.stagger, i, times[i], io.total,
                           stateless_n[i], mismatch_ids.size());
            }
            if (mismatches <= 2) {
              auto replay = [&](int carry_on, SplitEffectFrameCollect& dest) {
                wds::chart_render::split_effect_set_eval_persist_enabled(0);
                wds::chart_render::split_effect_set_eval_carry_enabled(carry_on);
                wds::chart_render::reset_split_effect_auto_seed_session();
                wds::chart_render::split_effect_clear_caches();
                SplitEffectPlaybackState st2;
                for (size_t j = 0; j <= i; ++j) {
                  const double s = now_sec_from_now_t(kDefaultStartMs, times[j]);
                  wds::chart_render::collect_split_effect_frame(splits, 0, s, false, st2, dest);
                }
              };
              replay(0, io_stateless);
              replay(1, io);
              wds::chart_render::split_effect_set_eval_carry_enabled(1);
              std::fprintf(stderr,
                           "carry-check mismatch effect=%d split=%d inst=%d stagger=%.2f i=%zu "
                           "now_t=%.5f n=%d/%d\n",
                           id, sc, spec.instances, spec.stagger, i, times[i], io.total,
                           stateless_n[i]);
              const int n = std::min(io.total, io_stateless.total);
              for (int pi = 0; pi < n; ++pi) {
                const auto& a = io.pool[static_cast<size_t>(pi)];
                const auto& b = io_stateless.pool[static_cast<size_t>(pi)];
                std::vector<uint8_t> ba;
                std::vector<uint8_t> bb;
                BufferWriter wa{ba};
                BufferWriter wb{bb};
                write_particle(wa, a);
                write_particle(wb, b);
                if (ba != bb) {
                  std::fprintf(stderr, "  first i=%d cat=%u seed=%u groups", pi, a.catalog_index,
                               a.seed);
                  for (int g = 0; g < kGoldenHashGroupCount; ++g) {
                    std::vector<uint8_t> ga;
                    std::vector<uint8_t> gb;
                    BufferWriter wga{ga};
                    BufferWriter wgb{gb};
                    write_particle_group(wga, a, g);
                    write_particle_group(wgb, b, g);
                    if (ga != gb) {
                      std::fprintf(stderr, " %s", kGoldenHashGroupNames[g]);
                      const size_t n = std::min(ga.size(), gb.size());
                      for (size_t bi = 0; bi < n; ++bi) {
                        if (ga[bi] != gb[bi]) {
                          std::fprintf(stderr, "[off=%zu %02x/%02x]", bi, ga[bi], gb[bi]);
                          break;
                        }
                      }
                      if (ga.size() != gb.size()) {
                        std::fprintf(stderr, "[len=%zu/%zu]", ga.size(), gb.size());
                      }
                    }
                  }
                  auto bits = [](float x) {
                    uint32_t u = 0;
                    std::memcpy(&u, &x, 4);
                    return u;
                  };
                  std::fprintf(stderr,
                               " flip=%u/%u inh=%u/%u "
                               "bpz=%.8g/%x vs %.8g/%x "
                               "boy=%.8g/%x vs %.8g/%x "
                               "edist=%.8g/%x vs %.8g/%x "
                               "piv=%.8g,%.8g,%.8g vs %.8g,%.8g,%.8g "
                               "wing=%.8g/%x vs %.8g/%x "
                               "wx=%.8g/%x vs %.8g/%x\n",
                               a.flip_u, b.flip_u, a.inherit_props, b.inherit_props,
                               a.birth_parent_z, bits(a.birth_parent_z), b.birth_parent_z,
                               bits(b.birth_parent_z), a.birth_origin_y_delta,
                               bits(a.birth_origin_y_delta), b.birth_origin_y_delta,
                               bits(b.birth_origin_y_delta), a.effective_distance,
                               bits(a.effective_distance), b.effective_distance,
                               bits(b.effective_distance), a.pivot_x, a.pivot_y, a.pivot_z,
                               b.pivot_x, b.pivot_y, b.pivot_z, a.butterfly_wing_x,
                               bits(a.butterfly_wing_x), b.butterfly_wing_x,
                               bits(b.butterfly_wing_x), a.world_x, bits(a.world_x), b.world_x,
                               bits(b.world_x));
                  break;
                }
              }
              if (io.total != io_stateless.total) {
                std::fprintf(stderr, "  count carry=%d stateless=%d\n", io.total,
                             io_stateless.total);
              }
            }
          }
        }
        ++sequences;
      }
    }
    if ((sequences % 24) == 0) {
      std::fprintf(stdout, "  carry-check progress sequences=%d effect=%d mismatches=%d\n",
                   sequences, id, mismatches);
      std::fflush(stdout);
    }
  }
  wds::chart_render::split_effect_set_eval_carry_enabled(-1);
  wds::chart_render::split_effect_set_eval_persist_enabled(-1);
  const auto cache = wds::chart_render::split_effect_eval_cache_stats();
  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  if (!mismatch_ids.empty()) {
    std::fprintf(stdout, "carry-check unique-fail ids=");
    bool first = true;
    std::vector<int32_t> ids(mismatch_ids.begin(), mismatch_ids.end());
    std::sort(ids.begin(), ids.end());
    for (int32_t fail_id : ids) {
      std::fprintf(stdout, "%s%d", first ? "" : ",", fail_id);
      first = false;
    }
    std::fprintf(stdout, " count=%zu\n", ids.size());
  }
  std::fprintf(stdout,
               "carry-check sequences=%d frames=%d mismatches=%d wall=%.2fs  carry used=%.2fMB "
               "systems=%zu particles=%zu hit/miss/fallback=%llu/%llu/%llu\n",
               sequences, frames, mismatches, sec,
               static_cast<double>(cache.carry_bytes_used) / (1024.0 * 1024.0), cache.carry_systems,
               cache.carry_particles,                static_cast<unsigned long long>(cache.carry_hits),
               static_cast<unsigned long long>(cache.carry_misses),
               static_cast<unsigned long long>(cache.carry_fallbacks));
  return mismatches == 0 ? 0 : 1;
}

struct PersistSeq {
  const char* name = "";
  std::vector<double> times;
  int restart_at = -1;
};

std::vector<PersistSeq> make_persist_sequences() {
  std::vector<PersistSeq> out;
  PersistSeq s60;
  s60.name = "dt1/60";
  s60.times.reserve(24);
  for (int i = 0; i < 24; ++i) {
    s60.times.push_back(0.10 + static_cast<double>(i) / 60.0);
  }
  out.push_back(std::move(s60));
  PersistSeq s30;
  s30.name = "dt1/30";
  for (int i = 0; i < 12; ++i) {
    s30.times.push_back(0.10 + static_cast<double>(i) / 30.0);
  }
  out.push_back(std::move(s30));
  out.push_back(PersistSeq{"gap0.25", {0.10, 0.35, 0.60, 0.85, 1.10}, -1});
  out.push_back(PersistSeq{"gap0.50", {0.10, 0.60, 1.10, 1.60}, -1});
  out.push_back(PersistSeq{"backward", {0.10, 0.30, 0.50, 0.20, 0.40}, -1});
  // fade ends at 1 + 1/60. Cross the boundary; seek into mid fade-in.
  out.push_back(PersistSeq{"cross-fade-end", {0.90, 1.00, 1.05, 1.10, 1.20}, -1});
  out.push_back(PersistSeq{"seek-fade-mid", {0.10, 0.40, 0.70}, -1});
  out.push_back(PersistSeq{"seek-into-fade", {1.50, 0.50, 0.60}, -1});
  PersistSeq restart;
  restart.name = "restart";
  restart.times = {0.10, 0.20, 0.30, 0.10, 0.20};
  restart.restart_at = 3;
  out.push_back(std::move(restart));
  return out;
}

int cmd_persist_check(const std::vector<int32_t>& only_ids, bool quick) {
  std::vector<int32_t> ids = only_ids.empty() ? wds::chart_render::official_split_effect_ids()
                                              : only_ids;
  if (quick && only_ids.empty()) {
    ids.assign(std::begin(kQuickAlwaysEffects), std::end(kQuickAlwaysEffects));
  }
  const int32_t split_counts[] = {1, 6};
  const auto seqs = make_persist_sequences();
  int sequences = 0;
  int frames = 0;
  int mismatches = 0;
  std::unordered_set<int32_t> mismatch_ids;
  const auto t0 = Clock::now();
  for (int32_t id : ids) {
    if (wds::chart_render::official_split_effect(id) == nullptr) {
      continue;
    }
    for (int32_t sc : split_counts) {
      const auto splits = make_instances(id, 1, 0.0, sc, SplitLaneType::Full);
      for (const PersistSeq& seq : seqs) {
        std::vector<uint64_t> remake_h;
        std::vector<int> remake_n;
        remake_h.reserve(seq.times.size());
        wds::chart_render::split_effect_set_eval_persist_enabled(0);
        wds::chart_render::split_effect_set_eval_carry_enabled(1);
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        SplitEffectFrameCollect io;
        SplitEffectPlaybackState state;
        for (size_t i = 0; i < seq.times.size(); ++i) {
          if (seq.restart_at >= 0 && static_cast<int>(i) == seq.restart_at) {
            wds::chart_render::reset_split_effect_auto_seed_session();
            wds::chart_render::split_effect_clear_caches();
            state.reset();
          }
          const double now_sec = now_sec_from_now_t(kDefaultStartMs, seq.times[i]);
          wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
          remake_h.push_back(hash_frame_collect(io));
          remake_n.push_back(io.total);
        }
        wds::chart_render::split_effect_set_eval_persist_enabled(1);
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        state.reset();
        SplitEffectFrameCollect io_p;
        for (size_t i = 0; i < seq.times.size(); ++i) {
          if (seq.restart_at >= 0 && static_cast<int>(i) == seq.restart_at) {
            wds::chart_render::reset_split_effect_auto_seed_session();
            wds::chart_render::split_effect_clear_caches();
            state.reset();
          }
          const double now_sec = now_sec_from_now_t(kDefaultStartMs, seq.times[i]);
          wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io_p);
          const uint64_t h = hash_frame_collect(io_p);
          ++frames;
          if (h != remake_h[i]) {
            ++mismatches;
            if (mismatch_ids.insert(id).second) {
              std::fprintf(stderr,
                           "persist-check first-fail effect=%d split=%d seq=%s i=%zu now_t=%.5f "
                           "n=%d/%d unique=%zu\n",
                           id, sc, seq.name, i, seq.times[i], io_p.total, remake_n[i],
                           mismatch_ids.size());
            }
          }
        }
        ++sequences;
      }
    }
    if ((sequences % 24) == 0) {
      std::fprintf(stdout, "  persist-check progress sequences=%d effect=%d mismatches=%d\n",
                   sequences, id, mismatches);
      std::fflush(stdout);
    }
  }
  wds::chart_render::split_effect_set_eval_persist_enabled(-1);
  wds::chart_render::split_effect_set_eval_carry_enabled(-1);
  const auto cache = wds::chart_render::split_effect_eval_cache_stats();
  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  if (!mismatch_ids.empty()) {
    std::fprintf(stdout, "persist-check unique-fail ids=");
    bool first = true;
    std::vector<int32_t> fails(mismatch_ids.begin(), mismatch_ids.end());
    std::sort(fails.begin(), fails.end());
    for (int32_t fail_id : fails) {
      std::fprintf(stdout, "%s%d", first ? "" : ",", fail_id);
      first = false;
    }
    std::fprintf(stdout, " count=%zu\n", fails.size());
  }
  std::fprintf(stdout,
               "persist-check sequences=%d frames=%d mismatches=%d wall=%.2fs  persist "
               "used=%.2fMB systems=%zu particles=%zu hit/miss/rebuild=%llu/%llu/%llu\n",
               sequences, frames, mismatches, sec,
               static_cast<double>(cache.persist_bytes_used) / (1024.0 * 1024.0),
               cache.persist_systems, cache.persist_particles,
               static_cast<unsigned long long>(cache.persist_hits),
               static_cast<unsigned long long>(cache.persist_misses),
               static_cast<unsigned long long>(cache.persist_rebuilds));
  return mismatches == 0 ? 0 : 1;
}

void print_persist_fade_frac(int32_t effect_id, int frames) {
  const double fade_end = static_cast<double>(wds::chart_render::shuriken::kSplitEffectFadeEndPlayT);
  auto dump = [&](const char* window, int fade_in, int post) {
    const int n = fade_in + post;
    if (n <= 0) {
      std::fprintf(stdout, "persist-fade-frac effect=%d window=%s frames=0 fade_end=%.5f\n",
                   effect_id, window, fade_end);
      return;
    }
    std::fprintf(stdout,
                 "persist-fade-frac effect=%d window=%s frames=%d fade_end=%.5f "
                 "fade_in=%d (%.1f%%) post_fade=%d (%.1f%%) persist_child_eligible=post_fade\n",
                 effect_id, window, n, fade_end, fade_in, 100.0 * fade_in / n, post,
                 100.0 * post / n);
  };
  {
    int fade_in = 0;
    int post = 0;
    for (int i = 0; i < frames; ++i) {
      const double now_t = static_cast<double>(i) / 60.0;
      if (now_t + 1e-9 < fade_end) {
        ++fade_in;
      } else {
        ++post;
      }
    }
    dump("from-t0", fade_in, post);
  }
  if (wds::chart_render::official_split_effect(effect_id) == nullptr) {
    return;
  }
  const auto splits = make_instances(effect_id, 2, 0.0, 6, SplitLaneType::Full);
  double range_lo = 0.0;
  double range_hi = 0.0;
  choose_peak_range(splits, 1, frames, range_lo, range_hi);
  const double origin = play_origin_sec(splits.empty() ? kDefaultStartMs : splits.front().start_ms);
  int fade_in = 0;
  int post = 0;
  for (int i = 0; i < frames; ++i) {
    const double now_sec = range_lo + static_cast<double>(i) / 60.0;
    if (now_sec > range_hi + 1e-12 && i > 0) {
      break;
    }
    const double now_t = now_sec - origin;
    if (now_t + 1e-9 < fade_end) {
      ++fade_in;
    } else {
      ++post;
    }
  }
  dump("peak-play-q1-2xFull6", fade_in, post);
}

void print_persist_child_classes(int32_t effect_id) {
  const auto* entry = wds::chart_render::official_split_effect(effect_id);
  if (entry == nullptr) {
    return;
  }
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  if (ps == nullptr) {
    return;
  }
  std::fprintf(stdout, "persist-phase-class effect=%d systems=%u\n", effect_id,
               static_cast<unsigned>(n));
  for (uint16_t i = 0; i < n; ++i) {
    const auto& p = ps[i];
    uint16_t sub_n = 0;
    const auto* subs = wds::chart_render::official_split_effect_subemitters_for_parent(
        static_cast<uint16_t>(entry->particle_begin + i), &sub_n);
    const bool is_child = [&] {
      const uint16_t abs_i = static_cast<uint16_t>(entry->particle_begin + i);
      for (uint16_t s = 0; s < wds::chart_render::official_split_effect_subemitter_count(); ++s) {
        const auto* sub = wds::chart_render::official_split_effect_subemitter_at(s);
        if (sub != nullptr && sub->child_index == abs_i) {
          return true;
        }
      }
      return false;
    }();
    if (!is_child && sub_n == 0) {
      continue;
    }
    const char* space = p.simulation_space == 0 ? "Local" : (p.simulation_space == 1 ? "World" : "Custom");
    const char* emit = p.emission_kind == 3 ? "RoD" : (p.emission_kind == 2 ? "RoT" : (p.emission_kind == 1 ? "Burst" : "None"));
    std::fprintf(stdout,
                 "  sys=%u child=%d parent_slots=%u space=%s emit=%s rod=%.4f noise=%d force=%d "
                 "force_world=%d inherit=%d inh_mode=%d vel=%d gravity=%.4f trails=%d\n",
                 static_cast<unsigned>(i), is_child ? 1 : 0, static_cast<unsigned>(sub_n), space,
                 emit, p.rate_over_distance, (p.flags & wds::chart_render::kSplitEffectFlagNoise) != 0,
                 p.force_enabled, p.force_in_world, p.inherit_enabled, p.inherit_mode,
                 (p.flags & wds::chart_render::kSplitEffectFlagVelocity) != 0, p.gravity_modifier,
                 (p.flags & wds::chart_render::kSplitEffectFlagTrails) != 0);
    (void)subs;
  }
}

int cmd_persist_phase(const std::vector<int32_t>& ids, int quality, int frames, int persist_mode) {
  const double fade_end = static_cast<double>(wds::chart_render::shuriken::kSplitEffectFadeEndPlayT);
  std::vector<int> persist_passes;
  if (persist_mode == 0 || persist_mode == 1) {
    persist_passes.push_back(persist_mode);
  } else {
    persist_passes.push_back(1);
    persist_passes.push_back(0);
  }
  for (int32_t id : ids) {
    if (wds::chart_render::official_split_effect(id) == nullptr) {
      continue;
    }
    print_persist_child_classes(id);
    const auto splits = make_instances(id, 2, 0.0, 6, SplitLaneType::Full);
    double range_lo = 0.0;
    double range_hi = 0.0;
    choose_peak_range(splits, quality, frames, range_lo, range_hi);
    const double origin = play_origin_sec(splits.empty() ? kDefaultStartMs : splits.front().start_ms);
    for (int persist_on : persist_passes) {
      wds::chart_render::split_effect_set_eval_persist_enabled(persist_on);
      wds::chart_render::reset_split_effect_auto_seed_session();
      wds::chart_render::split_effect_clear_caches();
      wds::chart_render::split_wall_reset();
      SplitEffectFrameCollect io;
      SplitEffectPlaybackState state;
      MsStats fade_ms;
      MsStats post_ms;
      MsStats all_ms;
      ParticleStats fade_ps;
      ParticleStats post_ps;
      double now_sec = range_lo;
      for (int i = 0; i < frames; ++i) {
        const double now_t = now_sec - origin;
        const double t = collect_ms(splits, quality, now_sec, true, state, io);
        all_ms.add(t);
        if (now_t + 1e-9 < fade_end) {
          fade_ms.add(t);
          fade_ps.add(io.total);
        } else {
          post_ms.add(t);
          post_ps.add(io.total);
        }
        now_sec = std::min(range_hi, now_sec + 1.0 / 60.0);
      }
      const std::string wall = wds::chart_render::split_wall_report();
      std::fprintf(stdout,
                   "persist-phase effect=%d persist=%d quality=%d frames=%d range_t=[%.4f,%.4f] "
                   "fade_end=%.5f fade_n=%zu post_n=%zu\n",
                   id, persist_on, quality, frames, range_lo - origin, range_hi - origin, fade_end,
                   fade_ms.samples.size(), post_ms.samples.size());
      std::fprintf(stdout,
                   "  fade-in  n=%zu particles avg=%.1f max=%d  collect_ms avg=%.3f p50=%.3f "
                   "p95=%.3f max=%.3f\n",
                   fade_ms.samples.size(), fade_ps.avg(), fade_ps.max, fade_ms.avg(),
                   fade_ms.pct(0.50), fade_ms.pct(0.95), fade_ms.max());
      std::fprintf(stdout,
                   "  post-fade n=%zu particles avg=%.1f max=%d  collect_ms avg=%.3f p50=%.3f "
                   "p95=%.3f max=%.3f\n",
                   post_ms.samples.size(), post_ps.avg(), post_ps.max, post_ms.avg(),
                   post_ms.pct(0.50), post_ms.pct(0.95), post_ms.max());
      std::fprintf(stdout,
                   "  all      n=%zu collect_ms avg=%.3f p50=%.3f p95=%.3f max=%.3f\n",
                   all_ms.samples.size(), all_ms.avg(), all_ms.pct(0.50), all_ms.pct(0.95),
                   all_ms.max());
      if (!wall.empty()) {
        std::fputs(wall.c_str(), stdout);
      }
    }
  }
  wds::chart_render::split_effect_set_eval_persist_enabled(-1);
  return 0;
}

int cmd_persist_ab(int32_t effect_id, int quality) {
  const auto splits = make_instances(effect_id, 2, 0.0, 6, SplitLaneType::Full);
  std::fprintf(stdout, "persist-ab effect=%d quality=%d instances=2 split=6\n", effect_id, quality);
  print_persist_fade_frac(effect_id, 120);
  struct Jump {
    const char* name;
    std::vector<double> times;
  };
  const Jump jumps[] = {
      {"play1/60x12",
       {0.50, 0.50 + 1.0 / 60.0, 0.50 + 2.0 / 60.0, 0.50 + 3.0 / 60.0, 0.50 + 4.0 / 60.0,
        0.50 + 5.0 / 60.0, 0.50 + 6.0 / 60.0, 0.50 + 7.0 / 60.0, 0.50 + 8.0 / 60.0,
        0.50 + 9.0 / 60.0, 0.50 + 10.0 / 60.0, 0.50 + 11.0 / 60.0}},
      {"jump0.1-0.9", {0.10, 0.90}},
      {"jump0.1-2.0", {0.10, 2.00}},
      {"jump9.0-9.5", {9.00, 9.50}},
      {"scrub0.1x30", {}},
      {"backward1.0-0.3", {1.00, 0.30}},
  };
  std::vector<Jump> work;
  for (const Jump& j : jumps) {
    Jump copy = j;
    if (std::strcmp(j.name, "scrub0.1x30") == 0) {
      copy.times.clear();
      for (int i = 0; i < 30; ++i) {
        copy.times.push_back(0.50 + 0.10 * static_cast<double>(i));
      }
    }
    work.push_back(std::move(copy));
  }
  for (const Jump& j : work) {
    for (int persist_on : {1, 0, 1, 0, 1, 0}) {
      wds::chart_render::split_effect_set_eval_persist_enabled(persist_on);
      wds::chart_render::reset_split_effect_auto_seed_session();
      wds::chart_render::split_effect_clear_caches();
      SplitEffectFrameCollect io;
      SplitEffectPlaybackState state;
      std::fprintf(stdout, "  %s persist=%d", j.name, persist_on);
      for (size_t i = 0; i < j.times.size(); ++i) {
        const double now_sec = now_sec_from_now_t(kDefaultStartMs, j.times[i]);
        const bool playing = std::strcmp(j.name, "play1/60x12") == 0 && i > 0;
        const double ms = collect_ms(splits, quality, now_sec, playing, state, io);
        std::fprintf(stdout, "  t=%.2f:%.3f", j.times[i], ms);
        if (ms >= 40.0) {
          const auto st = wds::chart_render::split_effect_eval_cache_stats();
          std::fprintf(stdout, "[spike persist sys=%zu part=%zu hit/miss/reb=%llu/%llu/%llu]",
                       st.persist_systems, st.persist_particles,
                       static_cast<unsigned long long>(st.persist_hits),
                       static_cast<unsigned long long>(st.persist_misses),
                       static_cast<unsigned long long>(st.persist_rebuilds));
        }
      }
      std::fprintf(stdout, "\n");
      std::fflush(stdout);
    }
  }
  wds::chart_render::split_effect_set_eval_persist_enabled(-1);
  return 0;
}

double process_rss_mb() noexcept {
#if defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) == KERN_SUCCESS) {
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
  }
#endif
  struct rusage ru {};
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
  return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
}

int cmd_persist_rss() {
  const auto splits = make_instances(10990, 2, 0.0, 6, SplitLaneType::Full);
  wds::chart_render::split_effect_set_eval_persist_enabled(1);
  wds::chart_render::reset_split_effect_auto_seed_session();
  wds::chart_render::split_effect_clear_caches();
  SplitEffectFrameCollect io;
  SplitEffectPlaybackState state;
  const auto t0 = Clock::now();
  int frames = 0;
  std::fprintf(stdout, "persist-rss rss0=%.1fMB\n", process_rss_mb());
  for (int i = 0; i < 3000; ++i) {
    const double now_t = 0.10 + static_cast<double>(i) / 60.0;
    const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
    wds::chart_render::collect_split_effect_frame(splits, 1, now_sec, true, state, io);
    ++frames;
    if (i == 0 || i == 599 || i == 1199 || i == 1799 || i == 2399 || i == 2999) {
      std::fprintf(stdout, "  play i=%d rss=%.1fMB\n", i, process_rss_mb());
    }
  }
  for (int i = 0; i < 120; ++i) {
    const double now_t = 0.10 + static_cast<double>((i * 17) % 2400) / 60.0;
    const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
    wds::chart_render::collect_split_effect_frame(splits, 1, now_sec, false, state, io);
    ++frames;
  }
  {
    const double now_sec = now_sec_from_now_t(kDefaultStartMs, 8.5);
    wds::chart_render::collect_split_effect_frame(splits, 1, now_sec, false, state, io);
    ++frames;
  }
  std::fprintf(stdout, "  after-scrub/seek rss=%.1fMB\n", process_rss_mb());
  wds::chart_render::split_effect_clear_caches();
  state.reset();
  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  const auto cache = wds::chart_render::split_effect_eval_cache_stats();
  std::fprintf(stdout,
               "persist-rss frames=%d wall=%.2fs peakish_rss=%.1fMB persist=%.2fMB systems=%zu "
               "particles=%zu\n",
               frames, sec, process_rss_mb(),
               static_cast<double>(cache.persist_bytes_used) / (1024.0 * 1024.0),
               cache.persist_systems, cache.persist_particles);
  wds::chart_render::split_effect_set_eval_persist_enabled(-1);
  return 0;
}

template <typename Fn>
void time_samples(MsStats& acc, int n, Fn&& fn) {
  for (int i = 0; i < n; ++i) {
    acc.add(fn());
  }
}

int cmd_resume_gap(int quality, double tk, int samples) {
  const int32_t ids[] = {11621, 10380, 10660, 11711};
  const double gaps[] = {0.25, 0.5, 1.0, 2.0, 4.0};
  std::fprintf(stdout,
               "resume-gap quality=%d tk=%.3f samples=%d instances=4 stagger=0.2\n"
               "sizeof CarryParticle=%zu ParticleState=%zu IntegratorGridState=%zu Birth=%zu "
               "grid+birth+seed~=%zu\n",
               quality, tk, samples, sizeof(wds::chart_render::shuriken::CarryParticle),
               sizeof(wds::chart_render::shuriken::ParticleState),
               sizeof(wds::chart_render::shuriken::IntegratorGridState),
               sizeof(wds::chart_render::shuriken::Birth),
               sizeof(wds::chart_render::shuriken::IntegratorGridState) +
                   sizeof(wds::chart_render::shuriken::Birth) + sizeof(uint32_t));
  for (int32_t id : ids) {
    if (wds::chart_render::official_split_effect(id) == nullptr) {
      std::fprintf(stdout, "resume-gap skip %d (not in catalog)\n", id);
      continue;
    }
    const auto splits = make_instances(id, 4, 0.2, 6, SplitLaneType::Full);
    for (double g : gaps) {
      const double tgt = tk + g;
      const double tgt_sec = now_sec_from_now_t(kDefaultStartMs, tgt);
      const double tk_sec = now_sec_from_now_t(kDefaultStartMs, tk);
      wds::chart_render::split_effect_set_eval_carry_enabled(0);
      wds::chart_render::reset_split_effect_auto_seed_session();
      wds::chart_render::split_effect_clear_caches();
      SplitEffectFrameCollect remake_io;
      SplitEffectPlaybackState remake_st;
      wds::chart_render::collect_split_effect_frame(splits, quality, tgt_sec, false, remake_st,
                                                    remake_io);
      const uint64_t remake_h = hash_frame_collect(remake_io);
      const int remake_n = remake_io.total;

      MsStats remake_ms;
      time_samples(remake_ms, samples, [&]() {
        wds::chart_render::split_effect_set_eval_carry_enabled(0);
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        SplitEffectFrameCollect io;
        SplitEffectPlaybackState st;
        return collect_ms(splits, quality, tgt_sec, false, st, io);
      });

      MsStats play_ms;
      time_samples(play_ms, samples, [&]() {
        wds::chart_render::split_effect_set_eval_carry_enabled(1);
        wds::chart_render::split_effect_set_eval_carry_max_gap(0.25f);
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        SplitEffectFrameCollect io;
        SplitEffectPlaybackState st;
        for (double t = 0.0; t + 1e-12 < tgt; t += 1.0 / 60.0) {
          const double sec = now_sec_from_now_t(kDefaultStartMs, t);
          (void)collect_ms(splits, quality, sec, true, st, io);
        }
        return collect_ms(splits, quality, tgt_sec, true, st, io);
      });

      uint64_t resume_h = 0;
      int resume_n = -1;
      int resume_fb = -1;
      MsStats resume_ms;
      time_samples(resume_ms, samples, [&]() {
        wds::chart_render::split_effect_set_eval_carry_enabled(1);
        wds::chart_render::split_effect_set_eval_carry_max_gap(0.25f);
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        SplitEffectFrameCollect io;
        SplitEffectPlaybackState st;
        for (double t = 0.0; t + 1e-12 < tk; t += 1.0 / 60.0) {
          const double sec = now_sec_from_now_t(kDefaultStartMs, t);
          (void)collect_ms(splits, quality, sec, true, st, io);
        }
        (void)collect_ms(splits, quality, tk_sec, true, st, io);
        wds::chart_render::split_effect_set_eval_carry_max_gap(static_cast<float>(g + 1e-3));
        const auto cache0 = wds::chart_render::split_effect_eval_cache_stats();
        const double ms = collect_ms(splits, quality, tgt_sec, false, st, io);
        const auto cache1 = wds::chart_render::split_effect_eval_cache_stats();
        resume_h = hash_frame_collect(io);
        resume_n = io.total;
        resume_fb = static_cast<int>(cache1.carry_fallbacks - cache0.carry_fallbacks);
        return ms;
      });

      const int exact = (resume_h == remake_h && resume_n == remake_n) ? 1 : 0;
      std::fprintf(stdout,
                   "resume-gap effect=%d g=%.2f tk=%.2f tgt=%.2f exact=%d n=%d/%d fallbacks=%d  "
                   "resume_ms avg=%.3f p50=%.3f  remake_ms avg=%.3f p50=%.3f  "
                   "play_ms avg=%.3f p50=%.3f\n",
                   id, g, tk, tgt, exact, resume_n, remake_n, resume_fb, resume_ms.avg(),
                   resume_ms.pct(0.50), remake_ms.avg(), remake_ms.pct(0.50), play_ms.avg(),
                   play_ms.pct(0.50));
      std::fflush(stdout);
    }
  }
  wds::chart_render::split_effect_set_eval_carry_max_gap(-1.0f);
  wds::chart_render::split_effect_set_eval_carry_enabled(-1);
  return 0;
}

std::vector<PreviewSplitLaneInstance> splits_from_official_csv_raw(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (f == nullptr) {
    return {};
  }
  std::vector<PreviewSplitLaneInstance> out;
  char line[512];
  int src = 1;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    double st = 0, en = 0;
    int typ = 0, g = 0, sl = 0;
    if (std::sscanf(line, "%lf,%lf,%d,%*d,%*d,%d,%d", &st, &en, &typ, &g, &sl) != 5) {
      continue;
    }
    if (typ != 0 || sl <= 0 || !wds::chart_editor::is_split_lane_gimmick(
                                    static_cast<wds::chart_editor::GimmickType>(g))) {
      continue;
    }
    if (wds::chart_render::official_split_effect(sl) == nullptr) {
      continue;
    }
    const int64_t start_ms = static_cast<int64_t>(std::llround(st * 1000.0));
    const int64_t end_ms = static_cast<int64_t>(std::llround(std::max(st, en) * 1000.0));
    PreviewSplitLaneInstance split;
    const auto gt = static_cast<wds::chart_editor::GimmickType>(g);
    split.apply_identity(src++, wds::chart_editor::get_split_count(gt),
                         wds::chart_editor::get_split_lane_type(gt), sl, start_ms, end_ms);
    split.apply_state(true, false, 6 + wds::chart_editor::get_split_count(gt));
    split.apply_animation(1.0f, 0.0f, 1.0f, 0.0f, 1);
    split.should_show = true;
    out.push_back(split);
  }
  std::fclose(f);
  return out;
}

std::vector<PreviewSplitLaneInstance> splits_from_official_chart(const std::string& path) {
  wds::chart_editor::NotationChart chart;
  const auto loaded = wds::chart_editor::OfficialChartFormat::load_chart_file(path, chart);
  if (loaded.error != wds::chart_editor::SerializeError::Ok) {
    std::fprintf(stderr, "kf-memory official parse failed (%s), raw csv fallback\n",
                 loaded.message.c_str());
    return splits_from_official_csv_raw(path);
  }
  std::vector<PreviewSplitLaneInstance> out;
  int fallback_id = 1;
  for (const auto& note : chart.notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) {
      continue;
    }
    const int32_t effect = note.scratch_length;
    if (wds::chart_render::official_split_effect(effect) == nullptr) {
      continue;
    }
    const int64_t start_ms = note.start_ms(chart.timing);
    const int64_t end_ms = std::max(start_ms, note.end_ms(chart.timing));
    PreviewSplitLaneInstance split;
    const int32_t src = note.id != 0 ? note.id : fallback_id++;
    split.apply_identity(src, wds::chart_editor::get_split_count(note.gimmick_type),
                         wds::chart_editor::get_split_lane_type(note.gimmick_type), effect, start_ms,
                         end_ms);
    split.apply_state(true, false, 6 + wds::chart_editor::get_split_count(note.gimmick_type));
    split.apply_animation(1.0f, 0.0f, 1.0f, 0.0f, 1);
    split.should_show = true;
    out.push_back(split);
  }
  return out;
}

std::vector<double> parse_gap_list(const char* raw) {
  std::vector<double> out;
  if (raw == nullptr || raw[0] == '\0') {
    return {0.25, 0.5, 1.0, 2.0};
  }
  const char* p = raw;
  while (*p) {
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) {
      break;
    }
    if (v > 0.0) {
      out.push_back(v);
    }
    p = (*end == ',' || *end == ';') ? end + 1 : end;
  }
  if (out.empty()) {
    out = {0.25, 0.5, 1.0, 2.0};
  }
  return out;
}

int cmd_kf_memory(const std::string& chart_path, const std::vector<double>& gaps, int quality) {
  const auto splits = splits_from_official_chart(chart_path);
  if (splits.empty()) {
    return 1;
  }
  double tmin = 1e300;
  double tmax = -1e300;
  for (const auto& s : splits) {
    tmin = std::min(tmin, static_cast<double>(s.start_ms) / 1000.0 - 1.0);
    tmax = std::max(tmax, static_cast<double>(s.end_ms) / 1000.0 + 3.0);
  }
  std::fprintf(stdout, "kf-memory chart=%s splits=%zu t=[%.2f,%.2f] quality=%d\n",
               chart_path.c_str(), splits.size(), tmin, tmax, quality);
  std::fprintf(stdout,
               "sizeof CarryParticle=%zu bytes/particle implied with 128B overhead per system\n",
               sizeof(wds::chart_render::shuriken::CarryParticle));
  for (double g : gaps) {
    std::vector<double> times;
    for (const auto& s : splits) {
      const double a = static_cast<double>(s.start_ms) / 1000.0 - 1.0;
      const double b = static_cast<double>(s.end_ms) / 1000.0 + 3.0;
      for (double t = a; t <= b + 1e-9; t += g) {
        times.push_back(t);
      }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(),
                            [](double a, double b) { return std::abs(a - b) < 1e-4; }),
                times.end());
    std::size_t sum_bytes = 0;
    std::size_t sum_sys = 0;
    std::size_t sum_part = 0;
    int live_slots = 0;
    int max_part = 0;
    for (double now_sec : times) {
      wds::chart_render::split_effect_set_eval_carry_enabled(1);
      wds::chart_render::reset_split_effect_auto_seed_session();
      wds::chart_render::split_effect_clear_caches();
      wds::chart_render::split_effect_set_eval_carry_store_inhibited(false);
      SplitEffectFrameCollect io;
      SplitEffectPlaybackState st;
      wds::chart_render::collect_split_effect_frame(splits, quality, now_sec, false, st, io);
      const auto cache = wds::chart_render::split_effect_eval_cache_stats();
      if (cache.carry_particles > 0 || io.total > 0) {
        sum_bytes += cache.carry_bytes_used;
        sum_sys += cache.carry_systems;
        sum_part += cache.carry_particles;
        ++live_slots;
        max_part = std::max(max_part, static_cast<int>(cache.carry_particles));
      }
    }
    const double mb = static_cast<double>(sum_bytes) / (1024.0 * 1024.0);
    const double bpp =
        sum_part > 0 ? static_cast<double>(sum_bytes) / static_cast<double>(sum_part) : 0.0;
    const double packed = (static_cast<double>(sum_part) * sizeof(wds::chart_render::shuriken::CarryParticle) +
                           static_cast<double>(sum_sys) * 128.0) /
                          (1024.0 * 1024.0);
    std::fprintf(stdout,
                 "kf-memory G=%.2f slots=%zu live=%d  sum=%.2fMB packed~%.2fMB systems=%zu "
                 "particles=%zu max_part=%d bytes/part=%.1f\n",
                 g, times.size(), live_slots, mb, packed, sum_sys, sum_part, max_part, bpp);
    std::fflush(stdout);
  }
  wds::chart_render::split_effect_set_eval_carry_enabled(-1);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || eq_i(argv[1], "--help") || eq_i(argv[1], "-h") || eq_i(argv[1], "help")) {
    usage();
    return argc < 2 ? 1 : 0;
  }

  const std::string cmd = argv[1];
  auto require = [&](int i, const char* name) -> const char* {
    if (i >= argc) {
      std::fprintf(stderr, "missing value for %s\n", name);
      return nullptr;
    }
    return argv[i];
  };

  if (cmd == "scan") {
    int quality = 0;
    int32_t split_count = 6;
    SplitLaneType type = SplitLaneType::Full;
    double dt = 0.0333;
    double tmax = kScanTmaxSec;
    double effect_timeout_sec = kScanEffectTimeoutSec;
    size_t from_index = 0;
    int jobs = 1;
    std::vector<int32_t> only_ids;
    std::string csv;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--quality") && require(i + 1, "--quality")) {
        quality = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--split-count") && require(i + 1, "--split-count")) {
        split_count = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--type") && require(i + 1, "--type")) {
        type = parse_type(argv[++i]);
      } else if (eq_i(argv[i], "--dt") && require(i + 1, "--dt")) {
        dt = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--tmax") && require(i + 1, "--tmax")) {
        tmax = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--effect-timeout") && require(i + 1, "--effect-timeout")) {
        effect_timeout_sec = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--only") && require(i + 1, "--only")) {
        only_ids = parse_effect_id_list(argv[++i]);
      } else if (eq_i(argv[i], "--from-index") && require(i + 1, "--from-index")) {
        from_index = static_cast<size_t>(std::strtoul(argv[++i], nullptr, 10));
      } else if (eq_i(argv[i], "--csv") && require(i + 1, "--csv")) {
        csv = argv[++i];
      } else if (eq_i(argv[i], "--jobs") && require(i + 1, "--jobs")) {
        jobs = std::atoi(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown scan arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (dt <= 0.0) {
      std::fprintf(stderr, "--dt must be > 0\n");
      return 1;
    }
    if (tmax < 0.0) {
      std::fprintf(stderr, "--tmax must be >= 0\n");
      return 1;
    }
    if (effect_timeout_sec <= 0.0) {
      std::fprintf(stderr, "--effect-timeout must be > 0\n");
      return 1;
    }
    if (jobs <= 0) {
      jobs = 1;
    }
    return cmd_scan(quality, split_count, type, dt, tmax, effect_timeout_sec, only_ids, from_index,
                    csv, jobs);
  }

  if (cmd == "scenario") {
    int32_t effect = 0;
    bool have_effect = false;
    int instances = kDefaultInstances;
    double stagger = 0.0;
    std::string mode = "all";
    int frames = kDefaultFrames;
    uint32_t seed = kDefaultSeed;
    double now_t = -1.0;
    double now_t_hi = -1.0;
    double warm_from_t = -1.0;
    int warm_frames = 0;
    double play_dt = 1.0 / 60.0;
    int quality = 0;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--effect") && require(i + 1, "--effect")) {
        effect = std::atoi(argv[++i]);
        have_effect = true;
      } else if (eq_i(argv[i], "--instances") && require(i + 1, "--instances")) {
        instances = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--stagger") && require(i + 1, "--stagger")) {
        stagger = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--mode") && require(i + 1, "--mode")) {
        mode = argv[++i];
      } else if (eq_i(argv[i], "--frames") && require(i + 1, "--frames")) {
        frames = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--seed") && require(i + 1, "--seed")) {
        seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      } else if (eq_i(argv[i], "--now-t") && require(i + 1, "--now-t")) {
        now_t = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--now-t-hi") && require(i + 1, "--now-t-hi")) {
        now_t_hi = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--warm-from-t") && require(i + 1, "--warm-from-t")) {
        warm_from_t = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--warm-frames") && require(i + 1, "--warm-frames")) {
        warm_frames = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--dt") && require(i + 1, "--dt")) {
        play_dt = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--quality") && require(i + 1, "--quality")) {
        quality = std::atoi(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown scenario arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (!have_effect) {
      std::fprintf(stderr, "scenario requires --effect <id>\n");
      return 1;
    }
    if (instances < 1 || frames < 1) {
      std::fprintf(stderr, "--instances and --frames must be >= 1\n");
      return 1;
    }
    return cmd_scenario(effect, instances, stagger, mode, frames, seed, now_t, now_t_hi,
                        warm_from_t, warm_frames, play_dt, quality);
  }

  if (cmd == "golden-write") {
    GoldenRunConfig cfg;
    cfg.jobs = default_golden_jobs();
    std::string out;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--out") && require(i + 1, "--out")) {
        out = argv[++i];
      } else if (eq_i(argv[i], "--scenario-effect") && require(i + 1, "--scenario-effect")) {
        cfg.scenario_effect = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--full")) {
        cfg.full_format = true;
      } else if (eq_i(argv[i], "--tier") && require(i + 1, "--tier")) {
        cfg.tier = parse_golden_tier(argv[++i]);
      } else if (eq_i(argv[i], "--jobs") && require(i + 1, "--jobs")) {
        cfg.jobs = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--group-timeout") && require(i + 1, "--group-timeout")) {
        cfg.group_timeout = std::atof(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown golden-write arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (out.empty()) {
      std::fprintf(stderr, "golden-write requires --out <file>\n");
      return 1;
    }
    if (cfg.jobs <= 0) {
      cfg.jobs = default_golden_jobs();
    }
    return cmd_golden_write(cfg, out);
  }

  if (cmd == "golden-compare") {
    GoldenRunConfig cfg;
    cfg.jobs = default_golden_jobs();
    std::string baseline;
    bool exact = false;
    bool explicit_tol = false;
    double tol_pos = 1e-3;
    double tol_color = 0.004;
    int max_report = 20;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--baseline") && require(i + 1, "--baseline")) {
        baseline = argv[++i];
      } else if (eq_i(argv[i], "--exact")) {
        exact = true;
      } else if (eq_i(argv[i], "--tol-pos") && require(i + 1, "--tol-pos")) {
        tol_pos = std::atof(argv[++i]);
        explicit_tol = true;
      } else if (eq_i(argv[i], "--tol-color") && require(i + 1, "--tol-color")) {
        tol_color = std::atof(argv[++i]);
        explicit_tol = true;
      } else if (eq_i(argv[i], "--max-report") && require(i + 1, "--max-report")) {
        max_report = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--scenario-effect") && require(i + 1, "--scenario-effect")) {
        cfg.scenario_effect = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--tier") && require(i + 1, "--tier")) {
        cfg.tier = parse_golden_tier(argv[++i]);
      } else if (eq_i(argv[i], "--jobs") && require(i + 1, "--jobs")) {
        cfg.jobs = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--group-timeout") && require(i + 1, "--group-timeout")) {
        cfg.group_timeout = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--sample-order") && require(i + 1, "--sample-order")) {
        cfg.sample_order = parse_sample_order(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown golden-compare arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (baseline.empty()) {
      std::fprintf(stderr, "golden-compare requires --baseline <file>\n");
      return 1;
    }
    if (cfg.jobs <= 0) {
      cfg.jobs = default_golden_jobs();
    }
    const bool v1_tolerance = !exact;
    return cmd_golden_compare(cfg, baseline, v1_tolerance, tol_pos, tol_color, max_report,
                              explicit_tol);
  }

  if (cmd == "golden-convert") {
    std::string in_path;
    std::string out_path;
    GoldenTier tier = GoldenTier::Full;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--in") && require(i + 1, "--in")) {
        in_path = argv[++i];
      } else if (eq_i(argv[i], "--out") && require(i + 1, "--out")) {
        out_path = argv[++i];
      } else if (eq_i(argv[i], "--tier") && require(i + 1, "--tier")) {
        tier = parse_golden_tier(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown golden-convert arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (in_path.empty() || out_path.empty()) {
      std::fprintf(stderr, "golden-convert requires --in and --out\n");
      return 1;
    }
    return cmd_golden_convert(in_path, out_path, tier);
  }

  if (cmd == "golden-compare-files") {
    std::string a_path;
    std::string b_path;
    int max_report = 20;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--a") && require(i + 1, "--a")) {
        a_path = argv[++i];
      } else if (eq_i(argv[i], "--b") && require(i + 1, "--b")) {
        b_path = argv[++i];
      } else if (eq_i(argv[i], "--max-report") && require(i + 1, "--max-report")) {
        max_report = std::atoi(argv[++i]);
      } else if (argv[i][0] != '-' && a_path.empty()) {
        a_path = argv[i];
      } else if (argv[i][0] != '-' && b_path.empty()) {
        b_path = argv[i];
      } else {
        std::fprintf(stderr, "unknown golden-compare-files arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (a_path.empty() || b_path.empty()) {
      std::fprintf(stderr, "golden-compare-files requires two v2 files (A B)\n");
      return 1;
    }
    return cmd_golden_compare_files(a_path, b_path, max_report);
  }

  if (cmd == "noise-catalog") {
    return cmd_noise_catalog();
  }

  if (cmd == "noise-hold-plan") {
    return cmd_noise_hold_plan();
  }

  if (cmd == "noise-error") {
    int32_t scenario_effect = kDefaultScenarioEffect;
    GoldenTier tier = GoldenTier::Full;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--scenario-effect") && require(i + 1, "--scenario-effect")) {
        scenario_effect = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--tier") && require(i + 1, "--tier")) {
        tier = parse_golden_tier(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown noise-error arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    return cmd_noise_error(scenario_effect, tier);
  }

  if (cmd == "golden-dump") {
    std::string key_str;
    std::string out_path;
    int32_t scenario_effect = kDefaultScenarioEffect;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--key") && require(i + 1, "--key")) {
        key_str = argv[++i];
      } else if (eq_i(argv[i], "--out") && require(i + 1, "--out")) {
        out_path = argv[++i];
      } else if (eq_i(argv[i], "--scenario-effect") && require(i + 1, "--scenario-effect")) {
        scenario_effect = std::atoi(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown golden-dump arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (key_str.empty() || out_path.empty()) {
      std::fprintf(stderr, "golden-dump requires --key and --out\n");
      return 1;
    }
    return cmd_golden_dump(key_str, out_path, scenario_effect);
  }

  if (cmd == "geom-dump") {
    std::vector<int32_t> effects;
    std::string out_path;
    double now_t = 0.5;
    int32_t line_index = 0;
    int fb_w = 1920;
    int fb_h = 1080;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--effect") && require(i + 1, "--effect")) {
        effects = parse_effect_id_list(argv[++i]);
      } else if (eq_i(argv[i], "--out") && require(i + 1, "--out")) {
        out_path = argv[++i];
      } else if (eq_i(argv[i], "--now-t") && require(i + 1, "--now-t")) {
        now_t = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--line") && require(i + 1, "--line")) {
        line_index = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--w") && require(i + 1, "--w")) {
        fb_w = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--h") && require(i + 1, "--h")) {
        fb_h = std::atoi(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown geom-dump arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    return cmd_geom_dump(effects, now_t, out_path, line_index, fb_w, fb_h);
  }

  if (cmd == "raster") {
    int32_t effect = 0;
    double now_t = 0.7;
    std::string out_path;
    int instances = 4;
    double stagger = 0.2;
    int width = 960;
    int height = 540;
    bool have_bounds = false;
    float bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;
    bool have_effect = false;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--effect") && require(i + 1, "--effect")) {
        effect = std::atoi(argv[++i]);
        have_effect = true;
      } else if (eq_i(argv[i], "--now-t") && require(i + 1, "--now-t")) {
        now_t = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--out") && require(i + 1, "--out")) {
        out_path = argv[++i];
      } else if (eq_i(argv[i], "--instances") && require(i + 1, "--instances")) {
        instances = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--stagger") && require(i + 1, "--stagger")) {
        stagger = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--w") && require(i + 1, "--w")) {
        width = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--h") && require(i + 1, "--h")) {
        height = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--bounds") && require(i + 1, "--bounds")) {
        if (std::sscanf(argv[++i], "%f,%f,%f,%f", &bx0, &bx1, &by0, &by1) != 4) {
          std::fprintf(stderr, "raster --bounds wants xmin,xmax,ymin,ymax\n");
          return 1;
        }
        have_bounds = true;
      } else {
        std::fprintf(stderr, "unknown raster arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (!have_effect || out_path.empty()) {
      std::fprintf(stderr, "raster requires --effect and --out\n");
      return 1;
    }
    return cmd_raster(effect, now_t, out_path, instances, stagger, width, height, have_bounds, bx0,
                      bx1, by0, by1);
  }

  if (cmd == "carry-check") {
    std::vector<int32_t> only_ids;
    bool quick = false;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--only") && require(i + 1, "--only")) {
        only_ids = parse_effect_id_list(argv[++i]);
      } else if (eq_i(argv[i], "--quick")) {
        quick = true;
      } else {
        std::fprintf(stderr, "unknown carry-check arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    return cmd_carry_check(only_ids, quick);
  }

  if (cmd == "persist-check") {
    std::vector<int32_t> only_ids;
    bool quick = false;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--only") && require(i + 1, "--only")) {
        only_ids = parse_effect_id_list(argv[++i]);
      } else if (eq_i(argv[i], "--quick")) {
        quick = true;
      } else {
        std::fprintf(stderr, "unknown persist-check arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    return cmd_persist_check(only_ids, quick);
  }

  if (cmd == "persist-fade-frac") {
    std::vector<int32_t> ids = {10990, 11803, 10850};
    int frames = 120;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--effect") && require(i + 1, "--effect")) {
        ids = parse_effect_id_list(argv[++i]);
      } else if (eq_i(argv[i], "--frames") && require(i + 1, "--frames")) {
        frames = std::atoi(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown persist-fade-frac arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (frames <= 0) {
      frames = 120;
    }
    for (int32_t id : ids) {
      print_persist_fade_frac(id, frames);
    }
    return 0;
  }

  if (cmd == "persist-phase") {
    std::vector<int32_t> ids = {10990, 11803, 10850};
    int quality = 1;
    int frames = 120;
    int persist_mode = -1;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--effect") && require(i + 1, "--effect")) {
        ids = parse_effect_id_list(argv[++i]);
      } else if (eq_i(argv[i], "--quality") && require(i + 1, "--quality")) {
        quality = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--frames") && require(i + 1, "--frames")) {
        frames = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--persist") && require(i + 1, "--persist")) {
        persist_mode = std::atoi(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown persist-phase arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (frames <= 0) {
      frames = 120;
    }
    return cmd_persist_phase(ids, quality, frames, persist_mode);
  }

  if (cmd == "persist-ab") {
    int32_t effect = 10990;
    int quality = 1;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--effect") && require(i + 1, "--effect")) {
        effect = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--quality") && require(i + 1, "--quality")) {
        quality = std::atoi(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown persist-ab arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    return cmd_persist_ab(effect, quality);
  }

  if (cmd == "persist-rss") {
    return cmd_persist_rss();
  }

  if (cmd == "resume-gap") {
    int quality = 1;
    double tk = 0.7;
    int samples = 6;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--quality") && require(i + 1, "--quality")) {
        quality = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--tk") && require(i + 1, "--tk")) {
        tk = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--samples") && require(i + 1, "--samples")) {
        samples = std::atoi(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown resume-gap arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    return cmd_resume_gap(quality, tk, std::max(1, samples));
  }

  if (cmd == "kf-memory") {
    std::string chart;
    std::vector<double> gaps = {0.25, 0.5, 1.0, 2.0};
    int quality = 1;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--chart") && require(i + 1, "--chart")) {
        chart = argv[++i];
      } else if (eq_i(argv[i], "--gap") && require(i + 1, "--gap")) {
        gaps = parse_gap_list(argv[++i]);
      } else if (eq_i(argv[i], "--quality") && require(i + 1, "--quality")) {
        quality = std::atoi(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown kf-memory arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    if (chart.empty()) {
      std::fprintf(stderr, "kf-memory requires --chart PATH\n");
      return 1;
    }
    return cmd_kf_memory(chart, gaps, quality);
  }


  if (cmd == "dead-skip-check") {
    double tmax = 8.0;
    double dt = 0.05;
    int instances = 1;
    int32_t split_count = 2;
    std::vector<int32_t> only_ids;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--tmax") && require(i + 1, "--tmax")) {
        tmax = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--dt") && require(i + 1, "--dt")) {
        dt = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--instances") && require(i + 1, "--instances")) {
        instances = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--split-count") && require(i + 1, "--split-count")) {
        split_count = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--only") && require(i + 1, "--only")) {
        only_ids = parse_effect_id_list(argv[++i]);
      } else {
        std::fprintf(stderr, "unknown dead-skip-check arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    std::vector<int32_t> ids = only_ids.empty() ? wds::chart_render::official_split_effect_ids()
                                                : only_ids;
    int samples = 0;
    int mismatches = 0;
    const auto t0 = Clock::now();
    for (int32_t id : ids) {
      if (wds::chart_render::official_split_effect(id) == nullptr) {
        continue;
      }
      const auto splits =
          make_instances(id, instances, 0.0, split_count, SplitLaneType::Full);
      for (double now_t = 0.0; now_t <= tmax + 1e-9; now_t += dt) {
        const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
        SplitEffectFrameCollect on;
        SplitEffectFrameCollect off;
        SplitEffectPlaybackState st_on;
        SplitEffectPlaybackState st_off;
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        wds::chart_render::split_effect_set_dead_system_skip(true);
        wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, st_on, on);
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        wds::chart_render::split_effect_set_dead_system_skip(false);
        wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, st_off, off);
        ++samples;
        bool bad = on.total != off.total;
        if (!bad) {
          std::vector<uint8_t> ha;
          std::vector<uint8_t> hb;
          BufferWriter wa{ha};
          BufferWriter wb{hb};
          for (int i = 0; i < on.total; ++i) {
            write_particle(wa, on.pool[static_cast<size_t>(i)]);
            write_particle(wb, off.pool[static_cast<size_t>(i)]);
          }
          bad = fnv1a64_buffer(ha) != fnv1a64_buffer(hb);
        }
        if (bad) {
          ++mismatches;
          if (mismatches <= 12) {
            std::fprintf(stderr, "dead-skip mismatch effect=%d now_t=%.2f n=%d/%d\n", id, now_t,
                         on.total, off.total);
          }
        }
      }
      // Latch only runs on playing=true. Remake first (playing=false clears
      // latches), then a sequential play walk; play must not go empty while
      // the remake at that now_t still has particles.
      std::vector<int> remake_n;
      for (double now_t = 0.0; now_t <= tmax + 1e-9; now_t += dt) {
        const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
        SplitEffectFrameCollect remake;
        SplitEffectPlaybackState st_re;
        wds::chart_render::reset_split_effect_auto_seed_session();
        wds::chart_render::split_effect_clear_caches();
        wds::chart_render::split_effect_set_dead_system_skip(true);
        wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, st_re, remake);
        remake_n.push_back(remake.total);
      }
      SplitEffectPlaybackState st_play;
      SplitEffectFrameCollect play_io;
      wds::chart_render::reset_split_effect_auto_seed_session();
      wds::chart_render::split_effect_clear_caches();
      wds::chart_render::split_effect_set_dead_system_skip(true);
      size_t ri = 0;
      for (double now_t = 0.0; now_t <= tmax + 1e-9; now_t += dt) {
        const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
        (void)collect_ms(splits, 0, now_sec, true, st_play, play_io);
        ++samples;
        if (ri < remake_n.size() && play_io.total == 0 && remake_n[ri] > 0) {
          ++mismatches;
          if (mismatches <= 12) {
            std::fprintf(stderr, "dead-skip latch dropped effect=%d now_t=%.2f remake_n=%d\n", id,
                         now_t, remake_n[ri]);
          }
        }
        ++ri;
      }
    }
    wds::chart_render::split_effect_set_dead_system_skip(true);
    const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
    std::fprintf(stdout, "dead-skip-check samples=%d mismatches=%d wall=%.2fs\n", samples,
                 mismatches, sec);
    return mismatches == 0 ? 0 : 1;
  }

  if (cmd == "schedule-compare") {
    std::string mode = "all";
    std::vector<int32_t> only_ids;
    GoldenTier tier = GoldenTier::Quick;
    int32_t split_count = 0;
    double now_t = -1.0;
    bool analyze_outliers = false;
    for (int i = 2; i < argc; ++i) {
      if (eq_i(argv[i], "--mode") && require(i + 1, "--mode")) {
        mode = argv[++i];
      } else if (eq_i(argv[i], "--effect") && require(i + 1, "--effect")) {
        only_ids = parse_effect_id_list(argv[++i]);
      } else if (eq_i(argv[i], "--tier") && require(i + 1, "--tier")) {
        tier = parse_golden_tier(argv[++i]);
      } else if (eq_i(argv[i], "--split-count") && require(i + 1, "--split-count")) {
        split_count = std::atoi(argv[++i]);
      } else if (eq_i(argv[i], "--now-t") && require(i + 1, "--now-t")) {
        now_t = std::atof(argv[++i]);
      } else if (eq_i(argv[i], "--analyze-outliers")) {
        analyze_outliers = true;
      } else {
        std::fprintf(stderr, "unknown schedule-compare arg: %s\n", argv[i]);
        usage();
        return 1;
      }
    }
    return cmd_schedule_compare(mode, only_ids, tier, split_count, now_t, analyze_outliers);
  }

  std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  usage();
  return 1;
}