#include <wds/core/chart_editor_engine.hpp>

#include <wds/core/edit_grid.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/sus_chart.hpp>

namespace wds::chart_editor {

ChartEditorEngine::ChartEditorEngine(PreviewConfig preview_config)
    : preview_config_(preview_config), snapshot_builder_(preview_config) {}

void ChartEditorEngine::set_preview_config(PreviewConfig config) {
  preview_config_ = config;
  snapshot_builder_.set_config(config);
  bump_revision();
  publish_snapshot();
}

void ChartEditorEngine::set_snapshot_callback(PreviewSnapshotCallback callback) {
  snapshot_callback_ = std::move(callback);
}

void ChartEditorEngine::set_preview_lead_in_visible_ms(int64_t visible_ms) noexcept {
  preview_lead_in_visible_ms_ = visible_ms < 0 ? 0 : visible_ms;
}

void ChartEditorEngine::load_chart(const NotationChart& chart, ChartEditMode mode) {
  document_.load_from_chart(chart, mode);
  if (!document_.is_read_only()) {
    repair_legacy_hold_heads(document_);
  }
  history_.clear();
  bump_revision();
  publish_snapshot();
}

SerializeResult ChartEditorEngine::save_to_file(const std::string& path) {
  if (document_.is_read_only()) {
    return {SerializeError::ReadOnly,
            "official preview charts cannot be saved as .wdschart; export CSV instead"};
  }

  // Normalize a copy for disk — never mutate the live document / history before I/O succeeds.
  NotationChart chart = document_.normalized_chart();
  const int64_t preserved_offset_ms = document_.timing().offset_ms;
  chart.timing.offset_ms = 0;

  const auto result = ChartSerializer::save_to_file(chart, path);
  if (result.error != SerializeError::Ok) {
    return result;
  }

  // Commit normalized IDs only after a successful write. History still refers to
  // pre-normalize ids, so clear it together with the in-memory renumber.
  chart.timing.offset_ms = preserved_offset_ms;
  document_.load_from_chart(chart, ChartEditMode::Editable);
  history_.clear();
  document_.mark_saved();
  bump_revision();
  publish_snapshot();
  return result;
}

SerializeResult ChartEditorEngine::load_from_file(const std::string& path) {
  NotationChart chart;
  const auto result = ChartSerializer::load_from_file(path, chart);
  if (result.error != SerializeError::Ok) {
    return result;
  }

  document_.load_from_chart(chart, ChartEditMode::Editable);
  repair_legacy_hold_heads(document_);
  history_.clear();
  bump_revision();
  publish_snapshot();
  return result;
}

SerializeResult ChartEditorEngine::load_project_from_file(const std::string& project_path,
                                                          WdsProject* out_project) {
  WdsProject project;
  const auto project_load = ProjectSerializer::load_from_file(project_path, project);
  if (project_load.error != SerializeError::Ok) {
    return project_load;
  }

  const std::string chart_path =
      ProjectSerializer::resolve_path(project_path, project.chart_path());
  const auto chart_load = load_from_file(chart_path);
  if (chart_load.error != SerializeError::Ok) {
    return chart_load;
  }

  // Project CHART_DELAY_MS is the chart-delay source of truth when opening a project.
  MusicTiming timing = document_.timing();
  timing.offset_ms = project.offset_ms;
  document_.set_timing(timing);
  bump_revision();
  publish_snapshot();
  document_.mark_saved();

  if (out_project != nullptr) {
    *out_project = project;
  }
  return {SerializeError::Ok, {}};
}

SerializeResult ChartEditorEngine::save_project_to_file(const std::string& project_path,
                                                        WdsProject project) {
  if (document_.is_read_only()) {
    return {SerializeError::ReadOnly,
            "official preview charts cannot be saved as a .wdsproject"};
  }
  if (project.chart_paths.empty() || project.chart_path().empty()) {
    return {SerializeError::ParseError, "project CHART path is required"};
  }

  project.offset_ms = document_.timing().offset_ms;

  const std::string chart_path =
      ProjectSerializer::resolve_path(project_path, project.chart_path());
  const auto chart_save = save_to_file(chart_path);
  if (chart_save.error != SerializeError::Ok) {
    return chart_save;
  }

  // Song chart delay stays in the project entry only (chart reload preserves memory offset).
  project.offset_ms = document_.timing().offset_ms;
  const auto project_save = ProjectSerializer::save_relativized(project, project_path);
  if (project_save.error != SerializeError::Ok) {
    return project_save;
  }

  document_.mark_saved();
  return {SerializeError::Ok, {}};
}

SerializeResult ChartEditorEngine::load_official_from_file(const std::string& chart_path,
                                                           const std::string& music_config_path) {
  NotationChart chart;
  const auto result =
      OfficialChartFormat::load_chart_with_music_config(chart_path, music_config_path, chart);
  if (result.error != SerializeError::Ok) {
    return result;
  }

  document_.load_from_chart(chart, ChartEditMode::OfficialPreviewOnly);
  history_.clear();
  bump_revision();
  publish_snapshot();
  return result;
}

SerializeResult ChartEditorEngine::load_sus_from_file(const std::string& path,
                                                      SusChartMetadata* out_meta,
                                                      std::vector<std::string>* out_warnings) {
  SusChartLoadResult loaded;
  const auto result = SusChartFormat::load_file(path, loaded);
  if (result.error != SerializeError::Ok) {
    return result;
  }
  document_.load_from_chart(loaded.chart, ChartEditMode::OfficialPreviewOnly);
  history_.clear();
  bump_revision();
  publish_snapshot();
  if (out_meta != nullptr) {
    *out_meta = std::move(loaded.meta);
  }
  if (out_warnings != nullptr) {
    *out_warnings = std::move(loaded.warnings);
  }
  return result;
}

SerializeResult ChartEditorEngine::load_auto_from_file(const std::string& path,
                                                       const std::string& music_config_path) {
  NotationChart chart;
  ChartEditMode mode = ChartEditMode::Editable;
  const auto result = ChartSerializer::load_auto(path, chart, music_config_path, &mode);
  if (result.error != SerializeError::Ok) {
    return result;
  }

  document_.load_from_chart(chart, mode);
  if (mode == ChartEditMode::Editable) {
    repair_legacy_hold_heads(document_);
  }
  history_.clear();
  bump_revision();
  publish_snapshot();
  return result;
}

SerializeResult ChartEditorEngine::export_official_to_file(const std::string& path) {
  if (document_.edit_mode() == ChartEditMode::OfficialPreviewOnly) {
    return {SerializeError::ReadOnly, "imported official charts cannot be exported"};
  }
  // Export from a normalized copy — never mutates a read-only document.
  const NotationChart chart = document_.normalized_chart();
  return OfficialChartFormat::save_chart_file(chart, path);
}

SerializeResult ChartEditorEngine::export_sus_to_file(const std::string& path,
                                                      const SusChartSaveOptions& options) {
  if (document_.edit_mode() == ChartEditMode::OfficialPreviewOnly) {
    return {SerializeError::ReadOnly, "imported charts cannot be exported"};
  }
  const NotationChart chart = document_.normalized_chart();
  return SusChartFormat::save_file(chart, path, options);
}

int32_t ChartEditorEngine::add_note(NotationNote note) {
  const int32_t id = document_.add_note(note);
  if (id < 0) {
    return id;
  }
  bump_revision();
  publish_snapshot();
  return id;
}

bool ChartEditorEngine::update_note(int32_t id, const NotationNote& note) {
  if (!document_.update_note(id, note)) {
    return false;
  }
  bump_revision();
  publish_snapshot();
  return true;
}

bool ChartEditorEngine::remove_note(int32_t id) {
  if (!document_.remove_note(id)) {
    return false;
  }
  bump_revision();
  publish_snapshot();
  return true;
}

bool ChartEditorEngine::set_notes(std::vector<NotationNote> notes) {
  if (!document_.set_notes(std::move(notes))) {
    return false;
  }
  bump_revision();
  publish_snapshot();
  return true;
}

bool ChartEditorEngine::execute_command(std::unique_ptr<IEditCommand> command) {
  if (!history_.execute(std::move(command), document_)) return false;
  bump_revision();
  publish_snapshot();
  return true;
}

bool ChartEditorEngine::undo() {
  if (!history_.undo(document_)) return false;
  bump_revision();
  publish_snapshot();
  return true;
}

bool ChartEditorEngine::redo() {
  if (!history_.redo(document_)) return false;
  bump_revision();
  publish_snapshot();
  return true;
}

void ChartEditorEngine::seek(int64_t time_ms) {
  clock_.seek(time_ms);
  publish_snapshot();
}

void ChartEditorEngine::play() {
  clock_.play();
  publish_snapshot();
}

void ChartEditorEngine::pause() {
  clock_.pause();
  publish_snapshot();
}

void ChartEditorEngine::toggle_playback() {
  clock_.toggle_playback();
  publish_snapshot();
}

void ChartEditorEngine::apply_timeline(const wds::common::TimelineSnapshot& snap) {
  clock_.apply(snap);
  publish_snapshot();
}

void ChartEditorEngine::tick(int64_t delta_ms) {
  clock_.tick(delta_ms);
  publish_snapshot();
}

const PreviewSnapshot& ChartEditorEngine::rebuild_snapshot() { return publish_snapshot(); }

int64_t ChartEditorEngine::timeline_ms() const noexcept { return clock_.current_time_ms(); }

int64_t ChartEditorEngine::timeline_us() const noexcept {
  return clock_.timeline().position().count();
}

PreviewPlaybackState ChartEditorEngine::playback_state() const noexcept {
  return clock_.playback_state();
}

void ChartEditorEngine::bump_revision() { ++revision_; }

const PreviewSnapshot& ChartEditorEngine::publish_snapshot() {
  // Transport clock is the visual clock (no lead-in remapping). Edit playhead
  // sits on the judgeline at 1:1; preview follows the same timeline.
  const int64_t clock_us = clock_.timeline().position().count();
  const int64_t visual_us =
      EditLeadIn::preview_chart_us(clock_us, preview_lead_in_visible_ms_);
  const int64_t preview_ms = visual_us / 1000;
  // content_generation invalidates note-lookup on document mutate without engine.bump_revision.
  const uint64_t snapshot_rev = document_.content_generation();
  snapshot_builder_.rebuild_or_update(snapshot_, document_.notes(), document_.timing(),
                                      document_.concurrent_lines(), document_.index(),
                                      preview_ms, clock_.playback_state(), snapshot_rev);
  snapshot_.timeline_ms = preview_ms;
  snapshot_.timeline_us = visual_us;
  if (snapshot_callback_) {
    snapshot_callback_(snapshot_);
  }
  return snapshot_;
}

}  // namespace wds::chart_editor
