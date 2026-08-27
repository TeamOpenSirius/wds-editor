#include "wds/ui/native_file_dialog.hpp"

#if defined(__APPLE__)
#include "wds/ui/macos_file_dialog.hpp"
#endif

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#if !defined(__APPLE__) && !defined(_WIN32)
#include <sys/wait.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace wds::ui::native_file_dialog {
namespace {

#if defined(_WIN32)
HWND g_owner_hwnd = nullptr;
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
std::string apple_quote(const std::string& value) {
  std::string out = "\"";
  for (char ch : value) {
    if (ch == '"' || ch == '\\') out += '\\';
    out += ch;
  }
  return out + '"';
}

std::string shell_single_quote(const std::string& value) {
  std::string out = "'";
  for (char ch : value) {
    if (ch == '\'') out += "'\\''";
    else out += ch;
  }
  return out + "'";
}

std::optional<std::string> run_line(const std::string& command) {
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
  if (!pipe) return std::nullopt;
  std::array<char, 1024> buffer{};
  std::string value;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get())) value += buffer.data();
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  return value.empty() ? std::nullopt : std::optional<std::string>(value);
}
#endif

std::string to_lower_ascii(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

std::string strip_dot(std::string ext) {
  while (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
  return ext;
}

bool ends_with_ci(const std::string& value, const std::string& suffix) {
  if (suffix.size() > value.size()) return false;
  return to_lower_ascii(value.substr(value.size() - suffix.size())) == to_lower_ascii(suffix);
}

bool path_has_allowed_extension(const std::string& path, const Filters& filters) {
  for (const auto& raw : filters) {
    const std::string ext = strip_dot(raw);
    if (ext.empty()) continue;
    if (ends_with_ci(path, "." + ext)) return true;
  }
  return false;
}

// If path lacks an allowed extension (or has a wrong one), force the preferred suffix.
std::string ensure_extension(std::string path, const Filters& filters) {
  if (path.empty() || filters.empty()) return path;
  if (path_has_allowed_extension(path, filters)) return path;

  const auto slash = path.find_last_of("/\\");
  const std::size_t base_start = slash == std::string::npos ? 0 : slash + 1;
  const auto dot = path.find_last_of('.');
  if (dot != std::string::npos && dot > base_start) {
    path.resize(dot);
  }
  path += '.';
  path += strip_dot(filters.front());
  return path;
}

#if defined(_WIN32)
std::string preferred_extension(const Filters& filters, const std::string& default_name) {
  const auto dot = default_name.rfind('.');
  if (dot != std::string::npos && dot + 1 < default_name.size()) {
    const std::string from_name = strip_dot(default_name.substr(dot + 1));
    if (!from_name.empty()) {
      if (filters.empty() || path_has_allowed_extension("x." + from_name, filters)) {
        return from_name;
      }
    }
  }
  if (!filters.empty()) return strip_dot(filters.front());
  return {};
}
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
std::string zenity_file_filter_args(const Filters& filters) {
  if (filters.empty()) return {};
  std::string label;
  std::string patterns;
  for (std::size_t i = 0; i < filters.size(); ++i) {
    const std::string ext = strip_dot(filters[i]);
    if (ext.empty()) continue;
    if (!label.empty()) label += '/';
    label += ext;
    if (!patterns.empty()) patterns += ' ';
    patterns += "*.";
    patterns += ext;
  }
  if (patterns.empty()) return {};
  // zenity: --file-filter='NAME | PATTERNS'
  return " --file-filter=" + shell_single_quote(label + " | " + patterns);
}
#endif

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

struct ComScope {
  bool ok_ = false;
  ComScope() {
    // Keep COM alive for the process. CoUninitialize mid-run tears down the
    // apartment DXGI/Vulkan/WASAPI may already be using → Win32 graphics faults.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ok_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE || hr == S_FALSE;
  }
  bool ok() const { return ok_; }
};

struct FilterScratch {
  std::wstring label;
  std::wstring pattern;
};

void build_filters(const Filters& filters, std::vector<FilterScratch>& scratch,
                   std::vector<COMDLG_FILTERSPEC>& specs) {
  scratch.clear();
  specs.clear();
  if (!filters.empty()) {
    FilterScratch entry;
    for (std::size_t i = 0; i < filters.size(); ++i) {
      const std::string ext = strip_dot(filters[i]);
      if (ext.empty()) continue;
      if (!entry.label.empty()) {
        entry.label += L";";
        entry.pattern += L";";
      }
      entry.label += utf8_to_wide(ext);
      entry.pattern += L"*.";
      entry.pattern += utf8_to_wide(ext);
    }
    if (!entry.pattern.empty()) scratch.push_back(std::move(entry));
  }
  // Keep an escape hatch after the typed filter(s).
  scratch.push_back(FilterScratch{L"All files", L"*.*"});
  specs.reserve(scratch.size());
  for (const auto& item : scratch) {
    specs.push_back(COMDLG_FILTERSPEC{item.label.c_str(), item.pattern.c_str()});
  }
}

std::optional<std::string> path_from_shell_item(IShellItem* item) {
  if (item == nullptr) return std::nullopt;
  PWSTR path_w = nullptr;
  const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path_w);
  if (FAILED(hr) || path_w == nullptr) return std::nullopt;
  std::string path = wide_to_utf8(path_w);
  CoTaskMemFree(path_w);
  return path.empty() ? std::nullopt : std::optional<std::string>(std::move(path));
}

std::optional<std::string> run_file_dialog(bool save, bool directory, const std::string& title,
                                           const std::string& default_name, const Filters& filters) {
  ComScope com;
  if (!com.ok()) return std::nullopt;

  IFileDialog* dialog = nullptr;
  HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog))
                    : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog));
  if (FAILED(hr) || dialog == nullptr) return std::nullopt;

  DWORD options = 0;
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (save) options |= FOS_OVERWRITEPROMPT;
    else options |= FOS_FILEMUSTEXIST;
    if (directory) options = (options | FOS_PICKFOLDERS) & ~FOS_FILEMUSTEXIST;
    // Prefer typed filters; still allow All files via the filter dropdown.
    if (!directory && !filters.empty() && !save) {
      options |= FOS_STRICTFILETYPES;
    }
    dialog->SetOptions(options);
  }

  const std::wstring title_w = utf8_to_wide(title);
  if (!title_w.empty()) dialog->SetTitle(title_w.c_str());

  if (!default_name.empty()) {
    const std::wstring name_w = utf8_to_wide(default_name);
    dialog->SetFileName(name_w.c_str());
  }

  // IFileDialog::SetDefaultExtension appends this when the typed name has no extension.
  if (save) {
    const std::string ext = preferred_extension(filters, default_name);
    if (!ext.empty()) {
      const std::wstring ext_w = utf8_to_wide(ext);
      dialog->SetDefaultExtension(ext_w.c_str());
    }
  }

  std::vector<FilterScratch> scratch;
  std::vector<COMDLG_FILTERSPEC> specs;
  if (!directory) {
    build_filters(filters, scratch, specs);
    if (!specs.empty()) {
      dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
      // Select the typed filter (index 1), not "All files".
      if (!filters.empty()) dialog->SetFileTypeIndex(1);
    }
  }

  hr = dialog->Show(g_owner_hwnd);
  std::optional<std::string> result;
  if (SUCCEEDED(hr)) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
      result = path_from_shell_item(item);
      item->Release();
    }
  }
  dialog->Release();
  if (result && save) {
    result = ensure_extension(std::move(*result), filters);
  }
  return result;
}
#endif

}  // namespace

void set_owner_window(void* hwnd) {
#if defined(_WIN32)
  g_owner_hwnd = static_cast<HWND>(hwnd);
#else
  (void)hwnd;
#endif
}

std::optional<std::string> open_file(const std::string& title, const Filters& filters) {
#if defined(__APPLE__)
  return macos_file_dialog::open_file(title, filters);
#elif defined(_WIN32)
  return run_file_dialog(/*save=*/false, /*directory=*/false, title, {}, filters);
#else
  std::string cmd = "zenity --file-selection --title=" + apple_quote(title);
  cmd += zenity_file_filter_args(filters);
  cmd += " 2>/dev/null";
  return run_line(cmd);
#endif
}

std::optional<std::string> save_file(const std::string& title, const std::string& default_name,
                                     const Filters& filters) {
#if defined(__APPLE__)
  auto path = macos_file_dialog::save_file(title, default_name, filters);
  if (!path) return std::nullopt;
  return ensure_extension(std::move(*path), filters);
#elif defined(_WIN32)
  return run_file_dialog(/*save=*/true, /*directory=*/false, title, default_name, filters);
#else
  std::string cmd = "zenity --file-selection --save --confirm-overwrite --title=" +
                    apple_quote(title) + " --filename=" + apple_quote(default_name);
  cmd += zenity_file_filter_args(filters);
  cmd += " 2>/dev/null";
  auto path = run_line(cmd);
  if (!path) return std::nullopt;
  return ensure_extension(std::move(*path), filters);
#endif
}

std::vector<std::string> open_files(const std::string& title, const Filters& filters) {
  auto line = open_file(title, filters);
  return line ? std::vector<std::string>{*line} : std::vector<std::string>{};
}

std::optional<std::string> choose_directory(const std::string& title) {
#if defined(__APPLE__)
  return macos_file_dialog::choose_directory(title);
#elif defined(_WIN32)
  return run_file_dialog(/*save=*/false, /*directory=*/true, title, {}, {});
#else
  return run_line("zenity --file-selection --directory --title=" + apple_quote(title) +
                  " 2>/dev/null");
#endif
}

bool confirm(const std::string& title, const std::string& message) {
#if defined(__APPLE__)
  return macos_file_dialog::confirm(title, message);
#elif defined(_WIN32)
  const std::wstring title_w = utf8_to_wide(title);
  const std::wstring message_w = utf8_to_wide(message);
  const int result =
      MessageBoxW(g_owner_hwnd, message_w.c_str(), title_w.c_str(),
                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
  return result == IDYES;
#else
  const int status = std::system(("zenity --question --title=" + apple_quote(title) +
                                  " --text=" + apple_quote(message) + " 2>/dev/null")
                                     .c_str());
  return status != -1 && WEXITSTATUS(status) == 0;
#endif
}

void alert_error(const std::string& title, const std::string& message) {
#if defined(__APPLE__)
  macos_file_dialog::alert_error(title, message);
#elif defined(_WIN32)
  const std::wstring title_w = utf8_to_wide(title);
  const std::wstring message_w = utf8_to_wide(message);
  MessageBoxW(g_owner_hwnd, message_w.c_str(), title_w.c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST);
#else
  // Prefer zenity; fall back to notify-send / stderr if the desktop helper is missing.
  const int status = std::system(("zenity --error --title=" + apple_quote(title) +
                                  " --text=" + apple_quote(message) + " --width=420 2>/dev/null")
                                     .c_str());
  if (status == -1 || WEXITSTATUS(status) == 127) {
    std::fprintf(stderr, "%s\n%s\n", title.c_str(), message.c_str());
  }
#endif
}

SaveDiscardCancel confirm_save_discard_cancel(const std::string& title, const std::string& message) {
#if defined(__APPLE__)
  return macos_file_dialog::confirm_save_discard_cancel(title, message);
#elif defined(_WIN32)
  const std::wstring title_w = utf8_to_wide(title);
  const std::wstring message_w = utf8_to_wide(message);
  const int result = MessageBoxW(g_owner_hwnd, message_w.c_str(), title_w.c_str(),
                                 MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
  if (result == IDYES) return SaveDiscardCancel::Save;
  if (result == IDNO) return SaveDiscardCancel::Discard;
  return SaveDiscardCancel::Cancel;
#else
  std::string cmd = "zenity --question --title=" + apple_quote(title) +
                    " --text=" + apple_quote(message) +
                    " --ok-label=保存 --cancel-label=不保存 --extra-button=取消 2>/dev/null";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return SaveDiscardCancel::Cancel;
  char buf[64] = {};
  const bool got = std::fgets(buf, sizeof(buf), pipe) != nullptr;
  const int status = pclose(pipe);
  std::string out = got ? std::string(buf) : std::string{};
  while (!out.empty() &&
         (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t')) {
    out.pop_back();
  }
  if (out.find("取消") != std::string::npos) return SaveDiscardCancel::Cancel;
  if (status != -1 && WEXITSTATUS(status) == 0) return SaveDiscardCancel::Save;
  return SaveDiscardCancel::Discard;
#endif
}

}  // namespace wds::ui::native_file_dialog
