#include <wds/core/preview_snapshot.hpp>

namespace wds::chart_editor {

void PreviewNoteInstance::apply_identity(int32_t id, NoteType type, GimmickType gimmick,
                                         int32_t note_lane, int32_t note_width,
                                         int32_t note_end_lane, int64_t note_start_ms,
                                         int64_t note_end_ms,
                                         int32_t note_scratch_length) noexcept {
  note_id = id;
  note_type = type;
  gimmick_type = gimmick;
  lane = note_lane;
  width = note_width;
  end_lane = note_end_lane;
  start_ms = note_start_ms;
  end_ms = note_end_ms;
  scratch_length = note_scratch_length;
}

void PreviewNoteInstance::apply_layout(float x, float y, float world_width, float world_height,
                                       float hold_length) noexcept {
  pos_x = x;
  pos_y = y;
  width_world = world_width;
  height_world = world_height;
  hold_body_length = hold_length;
}

void PreviewNoteInstance::apply_visual(PreviewNoteVisualState state,
                                       bool grayed_out) noexcept {
  visual_state = state;
  is_grayed_out = grayed_out;
}

void PreviewNoteInstance::apply_jump_scratch(bool enabled, int32_t lane_from,
                                             int32_t lane_to) noexcept {
  uses_jump_scratch_position = enabled;
  jump_scratch_lane_from = lane_from;
  jump_scratch_lane_to = lane_to;
}

void PreviewNoteInstance::assign(const PreviewNoteInstance& other) noexcept {
  if (this == &other) {
    return;
  }
  *this = other;
}

void PreviewSplitLaneInstance::apply_identity(int32_t source_id, int32_t count,
                                              SplitLaneType type, int32_t value,
                                              int64_t note_start_ms,
                                              int64_t note_end_ms) noexcept {
  source_note_id = source_id;
  split_count = count;
  split_lane_type = type;
  scratch_length = value;
  start_ms = note_start_ms;
  end_ms = note_end_ms;
}

void PreviewSplitLaneInstance::apply_state(bool show, bool continued,
                                           int32_t lane_count) noexcept {
  should_show = show;
  is_continued = continued;
  effective_lane_count = lane_count;
}

void PreviewSplitLaneInstance::apply_animation(float line_alpha, float percent_start,
                                               float percent_end, float cover_alpha,
                                               int32_t anim_phase) noexcept {
  split_line_alpha = line_alpha;
  split_percent_start = percent_start;
  split_percent_end = percent_end;
  stage_cover_alpha = cover_alpha;
  split_anim_phase = anim_phase;
}

void PreviewSplitLaneInstance::assign(const PreviewSplitLaneInstance& other) noexcept {
  if (this == &other) {
    return;
  }
  *this = other;
}

void PreviewConcurrentLineInstance::apply_identity(int64_t ms, int32_t lane,
                                                   int32_t line_width) noexcept {
  milliseconds = ms;
  start_lane = lane;
  width = line_width;
}

void PreviewConcurrentLineInstance::apply_layout(float y) noexcept { pos_y = y; }

void PreviewConcurrentLineInstance::assign(const PreviewConcurrentLineInstance& other) noexcept {
  if (this == &other) {
    return;
  }
  *this = other;
}

uint64_t PreviewSnapshot::concurrent_line_key(int64_t milliseconds,
                                              int32_t start_lane) noexcept {
  return (static_cast<uint64_t>(milliseconds) << 16) ^
         static_cast<uint64_t>(static_cast<uint32_t>(start_lane));
}

void PreviewSnapshot::clear_keep_capacity() noexcept {
  clear_notes_keep_capacity();
  clear_concurrent_lines_keep_capacity();
  clear_split_lanes_keep_capacity();
  active_lane_count = 0;
}

void PreviewSnapshot::reserve(size_t note_capacity, size_t line_capacity,
                              size_t split_capacity) {
  notes.reserve(note_capacity);
  note_index_.reserve(note_capacity);
  if (line_capacity > 0) {
    concurrent_lines.reserve(line_capacity);
    concurrent_line_index_.reserve(line_capacity);
  }
  if (split_capacity > 0) {
    split_lanes.reserve(split_capacity);
    split_lane_index_.reserve(split_capacity);
  }
}

PreviewNoteInstance* PreviewSnapshot::find_note(int32_t note_id) noexcept {
  const auto it = note_index_.find(note_id);
  if (it == note_index_.end() || it->second >= notes.size()) {
    return nullptr;
  }
  return &notes[it->second];
}

const PreviewNoteInstance* PreviewSnapshot::find_note(int32_t note_id) const noexcept {
  const auto it = note_index_.find(note_id);
  if (it == note_index_.end() || it->second >= notes.size()) {
    return nullptr;
  }
  return &notes[it->second];
}

PreviewNoteInstance& PreviewSnapshot::upsert_note(const PreviewNoteInstance& instance) {
  if (PreviewNoteInstance* existing = find_note(instance.note_id)) {
    existing->assign(instance);
    return *existing;
  }

  const size_t index = notes.size();
  notes.push_back(instance);
  note_index_[instance.note_id] = index;
  return notes.back();
}

bool PreviewSnapshot::remove_note(int32_t note_id) noexcept {
  const auto it = note_index_.find(note_id);
  if (it == note_index_.end()) {
    return false;
  }
  return remove_note_at(it->second);
}

bool PreviewSnapshot::remove_note_at(size_t index) noexcept {
  if (index >= notes.size()) {
    return false;
  }

  const int32_t removed_id = notes[index].note_id;
  note_index_.erase(removed_id);

  if (index != notes.size() - 1) {
    notes[index] = notes.back();
    note_index_[notes[index].note_id] = index;
  }
  notes.pop_back();
  return true;
}

PreviewSplitLaneInstance* PreviewSnapshot::find_split_lane(int32_t source_note_id) noexcept {
  const auto it = split_lane_index_.find(source_note_id);
  if (it == split_lane_index_.end() || it->second >= split_lanes.size()) {
    return nullptr;
  }
  return &split_lanes[it->second];
}

const PreviewSplitLaneInstance* PreviewSnapshot::find_split_lane(
    int32_t source_note_id) const noexcept {
  const auto it = split_lane_index_.find(source_note_id);
  if (it == split_lane_index_.end() || it->second >= split_lanes.size()) {
    return nullptr;
  }
  return &split_lanes[it->second];
}

PreviewSplitLaneInstance& PreviewSnapshot::upsert_split_lane(
    const PreviewSplitLaneInstance& instance) {
  if (PreviewSplitLaneInstance* existing = find_split_lane(instance.source_note_id)) {
    existing->assign(instance);
    return *existing;
  }

  const size_t index = split_lanes.size();
  split_lanes.push_back(instance);
  split_lane_index_[instance.source_note_id] = index;
  return split_lanes.back();
}

bool PreviewSnapshot::remove_split_lane(int32_t source_note_id) noexcept {
  const auto it = split_lane_index_.find(source_note_id);
  if (it == split_lane_index_.end()) {
    return false;
  }
  return remove_split_lane_at(it->second);
}

bool PreviewSnapshot::remove_split_lane_at(size_t index) noexcept {
  if (index >= split_lanes.size()) {
    return false;
  }

  const int32_t removed_id = split_lanes[index].source_note_id;
  split_lane_index_.erase(removed_id);

  if (index != split_lanes.size() - 1) {
    split_lanes[index] = split_lanes.back();
    split_lane_index_[split_lanes[index].source_note_id] = index;
  }
  split_lanes.pop_back();
  return true;
}

PreviewConcurrentLineInstance* PreviewSnapshot::find_concurrent_line(
    int64_t milliseconds, int32_t start_lane) noexcept {
  const auto it = concurrent_line_index_.find(concurrent_line_key(milliseconds, start_lane));
  if (it == concurrent_line_index_.end() || it->second >= concurrent_lines.size()) {
    return nullptr;
  }
  return &concurrent_lines[it->second];
}

const PreviewConcurrentLineInstance* PreviewSnapshot::find_concurrent_line(
    int64_t milliseconds, int32_t start_lane) const noexcept {
  const auto it = concurrent_line_index_.find(concurrent_line_key(milliseconds, start_lane));
  if (it == concurrent_line_index_.end() || it->second >= concurrent_lines.size()) {
    return nullptr;
  }
  return &concurrent_lines[it->second];
}

PreviewConcurrentLineInstance& PreviewSnapshot::upsert_concurrent_line(
    const PreviewConcurrentLineInstance& instance) {
  if (PreviewConcurrentLineInstance* existing =
          find_concurrent_line(instance.milliseconds, instance.start_lane)) {
    existing->assign(instance);
    return *existing;
  }

  const size_t index = concurrent_lines.size();
  concurrent_lines.push_back(instance);
  concurrent_line_index_[concurrent_line_key(instance.milliseconds, instance.start_lane)] =
      index;
  return concurrent_lines.back();
}

bool PreviewSnapshot::remove_concurrent_line(int64_t milliseconds,
                                             int32_t start_lane) noexcept {
  const auto it = concurrent_line_index_.find(concurrent_line_key(milliseconds, start_lane));
  if (it == concurrent_line_index_.end()) {
    return false;
  }
  return remove_concurrent_line_at(it->second);
}

bool PreviewSnapshot::remove_concurrent_line_at(size_t index) noexcept {
  if (index >= concurrent_lines.size()) {
    return false;
  }

  const auto& removed = concurrent_lines[index];
  concurrent_line_index_.erase(concurrent_line_key(removed.milliseconds, removed.start_lane));

  if (index != concurrent_lines.size() - 1) {
    concurrent_lines[index] = concurrent_lines.back();
    concurrent_line_index_[concurrent_line_key(concurrent_lines[index].milliseconds,
                                               concurrent_lines[index].start_lane)] = index;
  }
  concurrent_lines.pop_back();
  return true;
}

void PreviewSnapshot::clear_notes_keep_capacity() noexcept {
  notes.clear();
  note_index_.clear();
}

void PreviewSnapshot::clear_concurrent_lines_keep_capacity() noexcept {
  concurrent_lines.clear();
  concurrent_line_index_.clear();
}

void PreviewSnapshot::clear_split_lanes_keep_capacity() noexcept {
  split_lanes.clear();
  split_lane_index_.clear();
}

void PreviewSnapshot::rebuild_note_index() {
  note_index_.clear();
  note_index_.reserve(notes.size());
  for (size_t i = 0; i < notes.size(); ++i) {
    note_index_[notes[i].note_id] = i;
  }
}

void PreviewSnapshot::rebuild_split_lane_index() {
  split_lane_index_.clear();
  split_lane_index_.reserve(split_lanes.size());
  for (size_t i = 0; i < split_lanes.size(); ++i) {
    split_lane_index_[split_lanes[i].source_note_id] = i;
  }
}

void PreviewSnapshot::rebuild_concurrent_line_index() {
  concurrent_line_index_.clear();
  concurrent_line_index_.reserve(concurrent_lines.size());
  for (size_t i = 0; i < concurrent_lines.size(); ++i) {
    concurrent_line_index_[concurrent_line_key(concurrent_lines[i].milliseconds,
                                               concurrent_lines[i].start_lane)] = i;
  }
}

}  // namespace wds::chart_editor
