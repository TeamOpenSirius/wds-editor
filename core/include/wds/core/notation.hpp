#pragma once

#include <wds/core/chart_index.hpp>
#include <wds/core/types.hpp>

// chart_index.hpp is included for ChartNoteIndex member; it only forward-declares NotationNote.

#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

// timing_map.hpp helpers operate on MusicTiming::points.

namespace wds::chart_editor {

// BPM / meter authoring point. Tick 0 is mandatory and undeletable.
// has_bpm / has_meter mark which fields are authored labels (values are always
// filled — inherited fields mirror the previous authored change).
struct TimingPoint {
  int32_t tick = 0;
  double bpm = 120.0;
  int32_t numerator = 4;    // beats per bar
  int32_t denominator = 4;  // note value of one beat (4 = quarter)
  bool has_bpm = true;
  bool has_meter = true;
};

// TPQ upper bound keeps tpq*4*numerator (den=1, num<=32) inside int32.
inline constexpr int32_t kDefaultTicksPerQuarter = 480;
inline constexpr int32_t kMaxTicksPerQuarter = std::numeric_limits<int32_t>::max() / 128;

inline bool is_valid_ticks_per_quarter(int32_t tpq) noexcept {
  return tpq >= 1 && tpq <= kMaxTicksPerQuarter;
}

struct MusicTiming {
  double bpm = 120.0;
  // Tick-based authoring (preferred over absolute seconds for precision).
  int32_t ticks_per_quarter = kDefaultTicksPerQuarter;
  // Chart delay (ms): tick 0 maps to this wall-clock time, so the edit area
  // shows leading blank and notes hit after the song starts. Owned by the
  // project (.wdsproject CHART_DELAY_MS), not the chart file.
  int64_t offset_ms = 0;
  // BPM / meter changes. Always normalized to include a tick-0 anchor.
  // bpm above mirrors points[0].bpm after normalize_timing_points().
  std::vector<TimingPoint> points;
  // Milliseconds from tick 0 (excluding offset_ms) at each points[i].tick.
  // Filled by normalize_timing_points / tick↔ms; not part of the file format.
  mutable std::vector<double> prefix_ms;
};

// Editor note — field layout mirrors official CSV columns, but times stay in ticks:
//   start_tick, end_tick, note_type, lane, width, gimmick_type, scratch_length
// Official CSV uses seconds + 1-based lane; wdschart keeps ticks + 0-based lane.
struct NotationNote {
  int32_t id = kAutoNoteId;
  // Integer ticks only (grid-snapped). Never fractional.
  int32_t start_tick = 0;
  // 0 = no duration (official endTime -1). Hold body / split use end_tick >= start_tick.
  int32_t end_tick = 0;
  NoteType note_type = NoteType::Normal;
  int32_t lane = 0;   // 0-based (official leftLane is 1-based)
  int32_t width = 1;  // official laneLength
  GimmickType gimmick_type = GimmickType::None;
  // Official scratchLength: flick/scratch span; JumpScratch span; split
  // Addressable SplitEffects/{id} (fadeIn growth follows LineHight rotation).
  int32_t scratch_length = 0;

  int32_t end_lane() const noexcept { return lane + width - 1; }
  int64_t start_ms(const MusicTiming& timing) const;
  int64_t end_ms(const MusicTiming& timing) const;
};

// One item in ChartDocument::apply_note_updates. note.id is forced to id.
struct NoteUpdate {
  int32_t id = kAutoNoteId;
  NotationNote note;
};

struct ConcurrentLineNote {
  int64_t milliseconds = 0;
  int32_t start_lane = 0;
  int32_t width = 1;
};

struct NotationChart {
  MusicTiming timing;
  std::vector<NotationNote> notes;
  std::vector<ConcurrentLineNote> concurrent_lines;
};

// Mutable chart document used by the editor UI.
// All edits are in-memory only; no disk I/O (see ChartSerializer for save/load).
class ChartDocument {
 public:
  explicit ChartDocument(MusicTiming timing = {});

  const MusicTiming& timing() const noexcept { return timing_; }
  // Returns false when OfficialPreviewOnly.
  bool set_timing(MusicTiming timing);
  // Song-level chart delay. Allowed even in OfficialPreviewOnly (preview alignment).
  bool set_offset_ms(int64_t offset_ms);

  const std::vector<NotationNote>& notes() const noexcept { return notes_; }
  const std::vector<ConcurrentLineNote>& concurrent_lines() const noexcept {
    return concurrent_lines_;
  }
  const ChartNoteIndex& index() const noexcept { return index_; }

  ChartEditMode edit_mode() const noexcept { return edit_mode_; }
  void set_edit_mode(ChartEditMode mode) noexcept { edit_mode_ = mode; }
  bool is_editable() const noexcept { return edit_mode_ == ChartEditMode::Editable; }
  bool is_read_only() const noexcept { return !is_editable(); }

  bool is_dirty() const noexcept { return is_dirty_; }
  void mark_saved() noexcept { is_dirty_ = false; }

  // Mutations return failure / no-op when read-only.
  int32_t add_note(NotationNote note);  // -1 when rejected
  bool update_note(int32_t id, const NotationNote& note);
  // Atomic batch of update_note. Empty succeeds without bumping generation.
  // Rejects the whole batch (no mutation) on read-only, duplicate ids, or unknown ids.
  bool apply_note_updates(const std::vector<NoteUpdate>& updates);
  bool remove_note(int32_t id);
  std::optional<NotationNote> find_note(int32_t id) const;

  bool set_notes(std::vector<NotationNote> notes);
  bool set_concurrent_lines(std::vector<ConcurrentLineNote> lines);

  // Derive concurrent (sync) lines from notes; excludes hold bodies / eighths / mid-stars.
  // Only emitted when ≥2 notes share a judgment time (multi-press).
  // Allowed in read-only (derived view, not authoring).
  void rebuild_concurrent_lines();

  // Sort notes by time, reassign ids 0..N-1, rebuild indexes. Used before save.
  // Returns false when read-only (does not mutate).
  bool normalize_for_save();

  // Normalized copy suitable for disk export without mutating this document.
  NotationChart normalized_chart() const;

  NotationChart to_notation_chart() const;
  void load_from_chart(const NotationChart& chart,
                       ChartEditMode mode = ChartEditMode::Editable);

  int32_t next_note_id() const;

  // Bumped on every note/timing mutation (including update_note). Preview
  // snapshot / note-lookup caches key off this.
  uint64_t content_generation() const noexcept { return content_generation_; }

 private:
  void sort_notes_for_display();
  static bool compare_notes_for_save(const NotationNote& a, const NotationNote& b) noexcept;
  static void normalize_notes_inplace(std::vector<NotationNote>& notes);
  void rebuild_id_index();
  void rebuild_index();
  // Concurrent-line derivation only — no dirty / generation bump.
  void derive_concurrent_lines();
  void mark_dirty() noexcept {
    is_dirty_ = true;
    ++content_generation_;
  }

  MusicTiming timing_;
  std::vector<NotationNote> notes_;
  std::vector<ConcurrentLineNote> concurrent_lines_;
  std::unordered_map<int32_t, size_t> id_to_index_;
  ChartNoteIndex index_;
  int32_t next_id_ = 0;
  bool is_dirty_ = false;
  uint64_t content_generation_ = 0;
  ChartEditMode edit_mode_ = ChartEditMode::Editable;
};

int64_t tick_to_milliseconds(int32_t tick, const MusicTiming& timing);
// Rounded to nearest integer tick (notes are always integer-tick).
int32_t milliseconds_to_tick(int64_t ms, const MusicTiming& timing);

bool is_hold_family(NoteType type) noexcept;
bool is_hold_start(NoteType type) noexcept;
bool is_hold_body(NoteType type) noexcept;
// Hold / CriticalHold / Scratch*Hold — has a judged end note (Sirius HoldEnd).
bool is_hold_with_tail(NoteType type) noexcept;
// NontailHold* — duration only, no end note / no end hit VFX.
bool is_nontail_hold_body(NoteType type) noexcept;
// ScratchHold / ScratchCriticalHold / NontailScratch* bodies.
bool is_scratch_hold_body(NoteType type) noexcept;
bool is_tap_family(NoteType type) noexcept;

// Hold soft-judge notes (Sound / ScratchSound / HoldEighth). Not sync contributors.
// Only Sound / ScratchSound are visible mid-stars; HoldEighth has no sprite in Sirius.
bool is_hold_mid_star(NoteType type) noexcept;

// Sirius SyncLine: hold bodies / HoldEighth / Sound mid-stars do not contribute.
// Heads, flats, flicks, and hold tails do; lines emit only for multi-press (≥2).
bool contributes_to_concurrent_at_start(NoteType type) noexcept;
bool contributes_to_concurrent_at_end(NoteType type) noexcept;

// Rebuild sync lines from notes (Sirius addSyncLine rules; multi-press only).
std::vector<ConcurrentLineNote> build_concurrent_lines(
    const std::vector<NotationNote>& notes, const MusicTiming& timing);

// Hold body soft-judge times: chart mid-stars (HoldEighth / Sound / ScratchSound)
// overlapping (head, tail), deduped. No synthetic eighth grid.
void collect_hold_body_judge_times(const NotationNote& hold,
                                   const std::vector<NotationNote>& notes,
                                   const MusicTiming& timing,
                                   std::vector<int64_t>& out_sorted_unique);

struct PreviewComboState {
  int32_t combo = 0;
  int64_t last_judge_ms = -1;
};

// All auto-preview combo hit times, sorted (seek-safe). Hold soft judges come
// from chart HoldEighth / Sound / ScratchSound only (absorbed once per hold).
void collect_preview_combo_hits(const std::vector<NotationNote>& notes, const MusicTiming& timing,
                                std::vector<int64_t>& out_sorted_hits);
PreviewComboState combo_from_sorted_hits(const std::vector<int64_t>& sorted_hits,
                                         int64_t preview_time_ms);

// Auto-preview combo up to preview_time_ms (seek-safe). Hold soft judges come
// from chart HoldEighth / Sound / ScratchSound only (absorbed once per hold).
PreviewComboState compute_preview_combo(const std::vector<NotationNote>& notes,
                                        const MusicTiming& timing,
                                        int64_t preview_time_ms);

}  // namespace wds::chart_editor
