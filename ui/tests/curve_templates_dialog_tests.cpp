#include "wds/ui/regions/settings/curve_templates_dialog.hpp"

#include <wds/interaction/events.hpp>
#include <wds/interaction/theme.hpp>
#include <wds/interaction/ui_painter.hpp>
#include <wds/interaction/widget_root.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/interaction/widgets/text_field.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
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
using wds::ui::CurveTemplate;
using wds::ui::CurveTemplateDialogSession;
using wds::ui::CurveTemplatesDialog;
using wds::ui::CurveTemplateUiState;
using wds::ui::kMaxCurveTemplates;

bool nearly_equal(double a, double b) {
  return std::fabs(a - b) <= 1e-12;
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

CurveTemplateUiState make_live(std::vector<CurveTemplate> templates, std::uint64_t selected_id) {
  CurveTemplateUiState live;
  live.templates = std::move(templates);
  live.selected_id = selected_id;
  live.direction = EasingDirection::Out;
  return live;
}

}  // namespace

int main() {
  // Open copies into a private working set; live state is unchanged by edits.
  {
    CurveTemplateUiState live =
        make_live({make_template(3, "orig", EasingAlgorithm::Poly, 2.5)}, 3);
    CurveTemplateDialogSession session;
    session.begin(live);
    CHECK_EQ(session.templates().size(), static_cast<std::size_t>(1));
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(3));
    CHECK(session.selected() != nullptr);
    session.set_selected_name("edited");
    session.set_selected_algorithm(EasingAlgorithm::Exp);
    session.set_selected_parameter(4.0);
    session.add();
    CHECK_EQ(live.templates.size(), static_cast<std::size_t>(1));
    CHECK_EQ(live.templates[0].name, std::string("orig"));
    CHECK(live.templates[0].algorithm == EasingAlgorithm::Poly);
    CHECK(nearly_equal(live.templates[0].parameter, 2.5));
    CHECK_EQ(live.selected_id, static_cast<std::uint64_t>(3));
    CHECK(live.direction == EasingDirection::Out);
  }

  // Cancel / discard leaves live unchanged; reopening recopies the original.
  {
    CurveTemplateUiState live =
        make_live({make_template(1, "keep", EasingAlgorithm::Linear, 1.0)}, 1);
    CurveTemplateDialogSession session;
    session.begin(live);
    session.set_selected_name("pending");
    session.add();
    session.discard();
    CHECK_EQ(live.templates.size(), static_cast<std::size_t>(1));
    CHECK_EQ(live.templates[0].name, std::string("keep"));
    CHECK_EQ(live.selected_id, static_cast<std::uint64_t>(1));
    session.begin(live);
    CHECK_EQ(session.templates().size(), static_cast<std::size_t>(1));
    CHECK_EQ(session.templates()[0].name, std::string("keep"));
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(1));
  }

  // Confirm replaces templates only; live fill id and direction stay unless the
  // fill template was removed. Editing/adding rows must not hijack the toolbar.
  {
    CurveTemplateUiState live =
        make_live({make_template(8, "old", EasingAlgorithm::Sine, 0.0)}, 8);
    CurveTemplateDialogSession session;
    session.begin(live);
    session.set_selected_name("新名称#");
    session.set_selected_algorithm(EasingAlgorithm::Poly);
    session.set_selected_parameter(3.5);
    session.add();
    CHECK_EQ(session.selected_id(), session.templates().back().id);
    session.commit(live);
    CHECK_EQ(live.templates.size(), static_cast<std::size_t>(2));
    CHECK_EQ(live.templates[0].name, std::string("新名称#"));
    CHECK(live.templates[0].algorithm == EasingAlgorithm::Poly);
    CHECK(nearly_equal(live.templates[0].parameter, 3.5));
    CHECK(live.templates[1].id != 0);
    CHECK(live.templates[1].id != live.templates[0].id);
    CHECK_EQ(live.selected_id, static_cast<std::uint64_t>(8));
    CHECK(live.direction == EasingDirection::Out);
  }

  // Deleting the selected row picks a deterministic nearest remaining row.
  {
    CurveTemplateUiState live = make_live(
        {make_template(1, "a", EasingAlgorithm::Linear, 0.0),
         make_template(2, "b", EasingAlgorithm::Linear, 0.0),
         make_template(3, "c", EasingAlgorithm::Linear, 0.0)},
        2);
    CurveTemplateDialogSession session;
    session.begin(live);
    session.remove_at(1);
    CHECK_EQ(session.templates().size(), static_cast<std::size_t>(2));
    CHECK_EQ(session.templates()[0].id, static_cast<std::uint64_t>(1));
    CHECK_EQ(session.templates()[1].id, static_cast<std::uint64_t>(3));
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(3));

    session.select_id(1);
    session.remove_at(0);
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(3));

    session.remove_at(0);
    CHECK(session.templates().empty());
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(0));
    CHECK(session.selected() == nullptr);
  }

  // Deleting a non-selected row keeps the current selection.
  {
    CurveTemplateUiState live = make_live(
        {make_template(1, "a", EasingAlgorithm::Linear, 0.0),
         make_template(2, "b", EasingAlgorithm::Linear, 0.0),
         make_template(3, "c", EasingAlgorithm::Linear, 0.0)},
        1);
    CurveTemplateDialogSession session;
    session.begin(live);
    session.remove_at(2);
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(1));
    CHECK_EQ(session.templates().size(), static_cast<std::size_t>(2));
  }

  // First unused default name is 曲线 N; add selects the new nonzero id.
  {
    CHECK_EQ(CurveTemplateDialogSession::next_default_name({}), std::string("曲线 1"));
    std::vector<CurveTemplate> existing = {
        make_template(1, "曲线 1", EasingAlgorithm::Linear, 0.0),
        make_template(4, "曲线 3", EasingAlgorithm::Linear, 0.0),
        make_template(9, "custom", EasingAlgorithm::Linear, 0.0),
    };
    CHECK_EQ(CurveTemplateDialogSession::next_default_name(existing), std::string("曲线 2"));

    CurveTemplateUiState live = make_live(existing, 4);
    CurveTemplateDialogSession session;
    session.begin(live);
    session.add();
    CHECK_EQ(session.templates().size(), static_cast<std::size_t>(4));
    CHECK_EQ(session.templates().back().name, std::string("曲线 2"));
    CHECK(session.templates().back().id != 0);
    CHECK_EQ(session.selected_id(), session.templates().back().id);
    session.add();
    CHECK_EQ(session.templates().back().name, std::string("曲线 4"));
  }

  // At the cap, add is a no-op.
  {
    std::vector<CurveTemplate> templates;
    templates.reserve(kMaxCurveTemplates);
    for (std::size_t i = 0; i < kMaxCurveTemplates; ++i) {
      templates.push_back(make_template(i + 1, "t" + std::to_string(i), EasingAlgorithm::Linear,
                                        0.0));
    }
    CurveTemplateUiState live = make_live(templates, 1);
    CurveTemplateDialogSession session;
    session.begin(live);
    CHECK(!session.can_add());
    session.add();
    CHECK_EQ(session.templates().size(), kMaxCurveTemplates);
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(1));
    CHECK_EQ(session.templates().back().id, static_cast<std::uint64_t>(kMaxCurveTemplates));
  }

  // Invalid selected-ID falls back to empty edit selection on open. Confirm
  // still keeps a live fill id that exists in the working templates.
  {
    CurveTemplateUiState live =
        make_live({make_template(2, "ok", EasingAlgorithm::Linear, 0.0)}, 99);
    CurveTemplateDialogSession session;
    session.begin(live);
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(0));
    CHECK(session.selected() == nullptr);

    live.selected_id = 2;
    session.begin(live);
    session.select_id(12345);
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(0));
    session.commit(live);
    CHECK_EQ(live.selected_id, static_cast<std::uint64_t>(2));
    CHECK_EQ(live.templates.size(), static_cast<std::size_t>(1));
  }

  // Deleting the live fill template falls back to empty / linear on confirm.
  // Deleting a different row keeps the fill id even if the edit row moved.
  {
    CurveTemplateUiState live = make_live(
        {make_template(10, "keep", EasingAlgorithm::Linear, 0.0),
         make_template(11, "drop", EasingAlgorithm::Linear, 0.0)},
        11);
    CurveTemplateDialogSession session;
    session.begin(live);
    session.remove_at(1);
    session.commit(live);
    CHECK_EQ(live.templates.size(), static_cast<std::size_t>(1));
    CHECK_EQ(live.selected_id, static_cast<std::uint64_t>(0));
    CHECK(live.direction == EasingDirection::Out);

    live.selected_id = 10;
    live.direction = EasingDirection::OutIn;
    session.begin(live);
    session.select_id(10);
    session.add();
    session.remove_at(1);
    session.commit(live);
    CHECK_EQ(live.selected_id, static_cast<std::uint64_t>(10));
    CHECK(live.direction == EasingDirection::OutIn);
  }

  // commit() must not reset a caller-supplied live direction (no In default).
  {
    CurveTemplateUiState live = make_live(
        {make_template(4, "a", EasingAlgorithm::Sine, 0.0),
         make_template(5, "b", EasingAlgorithm::Poly, 1.0)},
        4);
    CurveTemplateDialogSession session;
    session.begin(live);
    session.select_id(5);
    CurveTemplateUiState dst;
    dst.selected_id = 4;
    dst.direction = EasingDirection::OutIn;
    session.commit(dst);
    CHECK_EQ(dst.selected_id, static_cast<std::uint64_t>(4));
    CHECK(dst.direction == EasingDirection::OutIn);
    CHECK_EQ(dst.templates.size(), static_cast<std::size_t>(2));
  }

  // Parameter is normalized to [0, 20]; non-finite values become 0.
  {
    CurveTemplateUiState live = make_live(
        {make_template(1, "p", EasingAlgorithm::Poly, 99.0),
         make_template(2, "n", EasingAlgorithm::Exp, std::numeric_limits<double>::quiet_NaN()),
         make_template(3, "i", EasingAlgorithm::Poly, std::numeric_limits<double>::infinity()),
         make_template(4, "neg", EasingAlgorithm::Exp, -4.0)},
        1);
    CurveTemplateDialogSession session;
    session.begin(live);
    session.set_selected_parameter(25.0);
    CHECK(nearly_equal(session.selected()->parameter, 20.0));
    session.select_id(4);
    session.set_selected_parameter(-1.0);
    CHECK(nearly_equal(session.selected()->parameter, 0.0));
    session.commit(live);
    CHECK(nearly_equal(live.templates[0].parameter, 20.0));
    CHECK(nearly_equal(live.templates[1].parameter, 0.0));
    CHECK(nearly_equal(live.templates[2].parameter, 0.0));
    CHECK(nearly_equal(live.templates[3].parameter, 0.0));
  }

  // Linear/Sine keep a stored parameter that set_selected_parameter can still write.
  {
    CurveTemplateUiState live =
        make_live({make_template(1, "lin", EasingAlgorithm::Linear, 7.0)}, 1);
    CurveTemplateDialogSession session;
    session.begin(live);
    session.set_selected_algorithm(EasingAlgorithm::Sine);
    CHECK(nearly_equal(session.selected()->parameter, 7.0));
    session.set_selected_parameter(1.25);
    CHECK(session.selected()->algorithm == EasingAlgorithm::Sine);
    CHECK(nearly_equal(session.selected()->parameter, 1.25));
  }

  // Empty selection is valid: item-specific writes are no-ops (no invalid access).
  {
    CurveTemplateDialogSession session;
    session.begin(make_live({}, 0));
    CHECK(session.templates().empty());
    CHECK(session.selected() == nullptr);
    session.set_selected_name("nope");
    session.set_selected_algorithm(EasingAlgorithm::Exp);
    session.set_selected_parameter(3.0);
    session.remove_at(0);
    CHECK(session.templates().empty());
    CHECK_EQ(session.selected_id(), static_cast<std::uint64_t>(0));
  }

  // First Escape on the WidgetRoot key path closes the modal even if a field has
  // focus. Live templates/direction stay unchanged; reopen recopies the original.
  {
    using wds::interaction::KeyCode;
    using wds::interaction::KeyDownEvent;
    using wds::interaction::TextInputEvent;
    using wds::interaction::WidgetRoot;

    const auto run_focused_escape = [&](bool focus_parameter) {
      CurveTemplateUiState live =
          make_live({make_template(1, "keep", EasingAlgorithm::Poly, 2.0)}, 1);
      WidgetRoot root;
      auto dialog = std::make_unique<CurveTemplatesDialog>();
      auto* d = dialog.get();
      root.add_child(std::move(dialog));
      root.set_bounds({0.0f, 0.0f, 1000.0f, 800.0f});
      d->open(live);
      d->layout(root.bounds());

      CHECK(d->is_open());
      CHECK_EQ(d->session().templates().size(), static_cast<std::size_t>(1));

      // children: kMax delete buttons, add, name, parameter, confirm, cancel.
      auto* name = static_cast<wds::interaction::TextField*>(
          d->children()[kMaxCurveTemplates + 1].get());
      auto* parameter = static_cast<wds::interaction::TextField*>(
          d->children()[kMaxCurveTemplates + 2].get());
      auto* focused = focus_parameter ? parameter : name;
      root.set_focus(focused);
      CHECK(root.focused_widget() == focused);
      root.process_frame(0.016f, {TextInputEvent{focus_parameter ? "9" : "X"}});
      CHECK(d->is_open());
      CHECK(d->session().selected() != nullptr);
      if (focus_parameter) {
        CHECK(!nearly_equal(d->session().selected()->parameter, 2.0));
      } else {
        CHECK(d->session().selected()->name.find('X') != std::string::npos);
      }

      root.process_frame(0.016f, {KeyDownEvent{KeyCode::Escape, {}, false}});
      CHECK(!d->is_open());
      CHECK_EQ(live.templates.size(), static_cast<std::size_t>(1));
      CHECK_EQ(live.templates[0].name, std::string("keep"));
      CHECK(nearly_equal(live.templates[0].parameter, 2.0));
      CHECK_EQ(live.selected_id, static_cast<std::uint64_t>(1));
      CHECK(live.direction == EasingDirection::Out);

      d->open(live);
      d->layout(root.bounds());
      CHECK(d->is_open());
      CHECK_EQ(d->session().templates().size(), static_cast<std::size_t>(1));
      CHECK_EQ(d->session().templates()[0].name, std::string("keep"));
      CHECK(nearly_equal(d->session().templates()[0].parameter, 2.0));
      CHECK_EQ(d->session().selected_id(), static_cast<std::uint64_t>(1));
    };

    run_focused_escape(false);
    run_focused_escape(true);
  }

  // Right panel: algorithm → name and parameter on one row → 2×2 preview.
  {
    namespace th = wds::interaction::theme;
    CurveTemplateUiState live =
        make_live({make_template(1, "keep", EasingAlgorithm::Poly, 2.0)}, 1);
    CurveTemplatesDialog dialog;
    dialog.set_bounds({0.0f, 0.0f, 1000.0f, 800.0f});
    dialog.open(live);
    dialog.layout({0.0f, 0.0f, 1000.0f, 800.0f});
    auto* name = static_cast<wds::interaction::TextField*>(
        dialog.children()[kMaxCurveTemplates + 1].get());
    auto* parameter = static_cast<wds::interaction::TextField*>(
        dialog.children()[kMaxCurveTemplates + 2].get());
    CHECK(name != nullptr);
    CHECK(parameter != nullptr);
    CHECK(std::fabs(name->bounds().y - parameter->bounds().y) < 0.5f);
    CHECK(std::fabs(name->bounds().h - th::kControlHeight) < 0.5f);
    CHECK(name->bounds().x + name->bounds().w <= parameter->bounds().x + 0.01f);
  }
  {
    wds::interaction::UiPainter painter;
    const wds::interaction::Rect inner{10.0f, 10.0f, 40.0f, 30.0f};
    wds::ui::paint_easing_preview_polyline(painter, inner, EasingAlgorithm::Linear,
                                           EasingDirection::In, 0.0, 0.5f);
    int cols = 0;
    float prev_x = inner.x - 1.0f;
    for (const auto& rect : painter.rects()) {
      CHECK(std::fabs(rect.bounds.w - 1.0f) < 0.01f);
      CHECK(rect.bounds.h + 0.01f >= 1.5f);
      CHECK(rect.bounds.x + 0.01f >= inner.x);
      CHECK(rect.bounds.x + 0.01f <= inner.x + inner.w);
      CHECK(rect.bounds.x + 0.01f >= prev_x + 0.99f);
      prev_x = rect.bounds.x;
      ++cols;
    }
    CHECK_EQ(cols, 40);
    CHECK(painter.sprites().empty());
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
