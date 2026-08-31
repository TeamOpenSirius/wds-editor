#include "wds/ui/curve_template.hpp"
#include "wds/ui/editor_ui_config.hpp"

#include <wds/core/file_io.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* expr, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __FILE__, __LINE__)

using wds::chart_editor::EasingAlgorithm;
using wds::chart_editor::EasingDirection;
using wds::chart_editor::SerializeError;
using wds::chart_editor::write_text_atomic;
using wds::ui::CurveTemplate;
using wds::ui::CurveTemplateUiState;
using wds::ui::EditorUiConfig;
using wds::ui::allocate_curve_template_id;
using wds::ui::apply_curve_template_state;
using wds::ui::capture_curve_template_state;
using wds::ui::find_curve_template_by_id;
using wds::ui::kMaxCurveTemplates;
using wds::ui::clamp_msaa_samples;
using wds::ui::load_editor_ui_config;
using wds::ui::normalize_curve_config;
using wds::ui::normalize_curve_template;
using wds::ui::resolve_selected_curve_easing;
using wds::ui::save_editor_ui_config;

namespace fs = std::filesystem;

fs::path temp_config_path(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "wds_ui_config_tests";
  fs::create_directories(dir);
  return dir / name;
}

std::string utf8_to_hex(const std::string& name) {
  std::string hex;
  hex.resize(name.size() * 2);
  static const char kDigits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < name.size(); ++i) {
    const unsigned char b = static_cast<unsigned char>(name[i]);
    hex[i * 2] = kDigits[b >> 4];
    hex[i * 2 + 1] = kDigits[b & 0xF];
  }
  return hex;
}

CurveTemplate make_template(std::uint64_t id, std::string name, EasingAlgorithm algorithm,
                            double parameter) {
  CurveTemplate tmpl;
  tmpl.id = id;
  tmpl.name = std::move(name);
  tmpl.algorithm = algorithm;
  tmpl.parameter = parameter;
  return tmpl;
}

bool nearly_equal(double a, double b) {
  return std::fabs(a - b) <= 1e-12;
}

bool has_count(const std::vector<CurveTemplate>& templates, std::size_t n) {
  CHECK_EQ(templates.size(), n);
  return templates.size() == n;
}

void test_old_config_without_curve_keys() {
  const auto path = temp_config_path("old_no_curve.yml");
  const std::string yaml =
      "# WDS editor UI preferences\n"
      "note_speed: 7.5\n"
      "music_muted: true\n"
      "visible_hectoms: 40\n";
  CHECK(write_text_atomic(path.string(), yaml).error == SerializeError::Ok);

  EditorUiConfig cfg;
  CHECK(load_editor_ui_config(path.string(), cfg));
  CHECK(nearly_equal(cfg.note_speed, 7.5));
  CHECK(cfg.music_muted);
  CHECK_EQ(cfg.visible_hectoms, 40);
  CHECK_EQ(cfg.subdivisions_per_beat, 4);
  CHECK(std::fabs(cfg.playback_rate - 1.0f) < 1e-5f);
  CHECK(cfg.curve_templates.empty());
  CHECK_EQ(cfg.curve_selected_template_id, static_cast<std::uint64_t>(0));
  CHECK(cfg.curve_selected_direction == EasingDirection::In);
  CHECK_EQ(cfg.msaa_samples, 2);
}

void test_msaa_samples_default_clamp_and_round_trip() {
  CHECK_EQ(clamp_msaa_samples(0), 1);
  CHECK_EQ(clamp_msaa_samples(1), 1);
  CHECK_EQ(clamp_msaa_samples(2), 2);
  CHECK_EQ(clamp_msaa_samples(3), 4);
  CHECK_EQ(clamp_msaa_samples(4), 4);
  CHECK_EQ(clamp_msaa_samples(8), 4);

  EditorUiConfig defaults;
  CHECK_EQ(defaults.msaa_samples, 2);

  const auto missing = temp_config_path("msaa_missing.yml");
  CHECK(write_text_atomic(missing.string(), "note_speed: 5.0\n").error == SerializeError::Ok);
  EditorUiConfig loaded_missing;
  CHECK(load_editor_ui_config(missing.string(), loaded_missing));
  CHECK_EQ(loaded_missing.msaa_samples, 2);

  for (int samples : {1, 2, 4}) {
    EditorUiConfig cfg;
    cfg.msaa_samples = samples;
    const auto path = temp_config_path(("msaa_" + std::to_string(samples) + ".yml").c_str());
    CHECK(save_editor_ui_config(path.string(), cfg));
    EditorUiConfig loaded;
    CHECK(load_editor_ui_config(path.string(), loaded));
    CHECK_EQ(loaded.msaa_samples, samples);
  }

  const auto invalid = temp_config_path("msaa_invalid.yml");
  CHECK(write_text_atomic(invalid.string(), "msaa_samples: 3\n").error == SerializeError::Ok);
  EditorUiConfig loaded_invalid;
  CHECK(load_editor_ui_config(invalid.string(), loaded_invalid));
  CHECK_EQ(loaded_invalid.msaa_samples, 4);

  const auto zero = temp_config_path("msaa_zero.yml");
  CHECK(write_text_atomic(zero.string(), "msaa_samples: 0\n").error == SerializeError::Ok);
  EditorUiConfig loaded_zero;
  CHECK(load_editor_ui_config(zero.string(), loaded_zero));
  CHECK_EQ(loaded_zero.msaa_samples, 1);
}

void test_name_round_trip_special_and_chinese() {
  EditorUiConfig cfg;
  cfg.curve_templates.push_back(make_template(1, "缓动模板", EasingAlgorithm::Poly, 2.5));
  cfg.curve_templates.push_back(make_template(2, "a#b:c", EasingAlgorithm::Exp, 1.0));
  cfg.curve_templates.push_back(make_template(3, "  lead trail  ", EasingAlgorithm::Sine, 0.0));
  cfg.curve_templates.push_back(make_template(4, "in ter ior", EasingAlgorithm::Linear, 4.0));
  cfg.curve_selected_template_id = 2;
  cfg.curve_selected_direction = EasingDirection::OutIn;

  const auto path = temp_config_path("names_round_trip.yml");
  CHECK(save_editor_ui_config(path.string(), cfg));

  wds::chart_editor::SerializeResult status;
  const std::string saved = wds::chart_editor::read_text_file(path.string(), status);
  CHECK(status.error == SerializeError::Ok);
  CHECK(saved.find("curve_template_0_name_hex: " + utf8_to_hex("缓动模板")) != std::string::npos);
  CHECK(saved.find("curve_template_1_name_hex: " + utf8_to_hex("a#b:c")) != std::string::npos);
  CHECK(saved.find("curve_template_2_name_hex: " + utf8_to_hex("  lead trail  ")) !=
        std::string::npos);
  CHECK(saved.find("a#b:c") == std::string::npos);

  EditorUiConfig loaded;
  CHECK(load_editor_ui_config(path.string(), loaded));
  if (has_count(loaded.curve_templates, 4)) {
    CHECK_EQ(loaded.curve_templates[0].name, std::string("缓动模板"));
    CHECK_EQ(loaded.curve_templates[1].name, std::string("a#b:c"));
    CHECK_EQ(loaded.curve_templates[2].name, std::string("  lead trail  "));
    CHECK_EQ(loaded.curve_templates[3].name, std::string("in ter ior"));
  }
  CHECK_EQ(loaded.curve_selected_template_id, static_cast<std::uint64_t>(2));
  CHECK(loaded.curve_selected_direction == EasingDirection::OutIn);
}

void test_duplicate_names_distinct_ids_and_order() {
  EditorUiConfig cfg;
  cfg.curve_templates.push_back(make_template(3, "same", EasingAlgorithm::Poly, 1.0));
  cfg.curve_templates.push_back(make_template(1, "same", EasingAlgorithm::Exp, 2.0));
  cfg.curve_templates.push_back(make_template(8, "other", EasingAlgorithm::Sine, 3.0));
  cfg.curve_selected_template_id = 1;
  cfg.curve_selected_direction = EasingDirection::Out;

  const auto path = temp_config_path("dup_names.yml");
  CHECK(save_editor_ui_config(path.string(), cfg));

  EditorUiConfig loaded;
  CHECK(load_editor_ui_config(path.string(), loaded));
  if (has_count(loaded.curve_templates, 3)) {
    CHECK_EQ(loaded.curve_templates[0].id, static_cast<std::uint64_t>(3));
    CHECK_EQ(loaded.curve_templates[1].id, static_cast<std::uint64_t>(1));
    CHECK_EQ(loaded.curve_templates[2].id, static_cast<std::uint64_t>(8));
    CHECK_EQ(loaded.curve_templates[0].name, std::string("same"));
    CHECK_EQ(loaded.curve_templates[1].name, std::string("same"));
    CHECK_EQ(loaded.curve_templates[2].name, std::string("other"));
    CHECK(loaded.curve_templates[0].algorithm == EasingAlgorithm::Poly);
    CHECK(loaded.curve_templates[1].algorithm == EasingAlgorithm::Exp);
  }
  CHECK_EQ(loaded.curve_selected_template_id, static_cast<std::uint64_t>(1));
  CHECK(loaded.curve_selected_direction == EasingDirection::Out);
}

void test_invalid_algorithm_direction_and_parameter() {
  const auto path = temp_config_path("invalid_values.yml");
  const std::string yaml =
      "curve_template_count: 4\n"
      "curve_template_0_id: 1\n"
      "curve_template_0_name_hex: 61\n"
      "curve_template_0_algorithm: cubic\n"
      "curve_template_0_parameter: 3\n"
      "curve_template_1_id: 2\n"
      "curve_template_1_name_hex: 62\n"
      "curve_template_1_algorithm: POLY\n"
      "curve_template_1_parameter: nan\n"
      "curve_template_2_id: 3\n"
      "curve_template_2_name_hex: 63\n"
      "curve_template_2_algorithm: exp\n"
      "curve_template_2_parameter: inf\n"
      "curve_template_3_id: 4\n"
      "curve_template_3_name_hex: 64\n"
      "curve_template_3_algorithm: sine\n"
      "curve_template_3_parameter: 25.5\n"
      "curve_selected_template_id: 1\n"
      "curve_selected_direction: sideways\n";
  CHECK(write_text_atomic(path.string(), yaml).error == SerializeError::Ok);

  EditorUiConfig cfg;
  CHECK(load_editor_ui_config(path.string(), cfg));
  if (has_count(cfg.curve_templates, 4)) {
    CHECK(cfg.curve_templates[0].algorithm == EasingAlgorithm::Linear);
    CHECK(nearly_equal(cfg.curve_templates[0].parameter, 3.0));
    CHECK(cfg.curve_templates[1].algorithm == EasingAlgorithm::Linear);
    CHECK(nearly_equal(cfg.curve_templates[1].parameter, 0.0));
    CHECK(cfg.curve_templates[2].algorithm == EasingAlgorithm::Exp);
    CHECK(nearly_equal(cfg.curve_templates[2].parameter, 0.0));
    CHECK(cfg.curve_templates[3].algorithm == EasingAlgorithm::Sine);
    CHECK(nearly_equal(cfg.curve_templates[3].parameter, 20.0));
  }
  CHECK(cfg.curve_selected_direction == EasingDirection::In);

  CurveTemplate tmpl = make_template(9, "x", EasingAlgorithm::Poly, -4.0);
  normalize_curve_template(tmpl);
  CHECK(nearly_equal(tmpl.parameter, 0.0));
  tmpl.parameter = std::numeric_limits<double>::quiet_NaN();
  normalize_curve_template(tmpl);
  CHECK(nearly_equal(tmpl.parameter, 0.0));
}

void test_malformed_hex_name() {
  const auto path = temp_config_path("malformed_hex.yml");
  const std::string yaml =
      "curve_template_count: 2\n"
      "curve_template_0_id: 7\n"
      "curve_template_0_name_hex: zzzz\n"
      "curve_template_0_algorithm: poly\n"
      "curve_template_0_parameter: 1\n"
      "curve_template_1_id: 8\n"
      "curve_template_1_name_hex: abc\n"
      "curve_template_1_algorithm: linear\n"
      "curve_template_1_parameter: 0\n"
      "curve_selected_template_id: 7\n"
      "curve_selected_direction: in\n";
  CHECK(write_text_atomic(path.string(), yaml).error == SerializeError::Ok);

  EditorUiConfig cfg;
  CHECK(load_editor_ui_config(path.string(), cfg));
  if (has_count(cfg.curve_templates, 2)) {
    CHECK_EQ(cfg.curve_templates[0].id, static_cast<std::uint64_t>(7));
    CHECK(cfg.curve_templates[0].name.empty());
    CHECK(cfg.curve_templates[0].algorithm == EasingAlgorithm::Poly);
    CHECK_EQ(cfg.curve_templates[1].id, static_cast<std::uint64_t>(8));
    CHECK(cfg.curve_templates[1].name.empty());
  }
}

void test_zero_and_duplicate_ids_repaired() {
  const auto path = temp_config_path("bad_ids.yml");
  const std::string yaml =
      "curve_template_count: 3\n"
      "curve_template_0_id: 0\n"
      "curve_template_0_name_hex: 61\n"
      "curve_template_0_algorithm: linear\n"
      "curve_template_0_parameter: 0\n"
      "curve_template_1_id: 5\n"
      "curve_template_1_name_hex: 62\n"
      "curve_template_1_algorithm: poly\n"
      "curve_template_1_parameter: 1\n"
      "curve_template_2_id: 5\n"
      "curve_template_2_name_hex: 63\n"
      "curve_template_2_algorithm: exp\n"
      "curve_template_2_parameter: 2\n"
      "curve_selected_template_id: 5\n"
      "curve_selected_direction: inout\n";
  CHECK(write_text_atomic(path.string(), yaml).error == SerializeError::Ok);

  EditorUiConfig cfg;
  CHECK(load_editor_ui_config(path.string(), cfg));
  if (has_count(cfg.curve_templates, 3)) {
    CHECK_EQ(cfg.curve_templates[0].id, static_cast<std::uint64_t>(6));
    CHECK_EQ(cfg.curve_templates[1].id, static_cast<std::uint64_t>(5));
    CHECK_EQ(cfg.curve_templates[2].id, static_cast<std::uint64_t>(7));
    CHECK_EQ(cfg.curve_templates[0].name, std::string("a"));
    CHECK_EQ(cfg.curve_templates[1].name, std::string("b"));
    CHECK_EQ(cfg.curve_templates[2].name, std::string("c"));
    CHECK(find_curve_template_by_id(cfg.curve_templates, 5) == &cfg.curve_templates[1]);
    CHECK(find_curve_template_by_id(cfg.curve_templates, 6) == &cfg.curve_templates[0]);
    CHECK(find_curve_template_by_id(cfg.curve_templates, 7) == &cfg.curve_templates[2]);
  }
  CHECK_EQ(cfg.curve_selected_template_id, static_cast<std::uint64_t>(5));
  CHECK(cfg.curve_selected_direction == EasingDirection::InOut);
}

void test_missing_and_deleted_selection_falls_back_to_zero() {
  std::vector<CurveTemplate> templates;
  templates.push_back(make_template(1, "keep", EasingAlgorithm::Poly, 2.0));
  templates.push_back(make_template(2, "drop", EasingAlgorithm::Exp, 3.0));
  std::uint64_t selected = 2;
  templates.erase(templates.begin() + 1);
  normalize_curve_config(templates, selected);
  CHECK_EQ(selected, static_cast<std::uint64_t>(0));
  const auto resolved_deleted =
      resolve_selected_curve_easing(templates, selected, EasingDirection::Out);
  CHECK(resolved_deleted.algorithm == EasingAlgorithm::Linear);
  CHECK(nearly_equal(resolved_deleted.parameter, 0.0));

  const auto path = temp_config_path("missing_selection.yml");
  const std::string yaml =
      "curve_template_count: 1\n"
      "curve_template_0_id: 11\n"
      "curve_template_0_name_hex: 61\n"
      "curve_template_0_algorithm: poly\n"
      "curve_template_0_parameter: 2\n"
      "curve_selected_template_id: 99\n"
      "curve_selected_direction: out\n";
  CHECK(write_text_atomic(path.string(), yaml).error == SerializeError::Ok);
  EditorUiConfig cfg;
  CHECK(load_editor_ui_config(path.string(), cfg));
  CHECK_EQ(cfg.curve_selected_template_id, static_cast<std::uint64_t>(0));
  CHECK(cfg.curve_selected_direction == EasingDirection::Out);
}

void test_allocate_id_after_sparse_high_ids() {
  std::vector<CurveTemplate> templates;
  CHECK_EQ(allocate_curve_template_id(templates), static_cast<std::uint64_t>(1));
  templates.push_back(make_template(1, "a", EasingAlgorithm::Linear, 0.0));
  templates.push_back(make_template(100, "b", EasingAlgorithm::Linear, 0.0));
  templates.push_back(make_template(5, "c", EasingAlgorithm::Linear, 0.0));
  CHECK_EQ(allocate_curve_template_id(templates), static_cast<std::uint64_t>(101));
  CHECK(find_curve_template_by_id(templates, 100) == &templates[1]);
  CHECK(find_curve_template_by_id(templates, 9) == nullptr);
}

void test_empty_selection_resolves_linear() {
  std::vector<CurveTemplate> templates;
  templates.push_back(make_template(4, "poly", EasingAlgorithm::Poly, 5.0));
  const auto empty =
      resolve_selected_curve_easing(templates, 0, EasingDirection::OutIn);
  CHECK(empty.algorithm == EasingAlgorithm::Linear);
  CHECK(empty.direction == EasingDirection::OutIn);
  CHECK(nearly_equal(empty.parameter, 0.0));

  const auto missing =
      resolve_selected_curve_easing(templates, 9, EasingDirection::In);
  CHECK(missing.algorithm == EasingAlgorithm::Linear);
  CHECK(nearly_equal(missing.parameter, 0.0));

  const auto selected =
      resolve_selected_curve_easing(templates, 4, EasingDirection::Out);
  CHECK(selected.algorithm == EasingAlgorithm::Poly);
  CHECK(selected.direction == EasingDirection::Out);
  CHECK(nearly_equal(selected.parameter, 5.0));

  EditorUiConfig cfg;
  CHECK(cfg.curve_templates.empty());
  CHECK_EQ(cfg.curve_selected_template_id, static_cast<std::uint64_t>(0));
  CHECK(cfg.curve_selected_direction == EasingDirection::In);
}

void append_template_keys(std::string& yaml, int index, std::uint64_t id, const std::string& name,
                          const char* algorithm, const char* parameter) {
  yaml += "curve_template_";
  yaml += std::to_string(index);
  yaml += "_id: ";
  yaml += std::to_string(id);
  yaml += "\ncurve_template_";
  yaml += std::to_string(index);
  yaml += "_name_hex: ";
  yaml += utf8_to_hex(name);
  yaml += "\ncurve_template_";
  yaml += std::to_string(index);
  yaml += "_algorithm: ";
  yaml += algorithm;
  yaml += "\ncurve_template_";
  yaml += std::to_string(index);
  yaml += "_parameter: ";
  yaml += parameter;
  yaml += '\n';
}

void test_curve_template_count_truncated_to_max() {
  CHECK(kMaxCurveTemplates >= 8);
  CHECK(kMaxCurveTemplates <= 256);
  const int over = static_cast<int>(kMaxCurveTemplates) + 8;
  std::string yaml = "curve_template_count: " + std::to_string(over) + "\n";
  for (int i = 0; i < over; ++i) {
    append_template_keys(yaml, i, static_cast<std::uint64_t>(i + 1), std::to_string(i), "linear",
                         "0");
  }
  yaml += "curve_selected_template_id: 1\n";
  yaml += "curve_selected_direction: out\n";

  const auto path = temp_config_path("count_over_max.yml");
  CHECK(write_text_atomic(path.string(), yaml).error == SerializeError::Ok);
  EditorUiConfig cfg;
  CHECK(load_editor_ui_config(path.string(), cfg));
  if (has_count(cfg.curve_templates, kMaxCurveTemplates)) {
    CHECK_EQ(cfg.curve_templates.front().id, static_cast<std::uint64_t>(1));
    CHECK_EQ(cfg.curve_templates.back().id, static_cast<std::uint64_t>(kMaxCurveTemplates));
    CHECK(find_curve_template_by_id(cfg.curve_templates, kMaxCurveTemplates + 1) == nullptr);
  }
  CHECK_EQ(cfg.curve_selected_template_id, static_cast<std::uint64_t>(1));
  CHECK(cfg.curve_selected_direction == EasingDirection::Out);
}

void test_over_cap_indexed_slots_ignored() {
  const int over = static_cast<int>(kMaxCurveTemplates) + 5;
  std::string yaml = "curve_template_count: " + std::to_string(over) + "\n";
  append_template_keys(yaml, 0, 11, "keep-first", "poly", "2");
  append_template_keys(yaml, static_cast<int>(kMaxCurveTemplates) - 1, 12, "keep-last", "exp",
                       "3");
  append_template_keys(yaml, static_cast<int>(kMaxCurveTemplates), 99, "over-cap", "sine", "4");
  append_template_keys(yaml, static_cast<int>(kMaxCurveTemplates) + 3, 100, "also-over", "linear",
                       "0");
  yaml += "curve_selected_template_id: 99\n";
  yaml += "curve_selected_direction: inout\n";

  const auto path = temp_config_path("slot_over_max.yml");
  CHECK(write_text_atomic(path.string(), yaml).error == SerializeError::Ok);
  EditorUiConfig cfg;
  CHECK(load_editor_ui_config(path.string(), cfg));
  if (has_count(cfg.curve_templates, kMaxCurveTemplates)) {
    CHECK_EQ(cfg.curve_templates.front().id, static_cast<std::uint64_t>(11));
    CHECK_EQ(cfg.curve_templates.front().name, std::string("keep-first"));
    CHECK_EQ(cfg.curve_templates.back().id, static_cast<std::uint64_t>(12));
    CHECK_EQ(cfg.curve_templates.back().name, std::string("keep-last"));
    CHECK(find_curve_template_by_id(cfg.curve_templates, 99) == nullptr);
    CHECK(find_curve_template_by_id(cfg.curve_templates, 100) == nullptr);
  }
  CHECK_EQ(cfg.curve_selected_template_id, static_cast<std::uint64_t>(0));
  CHECK(cfg.curve_selected_direction == EasingDirection::InOut);
}

void test_settings_save_preserves_curve_state() {
  CurveTemplateUiState live;
  live.templates.push_back(make_template(7, "保留", EasingAlgorithm::Poly, 2.5));
  live.templates.push_back(make_template(9, "a#b", EasingAlgorithm::Exp, 1.0));
  live.selected_id = 9;
  live.direction = EasingDirection::OutIn;

  EditorUiConfig settings_save;
  settings_save.note_speed = 8.0;
  settings_save.music_muted = true;
  apply_curve_template_state(settings_save, live);

  const auto path = temp_config_path("settings_save_keeps_curves.yml");
  CHECK(save_editor_ui_config(path.string(), settings_save));

  EditorUiConfig loaded;
  CHECK(load_editor_ui_config(path.string(), loaded));
  CHECK(nearly_equal(loaded.note_speed, 8.0));
  CHECK(loaded.music_muted);
  if (has_count(loaded.curve_templates, 2)) {
    CHECK_EQ(loaded.curve_templates[0].id, static_cast<std::uint64_t>(7));
    CHECK_EQ(loaded.curve_templates[0].name, std::string("保留"));
    CHECK_EQ(loaded.curve_templates[1].id, static_cast<std::uint64_t>(9));
    CHECK_EQ(loaded.curve_templates[1].name, std::string("a#b"));
  }
  CHECK_EQ(loaded.curve_selected_template_id, static_cast<std::uint64_t>(9));
  CHECK(loaded.curve_selected_direction == EasingDirection::OutIn);

  CurveTemplateUiState captured;
  capture_curve_template_state(loaded, captured);
  CHECK_EQ(captured.selected_id, static_cast<std::uint64_t>(9));
  CHECK(captured.direction == EasingDirection::OutIn);
  if (has_count(captured.templates, 2)) {
    CHECK_EQ(captured.templates[1].id, static_cast<std::uint64_t>(9));
  }
}

void test_save_truncates_in_memory_over_max() {
  EditorUiConfig cfg;
  for (std::size_t i = 0; i < kMaxCurveTemplates + 4; ++i) {
    cfg.curve_templates.push_back(
        make_template(i + 1, "t", EasingAlgorithm::Linear, 0.0));
  }
  cfg.curve_selected_template_id = kMaxCurveTemplates + 2;
  cfg.curve_selected_direction = EasingDirection::Out;

  const auto path = temp_config_path("save_truncates.yml");
  CHECK(save_editor_ui_config(path.string(), cfg));
  EditorUiConfig loaded;
  CHECK(load_editor_ui_config(path.string(), loaded));
  if (has_count(loaded.curve_templates, kMaxCurveTemplates)) {
    CHECK_EQ(loaded.curve_templates.front().id, static_cast<std::uint64_t>(1));
    CHECK_EQ(loaded.curve_templates.back().id, static_cast<std::uint64_t>(kMaxCurveTemplates));
  }
  CHECK_EQ(loaded.curve_selected_template_id, static_cast<std::uint64_t>(0));
}

void test_subdivisions_and_playback_rate_round_trip() {
  CHECK_EQ(wds::ui::clamp_subdivisions_per_beat(0), 1);
  CHECK_EQ(wds::ui::clamp_subdivisions_per_beat(8), 8);
  CHECK_EQ(wds::ui::clamp_subdivisions_per_beat(100), 64);
  CHECK(std::fabs(wds::ui::clamp_playback_rate(0.1f) - 0.25f) < 1e-5f);
  CHECK(std::fabs(wds::ui::clamp_playback_rate(1.5f) - 1.5f) < 1e-5f);
  CHECK(std::fabs(wds::ui::clamp_playback_rate(3.0f) - 2.0f) < 1e-5f);

  EditorUiConfig cfg;
  cfg.subdivisions_per_beat = 12;
  cfg.playback_rate = 0.5f;
  const auto path = temp_config_path("grid_and_rate.yml");
  CHECK(save_editor_ui_config(path.string(), cfg));
  EditorUiConfig loaded;
  CHECK(load_editor_ui_config(path.string(), loaded));
  CHECK_EQ(loaded.subdivisions_per_beat, 12);
  CHECK(std::fabs(loaded.playback_rate - 0.5f) < 1e-5f);

  const auto invalid = temp_config_path("grid_and_rate_clamp.yml");
  CHECK(write_text_atomic(invalid.string(),
                          "subdivisions_per_beat: 200\nplayback_rate: 0.05\n")
            .error == SerializeError::Ok);
  EditorUiConfig clamped;
  CHECK(load_editor_ui_config(invalid.string(), clamped));
  CHECK_EQ(clamped.subdivisions_per_beat, 64);
  CHECK(std::fabs(clamped.playback_rate - 0.25f) < 1e-5f);
}

void test_allow_crash_log_sensitive_default_and_round_trip() {
  EditorUiConfig defaults;
  CHECK(!defaults.allow_crash_log_sensitive);

  const auto missing = temp_config_path("privacy_missing.yml");
  CHECK(write_text_atomic(missing.string(), "note_speed: 5.0\n").error == SerializeError::Ok);
  EditorUiConfig loaded_missing;
  CHECK(load_editor_ui_config(missing.string(), loaded_missing));
  CHECK(!loaded_missing.allow_crash_log_sensitive);

  EditorUiConfig cfg;
  cfg.allow_crash_log_sensitive = true;
  const auto path = temp_config_path("privacy_on.yml");
  CHECK(save_editor_ui_config(path.string(), cfg));
  EditorUiConfig loaded;
  CHECK(load_editor_ui_config(path.string(), loaded));
  CHECK(loaded.allow_crash_log_sensitive);

  EditorUiConfig off;
  off.allow_crash_log_sensitive = false;
  const auto off_path = temp_config_path("privacy_off.yml");
  CHECK(save_editor_ui_config(off_path.string(), off));
  EditorUiConfig loaded_off;
  CHECK(load_editor_ui_config(off_path.string(), loaded_off));
  CHECK(!loaded_off.allow_crash_log_sensitive);
}

}  // namespace

int main() {
  test_old_config_without_curve_keys();
  test_msaa_samples_default_clamp_and_round_trip();
  test_name_round_trip_special_and_chinese();
  test_duplicate_names_distinct_ids_and_order();
  test_invalid_algorithm_direction_and_parameter();
  test_malformed_hex_name();
  test_zero_and_duplicate_ids_repaired();
  test_missing_and_deleted_selection_falls_back_to_zero();
  test_allocate_id_after_sparse_high_ids();
  test_empty_selection_resolves_linear();
  test_curve_template_count_truncated_to_max();
  test_over_cap_indexed_slots_ignored();
  test_settings_save_preserves_curve_state();
  test_save_truncates_in_memory_over_max();
  test_subdivisions_and_playback_rate_round_trip();
  test_allow_crash_log_sensitive_default_and_round_trip();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
