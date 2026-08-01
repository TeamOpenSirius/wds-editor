#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wds::chart_editor {

void normalize_timing_points(MusicTiming& timing) {
  if (timing.points.empty()) {
    TimingPoint root;
    root.tick = 0;
    root.bpm = timing.bpm > 0.0 ? timing.bpm : 120.0;
    root.numerator = 4;
    root.denominator = 4;
    timing.points.push_back(root);
  }
  std::sort(timing.points.begin(), timing.points.end(),
            [](const TimingPoint& a, const TimingPoint& b) { return a.tick < b.tick; });
  // Drop duplicates (keep first).
  auto last = timing.points.begin();
  for (auto it = timing.points.begin() + 1; it != timing.points.end(); ++it) {
    if (it->tick != last->tick) {
      *++last = *it;
    }
  }
  timing.points.erase(last + 1, timing.points.end());
  if (timing.points.front().tick != 0) {
    TimingPoint root = timing.points.front();
    root.tick = 0;
    timing.points.insert(timing.points.begin(), root);
  }
  for (auto& p : timing.points) {
    // Any bpm > 0 is valid; non-finite / non-positive values fall back to 120.
    if (!std::isfinite(p.bpm) || !(p.bpm > 0.0)) {
      p.bpm = 120.0;
    } else {
      p.bpm = std::min(p.bpm, 600.0);
    }
    p.numerator = std::clamp(p.numerator, 1, 32);
    p.denominator = std::clamp(p.denominator, 1, 32);
  }
  timing.bpm = timing.points.front().bpm;
}

int32_t measure_length_ticks(const TimingPoint& point, int32_t ticks_per_quarter) noexcept {
  const int32_t tpq = std::max(1, ticks_per_quarter);
  const int32_t den = std::max(1, point.denominator);
  // One beat of 1/den note = tpq * 4 / den ticks.
  const int32_t beat = std::max(1, tpq * 4 / den);
  return std::max(1, point.numerator * beat);
}

const TimingPoint& timing_point_at(const MusicTiming& timing, int32_t tick) noexcept {
  static const TimingPoint kFallback{0, 120.0, 4, 4};
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

std::vector<int32_t> measure_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                            const MusicTiming& timing) {
  std::vector<int32_t> out;
  if (end_tick < start_tick || timing.points.empty()) return out;
  const int32_t tpq = std::max(1, timing.ticks_per_quarter);
  const int32_t hard_end = end_tick;

  for (size_t i = 0; i < timing.points.size(); ++i) {
    const auto& p = timing.points[i];
    const int32_t seg_end =
        (i + 1 < timing.points.size()) ? timing.points[i + 1].tick : hard_end + 1;
    const int32_t bar = measure_length_ticks(p, tpq);
    int32_t t = p.tick;
    // Advance to first measure >= start_tick.
    if (t < start_tick) {
      const int32_t delta = start_tick - t;
      t += ((delta + bar - 1) / bar) * bar;
    }
    for (; t < seg_end && t <= hard_end; t += bar) {
      if (t >= start_tick) out.push_back(t);
    }
  }
  return out;
}

int32_t snap_to_measure(int32_t tick, const MusicTiming& timing) noexcept {
  if (timing.points.empty()) return std::max(0, tick);
  const int32_t tpq = std::max(1, timing.ticks_per_quarter);
  const TimingPoint& p = timing_point_at(timing, tick);
  // Find segment end.
  int32_t seg_end = std::numeric_limits<int32_t>::max() / 4;
  for (const auto& q : timing.points) {
    if (q.tick > p.tick) {
      seg_end = q.tick;
      break;
    }
  }
  const int32_t bar = measure_length_ticks(p, tpq);
  const int32_t rel = std::max(0, tick - p.tick);
  const int32_t n = (rel + bar / 2) / bar;
  int32_t snapped = p.tick + n * bar;
  if (snapped >= seg_end && seg_end > p.tick) {
    snapped = seg_end;  // land on next timing point (also a measure start)
  }
  return std::max(0, snapped);
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
