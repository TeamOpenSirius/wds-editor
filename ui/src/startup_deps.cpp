#include "wds/ui/startup_deps.hpp"

#include "wds/ui/native_file_dialog.hpp"
#include "wds/ui/resource_paths.hpp"

#include <wds/audio/audio_engine.hpp>
#include <wds/common/log.hpp>
#include <wds/common/utf8_path.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wds::ui {
namespace fs = std::filesystem;

namespace {

using wds::common::executable_dir;
using wds::common::path_from_utf8;
using wds::common::path_to_utf8;

bool is_regular(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec) && !ec;
}

bool is_dir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec) && !ec;
}

bool any_exists(std::initializer_list<fs::path> paths) {
  for (const auto& p : paths) {
    if (is_regular(p)) {
      return true;
    }
  }
  return false;
}

#if defined(__APPLE__)
bool looks_like_macos_app(const fs::path& exe_dir) {
  // .../Something.app/Contents/MacOS
  const auto macos = exe_dir.filename();
  const auto contents = exe_dir.parent_path().filename();
  return macos == "MacOS" && contents == "Contents";
}
#endif

void require_resource_dir(StartupDependencyReport& report, const std::string& resolved,
                          const char* label, bool (*looks_ok)(const std::string&)) {
  if (resolved.empty() || !looks_ok(resolved)) {
    report.missing.push_back(std::string(label) + "（未找到可用目录）");
  }
}

void check_bass_runtime(StartupDependencyReport& report, const fs::path& exe_dir) {
  // If we reached main(), the dynamic linker already mapped BASS. Still verify
  // the API responds, and that the companion file exists for portable layouts.
  const uint32_t ver = wds::audio::linked_bass_version();
  if (ver == 0) {
    report.missing.push_back("BASS 音频库（linked_bass_version 失败）");
    return;
  }
#if defined(_WIN32)
  if (!any_exists({exe_dir / "bass.dll", exe_dir / "lib" / "bass.dll"})) {
    report.missing.push_back("bass.dll（应位于程序目录）");
  }
#elif defined(__APPLE__)
  if (!any_exists({exe_dir / "libbass.dylib", exe_dir / "lib" / "libbass.dylib"})) {
    report.missing.push_back("libbass.dylib（应位于程序目录或 lib/）");
  }
#else
  if (!any_exists({exe_dir / "libbass.so", exe_dir / "lib" / "libbass.so"})) {
    report.missing.push_back("libbass.so（应位于程序目录或 lib/）");
  }
#endif
}

#if defined(__APPLE__)
fs::path bundled_moltenvk_icd(const fs::path& exe_dir) {
  // Loader auto-discovery path for .app bundles. Must live under Resources/
  // (not Contents/MacOS/) so codesign can seal the bundle.
  const fs::path standard =
      (exe_dir / ".." / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json").lexically_normal();
  if (is_regular(standard)) {
    return standard;
  }
  const fs::path legacy =
      (exe_dir / ".." / "Resources" / "share" / "vulkan" / "icd" / "MoltenVK_icd.json")
          .lexically_normal();
  if (is_regular(legacy)) {
    return legacy;
  }
  return {};
}
#endif

void check_vulkan_runtime_files(StartupDependencyReport& report, const fs::path& exe_dir) {
#if defined(__APPLE__)
  if (looks_like_macos_app(exe_dir)) {
    const fs::path molten = exe_dir / "lib" / "libMoltenVK.dylib";
    const fs::path icd = bundled_moltenvk_icd(exe_dir);
    if (!is_regular(molten)) {
      report.missing.push_back("libMoltenVK.dylib（.app 内 Contents/MacOS/lib/）");
    }
    if (icd.empty()) {
      report.missing.push_back(
          "MoltenVK_icd.json（.app 内 Contents/Resources/vulkan/icd.d/）");
    }
  } else {
    // Dev / zip layout: optional MoltenVK beside the binary; loader may use brew ICD.
    (void)exe_dir;
  }
#elif defined(_WIN32)
  // Packaged builds ship vulkan-1.dll next to the exe. System installs may omit it.
  const fs::path local_vk = exe_dir / "vulkan-1.dll";
  if (is_regular(local_vk)) {
    return;  // bundled loader present
  }
  if (::GetModuleHandleW(L"vulkan-1.dll") == nullptr) {
    // Not loaded yet. Probe LoadLibrary without keeping it.
    HMODULE mod = ::LoadLibraryW(L"vulkan-1.dll");
    if (mod == nullptr) {
      report.missing.push_back(
          "vulkan-1.dll（请安装 Vulkan Runtime，或将 DLL 放到程序目录）");
    } else {
      ::FreeLibrary(mod);
    }
  }
#else
  // Linux: ICD comes from the GPU driver; only ensure the loader is findable.
  void* vk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_NOLOAD);
  if (vk == nullptr) {
    vk = dlopen("libvulkan.so.1", RTLD_LAZY);
  }
  if (vk == nullptr) {
    report.missing.push_back("libvulkan.so.1（请安装 Vulkan 驱动 / mesa-vulkan-drivers）");
  } else {
    dlclose(vk);
  }
  (void)exe_dir;
#endif
}

}  // namespace

std::string StartupDependencyReport::format_message() const {
  std::string body = "启动失败：缺少必要的外部依赖或资源。\n\n";
  for (const auto& line : missing) {
    body += "• ";
    body += line;
    body += '\n';
  }
  body += "\n请重新安装完整程序包，或检查显卡驱动 / Vulkan 运行时。";
#if defined(__APPLE__)
  body += "\n若刚从浏览器下载，可先执行：\n  xattr -cr \"/Applications/WDS Editor.app\"";
#endif
  return body;
}

void prepare_macos_vulkan_environment(const char* argv0) {
#if defined(__APPLE__)
  const fs::path exe_dir = executable_dir(argv0);
  if (!looks_like_macos_app(exe_dir)) {
    WDS_LOG("vulkan icd: not a .app bundle, leaving loader discovery\n");
    return;
  }
  const fs::path icd = bundled_moltenvk_icd(exe_dir);
  if (icd.empty()) {
    WDS_LOG("vulkan icd: bundled MoltenVK_icd.json missing\n");
    return;
  }
  // Always prefer the bundled ICD inside a shipped .app. Stale host
  // VK_ICD_FILENAMES values otherwise override Resources/vulkan/icd.d discovery
  // and vkCreateInstance fails with VK_ERROR_INCOMPATIBLE_DRIVER (-9).
  std::error_code ec;
  const fs::path abs_icd = fs::weakly_canonical(icd, ec);
  const std::string icd_utf8 =
      path_to_utf8(!ec && !abs_icd.empty() ? abs_icd : icd);
  ::setenv("VK_ICD_FILENAMES", icd_utf8.c_str(), 1);
  ::setenv("VK_DRIVER_FILES", icd_utf8.c_str(), 1);
  WDS_LOG("vulkan icd: pinned %s\n", icd_utf8.c_str());
#else
  (void)argv0;
  WDS_LOG("vulkan icd: non-macOS, using system loader\n");
#endif
}

StartupDependencyReport check_startup_dependencies(const char* argv0) {
  StartupDependencyReport report;
  const fs::path exe_dir = executable_dir(argv0);

  check_bass_runtime(report, exe_dir);
  check_vulkan_runtime_files(report, exe_dir);

  require_resource_dir(report, resolve_skins_dir(argv0), "skins 皮肤资源", looks_like_skins_dir);
  require_resource_dir(report, resolve_effects_dir(argv0), "effects 音效资源",
                       looks_like_effects_dir);

  const std::string icons = resolve_icons_dir(argv0);
  const fs::path icons_path = path_from_utf8(icons);
  if (icons.empty() || !is_dir(icons_path) ||
      !(is_regular(icons_path / "open.svg") || is_regular(icons_path / "open.png"))) {
    report.missing.push_back("icons 工具栏图标（open.svg / open.png）");
  }

  if (resolve_ui_font_path(argv0).empty()) {
    report.missing.push_back("UI 字体（fonts/NotoSansSC-Regular.ttf）");
  }

  return report;
}

void add_vulkan_unavailable(StartupDependencyReport& report) {
  report.missing.push_back(
      "Vulkan 不可用（QVulkanInstance 创建失败：请检查 GPU 驱动 / MoltenVK / ICD）");
}

int fail_startup_dependencies(const StartupDependencyReport& report) {
  const std::string title = "WDS Editor — 依赖缺失";
  const std::string message = report.format_message();
  WDS_LOG("startup deps failed count=%zu\n", report.missing.size());
  for (const auto& line : report.missing) {
    WDS_LOG("startup missing: %s\n", line.c_str());
  }
  native_file_dialog::alert_error(title, message);
  return 1;
}

}  // namespace wds::ui
