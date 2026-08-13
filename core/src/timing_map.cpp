#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wds::chart_editor {
namespace {

void propagate_inherited_fields(MusicTiming& timing) {
  double cur_bpm = timing.bpm > 0.0 ? timing.bpm : 120.0;
  int32_t cur_num = 4;
  int32_t cur_den = 4;
  for (auto& p : timing.points) {
    if (p.has_bpm) {
      cur_bpm = p.bpm;
    } else {
      p.bpm = cur_bpm;
    }
    if (p.has_meter) {
      cur_num = p.numerator;
      cur_den = p.denominator;
    } else {
      p.numerator = cur_num;
      p.denominator = cur_den;
    }
  }
}

}  // namespace

void normalize_timing_points(MusicTiming& timing) {
  if (timing.points.empty()) {
    TimingPoint root;
    root.tick = 0;
    root.bpm = timing.bpm > 0.0 ? timing.bpm : 120.0;
    root.numerator = 4;
    root.denominator = 4;
    root.has_bpm = true;
    root.has_meter = true;
    timing.points.push_back(root);
  }
  std::sort(timing.points.begin(), timing.points.end(),
            [](const TimingPoint& a, const TimingPoint& b) { return a.tick < b.tick; });

  // Merge same-tick rows: OR flags, prefer authored values.
  std::vector<TimingPoint> merged;
  merged.reserve(timing.points.size());
  for (const auto& p : timing.points) {
    if (!merged.empty() && merged.back().tick == p.tick) {
      auto& dst = merged.back();
      if (p.has_bpm) {
        dst.bpm = p.bpm;
        dst.has_bpm = true;
      }
      if (p.has_meter) {
        dst.numerator = p.numerator;
        dst.denominator = p.denominator;
        dst.has_meter = true;
      }
      continue;
    }
    merged.push_back(p);
  }
  timing.points = std::move(merged);

  if (timing.points.front().tick != 0) {
    TimingPoint root = timing.points.front();
    root.tick = 0;
    root.has_bpm = true;
    root.has_meter = true;
    timing.points.insert(timing.points.begin(), root);
  }
  timing.points.front().has_bpm = true;
  timing.points.front().has_meter = true;

  for (auto& p : timing.points) {
    if (!std::isfinite(p.bpm) || !(p.bpm > 0.0)) {
      p.bpm = 120.0;
    } else {
      p.bpm = std::min(p.bpm, 10000.0);
    }
    p.numerator = std::clamp(p.numerator, 1, 32);
    p.denominator = std::clamp(p.denominator, 1, 32);
    if (!p.has_bpm && !p.has_meter && p.tick != 0) {
      // Drop empty mid points; cleaned again after propagate if needed.
    }
  }

  propagate_inherited_fields(timing);

  timing.points.erase(std::remove_if(timing.points.begin(), timing.points.end(),
                                     [](const TimingPoint& p) {
                                       return p.tick != 0 && !p.has_bpm && !p.has_meter;
                                     }),
                      timing.points.end());
  if (timing.points.empty()) {
    TimingPoint root;
    root.tick = 0;
    root.bpm = 120.0;
    root.has_bpm = true;
    root.has_meter = true;
    timing.points.push_back(root);
  }
  timing.points.front().has_bpm = true;
  timing.points.front().has_meter = true;
  timing.bpm = timing.points.front().bpm;
  rebuild_timing_prefix_ms(timing);
}

void rebuild_timing_prefix_ms(const MusicTiming& timing) {
  const auto& pts = timing.points;
  timing.prefix_ms.resize(pts.size());
  if (pts.empty()) {
    return;
  }
  timing.prefix_ms[0] = 0.0;
  const double tpq = static_cast<double>(std::max(1, timing.ticks_per_quarter));
  for (size_t i = 1; i < pts.size(); ++i) {
    const double bpm = pts[i - 1].bpm > 0.0 ? pts[i - 1].bpm : 120.0;
    const double rate = 60000.0 / (bpm * tpq);
    timing.prefix_ms[i] =
        timing.prefix_ms[i - 1] + static_cast<double>(pts[i].tick - pts[i - 1].tick) * rate;
  }
}

int32_t beat_length_ticks(const TimingPoint& point, int32_t ticks_per_quarter) noexcept {
  const int32_t tpq = std::max(1, ticks_per_quarter);
  const int32_t den = std::max(1, point.denominator);
  return std::max(1, tpq * 4 / den);
}

int32_t measure_length_ticks(const TimingPoint& point, int32_t ticks_per_quarter) noexcept {
  return std::max(1, point.numerator * beat_length_ticks(point, ticks_per_quarter));
}

int32_t subdivision_length_ticks(const TimingPoint& point, int32_t ticks_per_quarter,
                                 int32_t subdivisions_per_beat) noexcept {
  const int32_t beat = beat_length_ticks(point, ticks_per_quarter);
  const int32_t subdivs = std::max(1, subdivisions_per_beat);
  return std::max(1, beat / subdivs);
}

int32_t subdivision_offset_ticks(int32_t beat_length, int32_t subdivisions_per_beat,
                                 int32_t index) noexcept {
  const int32_t beat = std::max(1, beat_length);
  const int32_t subdivs = std::max(1, subdivisions_per_beat);
  const int32_t i = std::clamp(index, 0, subdivs);
  return static_cast<int32_t>((static_cast<int64_t>(i) * beat) / subdivs);
}

int32_t snap_to_subdivision(int32_t tick, const MusicTiming& timing,
                            int32_t subdivisions_per_beat) noexcept {
  if (timing.points.empty()) return std::max(0, tick);
  const int32_t tpq = std::max(1, timing.ticks_per_quarter);
  const TimingPoint& p = timing_meter_at(timing, tick);
  int32_t seg_end = std::numeric_limits<int32_t>::max() / 4;
  for (const auto& q : timing.points) {
    if (q.has_meter && q.tick > p.tick) {
      seg_end = q.tick;
      break;
    }
  }
  const int32_t beat = beat_length_ticks(p, tpq);
  const int32_t subdivs = std::max(1, subdivisions_per_beat);
  const int32_t rel = std::max(0, tick - p.tick);
  const int32_t beat_start = p.tick + (rel / beat) * beat;
  if (beat_start >= seg_end && seg_end > p.tick) {
    return std::max(0, seg_end);
  }
  const int32_t within = std::max(0, tick - beat_start);
  const int32_t i = static_cast<int32_t>(
      (static_cast<int64_t>(within) * subdivs + beat / 2) / beat);
  int32_t snapped = beat_start + subdivision_offset_ticks(beat, subdivs, std::min(i, subdivs));
  if (snapped >= seg_end && seg_end > p.tick) {
    snapped = seg_end;
  }
  return std::max(0, snapped);
}

std::vector<int32_t> subdivision_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                                const MusicTiming& timing,
                                                int32_t subdivisions_per_beat) {
  std::vector<int32_t> out;
  if (end_tick < start_tick || timing.points.empty()) return out;
  const int32_t tpq = std::max(1, timing.ticks_per_quarter);
  const int32_t subdivs = std::max(1, subdivisions_per_beat);

  std::vector<const TimingPoint*> meters;
  meters.reserve(timing.points.size());
  for (const auto& p : timing.points) {
    if (p.has_meter) meters.push_back(&p);
  }
  if (meters.empty()) return out;

  for (size_t mi = 0; mi < meters.size(); ++mi) {
    const auto& p = *meters[mi];
    const int32_t seg_end =
        (mi + 1 < meters.size()) ? meters[mi + 1]->tick : end_tick + 1;
    const int32_t beat = beat_length_ticks(p, tpq);
    int32_t beat_start = p.tick;
    if (beat_start < start_tick) {
      const int32_t delta = start_tick - beat_start;
      beat_start += (delta / beat) * beat;  // floor onto beat containing start
      if (beat_start < p.tick) beat_start = p.tick;
    }
    for (int64_t beat_i = beat_start; beat_i < seg_end && beat_i <= end_tick;
         beat_i += beat) {
      for (int32_t i = 0; i <= subdivs; ++i) {
        const int64_t t64 =
            beat_i + static_cast<int64_t>(subdivision_offset_ticks(beat, subdivs, i));
        if (t64 > end_tick || t64 >= seg_end) break;
        if (t64 >= start_tick) {
          const int32_t t = static_cast<int32_t>(t64);
          if (out.empty() || out.back() != t) out.push_back(t);
        }
      }
    }
  }
  return out;
}

const TimingPoint& timing_point_at(const MusicTiming& timing, int32_t tick) noexcept {
  static const TimingPoint kFallback{0, 120.0, 4, 4, true, true};
  if (timing.points.empty()) {
    return kFallback;
  }
  const TimingPoint* best = &timing.points.front();
  for (const auto& p : timing.points) {
    if (p.tick > tick) break;
    best = &p;
  }
  return *best;
}

const TimingPoint& timing_meter_at(const MusicTiming& timing, int32_t tick) noexcept {
  static const TimingPoint kFallback{0, 120.0, 4, 4, true, true};
  if (timing.points.empty()) {
    return kFallback;
  }
  const TimingPoint* best = &timing.points.front();
  for (const auto& p : timing.points) {
    if (p.tick > tick) break;
    if (p.has_meter) best = &p;
  }
  return *best;
}

namespace {

// Emit ticks from meter-change anchors stepping by `step_fn(point)`.
template <typename StepFn>
std::vector<int32_t> ticks_from_meter_segments(int32_t start_tick, int32_t end_tick,
                                               const MusicTiming& timing, StepFn step_fn) {
  std::vector<int32_t> out;
  if (end_tick < start_tick || timing.points.empty()) return out;
  const int32_t tpq = std::max(1, timing.ticks_per_quarter);

  std::vector<const TimingPoint*> meters;
  meters.reserve(timing.points.size());
  for (const auto& p : timing.points) {
    if (p.has_meter) meters.push_back(&p);
  }
  if (meters.empty()) return out;

  for (size_t i = 0; i < meters.size(); ++i) {
    const auto& p = *meters[i];
    const int32_t seg_end =
        (i + 1 < meters.size()) ? meters[i + 1]->tick : end_tick + 1;
    const int32_t step = std::max(1, step_fn(p, tpq));
    int32_t t = p.tick;
    if (t < start_tick) {
      const int32_t delta = start_tick - t;
      t += ((delta + step - 1) / step) * step;
    }
    for (int64_t t64 = t; t64 < seg_end && t64 <= end_tick; t64 += step) {
      if (t64 >= start_tick) out.push_back(static_cast<int32_t>(t64));
    }
  }
  return out;
}

}  // namespace

std::vector<int32_t> measure_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                            const MusicTiming& timing) {
  return ticks_from_meter_segments(start_tick, end_tick, timing, measure_length_ticks);
}

std::vector<int32_t> beat_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                         const MusicTiming& timing) {
  return ticks_from_meter_segments(start_tick, end_tick, timing, beat_length_ticks);
}

bool is_measure_tick(int32_t tick, const MusicTiming& timing) noexcept {
  if (tick < 0 || timing.points.empty()) return false;
  const auto hits = measure_ticks_in_range(tick, tick, timing);
  return !hits.empty() && hits.front() == tick;
}

int32_t snap_to_measure(int32_t tick, const MusicTiming& timing) noexcept {
  if (timing.points.empty()) return std::max(0, tick);
  const int32_t tpq = std::max(1, timing.ticks_per_quarter);
  const TimingPoint& p = timing_meter_at(timing, tick);
  int32_t seg_end = std::numeric_limits<int32_t>::max() / 4;
  for (const auto& q : timing.points) {
    if (q.has_meter && q.tick > p.tick) {
      seg_end = q.tick;
      break;
    }
  }
  const int32_t bar = measure_length_ticks(p, tpq);
  const int32_t rel = std::max(0, tick - p.tick);
  const int32_t n = (rel + bar / 2) / bar;
  int32_t snapped = p.tick + n * bar;
  if (snapped >= seg_end && seg_end > p.tick) {
    snapped = seg_end;
  }
  return std::max(0, snapped);
}

void prune_orphaned_meter_changes(MusicTiming& timing, int32_t edited_tick) {
  normalize_timing_points(timing);
  for (;;) {
    TimingPoint* victim = nullptr;
    for (auto& p : timing.points) {
      if (!p.has_meter || p.tick <= edited_tick) continue;
      victim = &p;
      break;
    }
    if (!victim) break;

    MusicTiming probe = timing;
    for (auto& p : probe.points) {
      if (p.tick >= victim->tick) p.has_meter = false;
    }
    normalize_timing_points(probe);
    if (is_measure_tick(victim->tick, probe)) break;

    victim->has_meter = false;
    normalize_timing_points(timing);
  }
}

GimmickType split_gimmick_for_count(int32_t split_count) noexcept {
  const int32_t n = std::clamp(split_count, 1, 6);
  return static_cast<GimmickType>(10 + n);  // Split1=11 … Split6=16
}

int32_t split_default_note_width(int32_t split_count, int32_t lane_count) noexcept {
  const int32_t n = std::max(1, split_count);
  return std::max(1, std::max(1, lane_count) / n);
}

int32_t seconds_to_ticks_at(float seconds, const MusicTiming& timing,
                            int32_t anchor_tick) noexcept {
  const TimingPoint& p = timing_point_at(timing, anchor_tick);
  const double bpm = p.bpm > 0.0 ? p.bpm : 120.0;
  const double tpq = static_cast<double>(std::max(1, timing.ticks_per_quarter));
  return std::max(1, static_cast<int32_t>(std::llround(seconds * bpm / 60.0 * tpq)));
}

}  // namespace wds::chart_editor
