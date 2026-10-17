// Benchmark for the static split-particle indicator (cold cluster builds, cached per-frame draw, uncached
// paint path, memory). Fails only on gross regressions so it is not flaky; read the printed table with
//   ctest --test-dir build-macos-arm -L perf -V
#include "wds/ui/qt/split_effect_snapshot.hpp"
#include "wds/ui/regions/edit/edit_gutters.hpp"

#include <wds/core/gimmick.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QString>
#include <QThread>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

namespace {

using wds::chart_editor::SplitLaneType;
using wds::ui::SplitIndicatorCluster;
using wds::ui::SplitIndicatorClusterKey;

// Median, 60 cached clusters at editor size (lane 40: ~100k px per cluster incl. transparent area, i.e. ~6-8 M px of
// Plus / SourceOver blending plus a 1600x1000 clear). Generous on purpose: only a gross regression should fail.
constexpr double kMaxCachedFrameMs = 12.0;
// Local Apple Silicon is well under 50 ms; GitHub macos-14 shared runners regularly
// spike a single editor-size cold build just over that (CI saw 50.262 ms on 10791).
constexpr double kMaxColdBuildMs = 100.0;  // any single cold build
constexpr double kLruCapMb = 48.0;
// Median GUI-thread time of a simulated editor frame (60 cluster requests + draws) while every cluster is cold.
// The GUI thread must not build, so this is dominated by the 1600x1000 clear plus the draws of finished clusters.
constexpr double kMaxColdRequestFrameMs = 8.0;

double ms_since(const QElapsedTimer& t) { return static_cast<double>(t.nsecsElapsed()) / 1.0e6; }

struct Stats {
  double median = 0, p95 = 0, max = 0, sum = 0;
};

Stats stats_of(std::vector<double> v) {
  Stats s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  s.median = v[v.size() / 2];
  s.p95 = v[std::min(v.size() - 1, static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(v.size()))) - 1)];
  s.max = v.back();
  s.sum = std::accumulate(v.begin(), v.end(), 0.0);
  return s;
}

void print_row(const char* name, const Stats& s, std::size_t n) {
  std::printf("  %-40s n=%-5zu median %8.3f  p95 %8.3f  max %8.3f ms\n", name, n, s.median, s.p95, s.max);
}

std::size_t cluster_bytes(const SplitIndicatorCluster& c) {
  return static_cast<std::size_t>(c.alpha_layer.sizeInBytes() + c.add_layer.sizeInBytes());
}

struct BuildSample {
  int32_t id = 0;
  SplitLaneType type = SplitLaneType::Full;
  double ms = 0;
};

// Line x for edge i of an n-way split, in lane units (matches the renderer's split_line_edge).
std::vector<float> line_xs(int32_t n, float lane_px) {
  std::vector<int32_t> mids;
  wds::ui::split_boundaries_12(n, mids);
  std::vector<float> xs(static_cast<std::size_t>(n) + 1);
  for (int32_t i = 0; i <= n; ++i) {
    int32_t edge = 12;
    if (i == 0) edge = 0;
    else if (i < n) edge = mids[static_cast<std::size_t>(i) - 1] + 1;
    xs[static_cast<std::size_t>(i)] = static_cast<float>(edge) * lane_px;
  }
  return xs;
}

const char* type_name(SplitLaneType t) {
  switch (t) {
    case SplitLaneType::BothEnds: return "BothEnds";
    case SplitLaneType::Full: return "Full";
    case SplitLaneType::Light: return "Light";
    case SplitLaneType::Ignore: return "Ignore";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);

  int failures = 0;
  const QString skins = QDir(QString::fromUtf8(WDS_REPO_ROOT)).filePath(QStringLiteral("skins"));
  wds::ui::ensure_split_effect_uv_atlas(skins);

  const std::vector<int32_t> ids = wds::ui::filter_split_picker_color_ids("");
  const SplitLaneType types[] = {SplitLaneType::BothEnds, SplitLaneType::Full, SplitLaneType::Light,
                                 SplitLaneType::Ignore};
  constexpr int32_t kSplitCount = 3;
  std::printf("split indicator perf: %zu color ids x %zu types, split_count %d\n", ids.size(),
              sizeof(types) / sizeof(types[0]), kSplitCount);
  if (ids.empty()) {
    std::fprintf(stderr, "FAIL: no picker color ids\n");
    return 1;
  }

  // Picker-size key: lane 14.08, band 44. Editor-size key: mirrors split_cluster_key() in chart_edit_widget.cpp:
  // lane 40, lane-independent band quantize_editor_indicator_band(251), size_scale and min sprite x
  // kEditorIndicatorSizeFactor, rank < kEditorIndicatorMaxRank.
  auto picker_key = [&](int32_t id, SplitLaneType t) {
    SplitIndicatorClusterKey k;
    k.color_id = id;
    k.split_count = kSplitCount;
    k.type = t;
    k.lane_px = 14.08f;
    k.band_h = 44;
    k.size_scale = wds::ui::kSplitIndicatorSizeScale;
    k.min_sprite_px = wds::ui::kSplitIndicatorRefMinSpritePx;
    return k;
  };
  const float editor_lane = 40.0f;
  const float editor_k = editor_lane / wds::ui::kSplitIndicatorRefLanePx;
  // lane_px: editor lane width; section 5 uses a lane no other section has so its clusters are cold.
  auto editor_key_at = [&](int32_t id, SplitLaneType t, float lane_px) {
    const float k_lane = lane_px / wds::ui::kSplitIndicatorRefLanePx;
    SplitIndicatorClusterKey k = picker_key(id, t);
    k.lane_px = lane_px;
    k.band_h = wds::ui::quantize_editor_indicator_band(251.0f);  // independent of lane
    k.size_scale = wds::ui::kSplitIndicatorSizeScale * wds::ui::kEditorIndicatorSizeFactor;
    k.min_sprite_px = wds::ui::kSplitIndicatorRefMinSpritePx * k_lane * wds::ui::kEditorIndicatorSizeFactor;
    k.max_rank = wds::ui::kEditorIndicatorMaxRank;
    return k;
  };
  auto editor_key = [&](int32_t id, SplitLaneType t) { return editor_key_at(id, t, editor_lane); };

  // ---- 1 + 4. Cold builds (picker size), memory --------------------------------------------------------------
  std::vector<BuildSample> picker_samples;
  picker_samples.reserve(ids.size() * 4);
  std::size_t picker_bytes = 0;
  std::size_t picker_nonempty = 0;
  // Hold implicitly shared copies of the images so the byte total is the real footprint even if the LRU evicts.
  std::vector<SplitIndicatorCluster> held;
  held.reserve(ids.size() * 4);
  for (SplitLaneType t : types) {
    for (int32_t id : ids) {
      QElapsedTimer tm;
      tm.start();
      const SplitIndicatorCluster* c = wds::ui::split_effect_indicator_cluster(picker_key(id, t));
      const double ms = ms_since(tm);
      picker_samples.push_back({id, t, ms});
      if (c) {
        picker_bytes += cluster_bytes(*c);
        if (!c->empty) ++picker_nonempty;
        held.push_back(*c);
      }
    }
  }
  held.clear();

  std::vector<BuildSample> editor_samples;
  editor_samples.reserve(ids.size() * 4);
  for (SplitLaneType t : types) {
    for (int32_t id : ids) {
      QElapsedTimer tm;
      tm.start();
      (void)wds::ui::split_effect_indicator_cluster(editor_key(id, t));
      editor_samples.push_back({id, t, ms_since(tm)});
    }
  }

  auto times_of = [](const std::vector<BuildSample>& v) {
    std::vector<double> t;
    t.reserve(v.size());
    for (const auto& s : v) t.push_back(s.ms);
    return t;
  };
  const std::vector<double> picker_times = times_of(picker_samples);
  const std::vector<double> editor_times = times_of(editor_samples);

  std::printf("\n[1] cold cluster build (split_count %d)\n", kSplitCount);
  print_row("picker size (lane 14.08, band 44)", stats_of(picker_times), picker_times.size());
  print_row("editor size (lane 40, band x2, rank<6)", stats_of(editor_times), editor_times.size());
  auto print_worst = [](const char* label, std::vector<BuildSample> v) {
    std::sort(v.begin(), v.end(), [](const BuildSample& a, const BuildSample& b) { return a.ms > b.ms; });
    std::printf("  worst 10 (%s):\n", label);
    for (std::size_t i = 0; i < std::min<std::size_t>(10, v.size()); ++i) {
      std::printf("    id %-8d %-8s %8.3f ms\n", v[i].id, type_name(v[i].type), v[i].ms);
    }
  };
  print_worst("picker size", picker_samples);
  print_worst("editor size", editor_samples);

  const double worst_cold = std::max(stats_of(picker_times).max, stats_of(editor_times).max);
  if (worst_cold > kMaxColdBuildMs) {
    std::fprintf(stderr, "FAIL: single cold build %.3f ms > %.1f ms\n", worst_cold, kMaxColdBuildMs);
    ++failures;
  }

  // ---- 2. Picker screen cost ---------------------------------------------------------------------------------
  {
    std::vector<double> sorted = picker_times;
    std::sort(sorted.begin(), sorted.end(), std::greater<double>());
    double worst15 = 0;
    for (std::size_t i = 0; i < std::min<std::size_t>(15, sorted.size()); ++i) worst15 += sorted[i];
    const double all_sum = std::accumulate(picker_times.begin(), picker_times.end(), 0.0);
    std::printf("\n[2] picker cost\n");
    std::printf("  one screen worst case (15 slowest builds): %8.2f ms\n", worst15);
    std::printf("  all %zu builds (non-lazy picker):          %8.2f ms\n", picker_times.size(), all_sum);
  }

  // ---- 3. Per-frame cost -------------------------------------------------------------------------------------
  constexpr int kClusters = 60;
  constexpr int kIters = 200;
  {
    // Pick the first 60 ids whose cluster has content; Full type, editor size.
    std::vector<int32_t> pick;
    for (int32_t id : ids) {
      const SplitIndicatorCluster* c = wds::ui::split_effect_indicator_cluster(editor_key(id, SplitLaneType::Full));
      if (c && !c->empty) pick.push_back(id);
      if (pick.size() >= static_cast<std::size_t>(kClusters)) break;
    }
    for (std::size_t i = 0; pick.size() < static_cast<std::size_t>(kClusters) && !pick.empty(); ++i) {
      pick.push_back(pick[i]);  // too few non-empty ids: repeat
    }
    if (pick.empty()) {
      std::fprintf(stderr, "FAIL: no non-empty editor clusters to draw\n");
      return 1;
    }
    // Copy the structs (QImages are shared) so later cache misses cannot invalidate them.
    std::vector<SplitIndicatorCluster> clusters;
    for (int32_t id : pick) {
      const SplitIndicatorCluster* c = wds::ui::split_effect_indicator_cluster(editor_key(id, SplitLaneType::Full));
      clusters.push_back(*c);
    }

    QImage frame(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    // Scatter in a 10 x 6 grid, each cluster 150 x 160 px apart.
    auto origin = [](int i) { return QPointF(20.0 + (i % 10) * 155.0, 20.0 + (i / 10) * 160.0); };

    std::vector<double> cached_ms;
    cached_ms.reserve(kIters);
    for (int it = 0; it < kIters; ++it) {
      QElapsedTimer tm;
      tm.start();
      frame.fill(Qt::transparent);
      QPainter p(&frame);
      for (int i = 0; i < kClusters; ++i) {
        wds::ui::draw_split_effect_indicator_cluster(p, clusters[static_cast<std::size_t>(i)], origin(i));
      }
      p.end();
      cached_ms.push_back(ms_since(tm));
    }

    // Uncached path for the same 60.
    wds::ui::SplitIndicatorSpec spec;
    spec.split_count = kSplitCount;
    spec.type = SplitLaneType::Full;
    spec.px_per_unit = editor_lane / 0.925f;
    spec.size_scale = wds::ui::kSplitIndicatorSizeScale * wds::ui::kEditorIndicatorSizeFactor;
    spec.min_sprite_px = wds::ui::kSplitIndicatorRefMinSpritePx * editor_k * wds::ui::kEditorIndicatorSizeFactor;
    spec.max_rank = wds::ui::kEditorIndicatorMaxRank;
    const std::vector<float> xs_rel = line_xs(kSplitCount, editor_lane);
    const float band_h = static_cast<float>(editor_key(1, SplitLaneType::Full).band_h);
    constexpr int kUncachedIters = 20;
    std::vector<double> uncached_ms;
    for (int it = 0; it < kUncachedIters; ++it) {
      QElapsedTimer tm;
      tm.start();
      frame.fill(Qt::transparent);
      QPainter p(&frame);
      for (int i = 0; i < kClusters; ++i) {
        spec.color_id = pick[static_cast<std::size_t>(i)];
        const QPointF o = origin(i);
        std::vector<float> xs = xs_rel;
        for (float& x : xs) x += static_cast<float>(o.x());
        wds::ui::paint_split_effect_indicator(p, spec, xs.data(), static_cast<int>(xs.size()),
                                              static_cast<float>(o.y()), static_cast<float>(o.y()) + band_h);
      }
      p.end();
      uncached_ms.push_back(ms_since(tm));
    }

    const Stats cs = stats_of(cached_ms);
    const Stats us = stats_of(uncached_ms);
    std::printf("\n[3] per-frame cost, %d clusters into 1600x1000 ARGB32_Premultiplied (frame clear included)\n",
                kClusters);
    print_row("cached draw_split_effect_indicator_cluster", cs, cached_ms.size());
    print_row("uncached paint_split_effect_indicator", us, uncached_ms.size());
    std::printf("  uncached / cached (median): %.1fx   (60 fps budget 16.7 ms)\n",
                cs.median > 0 ? us.median / cs.median : 0.0);
    if (cs.median > kMaxCachedFrameMs) {
      std::fprintf(stderr, "FAIL: cached frame median %.3f ms > %.1f ms\n", cs.median, kMaxCachedFrameMs);
      ++failures;
    }
  }

  // ---- 4. Memory ---------------------------------------------------------------------------------------------
  {
    const double mb = static_cast<double>(picker_bytes) / (1024.0 * 1024.0);
    std::printf("\n[4] memory: %zu picker clusters (%zu non-empty) = %.2f MB of layer data vs ~%.0f MB LRU cap (%.0f%%)\n",
                picker_samples.size(), picker_nonempty, mb, kLruCapMb, 100.0 * mb / kLruCapMb);
  }

  // ---- 5. Editor scenario with the background builder --------------------------------------------------------
  // 60 distinct clusters, all cold (a lane_px no earlier section used, so neither the cache nor the worker has them).
  // Each simulated frame = clear + split_effect_indicator_cluster_request() + draw for every cluster, exactly the
  // editor's paint loop. Only that paint work is timed as "GUI-thread frame time"; the event loop is pumped between
  // frames with QCoreApplication::processEvents (result delivery is timed separately).
  {
    constexpr double kColdLane = 41.25;  // unused by sections 1-4 (they use 14.08 and 40)
    auto cold_key = [&](int32_t id) {
      return editor_key_at(id, SplitLaneType::Full, static_cast<float>(kColdLane));
    };
    std::vector<int32_t> pick;
    for (int32_t id : ids) {
      pick.push_back(id);
      if (pick.size() >= static_cast<std::size_t>(kClusters)) break;
    }
    const std::size_t n = pick.size();

    QObject notifier;  // notify target: lives for the whole section
    int callbacks = 0;
    std::vector<char> ready(n, 0);
    std::size_t ready_count = 0;
    QImage frame(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    auto origin = [](std::size_t i) {
      return QPointF(20.0 + static_cast<double>(i % 10) * 155.0, 20.0 + static_cast<double>(i / 10) * 160.0);
    };

    std::vector<double> frame_ms;
    std::vector<double> pump_ms;
    double first_frame_ms = 0;
    QElapsedTimer wall;
    wall.start();
    constexpr double kTimeoutMs = 120000.0;
    while (ready_count < n && ms_since(wall) < kTimeoutMs) {
      QElapsedTimer tm;
      tm.start();
      frame.fill(Qt::transparent);
      {
        QPainter p(&frame);
        for (std::size_t i = 0; i < n; ++i) {
          const SplitIndicatorCluster* c =
              wds::ui::split_effect_indicator_cluster_request(cold_key(pick[i]), &notifier, [&callbacks] { ++callbacks; });
          if (c != nullptr) {
            if (!ready[i]) {
              ready[i] = 1;
              ++ready_count;
            }
            if (!c->empty) wds::ui::draw_split_effect_indicator_cluster(p, *c, origin(i));
          }  // c is not kept across processEvents below
        }
      }
      const double ms = ms_since(tm);
      if (frame_ms.empty()) first_frame_ms = ms;
      frame_ms.push_back(ms);

      QElapsedTimer pt;
      pt.start();
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
      pump_ms.push_back(ms_since(pt));
      QThread::msleep(2);  // let the single worker run; a real frame loop idles at least this long
    }
    const double wall_ms = ms_since(wall);
    // Drain the remaining queued on_ready callbacks.
    for (int i = 0; i < 20 && callbacks < static_cast<int>(n); ++i) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
      QThread::msleep(1);
    }

    const Stats fs = stats_of(frame_ms);
    const Stats ps = stats_of(pump_ms);
    std::printf("\n[5] editor scenario, %zu cold clusters (lane %.2f) built on the background worker\n", n, kColdLane);
    print_row("GUI-thread frame (clear+request+draw)", fs, frame_ms.size());
    print_row("processEvents (result delivery)", ps, pump_ms.size());
    std::printf("  first frame (all %zu cold requests):   %8.3f ms\n", n, first_frame_ms);
    std::printf("  wall until all ready: %8.1f ms over %zu frames, %d/%zu on_ready callbacks delivered\n", wall_ms,
                frame_ms.size(), callbacks, n);
    if (ready_count < n) {
      std::fprintf(stderr, "FAIL: only %zu/%zu clusters ready after %.0f ms\n", ready_count, n, wall_ms);
      ++failures;
    }
    if (fs.median > kMaxColdRequestFrameMs) {
      std::fprintf(stderr, "FAIL: GUI-thread frame median with cold requests %.3f ms > %.1f ms\n", fs.median,
                   kMaxColdRequestFrameMs);
      ++failures;
    }
  }

  std::printf("\nthresholds: cached frame median <= %.1f ms, single cold build <= %.1f ms, cold-request frame median "
              "<= %.1f ms -> %s\n",
              kMaxCachedFrameMs, kMaxColdBuildMs, kMaxColdRequestFrameMs, failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
