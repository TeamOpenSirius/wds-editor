#include <wds/core/preview_snapshot_builder.hpp>

#include <wds/core/gimmick.hpp>
#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace wds::chart_editor {
namespace {

int64_t sat_sub_nonneg(int64_t value, int64_t amount) {
  if (amount <= 0) {
    return value;
  }
  if (value < std::numeric_limits<int64_t>::min() + amount) {
    return std::numeric_limits<int64_t>::min();
  }
  return value - amount;
}

int64_t sat_add_nonneg(int64_t value, int64_t amount) {
  if (amount <= 0) {
    return value;
  }
  if (value > std::numeric_limits<int64_t>::max() - amount) {
    return std::numeric_limits<int64_t>::max();
  }
  return value + amount;
}

void query_preview_split_candidates(const ChartNoteIndex& index, int64_t preview_time_ms,
                                    const PreviewConfig& config,
                                    std::vector<int32_t>& out_note_ids) {
  const int64_t appear_ms = split_fade_sec_to_ms(config.split_line_animation_start_sec);
  const int64_t disappear_ms = split_fade_sec_to_ms(config.split_line_animation_end_sec);
  const int64_t lower_ms =
      sat_sub_nonneg(sat_sub_nonneg(preview_time_ms, index.max_split_span_ms()), disappear_ms);
  const int64_t upper_ms = sat_add_nonneg(preview_time_ms, appear_ms);
  index.query_split_lanes_in_range(lower_ms, upper_ms, out_note_ids);
}

}  // namespace

PreviewSnapshotBuilder::PreviewSnapshotBuilder(PreviewConfig config)
    : config_(config),
      position_calculator_(config),
      auto_judge_(config),
      split_lane_simulator_(config) {}

void PreviewSnapshotBuilder::set_config(PreviewConfig config) {
  config_ = config;
  position_calculator_.set_config(config);
  auto_judge_.set_config(config);
  split_lane_simulator_.set_config(config);
}

bool PreviewSnapshotBuilder::is_spawn_visible(const NotationNote& note, int64_t preview_time_ms,
                                              const MusicTiming& timing) const {
  const int64_t start_ms = note.start_ms(timing);
  return preview_time_ms >= start_ms - spawn_lead_ms();
}

bool PreviewSnapshotBuilder::is_expired(const NotationNote& note, int64_t preview_time_ms,
                                        PreviewNoteVisualState state,
                                        const MusicTiming& timing) const {
  if (state == PreviewNoteVisualState::Hidden) {
    return true;
  }

  const int64_t start_ms = note.start_ms(timing);
  const int64_t end_ms = note.end_ms(timing);
  // Only duration hold bodies expire against end_ms. HoldStart / mid-stars / taps use start.
  const bool duration_hold =
      is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type);
  const int64_t time_anchor = duration_hold ? end_ms : start_ms;

  if (state == PreviewNoteVisualState::Holding) {
    return preview_time_ms > end_ms + config_.post_miss_visible_ms;
  }

  if (state == PreviewNoteVisualState::AutoHit) {
    return preview_time_ms > time_anchor + config_.auto_hit_feedback_ms;
  }

  if (state == PreviewNoteVisualState::Missed) {
    return preview_time_ms > time_anchor + config_.post_miss_visible_ms;
  }

  return preview_time_ms > time_anchor + config_.miss_window_ms + config_.post_miss_visible_ms;
}

bool PreviewSnapshotBuilder::is_concurrent_line_visible(const ConcurrentLineNote& line,
                                                        int64_t preview_time_ms) const {
  const int64_t lead = spawn_lead_ms();
  return preview_time_ms >= line.milliseconds - lead &&
         preview_time_ms <= line.milliseconds + config_.post_miss_visible_ms;
}

void PreviewSnapshotBuilder::apply_gimmick_position(PreviewNoteInstance& instance,
                                                    const NotationNote& note) const {
  // Same resolve_end_lane_span as edit draw (ScratchHold / JumpScratch / body).
  if (is_scratch_hold_body(note.note_type) || is_jump_scratch(note.gimmick_type)) {
    const auto [lane, width] = resolve_end_lane_span(note);
    instance.apply_jump_scratch(true, lane, lane + width - 1);
  } else {
    instance.apply_jump_scratch(false, 0, 0);
  }
}

int32_t PreviewSnapshotBuilder::resolve_active_lane_count(
    const std::vector<PreviewSplitLaneInstance>& splits, int32_t base_lane_count) const {
  if (splits.empty()) {
    return base_lane_count;
  }
  return splits.back().effective_lane_count;
}

int64_t PreviewSnapshotBuilder::spawn_lead_ms() const {
  return static_cast<int64_t>(position_calculator_.move_seconds() * 1000.0f);
}

int64_t PreviewSnapshotBuilder::tail_ms(const ChartNoteIndex& index) const {
  return std::max(index.max_hold_span_ms() + config_.post_miss_visible_ms,
                  config_.preview_tail_fallback_ms);
}

size_t PreviewSnapshotBuilder::size_delta(size_t a, size_t b) noexcept {
  return (a > b) ? (a - b) : (b - a);
}

void PreviewSnapshotBuilder::ensure_note_lookup(const std::vector<NotationNote>& notes,
                                                uint64_t revision) const {
  if (cached_lookup_revision_ == revision && cached_notes_ == &notes &&
      note_id_to_chart_index_.size() == notes.size()) {
    return;
  }

  note_id_to_chart_index_.clear();
  note_id_to_chart_index_.reserve(notes.size());
  for (size_t i = 0; i < notes.size(); ++i) {
    note_id_to_chart_index_[notes[i].id] = i;
  }
  cached_lookup_revision_ = revision;
  cached_notes_ = &notes;
}

void PreviewSnapshotBuilder::ensure_combo_hits(const std::vector<NotationNote>& notes,
                                               const MusicTiming& timing,
                                               uint64_t revision) const {
  if (cached_combo_revision_ == revision && cached_combo_notes_ == &notes) {
    return;
  }
  collect_preview_combo_hits(notes, timing, cached_combo_hits_);
  cached_combo_revision_ = revision;
  cached_combo_notes_ = &notes;
}

const NotationNote* PreviewSnapshotBuilder::lookup_note(int32_t note_id) const {
  if (cached_notes_ == nullptr) {
    return nullptr;
  }
  const auto it = note_id_to_chart_index_.find(note_id);
  if (it == note_id_to_chart_index_.end() || it->second >= cached_notes_->size()) {
    return nullptr;
  }
  return &(*cached_notes_)[it->second];
}

void PreviewSnapshotBuilder::fill_note_instance(PreviewNoteInstance& instance,
                                                const NotationNote& note,
                                                int64_t preview_time_ms,
                                                const MusicTiming& timing,
                                                int32_t lane_count) const {
  const auto judged = auto_judge_.evaluate(note, preview_time_ms, timing);

  instance.apply_identity(note.id, note.note_type, note.gimmick_type, note.lane, note.width,
                          note.end_lane(), note.start_ms(timing), note.end_ms(timing),
                          note.scratch_length);
  instance.apply_visual(judged.visual_state,
                        judged.consumed && judged.visual_state == PreviewNoteVisualState::AutoHit);

  const float pos_y =
      position_calculator_.calculate_position_y(instance.start_ms, preview_time_ms);
  const float width_world = position_calculator_.note_width(lane_count, note.width);
  const float pos_x = position_calculator_.note_position_x(note.lane, lane_count, note.width);

  float hold_body_length = 0.0f;
  float height_world = 0.25f;
  if (is_hold_start(note.note_type) || is_hold_body(note.note_type)) {
    hold_body_length = position_calculator_.calculate_hold_length(
        instance.start_ms, instance.end_ms, preview_time_ms);
    height_world = std::max(0.15f, hold_body_length);
  }

  instance.apply_layout(pos_x, pos_y, width_world, height_world, hold_body_length);
  apply_gimmick_position(instance, note);
}

void PreviewSnapshotBuilder::fill_concurrent_line_instance(
    PreviewConcurrentLineInstance& instance, const ConcurrentLineNote& line,
    int64_t preview_time_ms) const {
  instance.apply_identity(line.milliseconds, line.start_lane, line.width);
  instance.apply_layout(
      position_calculator_.calculate_position_y(line.milliseconds, preview_time_ms));
}

void PreviewSnapshotBuilder::rebuild_split_lanes(PreviewSnapshot& out,
                                                 const std::vector<NotationNote>& notes,
                                                 const MusicTiming& timing,
                                                 const ChartNoteIndex& index,
                                                 int64_t preview_time_ms) const {
  (void)notes;
  // Appear starts at beat - animationStart; include future splits within that window.
  // Lower bound also drops expired splits: preview - max_split_span - disappear.
  query_preview_split_candidates(index, preview_time_ms, config_, split_buffer_);
  out.clear_split_lanes_keep_capacity();
  out.reserve(0, 0, split_buffer_.size());

  for (int32_t note_id : split_buffer_) {
    const NotationNote* note = lookup_note(note_id);
    if (note == nullptr || !split_lane_simulator_.is_split_active(*note, timing, preview_time_ms)) {
      continue;
    }
    PreviewSplitLaneInstance instance;
    split_lane_simulator_.fill_instance(instance, *note, timing, preview_time_ms);
    out.upsert_split_lane(instance);
  }

  std::sort(out.split_lanes.begin(), out.split_lanes.end(),
            [](const PreviewSplitLaneInstance& a, const PreviewSplitLaneInstance& b) {
              return a.start_ms < b.start_ms;
            });
  out.rebuild_split_lane_index();
  out.active_lane_count = resolve_active_lane_count(out.split_lanes, config_.lane_count);
}

void PreviewSnapshotBuilder::update_split_lanes_incremental(
    PreviewSnapshot& inout, const std::vector<NotationNote>& notes, const MusicTiming& timing,
    const ChartNoteIndex& index, int64_t preview_time_ms) const {
  (void)notes;
  query_preview_split_candidates(index, preview_time_ms, config_, split_buffer_);
  visited_buffer_.assign(inout.split_lanes.size(), 0);

  for (int32_t note_id : split_buffer_) {
    const NotationNote* note = lookup_note(note_id);
    if (note == nullptr || !split_lane_simulator_.is_split_active(*note, timing, preview_time_ms)) {
      continue;
    }

    if (PreviewSplitLaneInstance* existing = inout.find_split_lane(note_id)) {
      const size_t idx = static_cast<size_t>(existing - inout.split_lanes.data());
      split_lane_simulator_.fill_instance(*existing, *note, timing, preview_time_ms);
      if (idx < visited_buffer_.size()) {
        visited_buffer_[idx] = 1;
      }
    } else {
      PreviewSplitLaneInstance instance;
      split_lane_simulator_.fill_instance(instance, *note, timing, preview_time_ms);
      inout.upsert_split_lane(instance);
      visited_buffer_.push_back(1);
    }
  }

  for (size_t i = inout.split_lanes.size(); i > 0; --i) {
    const size_t idx = i - 1;
    if (idx >= visited_buffer_.size() || visited_buffer_[idx] != 0) {
      continue;
    }
    const size_t last = inout.split_lanes.size() - 1;
    if (idx != last) {
      visited_buffer_[idx] = visited_buffer_[last];
    }
    visited_buffer_.pop_back();
    inout.remove_split_lane_at(idx);
  }

  std::sort(inout.split_lanes.begin(), inout.split_lanes.end(),
            [](const PreviewSplitLaneInstance& a, const PreviewSplitLaneInstance& b) {
              return a.start_ms < b.start_ms;
            });
  inout.rebuild_split_lane_index();
  inout.active_lane_count = resolve_active_lane_count(inout.split_lanes, config_.lane_count);
}

void PreviewSnapshotBuilder::rebuild_concurrent_lines(
    PreviewSnapshot& out, const std::vector<ConcurrentLineNote>& concurrent_lines,
    int64_t preview_time_ms) const {
  out.clear_concurrent_lines_keep_capacity();
  out.reserve(0, concurrent_lines.size(), 0);

  for (const auto& line : concurrent_lines) {
    if (!is_concurrent_line_visible(line, preview_time_ms)) {
      continue;
    }
    PreviewConcurrentLineInstance instance;
    fill_concurrent_line_instance(instance, line, preview_time_ms);
    out.upsert_concurrent_line(instance);
  }
}

void PreviewSnapshotBuilder::update_concurrent_lines_incremental(
    PreviewSnapshot& inout, const std::vector<ConcurrentLineNote>& concurrent_lines,
    int64_t preview_time_ms) const {
  visited_buffer_.assign(inout.concurrent_lines.size(), 0);

  for (const auto& line : concurrent_lines) {
    if (!is_concurrent_line_visible(line, preview_time_ms)) {
      continue;
    }

    if (PreviewConcurrentLineInstance* existing =
            inout.find_concurrent_line(line.milliseconds, line.start_lane)) {
      const size_t idx = static_cast<size_t>(existing - inout.concurrent_lines.data());
      fill_concurrent_line_instance(*existing, line, preview_time_ms);
      if (idx < visited_buffer_.size()) {
        visited_buffer_[idx] = 1;
      }
    } else {
      PreviewConcurrentLineInstance instance;
      fill_concurrent_line_instance(instance, line, preview_time_ms);
      inout.upsert_concurrent_line(instance);
      visited_buffer_.push_back(1);
    }
  }

  for (size_t i = inout.concurrent_lines.size(); i > 0; --i) {
    const size_t idx = i - 1;
    if (idx >= visited_buffer_.size() || visited_buffer_[idx] != 0) {
      continue;
    }
    const size_t last = inout.concurrent_lines.size() - 1;
    if (idx != last) {
      visited_buffer_[idx] = visited_buffer_[last];
    }
    visited_buffer_.pop_back();
    inout.remove_concurrent_line_at(idx);
  }
}

void PreviewSnapshotBuilder::rebuild_notes(PreviewSnapshot& out, int64_t preview_time_ms,
                                           const MusicTiming& timing,
                                           const ChartNoteIndex& index, int32_t lane_count,
                                           size_t concurrent_capacity) const {
  out.clear_notes_keep_capacity();
  index.query_candidates(preview_time_ms, spawn_lead_ms(), tail_ms(index), candidate_buffer_);
  out.reserve(candidate_buffer_.size(), concurrent_capacity, out.split_lanes.capacity());

  for (int32_t note_id : candidate_buffer_) {
    const NotationNote* note = lookup_note(note_id);
    if (note == nullptr) {
      continue;
    }
    // Split gimmicks only drive split_lanes / STAGE_COVER — never a playable tap sprite.
    if (is_split_lane_gimmick(note->gimmick_type)) {
      continue;
    }
    if (!is_spawn_visible(*note, preview_time_ms, timing)) {
      continue;
    }

    const auto judged = auto_judge_.evaluate(*note, preview_time_ms, timing);
    if (is_expired(*note, preview_time_ms, judged.visual_state, timing)) {
      continue;
    }

    PreviewNoteInstance instance;
    fill_note_instance(instance, *note, preview_time_ms, timing, lane_count);
    out.upsert_note(instance);
  }
}

void PreviewSnapshotBuilder::update_notes_incremental(PreviewSnapshot& inout,
                                                      int64_t preview_time_ms,
                                                      const MusicTiming& timing,
                                                      const ChartNoteIndex& index,
                                                      int32_t lane_count) const {
  index.query_candidates(preview_time_ms, spawn_lead_ms(), tail_ms(index), candidate_buffer_);
  visited_buffer_.assign(inout.notes.size(), 0);

  for (int32_t note_id : candidate_buffer_) {
    const NotationNote* note = lookup_note(note_id);
    if (note == nullptr) {
      continue;
    }
    if (is_split_lane_gimmick(note->gimmick_type)) {
      continue;
    }
    if (!is_spawn_visible(*note, preview_time_ms, timing)) {
      continue;
    }

    const auto judged = auto_judge_.evaluate(*note, preview_time_ms, timing);
    if (is_expired(*note, preview_time_ms, judged.visual_state, timing)) {
      continue;
    }

    if (PreviewNoteInstance* existing = inout.find_note(note_id)) {
      const size_t index_in_snap = static_cast<size_t>(existing - inout.notes.data());
      fill_note_instance(*existing, *note, preview_time_ms, timing, lane_count);
      if (index_in_snap < visited_buffer_.size()) {
        visited_buffer_[index_in_snap] = 1;
      }
    } else {
      PreviewNoteInstance instance;
      fill_note_instance(instance, *note, preview_time_ms, timing, lane_count);
      inout.upsert_note(instance);
      visited_buffer_.push_back(1);
    }
  }

  for (size_t i = inout.notes.size(); i > 0; --i) {
    const size_t idx = i - 1;
    if (idx >= visited_buffer_.size() || visited_buffer_[idx] != 0) {
      continue;
    }
    const size_t last = inout.notes.size() - 1;
    if (idx != last) {
      visited_buffer_[idx] = visited_buffer_[last];
    }
    visited_buffer_.pop_back();
    inout.remove_note_at(idx);
  }
}

SnapshotUpdateStrategy PreviewSnapshotBuilder::choose_strategy(
    const SnapshotDiffEstimate& estimate) const {
  if (estimate.previous_visible_count == 0) {
    return SnapshotUpdateStrategy::FullRebuild;
  }

  if (std::llabs(estimate.time_delta_ms) > config_.incremental_max_time_delta_ms) {
    return SnapshotUpdateStrategy::FullRebuild;
  }

  if (estimate.estimated_churn_ratio > config_.incremental_churn_ratio_threshold) {
    return SnapshotUpdateStrategy::FullRebuild;
  }

  return SnapshotUpdateStrategy::IncrementalPatch;
}

SnapshotDiffEstimate PreviewSnapshotBuilder::estimate_diff(
    const PreviewSnapshot& previous, const ChartNoteIndex& index,
    const std::vector<ConcurrentLineNote>& concurrent_lines, int64_t preview_time_ms,
    uint64_t revision) const {
  SnapshotDiffEstimate estimate;
  estimate.previous_timeline_ms = previous.timeline_ms;
  estimate.next_timeline_ms = preview_time_ms;
  estimate.time_delta_ms = preview_time_ms - previous.timeline_ms;
  estimate.previous_note_count = previous.notes.size();
  estimate.previous_split_count = previous.split_lanes.size();
  estimate.previous_concurrent_count = previous.concurrent_lines.size();
  estimate.previous_visible_count = estimate.previous_note_count +
                                    estimate.previous_split_count +
                                    estimate.previous_concurrent_count;
  estimate.revision_changed = previous.revision != revision;

  index.query_candidates(preview_time_ms, spawn_lead_ms(), tail_ms(index), candidate_buffer_);
  estimate.note_candidate_count = candidate_buffer_.size();

  query_preview_split_candidates(index, preview_time_ms, config_, split_buffer_);
  estimate.split_candidate_count = split_buffer_.size();

  size_t concurrent_visible = 0;
  for (const auto& line : concurrent_lines) {
    if (is_concurrent_line_visible(line, preview_time_ms)) {
      ++concurrent_visible;
    }
  }
  estimate.concurrent_candidate_count = concurrent_visible;

  estimate.candidate_count = estimate.note_candidate_count + estimate.split_candidate_count +
                             estimate.concurrent_candidate_count;

  estimate.estimated_churn =
      size_delta(estimate.previous_note_count, estimate.note_candidate_count) +
      size_delta(estimate.previous_split_count, estimate.split_candidate_count) +
      size_delta(estimate.previous_concurrent_count, estimate.concurrent_candidate_count);

  const size_t denom = std::max(estimate.previous_visible_count, estimate.candidate_count);
  estimate.estimated_churn_ratio =
      denom == 0 ? 1.0f
                 : static_cast<float>(estimate.estimated_churn) / static_cast<float>(denom);

  if (estimate.revision_changed) {
    estimate.estimated_churn_ratio =
        std::min(1.0f, estimate.estimated_churn_ratio + 0.15f);
  }

  estimate.strategy = choose_strategy(estimate);
  estimate.prefer_incremental =
      estimate.strategy == SnapshotUpdateStrategy::IncrementalPatch;
  return estimate;
}

void PreviewSnapshotBuilder::build_into(
    PreviewSnapshot& out, const std::vector<NotationNote>& notes, const MusicTiming& timing,
    const std::vector<ConcurrentLineNote>& concurrent_lines, const ChartNoteIndex& index,
    int64_t preview_time_ms, PreviewPlaybackState playback_state, uint64_t revision) const {
  ensure_note_lookup(notes, revision);

  out.timeline_ms = preview_time_ms;
  out.timeline_us = wds::common::ms_to_us(preview_time_ms).count();
  out.bpm = timing.bpm;
  out.ticks_per_quarter = timing.ticks_per_quarter;
  out.playback_state = playback_state;
  out.revision = revision;
  out.last_update_strategy = SnapshotUpdateStrategy::FullRebuild;

  rebuild_split_lanes(out, notes, timing, index, preview_time_ms);
  rebuild_concurrent_lines(out, concurrent_lines, preview_time_ms);
  rebuild_notes(out, preview_time_ms, timing, index, out.active_lane_count,
                concurrent_lines.size());

  ensure_combo_hits(notes, timing, revision);
  const PreviewComboState combo = combo_from_sorted_hits(cached_combo_hits_, preview_time_ms);
  out.combo_count = combo.combo;
  out.last_judge_ms = combo.last_judge_ms;
}

void PreviewSnapshotBuilder::update_incremental(
    PreviewSnapshot& inout, const std::vector<NotationNote>& notes, const MusicTiming& timing,
    const std::vector<ConcurrentLineNote>& concurrent_lines, const ChartNoteIndex& index,
    int64_t preview_time_ms, PreviewPlaybackState playback_state, uint64_t revision) const {
  ensure_note_lookup(notes, revision);

  inout.timeline_ms = preview_time_ms;
  inout.timeline_us = wds::common::ms_to_us(preview_time_ms).count();
  inout.bpm = timing.bpm;
  inout.ticks_per_quarter = timing.ticks_per_quarter;
  inout.playback_state = playback_state;
  inout.revision = revision;
  inout.last_update_strategy = SnapshotUpdateStrategy::IncrementalPatch;

  // Split lanes first: active_lane_count affects note layout.
  update_split_lanes_incremental(inout, notes, timing, index, preview_time_ms);
  update_concurrent_lines_incremental(inout, concurrent_lines, preview_time_ms);
  update_notes_incremental(inout, preview_time_ms, timing, index, inout.active_lane_count);

  ensure_combo_hits(notes, timing, revision);
  const PreviewComboState combo = combo_from_sorted_hits(cached_combo_hits_, preview_time_ms);
  inout.combo_count = combo.combo;
  inout.last_judge_ms = combo.last_judge_ms;
}

void PreviewSnapshotBuilder::rebuild_or_update(
    PreviewSnapshot& inout, const std::vector<NotationNote>& notes, const MusicTiming& timing,
    const std::vector<ConcurrentLineNote>& concurrent_lines, const ChartNoteIndex& index,
    int64_t preview_time_ms, PreviewPlaybackState playback_state, uint64_t revision) const {
  const SnapshotDiffEstimate estimate =
      estimate_diff(inout, index, concurrent_lines, preview_time_ms, revision);

  if (estimate.strategy == SnapshotUpdateStrategy::IncrementalPatch) {
    update_incremental(inout, notes, timing, concurrent_lines, index, preview_time_ms,
                       playback_state, revision);
  } else {
    build_into(inout, notes, timing, concurrent_lines, index, preview_time_ms, playback_state,
               revision);
  }
}

PreviewSnapshot PreviewSnapshotBuilder::build(const NotationChart& chart,
                                              const ChartNoteIndex& index,
                                              int64_t preview_time_ms,
                                              PreviewPlaybackState playback_state,
                                              uint64_t revision) const {
  return build(chart.notes, chart.timing, chart.concurrent_lines, index, preview_time_ms,
               playback_state, revision);
}

PreviewSnapshot PreviewSnapshotBuilder::build(
    const std::vector<NotationNote>& notes, const MusicTiming& timing,
    const std::vector<ConcurrentLineNote>& concurrent_lines, const ChartNoteIndex& index,
    int64_t preview_time_ms, PreviewPlaybackState playback_state, uint64_t revision) const {
  PreviewSnapshot snapshot;
  build_into(snapshot, notes, timing, concurrent_lines, index, preview_time_ms, playback_state,
             revision);
  return snapshot;
}

}  // namespace wds::chart_editor
