#include "wds/ui/editor_session.hpp"

#include "wds/ui/native_file_dialog.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/status/status_bar.hpp"

#include <wds/core/chart_editor_engine.hpp>
#include <wds/core/chart_serializer.hpp>
#include <wds/core/file_io.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/project.hpp>
#include <wds/core/sus_chart.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wds::ui {
namespace {

namespace fs = std::filesystem;

bool ok(const wds::chart_editor::SerializeResult& result) {
  return result.error == wds::chart_editor::SerializeError::Ok;
}

// Normalize a chart copy for disk (sort + dense ids). Does not mutate `chart`.
wds::chart_editor::NotationChart normalized_copy(
    const wds::chart_editor::NotationChart& chart) {
  wds::chart_editor::ChartDocument doc;
  doc.load_from_chart(chart, wds::chart_editor::ChartEditMode::Editable);
  return doc.normalized_chart();
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

bool ends_with_ci(const std::string& value, const std::string& suffix) {
  if (value.size() < suffix.size()) return false;
  return equals_ci(value.substr(value.size() - suffix.size()), suffix);
}

bool starts_with_ci(const std::string& value, const std::string& prefix) {
  if (value.size() < prefix.size()) return false;
  return equals_ci(value.substr(0, prefix.size()), prefix);
}

fs::path path_from_utf8(const std::string& utf8) {
#if defined(_WIN32)
  return fs::u8path(utf8);
#else
  return fs::path(utf8);
#endif
}

std::string path_to_utf8(const fs::path& path) {
#if defined(_WIN32)
  const auto u8 = path.generic_u8string();
  return std::string(u8.begin(), u8.end());
#else
  return path.generic_string();
#endif
}

std::string join_utf8(const std::string& dir_utf8, const std::string& name_utf8) {
  if (dir_utf8.empty()) return name_utf8;
  const char last = dir_utf8.back();
  if (last == '/' || last == '\\') return dir_utf8 + name_utf8;
  return dir_utf8 + "/" + name_utf8;
}

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
  return wide;
}

std::string wide_to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                          nullptr, nullptr);
  if (needed <= 0) return {};
  std::string utf8(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed,
                      nullptr, nullptr);
  return utf8;
}

DWORD win_attrs(const std::string& utf8) {
  const std::wstring wide = utf8_to_wide(utf8);
  if (wide.empty() && !utf8.empty()) return INVALID_FILE_ATTRIBUTES;
  return GetFileAttributesW(wide.c_str());
}

bool win_is_regular_file(const std::string& utf8) {
  const DWORD attr = win_attrs(utf8);
  if (attr == INVALID_FILE_ATTRIBUTES) return false;
  return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool win_is_directory(const std::string& utf8) {
  const DWORD attr = win_attrs(utf8);
  if (attr == INVALID_FILE_ATTRIBUTES) return false;
  return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool win_exists(const std::string& utf8) {
  return win_attrs(utf8) != INVALID_FILE_ATTRIBUTES;
}

bool win_create_parent_dirs(const std::wstring& wide_file) {
  const std::size_t pos = wide_file.find_last_of(L"\\/");
  if (pos == std::wstring::npos || pos == 0) return true;
  std::wstring dir = wide_file.substr(0, pos);
  if (dir.size() == 2 && dir[1] == L':') return true;
  const DWORD attr = GetFileAttributesW(dir.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (!win_create_parent_dirs(dir)) return false;
  if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool copy_file_utf8_overwrite(const std::string& from_utf8, const std::string& to_utf8) {
  const std::wstring from = utf8_to_wide(from_utf8);
  const std::wstring to = utf8_to_wide(to_utf8);
  if ((from.empty() && !from_utf8.empty()) || (to.empty() && !to_utf8.empty())) return false;
  if (!win_create_parent_dirs(to)) return false;
  return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

std::vector<std::string> win_list_regular_files(const std::string& dir_utf8) {
  std::vector<std::string> out;
  std::wstring pattern = utf8_to_wide(dir_utf8);
  if (pattern.empty()) return out;
  if (pattern.back() != L'\\' && pattern.back() != L'/') pattern.push_back(L'\\');
  pattern.push_back(L'*');
  WIN32_FIND_DATAW fd{};
  const HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
  if (handle == INVALID_HANDLE_VALUE) return out;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    const std::string name = wide_to_utf8(fd.cFileName);
    if (name.empty() || name == "." || name == "..") continue;
    out.push_back(join_utf8(dir_utf8, name));
  } while (FindNextFileW(handle, &fd));
  FindClose(handle);
  return out;
}
#endif

bool file_is_regular_utf8(const std::string& utf8) {
#if defined(_WIN32)
  return win_is_regular_file(utf8);
#else
  std::error_code ec;
  return fs::is_regular_file(path_from_utf8(utf8), ec) && !ec;
#endif
}

bool dir_is_directory_utf8(const std::string& utf8) {
#if defined(_WIN32)
  return win_is_directory(utf8);
#else
  std::error_code ec;
  return fs::is_directory(path_from_utf8(utf8), ec) && !ec;
#endif
}

bool path_exists_utf8(const std::string& utf8) {
#if defined(_WIN32)
  return win_exists(utf8);
#else
  std::error_code ec;
  return fs::exists(path_from_utf8(utf8), ec) && !ec;
#endif
}

bool copy_utf8_overwrite(const std::string& from_utf8, const std::string& to_utf8,
                         std::error_code& ec) {
#if defined(_WIN32)
  if (copy_file_utf8_overwrite(from_utf8, to_utf8)) {
    ec.clear();
    return true;
  }
  ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
  return false;
#else
  std::error_code local;
  const fs::path to = path_from_utf8(to_utf8);
  fs::create_directories(to.parent_path(), local);
  if (local) {
    ec = local;
    return false;
  }
  fs::copy_file(path_from_utf8(from_utf8), to, fs::copy_options::overwrite_existing, local);
  ec = local;
  return !local;
#endif
}

bool is_music_config_name(const std::string& filename_utf8) {
  return equals_ci(filename_utf8, "music_config.csv");
}

bool is_music_config_path(const std::string& path_utf8) {
  return is_music_config_name(path_to_utf8(path_from_utf8(path_utf8).filename()));
}

std::string sibling_music_config(const std::string& chart_path) {
  const std::string candidate =
      path_to_utf8(path_from_utf8(chart_path).parent_path() / "music_config.csv");
  if (file_is_regular_utf8(candidate)) return candidate;
  return {};
}

std::string read_text_utf8(const std::string& path_utf8) {
  wds::chart_editor::SerializeResult status;
  return wds::chart_editor::read_text_file(path_utf8, status);
}

bool is_official_chart_file(const std::string& path_utf8) {
  const std::string name = path_to_utf8(path_from_utf8(path_utf8).filename());
  if (!ends_with_ci(name, ".csv")) return false;
  if (is_music_config_name(name)) return false;
  const std::string text = read_text_utf8(path_utf8);
  if (text.empty()) return false;
  return wds::chart_editor::OfficialChartFormat::looks_like_official_chart_text(text);
}

// Prefer numeric stems (1.csv, 2.csv) then lexicographic.
bool chart_path_less(const std::string& a, const std::string& b) {
  const std::string sa = path_to_utf8(path_from_utf8(a).stem());
  const std::string sb = path_to_utf8(path_from_utf8(b).stem());
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

std::vector<std::string> list_official_charts(const std::string& dir_utf8) {
  std::vector<std::string> charts;
  if (!dir_is_directory_utf8(dir_utf8)) return charts;
#if defined(_WIN32)
  for (const auto& path : win_list_regular_files(dir_utf8)) {
    if (is_official_chart_file(path)) charts.push_back(path);
  }
#else
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(path_from_utf8(dir_utf8), ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) continue;
    const std::string path = path_to_utf8(entry.path());
    if (is_official_chart_file(path)) charts.push_back(path);
  }
#endif
  std::sort(charts.begin(), charts.end(), chart_path_less);
  return charts;
}

std::string find_music_ogg(const std::string& dir_utf8, const std::string& cue_name) {
  if (!cue_name.empty()) {
    const std::string preferred = join_utf8(dir_utf8, "music_" + cue_name + ".ogg");
    if (file_is_regular_utf8(preferred)) return preferred;
  }
  if (!dir_is_directory_utf8(dir_utf8)) return {};
  std::vector<std::string> matches;
#if defined(_WIN32)
  for (const auto& path : win_list_regular_files(dir_utf8)) {
    const std::string name = path_to_utf8(path_from_utf8(path).filename());
    if (starts_with_ci(name, "music_") && ends_with_ci(name, ".ogg")) matches.push_back(path);
  }
#else
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(path_from_utf8(dir_utf8), ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) continue;
    const std::string name = path_to_utf8(entry.path().filename());
    if (starts_with_ci(name, "music_") && ends_with_ci(name, ".ogg")) {
      matches.push_back(path_to_utf8(entry.path()));
    }
  }
#endif
  if (matches.empty()) return {};
  std::sort(matches.begin(), matches.end());
  return matches.front();
}

// Keep embedded BGM / cue ids portable across Windows ACP-limited tooling.
std::string ascii_token_or(const std::string& value, const std::string& fallback) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        ch == '_' || ch == '-') {
      out.push_back(static_cast<char>(ch));
    }
  }
  return out.empty() ? fallback : out;
}

}  // namespace

EditorSession::EditorSession(ChartPreviewPanel& preview) : preview_(preview) {}

void EditorSession::status(std::string text, StatusLevel level) {
  if (status_handler_) status_handler_(std::move(text), level);
}

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
  // Always emit an ASCII filename so embedded BGM never reintroduces non-ASCII
  // path components into the project directory (MinGW ACP-safe, portable).
  const std::string cue = ascii_token_or(cue_name_.empty() ? "1" : cue_name_, "1");
  std::string ext = ".ogg";
  if (!music_path_.empty()) {
    const std::string e = to_lower_ascii(path_to_utf8(path_from_utf8(music_path_).extension()));
    if (e == ".ogg" || e == ".wav") ext = e;
  }
  return "music_" + cue + ext;
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
    options.meta.wave_path = path_to_utf8(path_from_utf8(music_path_).filename());
  }
  options.ched_lane_padding = true;
  return options;
}

void EditorSession::apply_chart_delay() {
  // Chart delay: tick 0 maps to music time offset_ms (may be negative).
  engine().document().set_offset_ms(offset_ms_);
  engine().rebuild_snapshot();
  preview_.transport().set_chart_offset_ms(offset_ms_);
}

void EditorSession::sync_active_chart() {
  if (charts_.empty() || active_chart_index_ >= charts_.size()) return;
  auto& slot = charts_[active_chart_index_];
  slot.chart = engine().document().to_notation_chart();
  slot.chart.timing.offset_ms = 0;
  slot.dirty = slot.dirty || engine().is_dirty();
}

void EditorSession::stash_active() {
  sync_active_chart();
  if (charts_.empty() || active_chart_index_ >= charts_.size()) return;
  charts_[active_chart_index_].history = std::move(engine().history());
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
  status("已新建工程", StatusLevel::Info);
  return true;
}

bool EditorSession::open_wdsproject(const std::string& path) {
  return apply_prepared_wdsproject(prepare_wdsproject(path));
}

PreparedWdsProject EditorSession::prepare_wdsproject(const std::string& path) {
  PreparedWdsProject prepared;
  prepared.project_path = path;
  wds::chart_editor::WdsProject project;
  if (!ok(wds::chart_editor::ProjectSerializer::load_from_file(path, project))) {
    prepared.error = "无法读取工程文件";
    return prepared;
  }

  prepared.music_path =
      wds::chart_editor::ProjectSerializer::resolve_path(path, project.music_path);
  prepared.chart_paths.reserve(project.chart_paths.size());
  prepared.charts.reserve(project.chart_paths.size());
  for (const auto& chart_rel : project.chart_paths) {
    const std::string chart_path =
        wds::chart_editor::ProjectSerializer::resolve_path(path, chart_rel);
    wds::chart_editor::NotationChart chart;
    if (!ok(wds::chart_editor::ChartSerializer::load_from_file(chart_path, chart))) {
      prepared.error = "无法加载谱面 " + chart_path;
      prepared.chart_paths.clear();
      prepared.charts.clear();
      return prepared;
    }
    chart.timing.offset_ms = 0;
    prepared.chart_paths.push_back(chart_path);
    prepared.charts.push_back(std::move(chart));
  }
  if (prepared.charts.empty()) {
    prepared.error = "工程未包含任何谱面";
    return prepared;
  }

  prepared.active_chart_index = static_cast<std::size_t>(std::clamp(
      project.active_chart_index, 0, static_cast<int32_t>(prepared.charts.size() - 1)));
  prepared.offset_ms = project.offset_ms;
  return prepared;
}

bool EditorSession::apply_prepared_wdsproject(PreparedWdsProject prepared) {
  if (!prepared.valid() || prepared.chart_paths.size() != prepared.charts.size()) {
    status("打开失败：" + (prepared.error.empty() ? std::string("工程数据无效")
                                                   : prepared.error),
           StatusLevel::Error);
    return false;
  }

  // Build session slots only on the owner thread. Preparation above is pure
  // file I/O/parsing and never exposes a half-loaded project to rendering.
  std::vector<ChartSlot> loaded;
  loaded.reserve(prepared.charts.size());
  for (std::size_t i = 0; i < prepared.charts.size(); ++i) {
    ChartSlot slot;
    slot.path = std::move(prepared.chart_paths[i]);
    slot.chart = std::move(prepared.charts[i]);
    slot.dirty = false;
    loaded.push_back(std::move(slot));
  }

  // Missing/unloadable music is non-fatal — charts still open.
  std::string loaded_music = std::move(prepared.music_path);
  std::string music_note;
  if (preview_.ready()) {
    if (!preview_.load_music(loaded_music, false)) {
      if (!loaded_music.empty()) {
        music_note = "（音乐未加载，可重新导入）";
      }
      (void)preview_.load_music({}, false);
    }
  }

  project_path_ = std::move(prepared.project_path);
  music_path_ = loaded_music;
  charts_ = std::move(loaded);
  active_chart_index_ = prepared.active_chart_index;
  offset_ms_ = prepared.offset_ms;
  metadata_dirty_ = false;
  read_only_ = false;
  allow_delay_when_read_only_ = false;
  reset_music_config_meta();

  if (!activate_chart(active_chart_index_)) {
    status("打开失败：无法激活谱面", StatusLevel::Error);
    return false;
  }
  preview_.reset_playback();
  status("已打开工程：" + project_path_ + music_note, StatusLevel::Info);
  return true;
}

bool EditorSession::collect_chart_paths_for_save(std::vector<std::string>& out_paths) {
  // Collect destinations without mutating session paths — commit only after each
  // write succeeds so a failed save never "locks" a bad path into the session.
  out_paths.clear();
  out_paths.reserve(charts_.size());
  for (std::size_t i = 0; i < charts_.size(); ++i) {
    if (!charts_[i].path.empty()) {
      out_paths.push_back(charts_[i].path);
      continue;
    }
    const std::string default_name = std::to_string(i + 1) + ".wdschart";
    const std::string title = "保存谱面 " + std::to_string(i + 1);
    auto chosen = native_file_dialog::save_file(title, default_name, {"wdschart"});
    if (!chosen || chosen->empty()) return false;
    out_paths.push_back(*chosen);
  }
  return true;
}

bool EditorSession::write_all_charts_and_project(const std::string& project_path) {
  if (read_only_) {
    status("只读预览无法保存（官方/SUS 预览）。开启「SUS 自动转换」后导入 SUS 可编辑",
           StatusLevel::Error);
    return false;
  }
  if (project_path.empty() || charts_.empty()) {
    status("保存失败：没有可写入的工程或谱面", StatusLevel::Error);
    return false;
  }
  // Snapshot chart data only — keep engine undo history (scheme B).
  sync_active_chart();

  std::vector<std::string> chart_paths;
  if (!collect_chart_paths_for_save(chart_paths)) {
    status("保存已取消：需要为未绑定谱面指定 .wdschart 路径", StatusLevel::Info);
    return false;
  }

  for (std::size_t i = 0; i < charts_.size(); ++i) {
    auto chart = normalized_copy(charts_[i].chart);
    chart.timing.offset_ms = 0;
    const auto chart_save =
        wds::chart_editor::ChartSerializer::save_to_file(chart, chart_paths[i]);
    if (!ok(chart_save)) {
      status("保存失败：无法写入谱面 " + chart_paths[i] +
                 (chart_save.message.empty() ? "" : " — " + chart_save.message),
             StatusLevel::Error);
      return false;
    }
    // Commit path only after the file is on disk. Keep slot chart ids session-stable.
    charts_[i].path = chart_paths[i];
    charts_[i].dirty = false;
  }

  wds::chart_editor::WdsProject project;
  project.music_path = music_path_;  // path reference only — never copy audio on save
  project.offset_ms = offset_ms_;
  project.active_chart_index = static_cast<int32_t>(active_chart_index_);
  project.chart_paths.clear();
  project.chart_paths.reserve(charts_.size());
  for (const auto& slot : charts_) {
    project.chart_paths.push_back(slot.path);
  }
  const auto project_save =
      wds::chart_editor::ProjectSerializer::save_relativized(std::move(project), project_path);
  if (!ok(project_save)) {
    status("保存失败：无法写入工程 " + project_path +
               (project_save.message.empty() ? "" : " — " + project_save.message),
           StatusLevel::Error);
    return false;
  }

  project_path_ = project_path;
  metadata_dirty_ = false;
  engine().mark_saved();
  status("已保存工程：" + project_path, StatusLevel::Info);
  return true;
}

bool EditorSession::save() {
  if (read_only_) {
    status("只读预览无法保存", StatusLevel::Error);
    return false;
  }
  if (project_path_.empty()) return false;
  return write_all_charts_and_project(project_path_);
}

bool EditorSession::save_as(const std::string& path) {
  if (read_only_) {
    status("只读预览无法保存", StatusLevel::Error);
    return false;
  }
  if (path.empty()) return false;
  return write_all_charts_and_project(path);
}

bool EditorSession::import_official_pack(const std::string& music_config_path) {
  wds::chart_editor::OfficialMusicConfig music;
  if (!ok(wds::chart_editor::OfficialChartFormat::load_music_config_file(music_config_path,
                                                                        music))) {
    return false;
  }

  const std::string dir = path_to_utf8(path_from_utf8(music_config_path).parent_path());
  const auto chart_paths = list_official_charts(dir);
  if (chart_paths.empty()) return false;

  std::vector<ChartSlot> loaded;
  loaded.reserve(chart_paths.size());
  for (const auto& chart_path : chart_paths) {
    wds::chart_editor::NotationChart chart;
    if (!ok(wds::chart_editor::OfficialChartFormat::load_chart_with_music_config(
            chart_path, music_config_path, chart))) {
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
  if (!activate_chart(0)) {
    status("导入失败：无法激活谱面", StatusLevel::Error);
    return false;
  }
  preview_.reset_playback();
  status("已导入官方曲包（只读预览）：" + music_config_path, StatusLevel::Info);
  return true;
}

bool EditorSession::import_official(const std::string& chart_path,
                                    const std::string& music_config_path) {
  if (chart_path.empty()) return false;
  if (is_music_config_path(chart_path)) {
    return import_official_pack(chart_path);
  }
  if (wds::chart_editor::SusChartFormat::looks_like_sus_path(chart_path)) {
    return import_sus(chart_path);
  }
  // Content sniff for mislabeled files.
  {
    const std::string bytes = read_text_utf8(chart_path);
    if (!bytes.empty()) {
      const std::string sample = bytes.substr(0, std::min<std::size_t>(bytes.size(), 512));
      if (wds::chart_editor::SusChartFormat::looks_like_sus_text(sample)) {
        return import_sus(chart_path);
      }
    }
  }

  std::string config_path = music_config_path;
  if (config_path.empty()) {
    config_path = sibling_music_config(chart_path);
  } else if (!file_is_regular_utf8(config_path)) {
    config_path.clear();
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
    // Official / SUS preview must not bind source paths — saving is read-only blocked,
    // and unbound charts must stay unbound until the user explicitly picks a destination.
    ChartSlot slot;
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
  status("已导入官方谱面（只读预览）：" + chart_path, StatusLevel::Info);
  return true;
}

bool EditorSession::import_sus(const std::string& path) {
  wds::chart_editor::SusChartMetadata meta;
  std::vector<std::string> warnings;
  if (!ok(engine().load_sus_from_file(path, &meta, &warnings))) return false;

  project_path_.clear();
  offset_ms_ = engine().document().timing().offset_ms;
  auto chart = engine().document().to_notation_chart();
  chart.timing.offset_ms = 0;
  charts_.clear();
  // Auto-convert: editable in-memory WDS project (do not bind the .sus path).
  // Preview-only also stays unbound so a failed later save cannot stick to the .sus.
  const bool convert = sus_auto_convert_;
  {
    ChartSlot slot;
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
    fs::path wave = path_from_utf8(meta.wave_path);
    if (!wave.is_absolute()) {
      wave = path_from_utf8(path).parent_path() / wave;
    }
    const std::string wave_utf8 = path_to_utf8(wave);
    if (file_is_regular_utf8(wave_utf8)) {
      music_path_ = wave_utf8;
    }
  }
  if (!music_path_.empty() && !preview_.load_music(music_path_, false)) {
    music_path_.clear();
    preview_.load_music({}, false);
  } else if (music_path_.empty()) {
    preview_.load_music({}, false);
  }
  // Reload into Editable (convert) or keep OfficialPreviewOnly (preview-only).
  if (!activate_chart(0)) {
    status("导入失败：无法激活 SUS 谱面", StatusLevel::Error);
    return false;
  }
  preview_.reset_playback();
  std::string msg;
  if (convert) {
    msg = "已导入 SUS 并转换为可编辑工程（内存，未绑定文件）：" + path;
  } else {
    msg = "已导入 SUS（只读预览）：" + path;
  }
  if (!warnings.empty()) {
    msg += "（警告 " + std::to_string(warnings.size()) + "：" + warnings.front();
    if (warnings.size() > 1) msg += " …";
    msg += "）";
    status(std::move(msg), StatusLevel::Warning);
  } else {
    status(std::move(msg), StatusLevel::Info);
  }
  return true;
}

bool EditorSession::export_official(const std::string& path) {
  // Imported official charts are intentionally preview-only and never re-exported.
  if (read_only_) {
    status("只读预览无法导出", StatusLevel::Error);
    return false;
  }
  if (!ok(engine().export_official_to_file(path))) {
    status("导出失败：" + path, StatusLevel::Error);
    return false;
  }
  status("已导出官方谱面：" + path, StatusLevel::Info);
  return true;
}

bool EditorSession::export_sus(const std::string& path) {
  if (read_only_ || path.empty()) {
    status(read_only_ ? "只读预览无法导出" : "导出已取消", StatusLevel::Error);
    return false;
  }
  if (!ok(engine().export_sus_to_file(path, make_sus_save_options()))) {
    status("导出 SUS 失败：" + path, StatusLevel::Error);
    return false;
  }
  status("已导出 SUS：" + path, StatusLevel::Info);
  return true;
}

bool EditorSession::export_sus_project(const std::string& directory) {
  if (read_only_) {
    status("只读预览无法导出", StatusLevel::Error);
    return false;
  }
  if (directory.empty() || charts_.empty()) {
    status("导出失败：无效目录或无谱面", StatusLevel::Error);
    return false;
  }
  sync_active_chart();

  if (!dir_is_directory_utf8(directory)) {
    status("导出失败：目录不可用", StatusLevel::Error);
    return false;
  }

  std::vector<std::string> planned;
  planned.reserve(charts_.size());
  for (std::size_t i = 0; i < charts_.size(); ++i) {
    planned.push_back(join_utf8(directory, sus_chart_filename(i)));
  }

  std::vector<std::string> conflicts;
  for (const auto& p : planned) {
    if (path_exists_utf8(p)) conflicts.push_back(p);
  }
  bool overwrite = true;
  if (!conflicts.empty()) {
    overwrite = native_file_dialog::confirm(
        "导出冲突", "目标目录存在同名文件，是否覆盖？选择 No 将跳过冲突文件。");
  }

  auto options = make_sus_save_options();
  bool any_written = false;
  for (std::size_t i = 0; i < charts_.size(); ++i) {
    const std::string& out = planned[i];
    const bool exists = path_exists_utf8(out);
    if (exists && !overwrite) continue;
    auto chart = charts_[i].chart;
    chart.timing.offset_ms = offset_ms_;
    if (!ok(wds::chart_editor::SusChartFormat::save_file(chart, out, options))) {
      status("导出 SUS 工程失败：" + out, StatusLevel::Error);
      return any_written;
    }
    any_written = true;
  }
  if (any_written || conflicts.empty()) {
    status("已导出 SUS 工程：" + directory, StatusLevel::Info);
    return true;
  }
  status("导出已跳过（存在冲突且未覆盖）", StatusLevel::Info);
  return false;
}

bool EditorSession::export_official_project(const std::string& directory) {
  if (read_only_) {
    status("只读预览无法导出", StatusLevel::Error);
    return false;
  }
  if (directory.empty() || charts_.empty()) {
    status("导出失败：无效目录或无谱面", StatusLevel::Error);
    return false;
  }
  sync_active_chart();

  if (!dir_is_directory_utf8(directory)) {
    status("导出失败：目录不可用", StatusLevel::Error);
    return false;
  }

  struct PlannedFile {
    std::string path;
    enum class Kind { Chart, MusicConfig, Audio } kind;
    std::size_t chart_index = 0;
  };

  std::vector<PlannedFile> planned;
  planned.reserve(charts_.size() + 2);
  for (std::size_t i = 0; i < charts_.size(); ++i) {
    planned.push_back({join_utf8(directory, official_chart_filename(i)), PlannedFile::Kind::Chart, i});
  }
  planned.push_back({join_utf8(directory, "music_config.csv"), PlannedFile::Kind::MusicConfig, 0});
  const bool has_audio = !music_path_.empty() && file_is_regular_utf8(music_path_);
  if (has_audio) {
    planned.push_back(
        {join_utf8(directory, official_audio_filename()), PlannedFile::Kind::Audio, 0});
  }

  std::vector<std::string> conflicts;
  for (const auto& item : planned) {
    if (path_exists_utf8(item.path)) conflicts.push_back(item.path);
  }

  bool overwrite = true;
  if (!conflicts.empty()) {
    overwrite = native_file_dialog::confirm(
        "导出冲突", "目标目录存在同名文件，是否覆盖？选择 No 将跳过冲突文件。");
  }

  bool any_written = false;
  for (const auto& item : planned) {
    const bool exists = path_exists_utf8(item.path);
    if (exists && !overwrite) continue;

    switch (item.kind) {
      case PlannedFile::Kind::Chart: {
        auto chart = charts_[item.chart_index].chart;
        chart.timing.offset_ms = 0;
        if (!ok(wds::chart_editor::OfficialChartFormat::save_chart_file(chart, item.path))) {
          status("导出失败：无法写入谱面 " + item.path, StatusLevel::Error);
          return any_written;
        }
        any_written = true;
        break;
      }
      case PlannedFile::Kind::MusicConfig: {
        if (!ok(wds::chart_editor::OfficialChartFormat::save_music_config_file(make_music_config(),
                                                                               item.path))) {
          status("导出失败：无法写入 music_config.csv", StatusLevel::Error);
          return any_written;
        }
        any_written = true;
        break;
      }
      case PlannedFile::Kind::Audio: {
        std::error_code copy_ec;
        if (!copy_utf8_overwrite(music_path_, item.path, copy_ec)) {
          status("导出失败：无法复制音乐文件", StatusLevel::Error);
          return any_written;
        }
        any_written = true;
        break;
      }
    }
  }
  if (any_written || conflicts.empty()) {
    status("已导出官方工程：" + directory, StatusLevel::Info);
    return true;
  }
  status("导出已跳过（存在冲突且未覆盖）", StatusLevel::Info);
  return false;
}

bool EditorSession::import_music(const std::string& path) {
  // Allowed in official read-only preview — BGM is independent of chart authoring.
  if (path.empty() || !preview_.load_music(path)) {
    status("导入音乐失败：无法加载文件", StatusLevel::Error);
    return false;
  }
  // Keep absolute path in-session for playback; save only stores a path reference.
  music_path_ = path;
  if (!read_only_) {
    metadata_dirty_ = true;
  }
  // If the imported file already uses official naming, adopt its cue id (ASCII only).
  const std::string name = path_to_utf8(path_from_utf8(path).filename());
  if (starts_with_ci(name, "music_") && ends_with_ci(name, ".ogg")) {
    const std::string stem = path_to_utf8(path_from_utf8(name).stem());  // music_1
    if (stem.size() > 6) {
      cue_name_ = ascii_token_or(stem.substr(6), cue_name_.empty() ? "1" : cue_name_);
    }
  }
  status("已导入音乐：" + path, StatusLevel::Info);
  return true;
}

bool EditorSession::set_base_bpm(double bpm) {
  if (!engine().is_editable() || !std::isfinite(bpm) || bpm <= 0.0 || bpm > 10000.0) {
    return false;
  }
  const auto before = engine().document().timing();
  if (std::abs(before.bpm - bpm) < 1e-9) return true;

  auto after = before;
  after.bpm = bpm;
  auto root = std::find_if(after.points.begin(), after.points.end(),
                           [](const auto& point) { return point.tick == 0; });
  if (root == after.points.end()) {
    after.points.push_back({0, bpm, 4, 4, true, true});
  } else {
    root->bpm = bpm;
    root->has_bpm = true;
  }
  return engine().execute_command(std::make_unique<wds::chart_editor::SetTimingCommand>(
      before, std::move(after), "Edit base BPM"));
}

bool EditorSession::set_offset_ms(int64_t offset_ms) {
  last_offset_violation_ids_.clear();
  if (!delay_editable()) return false;
  if (offset_ms < -60000 || offset_ms > 60000) return false;
  if (offset_ms == offset_ms_) return true;

  auto collect = [&](const std::vector<wds::chart_editor::NotationNote>& notes,
                     wds::chart_editor::MusicTiming timing) {
    timing.offset_ms = offset_ms;
    return wds::chart_editor::notes_in_negative_music_time(notes, timing);
  };

  bool blocked = false;
  auto active_ids = collect(engine().document().notes(), engine().document().timing());
  if (!active_ids.empty()) {
    last_offset_violation_ids_ = std::move(active_ids);
    blocked = true;
  }
  for (std::size_t i = 0; i < charts_.size(); ++i) {
    if (i == active_chart_index_) continue;
    if (!collect(charts_[i].chart.notes, charts_[i].chart.timing).empty()) {
      blocked = true;
    }
  }
  if (blocked) return false;

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
