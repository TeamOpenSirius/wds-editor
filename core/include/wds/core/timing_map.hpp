#pragma once

#include <wds/core/notation.hpp>
#include <wds/core/types.hpp>

#include <cstdint>
#include <vector>

namespace wds::chart_editor {

// Guarantee a tick-0 point (BPM+meter), propagate inherited fields, drop empty points.
void normalize_timing_points(MusicTiming& timing);

// Ticks per one beat under a point (TPQ is quarter-note based).
int32_t beat_length_ticks(const TimingPoint& point, int32_t ticks_per_quarter) noexcept;

// Ticks per measure under a point.
int32_t measure_length_ticks(const TimingPoint& point, int32_t ticks_per_quarter) noexcept;

// Approximate ticks per subdivision (floor(beat/subdivs)). Prefer
// subdivision_offset_ticks for exact in-beat positions when beat % subdivs != 0.
int32_t subdivision_length_ticks(const TimingPoint& point, int32_t ticks_per_quarter,
                                 int32_t subdivisions_per_beat) noexcept;

// Exact tick offset of subdivision index `index` within a beat (index in 0..subdivs).
// Multiply-then-divide so index==subdivs lands exactly on beat_length.
int32_t subdivision_offset_ticks(int32_t beat_length, int32_t subdivisions_per_beat,
                                 int32_t index) noexcept;

// Nearest subdivision tick (uses active meter beat length).
int32_t snap_to_subdivision(int32_t tick, const MusicTiming& timing,
                            int32_t subdivisions_per_beat) noexcept;

// Interior + boundary subdivision ticks in [start_tick, end_tick] under meter beats.
// Boundaries coincide with beat ticks; callers may filter those out for drawing.
std::vector<int32_t> subdivision_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                                const MusicTiming& timing,
                                                int32_t subdivisions_per_beat);

// Active timing point at or before `tick` (requires normalized points).
const TimingPoint& timing_point_at(const MusicTiming& timing, int32_t tick) noexcept;

// Last authored meter change at or before `tick`.
const TimingPoint& timing_meter_at(const MusicTiming& timing, int32_t tick) noexcept;

// True when `tick` is a whole-measure boundary under current meter changes.
bool is_measure_tick(int32_t tick, const MusicTiming& timing) noexcept;

// Whole-measure boundaries in [start_tick, end_tick].
std::vector<int32_t> measure_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                            const MusicTiming& timing);

// Whole-beat boundaries in [start_tick, end_tick] (includes measure ticks).
std::vector<int32_t> beat_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                         const MusicTiming& timing);

// Nearest measure tick to `tick` (prefers earlier on ties).
int32_t snap_to_measure(int32_t tick, const MusicTiming& timing) noexcept;

// After editing meter at `edited_tick`, drop later meter labels that no longer
// land on a whole-measure boundary. Stops at the first later meter that still does.
void prune_orphaned_meter_changes(MusicTiming& timing, int32_t edited_tick);

// GimmickType::SplitN for N in 1..6.
GimmickType split_gimmick_for_count(int32_t split_count) noexcept;

// Default note width under an active split (12/N for equal splits).
int32_t split_default_note_width(int32_t split_count, int32_t lane_count = 12) noexcept;

// Convert fade windows (seconds) → ticks using BPM active at `anchor_tick`.
int32_t seconds_to_ticks_at(float seconds, const MusicTiming& timing, int32_t anchor_tick) noexcept;

}  // namespace wds::chart_editor
