#pragma once

#include <string>
#include <vector>

namespace wds::ui {

// Lightweight startup checks for bundled runtimes + required assets.
// Safe to call before glfwInit (Vulkan support is checked separately after GLFW).
struct StartupDependencyReport {
  std::vector<std::string> missing;

  bool ok() const noexcept { return missing.empty(); }
  std::string format_message() const;
};

// Probe companion libs (BASS / MoltenVK / vulkan-1) and skins/fonts/icons/effects.
StartupDependencyReport check_startup_dependencies(const char* argv0);

// Append a Vulkan availability failure (call after glfwInit + glfwVulkanSupported).
void add_vulkan_unavailable(StartupDependencyReport& report);

// Show a blocking native error dialog and return a process exit code.
int fail_startup_dependencies(const StartupDependencyReport& report);

}  // namespace wds::ui
