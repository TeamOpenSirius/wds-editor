#include <wds/core/file_io.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace wds::chart_editor {
namespace {

namespace fs = std::filesystem;

std::string make_temp_path(const std::string& path) {
  return path + ".wds-tmp";
}

}  // namespace

SerializeResult replace_file_atomic(const std::string& path, const std::string& temp_path) {
  const fs::path target(path);
  const fs::path temp(temp_path);
  std::error_code ec;
  fs::rename(temp, target, ec);
  if (ec) {
    // Windows (and some network FS) refuse rename over an existing file.
    fs::remove(target, ec);
    ec.clear();
    fs::rename(temp, target, ec);
  }
  if (ec) {
    std::error_code cleanup;
    fs::remove(temp, cleanup);
    return {SerializeError::IoError, "failed to replace file: " + path + " (" + ec.message() + ")"};
  }
  return {SerializeError::Ok, {}};
}

SerializeResult write_text_atomic(const std::string& path, const std::string& text) {
  const std::string temp_path = make_temp_path(path);
  {
    std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      return {SerializeError::IoError, "failed to open temp file for writing: " + temp_path};
    }
    file << text;
    file.flush();
    if (!file) {
      std::error_code cleanup;
      fs::remove(temp_path, cleanup);
      return {SerializeError::IoError, "failed while writing: " + temp_path};
    }
  }
  return replace_file_atomic(path, temp_path);
}

}  // namespace wds::chart_editor
