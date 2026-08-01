#include "wds/ui/editor_session.hpp"

#include "wds/ui/native_file_dialog.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"

#include <wds/core/chart_editor_engine.hpp>
#include <wds/core/chart_serializer.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/project.hpp>
#include <wds/core/sus_chart.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace wds::ui {
namespace {

namespace fs = std::filesystem;

bool ok(const wds::chart_editor::SerializeResult& result) {
  return result.error == wds::chart_editor::SerializeError::Ok;
}

std::string sibling_music_config(const std::string& chart_path) {
  std::error_code ec;
  const fs::path candidate = fs::path(chart_path).parent_path() / "music_config.csv";
  if (fs::is_regular_file(candidate, ec) && !ec) {
    return candidate.generic_string();
  }
  return {};
}

std::string to_lower_ascii(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

bool equals_ci(const std::string& a, const std::string& b) {
  return to_lower_ascii(a) == to_lower_ascii(b);
}

bool is_music_config_path(const fs::path& path) {
  return equals_ci(path.filename().string(), "music_config.csv");
}

bool ends_with_ci(const std::string& value, const std::string& suffix) {
  if (value.size() < suffix.size()) return false;
  return equals_ci(value.substr(value.size() - suffix.size()), suffix);
}

bool starts_with_ci(const std::string& value, const std::string& prefix) {
  if (value.size() < prefix.size()) return false;
  return equals_ci(value.substr(0, prefix.size()), prefix);
}

std::string read_text_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool is_official_chart_file(const fs::path& path) {
  if (!ends_with_ci(path.filename().string(), ".csv")) return false;
  if (is_music_config_path(path)) return false;
  const std::string text = read_text_file(path);
  if (text.empty()) return false;
  return wds::chart_editor::OfficialChartFormat::looks_like_official_chart_text(text);
}

// Prefer numeric stems (1.csv, 2.csv) then lexicographic.
bool chart_path_less(const fs::path& a, const fs::path& b) {
  const std::string sa = a.stem().string();
  const std::string sb = b.stem().string();
  auto parse_num = [](const std::string& s, int& out) {
    if (s.empty()) return false;
    for (char ch : s) {
      if (ch < '0' || ch > '9') return false;
    }
    try {
      out = std::stoi(s);
      return true;
    } catch (...) {
      return false;
    }
  };
  int na = 0;
  int nb = 0;
  const bool a_num = parse_num(sa, na);
  const bool b_num = parse_num(sb, nb);
  if (a_num && b_num) return na < nb;
  if (a_num != b_num) return a_num;  // numeric names first
  return sa < sb;
}

std::vector<fs::path> list_official_charts(const fs::path& dir) {
  std::vector<fs::path> charts;
  std::error_code ec;
  if (!fs::is_directory(dir, ec) || ec) return charts;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) continue;
    if (is_official_chart_file(entry.path())) {
      charts.push_back(entry.path());
    }
  }
  std::sort(charts.begin(), charts.end(), chart_path_less);
  return charts;
}

std::string find_music_ogg(const fs::path& dir, const std::string& cue_name) {
  std::error_code ec;
  if (!cue_name.empty()) {
    const fs::path preferred = dir / ("music_" + cue_name + ".ogg");
    if (fs::is_regular_file(preferred, ec) && !ec) {
      return preferred.generic_string();
    }
  }
  if (!fs::is_directory(dir, ec) || ec) return {};
  std::vector<fs::path> matches;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) continue;
    const std::string name = entry.path().filename().string();
    if (starts_with_ci(name, "music_") && ends_with_ci(name, ".ogg")) {
      matches.push_back(entry.path());
    }
  }
  if (matches.empty()) return {};
  std::sort(matches.begin(), matches.end());
  return matches.front().generic_string();
}

bool copy_file_overwrite(const fs::path& from, const fs::path& to, std::error_code& ec) {
  fs::create_directories(to.parent_path(), ec);
  if (ec) return false;
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

}  // namespace

EditorSession::EditorSession(ChartPreviewPanel& preview) : preview_(preview) {}

wds::chart_editor::ChartEditorEngine& EditorSession::engine() noexcept { return preview_.engine(); }
const wds::chart_editor::ChartEditorEngine& EditorSession::engine() const noexcept {
  return preview_.engine();
}

void EditorSession::reset_music_config_meta() {
  cue_sheet_name_ = "Music";
  cue_name_ = "1";
  cue_sheet_directory_ = "Game";
}

wds::chart_editor::OfficialMusicConfig EditorSession::make_music_config() const {
  wds::chart_editor::OfficialMusicConfig config;
  config.cue_sheet_name = cue_sheet_name_.empty() ? "Music" : cue_sheet_name_;
  config.cue_name = cue_name_.empty() ? "1" : cue_name_;
  config.delay_seconds = static_cast<double>(offset_ms_) / 1000.0;
  config.cue_sheet_directory = cue_sheet_directory_.empty() ? "Game" : cue_sheet_directory_;
  return config;
}

std::string EditorSession::official_audio_filename() const {
  const std::string cue = cue_name_.empty() ? "1" : cue_name_;
  if (!music_path_.empty()) {
    const fs::path src(music_path_);
    const std::string name = src.filename().string();
    if (starts_with_ci(name, "music_") && ends_with_ci(name, ".ogg")) {
      return name;
    }
    const std::string ext = src.extension().string();
    if (!ext.empty()) {
      return "music_" + cue + to_lower_ascii(ext);
    }
  }
  return "music_" + cue + ".ogg";
}

std::string EditorSession::official_chart_filename(std::size_t index) const {
  return std::to_string(index + 1) + ".csv";
}

std::string EditorSession::sus_chart_filename(std::size_t index) const {
  return std::to_string(index + 1) + ".sus";
}

wds::chart_editor::SusChartSaveOptions EditorSession::make_sus_save_options() const {
  wds::chart_editor::SusChartSaveOptions options;
  options.meta = sus_meta_;
  options.meta.wave_offset_sec = static_cast<double>(offset_ms_) / 1000.0;
  if (!music_path_.empty()) {
    options.meta.wave_path = fs::path(music_path_).filename().generic_string();
  }
  options.ched_lane_padding = true;
  return options;
}

void EditorSession::apply_chart_delay() {
  // Chart delay: how much later the chart starts than the music (tick0 → offset_ms).
  // Edit area shows leading blank before notes; BGM still starts at timeline 0.
  engine().document().set_offset_ms(offset_ms_);
  engine().rebuild_snapshot();
  preview_.transport().set_chart_offset_ms(offset_ms_);
}

void EditorSession::stash_active() {
  if (charts_.empty() || active_chart_index_ >= charts_.size()) return;
  auto& slot = charts_[active_chart_index_];
  slot.chart = engine().document().to_notation_chart();
  slot.chart.timing.offset_ms = 0;
  slot.dirty = slot.dirty || engine().is_dirty();
  slot.history = std::move(engine().history());
}

bool EditorSession::activate_chart(std::size_t index) {
  if (index >= charts_.size()) return false;
  active_chart_index_ = index;
  auto chart = charts_[index].chart;
  chart.timing.offset_ms = offset_ms_;
  const auto mode = read_only_ ? wds::chart_editor::ChartEditMode::OfficialPreviewOnly
                               : wds::chart_editor::ChartEditMode::Editable;
  engine().load_chart(chart, mode);  // clears engine history
  engine().history() = std::move(charts_[index].history);
  apply_chart_delay();
  if (!charts_[index].dirty) {
    engine().mark_saved();
  }
  return true;
}

bool EditorSession::new_project() {
  wds::chart_editor::MusicTiming timing;
  timing.offset_ms = 0;
  engine().load_chart({timing, {}, {}}, wds::chart_editor::ChartEditMode::Editable);
  engine().history().clear();
  project_path_.clear();
  music_path_.clear();
  charts_.clear();
  charts_.push_back(ChartSlot{});
  charts_.back().chart = engine().document().to_notation_chart();
  charts_.back().chart.timing.offset_ms = 0;
  active_chart_index_ = 0;
  offset_ms_ = 0;
  metadata_dirty_ = false;
  read_only_ = false;
  allow_delay_when_read_only_ = false;
  reset_music_config_meta();
  apply_chart_delay();
  // Opening a fresh project must not inherit the previous song's playhead —
  // preview only draws the current time window, so a late cursor looks empty.
  preview_.load_music({}, false);  // clear any previously imported BGM
  preview_.reset_playback();
  return true;
}

bool EditorSession::open_wdsproject(const std::string& path) {
  wds::chart_editor::WdsProject project;
  if (!ok(wds::chart_editor::ProjectSerializer::load_from_file(path, project))) {
    native_file_dialog::alert_error("打开工程失败", "无法读取工程文件:\n" + path);
    return false;
  }

  // Resolve and load every chart into a temporary buffer first — never mutate the
  // live session until the whole project is known to be loadable.
  const std::string new_music =
      wds::chart_editor::ProjectSerializer::resolve_path(path, project.music_path);
  std::vector<ChartSlot> loaded;
  loaded.reserve(project.chart_paths.size());
  for (const auto& chart_rel : project.chart_paths) {
    const std::string chart_path =
        wds::chart_editor::ProjectSerializer::resolve_path(path, chart_rel);
    wds::chart_editor::NotationChart chart;
    if (!ok(wds::chart_editor::ChartSerializer::load_from_file(chart_path, chart))) {
      native_file_dialog::alert_error("打开工程失败", "无法加载谱面:\n" + chart_path);
      return false;
    }
    chart.timing.offset_ms = 0;
    ChartSlot slot;
    slot.path = chart_path;
    slot.chart = std::move(chart);
    slot.dirty = false;
    loaded.push_back(std::move(slot));
  }
  if (loaded.empty()) {
    native_file_dialog::alert_error("打开工程失败", "工程未包含任何谱面");
    return false;
  }

  const std::size_t new_active = static_cast<std::size_t>(
      std::clamp(project.active_chart_index, 0, static_cast<int32_t>(loaded.size() - 1)));

  // Try the new BGM before committing session metadata. On failure, restore the
  // previous track so the in-memory project stays consistent.
  const std::string previous_music = music_path_;
  if (!preview_.load_music(new_music, false)) {
    (void)preview_.load_music(previous_music, false);
    native_file_dialog::alert_error("打开工程失败",
                                    "无法加载音乐文件:\n" +
                                        (new_music.empty() ? std::string("(空路径)") : new_music));
    return false;
  }

  // Commit project state only after charts + music succeeded.
  project_path_ = path;
  music_path_ = new_music;
  charts_ = std::move(loaded);
  active_chart_index_ = new_active;
  offset_ms_ = project.offset_ms;
  metadata_dirty_ = false;
  read_only_ = false;
  allow_delay_when_read_only_ = false;
  reset_music_config_meta();

  if (!activate_chart(active_chart_index_)) {
    native_file_dialog::alert_error("打开工程失败", "无法激活工程中的谱面");
    return false;
  }
  preview_.reset_playback();
  return true;
}

bool EditorSession::ensure_chart_paths_for_save() {
  for (std::size_t i = 0; i < charts_.size(); ++i) {
    if (!charts_[i].path.empty()) continue;
    const std::string default_name = std::to_string(i + 1) + ".wdschart";
    const std::string title = "保存谱面 " + std::to_string(i + 1);
    auto chosen = native_file_dialog::save_file(title, default_name, {"wdschart"});
    if (!chosen || chosen->empty()) return false;
    charts_[i].path = *chosen;
  }
  return true;
}

bool EditorSession::write_all_charts_and_project(const std::string& project_path) {
  if (read_only_ || project_path.empty() || charts_.empty()) return false;
  stash_active();
  if (!ensure_chart_paths_for_save()) return false;

  for (std::size_t i = 0; i < charts_.size(); ++i) {
    auto chart = charts_[i].chart;
    chart.timing.offset_ms = 0;
    if (!ok(wds::chart_editor::ChartSerializer::save_to_file(chart, charts_[i].path))) {
      return false;
    }
    charts_[i].dirty = false;
  }

  wds::chart_editor::WdsProject project;
  project.music_path = music_path_;
  project.offset_ms = offset_ms_;
  project.active_chart_index = static_cast<int32_t>(active_chart_index_);
  project.chart_paths.clear();
  project.chart_paths.reserve(charts_.size());
  for (const auto& slot : charts_) {
    project.chart_paths.push_back(slot.path);
  }
  if (!ok(wds::chart_editor::ProjectSerializer::save_relativized(std::move(project),
                                                                  project_path))) {
    return false;
  }

  project_path_ = project_path;
  metadata_dirty_ = false;
  engine().mark_saved();
  return true;
}

bool EditorSession::save() {
  if (project_path_.empty()) return false;
  return write_all_charts_and_project(project_path_);
}

bool EditorSession::save_as(const std::string& path) {
  if (read_only_ || path.empty()) return false;
  return write_all_charts_and_project(path);
}

bool EditorSession::import_official_pack(const std::string& music_config_path) {
  wds::chart_editor::OfficialMusicConfig music;
  if (!ok(wds::chart_editor::OfficialChartFormat::load_music_config_file(music_config_path,
                                                                        music))) {
    return false;
  }

  const fs::path dir = fs::path(music_config_path).parent_path();
  const auto chart_paths = list_official_charts(dir);
  if (chart_paths.empty()) return false;

  std::vector<ChartSlot> loaded;
  loaded.reserve(chart_paths.size());
  for (const auto& chart_path : chart_paths) {
    wds::chart_editor::NotationChart chart;
    if (!ok(wds::chart_editor::OfficialChartFormat::load_chart_with_music_config(
            chart_path.generic_string(), music_config_path, chart))) {
      return false;
    }
    chart.timing.offset_ms = 0;
    // Keep charts in memory only — do not bind official CSV paths (avoids overwriting
    // them when the user later saves a .wdsproject / .wdschart).
    ChartSlot slot;
    slot.chart = std::move(chart);
    loaded.push_back(std::move(slot));
  }

  const std::string audio = find_music_ogg(dir, music.cue_name);

  project_path_.clear();
  music_path_ = audio;
  charts_ = std::move(loaded);
  active_chart_index_ = 0;
  offset_ms_ = static_cast<int64_t>(std::llround(music.delay_seconds * 1000.0));
  metadata_dirty_ = false;
  read_only_ = true;
  allow_delay_when_read_only_ = false;
  cue_sheet_name_ = music.cue_sheet_name.empty() ? "Music" : music.cue_sheet_name;
  cue_name_ = music.cue_name.empty() ? "1" : music.cue_name;
  cue_sheet_directory_ =
      music.cue_sheet_directory.empty() ? "Game" : music.cue_sheet_directory;

  if (!preview_.load_music(music_path_, false)) {
    // Missing/unloadable audio is non-fatal for chart editing.
    music_path_.clear();
    preview_.load_music({}, false);
  }
  if (!activate_chart(0)) return false;
  preview_.reset_playback();
  return true;
}

bool EditorSession::import_official(const std::string& chart_path,
                                    const std::string& music_config_path) {
  if (chart_path.empty()) return false;
  if (is_music_config_path(fs::path(chart_path))) {
    return import_official_pack(chart_path);
  }
  if (wds::chart_editor::SusChartFormat::looks_like_sus_path(chart_path)) {
    return import_sus(chart_path);
  }
  // Content sniff for mislabeled files.
  {
    std::ifstream peek(chart_path, std::ios::binary);
    if (peek) {
      std::string sample(512, '\0');
      peek.read(sample.data(), static_cast<std::streamsize>(sample.size()));
      sample.resize(static_cast<size_t>(std::max<std::streamsize>(0, peek.gcount())));
      if (wds::chart_editor::SusChartFormat::looks_like_sus_text(sample)) {
        return import_sus(chart_path);
      }
    }
  }

  std::string config_path = music_config_path;
  if (config_path.empty()) {
    config_path = sibling_music_config(chart_path);
  } else {
    std::error_code ec;
    if (!fs::is_regular_file(config_path, ec) || ec) {
      config_path.clear();
    }
  }
  const bool found_config = !config_path.empty();

  // Empty config_path → load chart only (DelaySeconds stays unset / 0).
  if (!ok(engine().load_official_from_file(chart_path, config_path))) return false;

  project_path_.clear();
  music_path_.clear();
  offset_ms_ = engine().document().timing().offset_ms;
  auto chart = engine().document().to_notation_chart();
  chart.timing.offset_ms = 0;
  charts_.clear();
  {
    ChartSlot slot;
    slot.path = chart_path;
    slot.chart = std::move(chart);
    charts_.push_back(std::move(slot));
  }
  active_chart_index_ = 0;
  apply_chart_delay();
  engine().mark_saved();
  metadata_dirty_ = false;
  read_only_ = true;
  // No music_config → allow editing 谱面延迟 for preview alignment.
  allow_delay_when_read_only_ = !found_config;
  reset_music_config_meta();
  sus_meta_ = {};
  if (found_config) {
    wds::chart_editor::OfficialMusicConfig music;
    if (ok(wds::chart_editor::OfficialChartFormat::load_music_config_file(config_path, music))) {
      cue_sheet_name_ = music.cue_sheet_name.empty() ? "Music" : music.cue_sheet_name;
      cue_name_ = music.cue_name.empty() ? "1" : music.cue_name;
      cue_sheet_directory_ =
          music.cue_sheet_directory.empty() ? "Game" : music.cue_sheet_directory;
    }
  }
  // single-chart official import does not bind a project BGM path
  preview_.load_music({}, false);
  preview_.reset_playback();
  return true;
}

bool EditorSession::import_sus(const std::string& path) {
  wds::chart_editor::SusChartMetadata meta;
  if (!ok(engine().load_sus_from_file(path, &meta))) return false;

  project_path_.clear();
  offset_ms_ = engine().document().timing().offset_ms;
  auto chart = engine().document().to_notation_chart();
  chart.timing.offset_ms = 0;
  charts_.clear();
  // Auto-convert: editable in-memory WDS project (do not bind the .sus path).
  const bool convert = sus_auto_convert_;
  {
    ChartSlot slot;
    slot.path = convert ? std::string{} : path;
    slot.chart = std::move(chart);
    slot.dirty = convert;
    charts_.push_back(std::move(slot));
  }
  active_chart_index_ = 0;
  metadata_dirty_ = convert;
  read_only_ = !convert;
  allow_delay_when_read_only_ = false;
  reset_music_config_meta();
  sus_meta_ = meta;

  // Resolve #WAVE relative to the .sus file when present.
  music_path_.clear();
  if (!meta.wave_path.empty()) {
    std::error_code ec;
    fs::path wave(meta.wave_path);
    if (!wave.is_absolute()) {
      wave = fs::path(path).parent_path() / wave;
    }
    if (fs::is_regular_file(wave, ec) && !ec) {
      music_path_ = wave.generic_string();
    }
  }
  if (!music_path_.empty() && !preview_.load_music(music_path_, false)) {
    music_path_.clear();
    preview_.load_music({}, false);
  } else if (music_path_.empty()) {
    preview_.load_music({}, false);
  }
  // Reload into Editable (convert) or keep OfficialPreviewOnly (preview-only).
  if (!activate_chart(0)) return false;
  preview_.reset_playback();
  return true;
}

bool EditorSession::export_official(const std::string& path) {
  // Imported official charts are intentionally preview-only and never re-exported.
  return !read_only_ && ok(engine().export_official_to_file(path));
}

bool EditorSession::export_sus(const std::string& path) {
  if (read_only_ || path.empty()) return false;
  stash_active();
  return ok(engine().export_sus_to_file(path, make_sus_save_options()));
}

bool EditorSession::export_sus_project(const std::string& directory) {
  if (read_only_ || directory.empty() || charts_.empty()) return false;
  stash_active();

  std::error_code ec;
  const fs::path dir(directory);
  if (!fs::is_directory(dir, ec) || ec) return false;

  std::vector<fs::path> planned;
  planned.reserve(charts_.size());
  for (std::size_t i = 0; i < charts_.size(); ++i) {
    planned.push_back(dir / sus_chart_filename(i));
  }

  std::vector<fs::path> conflicts;
  for (const auto& p : planned) {
    if (fs::exists(p, ec) && !ec) conflicts.push_back(p);
  }
  bool overwrite = true;
  if (!conflicts.empty()) {
    overwrite = native_file_dialog::confirm(
        "导出冲突", "目标目录存在同名文件，是否覆盖？选择 No 将跳过冲突文件。");
  }

  auto options = make_sus_save_options();
  bool any_written = false;
  for (std::size_t i = 0; i < charts_.size(); ++i) {
    const fs::path out = planned[i];
    const bool exists = fs::exists(out, ec) && !ec;
    if (exists && !overwrite) continue;
    auto chart = charts_[i].chart;
    chart.timing.offset_ms = offset_ms_;
    if (!ok(wds::chart_editor::SusChartFormat::save_file(chart, out.generic_string(), options))) {
      return any_written;
    }
    any_written = true;
  }
  return any_written || conflicts.empty();
}

bool EditorSession::export_official_project(const std::string& directory) {
  if (read_only_ || directory.empty() || charts_.empty()) return false;
  stash_active();

  std::error_code ec;
  const fs::path dir(directory);
  if (!fs::is_directory(dir, ec) || ec) return false;

  struct PlannedFile {
    fs::path path;
    enum class Kind { Chart, MusicConfig, Audio } kind;
    std::size_t chart_index = 0;
  };

  std::vector<PlannedFile> planned;
  planned.reserve(charts_.size() + 2);
  for (std::size_t i = 0; i < charts_.size(); ++i) {
    planned.push_back({dir / official_chart_filename(i), PlannedFile::Kind::Chart, i});
  }
  planned.push_back({dir / "music_config.csv", PlannedFile::Kind::MusicConfig, 0});
  const bool has_audio = !music_path_.empty() && fs::is_regular_file(music_path_, ec) && !ec;
  if (has_audio) {
    planned.push_back({dir / official_audio_filename(), PlannedFile::Kind::Audio, 0});
  }

  std::vector<fs::path> conflicts;
  for (const auto& item : planned) {
    if (fs::exists(item.path, ec) && !ec) {
      conflicts.push_back(item.path);
    }
  }

  bool overwrite = true;
  if (!conflicts.empty()) {
    overwrite = native_file_dialog::confirm(
        "导出冲突", "目标目录存在同名文件，是否覆盖？选择 No 将跳过冲突文件。");
  }

  bool any_written = false;
  for (const auto& item : planned) {
    const bool exists = fs::exists(item.path, ec) && !ec;
    if (exists && !overwrite) continue;

    switch (item.kind) {
      case PlannedFile::Kind::Chart: {
        auto chart = charts_[item.chart_index].chart;
        chart.timing.offset_ms = 0;
        if (!ok(wds::chart_editor::OfficialChartFormat::save_chart_file(
                chart, item.path.generic_string()))) {
          return any_written;
        }
        any_written = true;
        break;
      }
      case PlannedFile::Kind::MusicConfig: {
        if (!ok(wds::chart_editor::OfficialChartFormat::save_music_config_file(
                make_music_config(), item.path.generic_string()))) {
          return any_written;
        }
        any_written = true;
        break;
      }
      case PlannedFile::Kind::Audio: {
        if (!copy_file_overwrite(fs::path(music_path_), item.path, ec)) {
          return any_written;
        }
        any_written = true;
        break;
      }
    }
  }
  return any_written || conflicts.empty();
}

bool EditorSession::import_music(const std::string& path) {
  // Allowed in official read-only preview — BGM is independent of chart authoring.
  if (path.empty() || !preview_.load_music(path)) return false;
  // Keep absolute path in-session for playback; save_relativized writes MUSIC as relative.
  music_path_ = path;
  if (!read_only_) {
    metadata_dirty_ = true;
  }
  // If the imported file already uses official naming, adopt its cue id.
  const std::string name = fs::path(path).filename().string();
  if (starts_with_ci(name, "music_") && ends_with_ci(name, ".ogg")) {
    const auto stem = fs::path(name).stem().string();  // music_1
    if (stem.size() > 6) {
      cue_name_ = stem.substr(6);
    }
  }
  return true;
}

bool EditorSession::set_offset_ms(int64_t offset_ms) {
  if (!delay_editable() || offset_ms < 0) return false;
  offset_ms_ = offset_ms;
  if (!read_only_) {
    metadata_dirty_ = true;
  }
  apply_chart_delay();
  return true;
}

bool EditorSession::switch_chart(std::size_t index) {
  // Allowed in read-only pack imports so users can preview each difficulty.
  if (index >= charts_.size() || index == active_chart_index_) return false;
  stash_active();
  return activate_chart(index);
}

bool EditorSession::add_chart() {
  if (read_only_) return false;
  stash_active();
  wds::chart_editor::MusicTiming timing;
  if (!charts_.empty()) {
    timing = charts_[active_chart_index_].chart.timing;
  }
  timing.offset_ms = 0;
  ChartSlot slot;
  slot.chart = {timing, {}, {}};
  slot.dirty = true;
  charts_.push_back(std::move(slot));
  metadata_dirty_ = true;
  return activate_chart(charts_.size() - 1);
}

bool EditorSession::add_chart_from_file(const std::string& path) {
  if (read_only_ || path.empty()) return false;
  wds::chart_editor::NotationChart chart;
  if (!ok(wds::chart_editor::ChartSerializer::load_from_file(path, chart))) return false;
  chart.timing.offset_ms = 0;
  stash_active();
  {
    ChartSlot slot;
    slot.path = path;
    slot.chart = std::move(chart);
    charts_.push_back(std::move(slot));
  }
  metadata_dirty_ = true;
  return activate_chart(charts_.size() - 1);
}

bool EditorSession::dirty() const noexcept {
  if (metadata_dirty_) return true;
  if (engine().is_dirty()) return true;
  for (const auto& slot : charts_) {
    if (slot.dirty) return true;
  }
  return false;
}

std::size_t EditorSession::chart_count() const noexcept { return charts_.size(); }

const std::string& EditorSession::chart_path(std::size_t index) const noexcept {
  static const std::string empty;
  if (index >= charts_.size()) return empty;
  return charts_[index].path;
}

}  // namespace wds::ui
