#pragma once

#include <wds/core/chart_serializer.hpp>
#include <wds/core/edit_history.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/preview_config.hpp>
#include <wds/core/preview_snapshot.hpp>
#include <wds/core/preview_snapshot_builder.hpp>
#include <wds/core/project.hpp>
#include <wds/core/seekable_clock.hpp>
#include <wds/core/sus_chart.hpp>
#include <wds/core/types.hpp>

#include <wds/common/time.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wds::chart_editor {

using PreviewSnapshotCallback = std::function<void(const PreviewSnapshot&)>;

// High-level chart editor + preview coordinator.
//
// Design goals (vs original Unity runtime):
// - Auto mode only
// - Editor timeline and preview share SeekableClock
// - Any-time pause/play/seek
// - Deterministic rebuild at arbitrary time (rollback safe)
// - Incremental snapshot patching when frame-to-frame delta is small
//
// Chart sources:
// - .wdsproject → project entry (music + offset + chart path); chart must be .wdschart
// - .wdschart → Editable (save/load native format)
// - official CSV → OfficialPreviewOnly (preview + export only; mutations rejected)
class ChartEditorEngine {
 public:
  explicit ChartEditorEngine(PreviewConfig preview_config = {});

  ChartDocument& document() noexcept { return document_; }
  const ChartDocument& document() const noexcept { return document_; }
  EditHistory& history() noexcept { return history_; }
  const EditHistory& history() const noexcept { return history_; }

  PreviewConfig& preview_config() noexcept { return preview_config_; }
  const PreviewConfig& preview_config() const noexcept { return preview_config_; }

  void set_preview_config(PreviewConfig config);
  void set_snapshot_callback(PreviewSnapshotCallback callback);

  // Edit-area visible window (ms). Kept for API compatibility; lead-in time
  // mapping is identity (playhead always on judgeline, no ease).
  void set_preview_lead_in_visible_ms(int64_t visible_ms) noexcept;
  int64_t preview_lead_in_visible_ms() const noexcept { return preview_lead_in_visible_ms_; }

  // Load/replace full chart (in-memory). Default editable.
  void load_chart(const NotationChart& chart,
                  ChartEditMode mode = ChartEditMode::Editable);

  bool is_editable() const noexcept { return document_.is_editable(); }
  bool is_read_only() const noexcept { return document_.is_read_only(); }

  // Explicit disk I/O — the only path that touches the filesystem.
  // save_to_file writes .wdschart only; rejected when OfficialPreviewOnly.
  SerializeResult save_to_file(const std::string& path);
  SerializeResult load_from_file(const std::string& path);

  // .wdsproject entry: load chart, then apply project CHART_DELAY_MS (song-level).
  // music_path is returned via out_project for the UI/audio layer; core does not
  // open audio files.
  SerializeResult load_project_from_file(const std::string& project_path,
                                         WdsProject* out_project = nullptr);

  // Save editable chart + project entry. Paths in `project` are relativized to
  // project_path. Song CHART_DELAY_MS is taken from document timing (not written
  // into the .wdschart).
  SerializeResult save_project_to_file(const std::string& project_path, WdsProject project);

  // Import official Sirius/WDS CSV for preview (+ optional music_config DelaySeconds).
  // Document becomes OfficialPreviewOnly — editing disabled. File HoldEighth
  // (type 900) rows are kept; eighths are not recomputed.
  SerializeResult load_official_from_file(const std::string& chart_path,
                                          const std::string& music_config_path = {});

  // Import SUS chart (metadata + notes). OfficialPreviewOnly.
  // out_meta receives WAVE / WAVEOFFSET / title when non-null.
  // out_warnings receives lossy-import messages when non-null.
  SerializeResult load_sus_from_file(const std::string& path, SusChartMetadata* out_meta = nullptr,
                                     std::vector<std::string>* out_warnings = nullptr);

  // Auto-detect .wdschart vs official .csv vs .sus.
  SerializeResult load_auto_from_file(const std::string& path,
                                      const std::string& music_config_path = {});

  // Export as official CSV (native editable charts only).
  // Does not clear dirty / does not switch edit mode.
  SerializeResult export_official_to_file(const std::string& path);

  // Export as SUS (native editable charts only).
  SerializeResult export_sus_to_file(const std::string& path, const SusChartSaveOptions& options);

  bool is_dirty() const noexcept { return document_.is_dirty(); }
  void mark_saved() noexcept { document_.mark_saved(); }

  // Chart mutations (rejected when read-only).
  int32_t add_note(NotationNote note);
  bool update_note(int32_t id, const NotationNote& note);
  bool remove_note(int32_t id);
  bool set_notes(std::vector<NotationNote> notes);
  bool execute_command(std::unique_ptr<IEditCommand> command);
  bool undo();
  bool redo();

  // Timeline controls (compat / tests). Prefer apply_timeline() when an external
  // audio/UI transport owns the shared clock.
  void seek(int64_t time_ms);
  void play();

  // Apply an externally authored timeline snapshot, then rebuild preview.
  void apply_timeline(const wds::common::TimelineSnapshot& snap);

  // Advance preview clock when playing, then rebuild snapshot.
  void tick(int64_t delta_ms);

  // Force rebuild without advancing time (call after UI edits).
  const PreviewSnapshot& rebuild_snapshot();
  const PreviewSnapshot& snapshot() const noexcept { return snapshot_; }

  int64_t timeline_ms() const noexcept;
  // Sub-ms timeline for smooth edit-viewport scroll (avoids 1ms stair-step jitter).
  int64_t timeline_us() const noexcept;
  PreviewPlaybackState playback_state() const noexcept;

 private:
  const PreviewSnapshot& publish_snapshot();

  ChartDocument document_;
  EditHistory history_;
  SeekableClock clock_;
  PreviewConfig preview_config_;
  PreviewSnapshotBuilder snapshot_builder_;
  PreviewSnapshot snapshot_;
  PreviewSnapshotCallback snapshot_callback_;
  int64_t preview_lead_in_visible_ms_ = 0;
};

}  // namespace wds::chart_editor
