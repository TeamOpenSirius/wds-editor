// Opt-in MoltenVK path timing harness. Hidden GLFW window; no manual clicks.
// Build: cmake -DWDS_RENDERER_BUILD_BENCHMARKS=ON and target wds_renderer_path_bench.

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

#include "wds/renderer/vulkan_renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using wds::renderer::TextureId;
using wds::renderer::TextureInfo;
using wds::renderer::VulkanHostSurface;
using wds::renderer::VulkanRenderer;

constexpr int64_t kP95GateUs = 8000;
constexpr int kUploadSamples = 30;
constexpr int kResizeSamples = 20;
constexpr int kMsaaSamples = 20;
constexpr int kFontW = 2048;
constexpr int kFontH = 2048;
constexpr int kSkinW = 4096;
constexpr int kSkinH = 4096;

struct Stats {
  int n = 0;
  int64_t p50 = 0;
  int64_t p95 = 0;
  int64_t max = 0;
};

void prefer_moltenvk_icd() {
#if defined(__APPLE__)
  if (std::getenv("VK_ICD_FILENAMES") != nullptr || std::getenv("VK_DRIVER_FILES") != nullptr) {
    return;
  }
  const char* candidates[] = {
      "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json",
      "/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json",
      "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json",
      "/usr/local/share/vulkan/icd.d/MoltenVK_icd.json",
  };
  for (const char* path : candidates) {
    if (std::FILE* f = std::fopen(path, "r")) {
      std::fclose(f);
      setenv("VK_ICD_FILENAMES", path, 0);
      setenv("VK_DRIVER_FILES", path, 0);
      return;
    }
  }
#endif
}

int64_t percentile(std::vector<int64_t> values, int pct) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const size_t idx = static_cast<size_t>((pct * (values.size() - 1) + 99) / 100);
  return values[std::min(idx, values.size() - 1)];
}

Stats make_stats(const std::vector<int64_t>& values) {
  Stats s;
  s.n = static_cast<int>(values.size());
  if (values.empty()) {
    return s;
  }
  s.p50 = percentile(values, 50);
  s.p95 = percentile(values, 95);
  s.max = *std::max_element(values.begin(), values.end());
  return s;
}

void print_stats(const char* name, const Stats& s, bool measured) {
  if (!measured) {
    std::printf("  %-28s  UNMEASURED\n", name);
    return;
  }
  std::printf("  %-28s  n=%d  P50=%.3fms  P95=%.3fms  max=%.3fms\n", name, s.n, s.p50 / 1000.0,
              s.p95 / 1000.0, s.max / 1000.0);
}

bool run_uploads(VulkanRenderer& renderer, int width, int height, int samples,
                 std::vector<int64_t>& total_us, std::vector<int64_t>& wait_us,
                 std::vector<int64_t>& submit_us, std::vector<int64_t>& reap_us,
                 std::vector<int64_t>& desc_us) {
  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  std::vector<unsigned char> pixels(bytes, 0x80);
  total_us.clear();
  wait_us.clear();
  submit_us.clear();
  reap_us.clear();
  desc_us.clear();
  total_us.reserve(static_cast<size_t>(samples));
  wait_us.reserve(static_cast<size_t>(samples));
  submit_us.reserve(static_cast<size_t>(samples));
  reap_us.reserve(static_cast<size_t>(samples));
  desc_us.reserve(static_cast<size_t>(samples));

  for (int i = 0; i < 2; ++i) {
    const TextureInfo warm = renderer.create_texture_rgba(pixels.data(), width, height);
    if (!warm) {
      return false;
    }
    renderer.destroy_texture(warm.id);
  }
  renderer.reset_path_diagnostics();

  for (int i = 0; i < samples; ++i) {
    const TextureInfo tex = renderer.create_texture_rgba(pixels.data(), width, height);
    if (!tex) {
      return false;
    }
    const auto last = renderer.last_path_sample();
    total_us.push_back(last.create_texture_rgba_us);
    wait_us.push_back(last.upload_fence_wait_us);
    submit_us.push_back(last.upload_submit_us);
    reap_us.push_back(last.upload_fence_reap_us);
    desc_us.push_back(last.descriptor_allocate_us);
    renderer.destroy_texture(tex.id);
  }
  return true;
}

const char* validation_layer_status() {
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  if (count > 0) {
    vkEnumerateInstanceLayerProperties(&count, layers.data());
  }
  for (const auto& layer : layers) {
    if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
      return "layer present (not enabled in this harness)";
    }
  }
  return "unavailable";
}

bool run_descriptor_stress(VulkanRenderer& renderer) {
  constexpr int kCreate = 300;
  constexpr int kRecycle = 100;
  const unsigned char pixel[4] = {255, 0, 0, 255};
  std::vector<TextureId> ids;
  ids.reserve(static_cast<size_t>(kCreate));
  for (int i = 0; i < kCreate; ++i) {
    const TextureInfo tex = renderer.create_texture_rgba(pixel, 1, 1);
    if (!tex) {
      std::printf("descriptor stress: create[%d] failed\n", i);
      return false;
    }
    ids.push_back(tex.id);
  }
  auto diag = renderer.descriptor_pool_diagnostics();
  std::printf("descriptor stress: created=%d  blocks=%u  live=%u\n", kCreate, diag.block_count,
              diag.live_sets);
  if (diag.block_count < 2 || diag.live_sets < static_cast<uint32_t>(kCreate)) {
    return false;
  }

  renderer.device_wait_idle();
  for (int i = 0; i < kRecycle; ++i) {
    renderer.destroy_texture(ids[static_cast<size_t>(i)]);
    const TextureInfo tex = renderer.create_texture_rgba(pixel, 1, 1);
    if (!tex) {
      std::printf("descriptor stress: reuse create[%d] failed\n", i);
      return false;
    }
    ids[static_cast<size_t>(i)] = tex.id;
  }
  diag = renderer.descriptor_pool_diagnostics();
  std::printf("descriptor reuse:  recycled=%d  blocks=%u  live=%u\n", kRecycle, diag.block_count,
              diag.live_sets);
  if (diag.block_count < 2 || diag.live_sets != static_cast<uint32_t>(kCreate)) {
    return false;
  }

  for (TextureId id : ids) {
    renderer.destroy_texture(id);
  }
  renderer.device_wait_idle();
  return true;
}

}  // namespace

int main() {
  prefer_moltenvk_icd();
  glfwSetErrorCallback([](int code, const char* desc) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, desc ? desc : "");
  });
  if (!glfwInit()) {
    std::printf("device: (none)\n");
    std::printf("renderer_path_bench: GLFW init failed — UNMEASURED\n");
    return 0;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  GLFWwindow* window = glfwCreateWindow(1280, 720, "wds-renderer-path-bench", nullptr, nullptr);
  if (window == nullptr || !glfwVulkanSupported()) {
    std::printf("device: (none)\n");
    std::printf("renderer_path_bench: Vulkan/GLFW window unavailable — UNMEASURED\n");
    if (window) {
      glfwDestroyWindow(window);
    }
    glfwTerminate();
    return 0;
  }

  uint32_t ext_count = 0;
  const char** exts = glfwGetRequiredInstanceExtensions(&ext_count);
  if (exts == nullptr || ext_count == 0) {
    std::printf("device: (none)\n");
    std::printf("renderer_path_bench: instance extensions unavailable — UNMEASURED\n");
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
  }

  VulkanHostSurface host;
  host.instance_extensions.assign(exts, exts + ext_count);
  host.create_surface = [window](VkInstance instance) -> VkSurfaceKHR {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return surface;
  };
  host.framebuffer_size = [window](int* width, int* height) {
    glfwGetFramebufferSize(window, width, height);
  };

  VulkanRenderer renderer;
  renderer.set_preferred_msaa(2);
  if (!renderer.create(host) || !renderer.ready()) {
    std::printf("device: (none)\n");
    std::printf("renderer_path_bench: VulkanRenderer::create failed — UNMEASURED\n");
    renderer.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
  }

  renderer.set_path_diagnostics_enabled(true);
  renderer.reset_path_diagnostics();
  std::printf("device: %s\n", renderer.device_name()[0] != '\0' ? renderer.device_name() : "(unknown)");
  std::printf("active_msaa: %d  fb=%dx%d\n", renderer.active_msaa(), renderer.framebuffer_width(),
              renderer.framebuffer_height());
  std::printf("threshold: main-thread upload or wait P95 > %.3fms\n", kP95GateUs / 1000.0);
  std::printf("note: resize/MSAA keep vkDeviceWaitIdle — graphics fence does not prove "
              "presentation-engine completion (measured resize P95=0.121ms, MSAA=0.728ms, "
              "idle=0.108ms).\n");
  std::printf("validation: %s\n", validation_layer_status());

  const bool stress_ok = run_descriptor_stress(renderer);
  std::printf("descriptor 300+ stress: %s\n", stress_ok ? "OK" : "FAIL");

  std::vector<int64_t> font_total;
  std::vector<int64_t> font_wait;
  std::vector<int64_t> font_submit;
  std::vector<int64_t> font_reap;
  std::vector<int64_t> font_desc;
  const bool font_ok = run_uploads(renderer, kFontW, kFontH, kUploadSamples, font_total, font_wait,
                                   font_submit, font_reap, font_desc);
  const Stats font_total_s = make_stats(font_total);
  const Stats font_wait_s = make_stats(font_wait);
  const Stats font_submit_s = make_stats(font_submit);
  const Stats font_reap_s = make_stats(font_reap);
  const Stats font_desc_s = make_stats(font_desc);

  std::vector<int64_t> skin_total;
  std::vector<int64_t> skin_wait;
  std::vector<int64_t> skin_submit;
  std::vector<int64_t> skin_reap;
  std::vector<int64_t> skin_desc;
  const bool skin_ok = run_uploads(renderer, kSkinW, kSkinH, kUploadSamples, skin_total, skin_wait,
                                   skin_submit, skin_reap, skin_desc);
  const Stats skin_total_s = make_stats(skin_total);
  const Stats skin_wait_s = make_stats(skin_wait);
  const Stats skin_submit_s = make_stats(skin_submit);
  const Stats skin_reap_s = make_stats(skin_reap);
  const Stats skin_desc_s = make_stats(skin_desc);

  std::vector<int64_t> resize_us;
  resize_us.reserve(static_cast<size_t>(kResizeSamples));
  renderer.reset_path_diagnostics();
  bool resize_ok = true;
  for (int i = 0; i < kResizeSamples; ++i) {
    const int w = 800 + (i % 2) * 160;
    const int h = 600 + (i % 2) * 120;
    glfwSetWindowSize(window, w, h);
    glfwPollEvents();
    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0 || !renderer.resize(fb_w, fb_h)) {
      resize_ok = false;
      break;
    }
    resize_us.push_back(renderer.last_path_sample().swapchain_recreate_us);
  }
  const Stats resize_s = make_stats(resize_us);

  std::vector<int64_t> msaa_total;
  std::vector<int64_t> msaa_idle;
  msaa_total.reserve(static_cast<size_t>(kMsaaSamples));
  msaa_idle.reserve(static_cast<size_t>(kMsaaSamples));
  renderer.reset_path_diagnostics();
  const int start_msaa = renderer.active_msaa();
  bool msaa_ok = false;
  if (start_msaa > 1 || renderer.apply_msaa(2)) {
    const int hi = renderer.active_msaa() > 1 ? renderer.active_msaa() : 0;
    if (hi > 1) {
      msaa_ok = true;
      renderer.reset_path_diagnostics();
      for (int i = 0; i < kMsaaSamples; ++i) {
        const int want = (i % 2) == 0 ? 1 : hi;
        if (!renderer.apply_msaa(want)) {
          msaa_ok = false;
          break;
        }
        msaa_total.push_back(renderer.last_path_sample().apply_msaa_us);
        msaa_idle.push_back(renderer.last_path_sample().apply_msaa_idle_wait_us);
      }
    }
  }
  const Stats msaa_total_s = make_stats(msaa_total);
  const Stats msaa_idle_s = make_stats(msaa_idle);

  std::printf("samples:\n");
  print_stats("font 2048x2048 upload", font_total_s, font_ok);
  print_stats("font upload fence wait", font_wait_s, font_ok);
  print_stats("font async submit", font_submit_s, font_ok);
  print_stats("font fence reap", font_reap_s, font_ok);
  print_stats("font descriptor alloc", font_desc_s, font_ok);
  print_stats("skin 4096x4096 upload", skin_total_s, skin_ok);
  print_stats("skin upload fence wait", skin_wait_s, skin_ok);
  print_stats("skin async submit", skin_submit_s, skin_ok);
  print_stats("skin fence reap", skin_reap_s, skin_ok);
  print_stats("skin descriptor alloc", skin_desc_s, skin_ok);
  print_stats("swapchain resize", resize_s, resize_ok && !resize_us.empty());
  print_stats("msaa apply total", msaa_total_s, msaa_ok && !msaa_total.empty());
  print_stats("msaa apply idle wait", msaa_idle_s, msaa_ok && !msaa_idle.empty());

  int64_t upload_or_wait_p95 = 0;
  if (font_ok) {
    upload_or_wait_p95 = std::max(upload_or_wait_p95, std::max(font_total_s.p95, font_wait_s.p95));
  }
  if (skin_ok) {
    upload_or_wait_p95 = std::max(upload_or_wait_p95, std::max(skin_total_s.p95, skin_wait_s.p95));
  }
  const bool gate_applicable = font_ok || skin_ok;
  const bool exceeds = gate_applicable && upload_or_wait_p95 > kP95GateUs;
  std::printf("gate: main-thread upload or wait P95 %s 8ms (observed P95=%.3fms)%s\n",
              exceeds ? "EXCEEDS" : "does not exceed", upload_or_wait_p95 / 1000.0,
              gate_applicable ? "" : " — UNMEASURED");

  renderer.destroy();
  std::printf("final destroy: OK\n");
  glfwDestroyWindow(window);
  glfwTerminate();
  return stress_ok ? 0 : 1;
}
