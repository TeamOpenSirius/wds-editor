#pragma once

#include <wds/core/edit_history.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/sus_chart.hpp>

#include "wds/ui/regions/status/status_bar.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wds::chart_editor {
class ChartEditorEngine;
struct SerializeResult;
}

namespace wds::ui {
class ChartPreviewPanel;

// File-I/O-only result that can be prepared on a worker thread. Applying it to
// EditorSession remains a GUI-thread operation because it touches the live
// editor engine, transport, and preview.
struct PreparedWdsProject {
  std::string project_path;
  std::string music_path;
  std::vector<std::string> chart_paths;
  std::vector<wds::chart_editor::NotationChart> charts;
  std::string error;
  std::size_t active_chart_index = 0;
  int64_t offset_ms = 0;

  bool valid() const noexcept { return error.empty() && !charts.empty(); }
};

enum class ExportFormat {
  OfficialCsv,
  Sus,
};

// Owns editor-level state while ChartPreviewPanel owns the live engine/transport.
// Multiple charts are kept in memory; only the active one is loaded into the engine.
// CHART_DELAY_MS is song-level chart delay: shifts document timing (edit-area blank)
// so notes hit after music starts. Not written into .wdschart.
class EditorSession {
 public:
  explicit EditorSession(ChartPreviewPanel& preview);

  wds::chart_editor::ChartEditorEngine& engine() noexcept;
  const wds::chart_editor::ChartEditorEngine& engine() const noexcept;

  // Optional sink for user-visible status lines (wired to the bottom status bar).
  void set_status_handler(std::function<void(std::string, StatusLevel)> handler) {
    status_handler_ = std::move(handler);
  }
  void report_status(std::string text, StatusLevel level = StatusLevel::Info) {
    status(std::move(text), level);
  }

  bool new_project();
  static PreparedWdsProject prepare_wdsproject(const std::string& path);
  bool apply_prepared_wdsproject(PreparedWdsProject prepared);
  bool open_wdsproject(const std::string& path);
  bool save();
  bool save_as(const std::string& path);
  // Auto-detect: music_config.csv pack / .sus / official CSV. All imports are read-only
  // unless SUS auto-convert is enabled (in-memory → editable .wdschart, unbound path).
  bool import_official(const std::string& chart_path, const std::string& music_config_path = {});
  bool export_official(const std::string& path);
  // Export audio + music_config.csv + 1.csv..N.csv into `directory` using official names.
  // Conflicting files prompt for overwrite; declined conflicts are skipped.
  bool export_official_project(const std::string& directory);
  bool export_sus(const std::string& path);
  // Export each chart as a separate .sus into `directory` (1.sus .. N.sus).
  bool export_sus_project(const std::string& directory);
  bool import_music(const std::string& path);
  bool set_offset_ms(int64_t offset_ms);
  const std::vector<int32_t>& last_offset_violation_ids() const noexcept {
    return last_offset_violation_ids_;
  }
  bool switch_chart(std::size_t index);
  bool add_chart();
  // Append an existing .wdschart into the current editable project.
  bool add_chart_from_file(const std::string& path);

  void set_sus_auto_convert(bool enabled) noexcept { sus_auto_convert_ = enabled; }
  bool sus_auto_convert() const noexcept { return sus_auto_convert_; }

  const std::string& project_path() const noexcept { return project_path_; }
  const std::string& music_path() const noexcept { return music_path_; }
  int64_t offset_ms() const noexcept { return offset_ms_; }
  bool dirty() const noexcept;
  bool read_only() const noexcept { return read_only_; }
  // Chart delay field: editable for normal projects, or official import without music_config.
  bool delay_editable() const noexcept { return !read_only_ || allow_delay_when_read_only_; }
  std::size_t chart_count() const noexcept;
  std::size_t active_chart_index() const noexcept { return active_chart_index_; }
  const std::string& chart_path(std::size_t index) const noexcept;
  // Official default name for the active chart (e.g. "1.csv").
  std::string official_chart_filename(std::size_t index) const;
  std::string sus_chart_filename(std::size_t index) const;

 private:
  struct ChartSlot {
    std::string path;  // empty = not yet bound to a .wdschart file
    wds::chart_editor::NotationChart chart;
    bool dirty = false;
    // Per-chart undo stack; swapped with the engine history on chart switch.
    wds::chart_editor::EditHistory history;
  };

  void status(std::string text, StatusLevel level);
  // Sync active chart snapshot into its slot (does not touch undo history).
  void sync_active_chart();
  // sync_active_chart + move engine history into the slot (chart switch only).
  void stash_active();
  bool activate_chart(std::size_t index);
  void apply_chart_delay();
  // Prompt for unbound chart destinations into `out_paths` (does not mutate session).
  bool collect_chart_paths_for_save(std::vector<std::string>& out_paths);
  bool write_all_charts_and_project(const std::string& project_path);
  bool import_official_pack(const std::string& music_config_path);
  bool import_sus(const std::string& path);
  void reset_music_config_meta();
  wds::chart_editor::OfficialMusicConfig make_music_config() const;
  wds::chart_editor::SusChartSaveOptions make_sus_save_options() const;
  std::string official_audio_filename() const;

  ChartPreviewPanel& preview_;
  std::function<void(std::string, StatusLevel)> status_handler_;
  std::string project_path_;
  std::string music_path_;
  std::vector<ChartSlot> charts_;
  std::size_t active_chart_index_ = 0;
  int64_t offset_ms_ = 0;
  std::vector<int32_t> last_offset_violation_ids_;
  bool metadata_dirty_ = false;
  bool read_only_ = false;
  bool allow_delay_when_read_only_ = false;
  bool sus_auto_convert_ = false;
  // Fields mirrored into music_config.csv on official project export.
  std::string cue_sheet_name_ = "Music";
  std::string cue_name_ = "1";
  std::string cue_sheet_directory_ = "Game";
  // SUS metadata retained for round-trip export.
  wds::chart_editor::SusChartMetadata sus_meta_{};
};
}  // namespace wds::ui
