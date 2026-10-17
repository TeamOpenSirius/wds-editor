// Diagnostic: renders the REAL split-indicator cluster output (synchronous path the app uses) for every colour id
// that has a shape-5 strip, at picker and editor size, into contact-sheet PNGs, plus index.txt and strips.tsv.
//   ctest --test-dir build-macos-arm -L dump -V      (output: <build>/split_indicator_dump)
// Regression gate: for every strip-bearing id at picker and editor size, the cluster's add+alpha layer coverage
// (pixels with alpha > 0.5, relative to the 12-lane x band area) and its bbox width are measured and printed as a
// table; an id is flagged "BLOCK" when coverage > 0.50 or the bbox is wider than 12 lanes + 2 x 3 lanes margin (the
// symptom of a ribbon blowing up into a colour block / triangle fan). Fails (non-zero) if any id is flagged or
// rendering throws. The PNG sheets and text files are written either way.
// The editor layout mirrors the editor's cluster key (lane-independent band via quantize_editor_indicator_band, rank < kEditorIndicatorMaxRank,
// size factor); coverage is normalised by that (taller) band. A second gate checks every strip after the ribbon
// return-to-line transform (head / tail on the split line, no sudden fold near the ends; see measure_ribbon), and the
// dump_ribbon_ends_{picker,editor}_0.png sheets show the split lines with head / tail markers.
#include "wds/ui/qt/split_effect_snapshot.hpp"
#include "wds/ui/qt/split_effect_snapshot_data.hpp"
#include "wds/ui/regions/edit/edit_gutters.hpp"

#include <wds/core/gimmick.hpp>

#include <QColor>
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

using wds::chart_editor::SplitLaneType;
using wds::ui::SplitIndicatorCluster;
using wds::ui::SplitIndicatorClusterKey;
using wds::ui::SplitSnapParticle;
using wds::ui::SplitSnapStripPoint;

constexpr int32_t kSplitCount = 3;
constexpr int kCols = 4;
constexpr std::size_t kPerSheet = 24;
constexpr int kMarginX = 24;      // horizontal margin around the 12 lanes inside a tile
constexpr int kMarginY = 48;      // vertical margin above/below the band (overflow stays visible, clipped at tile)
constexpr int kLabelH = 16;

struct Layout {
  const char* name;
  float lane_px;
  int band_h;
  float min_sprite;
  float size_scale;  // picker: kSplitIndicatorSizeScale; editor: x kEditorIndicatorSizeFactor (as split_cluster_key)
  int max_rank;      // picker: 2; editor: kEditorIndicatorMaxRank
};

// Tile background: dark plate (the picker's / the old sheets' fill) or a mid-grey striped scene, on which the
// cut-out of subtractive particles (colour ids 10620 / 11070) shows up as a hole in the stripes.
enum class Bg { Plate, GreyStripes };

// Picker lane: same arithmetic as preview_layout(192) in split_picker_dialog.cpp.
float picker_lane_px() {
  constexpr float box_w = 192.0f;  // kPreviewMinBox
  const float line_w = std::clamp(box_w / 12.0f * 0.14f, 1.5f, 2.5f);
  const float pad = std::max(line_w * 0.5f + 2.0f, 0.06f * box_w);  // indicator_h_inset
  const float inner_w = std::max(1.0f, box_w - pad * 2.0f);
  return inner_w / 12.0f;
}

SplitIndicatorClusterKey make_key(int32_t id, const Layout& l) {
  SplitIndicatorClusterKey k;
  k.color_id = id;
  k.split_count = kSplitCount;
  k.type = SplitLaneType::Full;
  k.lane_px = l.lane_px;
  k.band_h = l.band_h;
  k.size_scale = l.size_scale;
  k.min_sprite_px = l.min_sprite;
  k.max_rank = l.max_rank;
  return k;
}

// Lane edge (0..12) of every official line of the dump's split: same rule as the renderer's split_line_edge.
std::vector<int> split_line_edges() {
  std::vector<int32_t> mids;
  wds::ui::split_boundaries_12(kSplitCount, mids);
  std::vector<int> e(static_cast<std::size_t>(kSplitCount) + 1);
  for (int i = 0; i <= kSplitCount; ++i) {
    e[static_cast<std::size_t>(i)] = i <= 0 ? 0 : i >= kSplitCount ? 12 : mids[static_cast<std::size_t>(i) - 1] + 1;
  }
  return e;
}

void fill_bg(QPainter& p, const QRect& r, Bg bg) {
  if (bg == Bg::Plate) {
    p.fillRect(r, QColor(5, 8, 13));
    return;
  }
  p.fillRect(r, QColor(112, 112, 112));
  for (int y = r.top(); y <= r.bottom(); y += 12) p.fillRect(QRect(r.left(), y, r.width(), 6), QColor(168, 168, 168));
}

bool has_strip(int32_t id) {
  for (int line = 0; line <= kSplitCount; ++line) {
    int n = 0;
    const SplitSnapParticle* ps = wds::ui::split_snap_particles(id, kSplitCount, line, &n);
    for (int i = 0; ps && i < n; ++i)
      if (ps[i].shape == 5) return true;
  }
  return false;
}

// Tile (w,h) for a layout.
QSize tile_size(const Layout& l) {
  return QSize(static_cast<int>(std::lround(12.0f * l.lane_px)) + 2 * kMarginX, l.band_h + 2 * kMarginY + kLabelH);
}

void draw_tile(QPainter& p, const QPoint& tl, int32_t id, const Layout& l, bool labels, Bg bg,
               bool split_lines = false) {
  const QSize ts = tile_size(l);
  p.save();
  p.setClipRect(QRect(tl, ts));
  fill_bg(p, QRect(tl, ts), bg);
  const double ox = tl.x() + kMarginX;
  const double oy = tl.y() + kLabelH + kMarginY;  // band_top
  const SplitIndicatorCluster* c = wds::ui::split_effect_indicator_cluster(make_key(id, l));
  if (c && !c->empty) wds::ui::draw_split_effect_indicator_cluster(p, *c, QPointF(ox, oy));
  // 13 lane-edge lines, thin grey, spanning the band.
  p.setCompositionMode(QPainter::CompositionMode_SourceOver);
  p.setPen(QPen(QColor(110, 110, 110, 160), 1.0));
  for (int i = 0; i <= 12; ++i) {
    const double x = ox + static_cast<double>(i) * l.lane_px;
    p.drawLine(QPointF(x, oy), QPointF(x, oy + l.band_h));
  }
  // Band outline ticks.
  p.setPen(QPen(QColor(70, 70, 70, 200), 1.0));
  p.drawLine(QPointF(ox, oy), QPointF(ox + 12.0 * l.lane_px, oy));
  p.drawLine(QPointF(ox, oy + l.band_h), QPointF(ox + 12.0 * l.lane_px, oy + l.band_h));
  if (split_lines) {
    // The split lines themselves (full tile height, red) and the strip ends as the cluster build produced them:
    // head (vertex 0) yellow, tail (last vertex) cyan, both should sit ON a red line.
    p.setPen(QPen(QColor(255, 70, 70, 220), 1.0));
    for (int e : split_line_edges()) {
      const double x = ox + static_cast<double>(e) * l.lane_px;
      p.drawLine(QPointF(x, tl.y() + kLabelH), QPointF(x, tl.y() + ts.height()));
    }
    p.setPen(Qt::NoPen);
    for (const auto& s : wds::ui::split_effect_indicator_debug_strips(make_key(id, l))) {
      if (s.x.empty()) continue;
      p.setBrush(QColor(255, 235, 60));
      p.drawEllipse(QPointF(ox + s.x.front(), oy + s.y.front()), 2.0, 2.0);
      p.setBrush(QColor(60, 235, 255));
      p.drawEllipse(QPointF(ox + s.x.back(), oy + s.y.back()), 2.0, 2.0);
    }
    p.setBrush(Qt::NoBrush);
  }
  if (labels) {
    p.setPen(QColor(230, 230, 230));
    p.drawText(QPointF(tl.x() + 3, tl.y() + 12), QString::number(id));
  }
  p.setPen(QPen(QColor(60, 60, 60), 1.0));
  p.setBrush(Qt::NoBrush);
  p.drawRect(QRect(tl, ts - QSize(1, 1)));
  p.restore();
}

std::string write_sheet(const QString& dir, const char* prefix, int n, const std::vector<int32_t>& ids,
                        const Layout& l, bool labels, std::string& file_out, Bg bg = Bg::Plate,
                        bool split_lines = false) {
  const QSize ts = tile_size(l);
  const int rows = static_cast<int>((ids.size() + kCols - 1) / kCols);
  QImage img(ts.width() * kCols, ts.height() * std::max(1, rows), QImage::Format_ARGB32_Premultiplied);
  img.fill(QColor(5, 8, 13));
  QPainter p(&img);
  if (labels) p.setFont(QFont());
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const QPoint tl(static_cast<int>(i % kCols) * ts.width(), static_cast<int>(i / kCols) * ts.height());
    draw_tile(p, tl, ids[i], l, labels, bg, split_lines);
  }
  p.end();
  file_out = std::string(prefix) + "_" + std::to_string(n) + ".png";
  const QString path = QDir(dir).filePath(QString::fromStdString(file_out));
  if (!img.save(path)) std::fprintf(stderr, "WARN: could not save %s\n", path.toUtf8().constData());
  std::string line = file_out + ":";
  for (int32_t id : ids) line += " " + std::to_string(id);
  return line;
}

const char* blend_name(uint8_t b) { return b == 0 ? "add" : b == 1 ? "alpha" : "other"; }

void describe_strips(int32_t id, std::string& out) {
  const SplitSnapStripPoint* pool = wds::ui::split_snap_strip_points();
  for (int line = 0; line <= kSplitCount; ++line) {
    int n = 0;
    const SplitSnapParticle* ps = wds::ui::split_snap_particles(id, kSplitCount, line, &n);
    for (int i = 0; ps && i < n; ++i) {
      const SplitSnapParticle& s = ps[i];
      if (s.shape != 5) continue;
      float maxw = 0, len = 0, ymin = 1e30f, ymax = -1e30f;
      for (uint32_t k = 0; k < s.strip_count; ++k) {
        const SplitSnapStripPoint& v = pool[s.strip_first + k];
        maxw = std::max(maxw, v.width);
        ymin = std::min(ymin, v.dy - v.width * 0.5f);
        ymax = std::max(ymax, v.dy + v.width * 0.5f);
        if (k > 0) {
          const SplitSnapStripPoint& u = pool[s.strip_first + k - 1];
          len += std::hypot(v.dx - u.dx, v.dy - u.dy);
        }
      }
      char buf[320];
      std::snprintf(buf, sizeof buf,
                    "    line %d kind %d verts %u uv %u blend %s maxw %.4f polylen %.4f dy[%.4f..%.4f] extent %.4f\n",
                    line, s.catalog_index, static_cast<unsigned>(s.strip_count), static_cast<unsigned>(s.uv),
                    blend_name(s.blend), maxw, len, ymin, ymax, ymax - ymin);
      out += buf;
    }
  }
}

// Dense sprite effects (e.g. 11591 glow pillars) reach ~0.41; the ribbon blow-up this guards against filled most of the band.
constexpr double kMaxCoverage = 0.50;       // alpha > 0.5 pixels / band area
// 12 lanes + the renderer's per-quad horizontal cap (3 lanes) on both sides: an edge particle may legitimately extend
// that far past each outer line.
constexpr double kMaxWidthLanes = 12.0 + 2.0 * 3.0;

// Number of pixels whose alpha is > 0.5 in the add layer OR the alpha layer (both share size and offset, a layer
// without content is null).
long long count_opaque(const QImage& a, const QImage& b) {
  const int w = !a.isNull() ? a.width() : b.isNull() ? 0 : b.width();
  const int h = !a.isNull() ? a.height() : b.isNull() ? 0 : b.height();
  long long n = 0;
  for (int y = 0; y < h; ++y) {
    const auto* ra = a.isNull() ? nullptr : reinterpret_cast<const QRgb*>(a.constScanLine(y));
    const auto* rb = b.isNull() ? nullptr : reinterpret_cast<const QRgb*>(b.constScanLine(y));
    for (int x = 0; x < w; ++x)
      if ((ra && qAlpha(ra[x]) > 127) || (rb && qAlpha(rb[x]) > 127)) ++n;
  }
  return n;
}

struct GateRow {
  double coverage = 0.0;
  double width_lanes = 0.0;
  bool block = false;
};

// Measures one id with the real synchronous cluster. Coverage = pixels opaque (alpha > 0.5) in either layer.
GateRow measure_gate(int32_t id, const Layout& l) {
  GateRow r;
  const SplitIndicatorCluster* c = wds::ui::split_effect_indicator_cluster(make_key(id, l));
  if (!c || c->empty) return r;
  const double band_area = 12.0 * static_cast<double>(l.lane_px) * static_cast<double>(l.band_h);
  const long long px = count_opaque(c->alpha_layer, c->add_layer);
  r.coverage = band_area > 0.0 ? static_cast<double>(px) / band_area : 0.0;
  const int w = !c->alpha_layer.isNull() ? c->alpha_layer.width()
                : !c->add_layer.isNull() ? c->add_layer.width()
                                         : c->sub_layer.width();
  r.width_lanes = static_cast<double>(w) / static_cast<double>(l.lane_px);
  r.block = r.coverage > kMaxCoverage || r.width_lanes > kMaxWidthLanes;
  return r;
}

// ---- ribbon end gate ---------------------------------------------------------------------------------------------
// Measured on the strips exactly as the cluster build draws them (split_effect_indicator_debug_strips: cropped,
// ribbon-return-to-line applied, band-fitted, screen pixels):
//  * head_x / tail_x: offset of the first / last vertex from its split line in WORLD units; |.| must be <= 0.05.
//  * turn: the largest turning angle between consecutive segments (segments shorter than 0.05 px ignored) at vertices
//    whose arc-length position lies in the first or the last 20 % of the screen polyline; must be <= 100 degrees
//    (a larger turn is a sudden fold back to the line).
constexpr double kMaxEndWorld = 0.05;
constexpr double kMaxEndTurnDeg = 100.0;
constexpr double kEndZone = 0.20;

struct RibbonRow {
  int line = 0;
  std::size_t verts = 0;
  double head_x = 0.0;  // world units
  double tail_x = 0.0;
  double turn_deg = 0.0;
  bool fail = false;
};

RibbonRow measure_ribbon(const wds::ui::SplitIndicatorDebugStrip& s) {
  RibbonRow r;
  r.line = s.line;
  r.verts = s.x.size();
  if (s.x.size() < 2) return r;
  const double ppu = static_cast<double>(s.px_per_unit);
  r.head_x = (static_cast<double>(s.x.front()) - static_cast<double>(s.line_x)) / ppu;
  r.tail_x = (static_cast<double>(s.x.back()) - static_cast<double>(s.line_x)) / ppu;
  const std::size_t n = s.x.size();
  std::vector<double> cum(n, 0.0);
  for (std::size_t i = 1; i < n; ++i) {
    cum[i] = cum[i - 1] + std::hypot(static_cast<double>(s.x[i] - s.x[i - 1]), static_cast<double>(s.y[i] - s.y[i - 1]));
  }
  const double total = cum.back();
  for (std::size_t i = 1; i + 1 < n && total > 0.0; ++i) {
    const double pos = cum[i] / total;
    if (pos > kEndZone && pos < 1.0 - kEndZone) continue;
    const double ax = static_cast<double>(s.x[i] - s.x[i - 1]);
    const double ay = static_cast<double>(s.y[i] - s.y[i - 1]);
    const double bx = static_cast<double>(s.x[i + 1] - s.x[i]);
    const double by = static_cast<double>(s.y[i + 1] - s.y[i]);
    const double la = std::hypot(ax, ay);
    const double lb = std::hypot(bx, by);
    if (la < 0.05 || lb < 0.05) continue;
    const double c = std::clamp((ax * bx + ay * by) / (la * lb), -1.0, 1.0);
    r.turn_deg = std::max(r.turn_deg, std::acos(c) * 180.0 / 3.14159265358979323846);
  }
  r.fail = std::fabs(r.head_x) > kMaxEndWorld || std::fabs(r.tail_x) > kMaxEndWorld || r.turn_deg > kMaxEndTurnDeg;
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  try {
    const QString skins = QDir(QString::fromUtf8(WDS_REPO_ROOT)).filePath(QStringLiteral("skins"));
    wds::ui::ensure_split_effect_uv_atlas(skins);

    const QString dir = QString::fromUtf8(WDS_DUMP_DIR);
    QDir().mkpath(dir);

    const std::vector<int32_t> all = wds::ui::filter_split_picker_color_ids("");
    std::vector<int32_t> strip_ids;
    for (int32_t id : all)
      if (has_strip(id)) strip_ids.push_back(id);
    std::printf("split indicator dump: %zu picker ids, %zu with shape-5 strips\n", all.size(), strip_ids.size());

    const float editor_k = 40.0f / wds::ui::kSplitIndicatorRefLanePx;
    const Layout picker{"picker", picker_lane_px(), static_cast<int>(wds::ui::kSplitIndicatorRefBandPx),
                        wds::ui::kSplitIndicatorRefMinSpritePx, wds::ui::kSplitIndicatorSizeScale, 2};
    // Mirrors split_cluster_key() in chart_edit_widget.cpp: the editor halves BOTH size_scale and min_sprite_px, has
    // a lane-independent time-axis band (explicit 251 px, quantized like the widget) and shows
    // rank < kEditorIndicatorMaxRank.
    const Layout editor{"editor",
                        40.0f,
                        wds::ui::quantize_editor_indicator_band(251.0f),
                        wds::ui::kSplitIndicatorRefMinSpritePx * editor_k * wds::ui::kEditorIndicatorSizeFactor,
                        wds::ui::kSplitIndicatorSizeScale * wds::ui::kEditorIndicatorSizeFactor,
                        wds::ui::kEditorIndicatorMaxRank};

    std::string index;
    index += "split_count " + std::to_string(kSplitCount) + ", type Full, " + std::to_string(kCols) +
             " columns, tiles in grid order (row-major). Tile = 12 lanes + " + std::to_string(kMarginX) +
             "px side margin, band + " + std::to_string(kMarginY) + "px above/below (clipped to the tile).\n";
    char hb[320];
    std::snprintf(hb, sizeof hb,
                  "picker: lane_px %.4f band %d min_sprite %.3f size_scale %.3f max_rank %d | editor: lane_px %.4f band "
                  "%d min_sprite %.3f size_scale %.3f max_rank %d\n\n",
                  picker.lane_px, picker.band_h, picker.min_sprite, picker.size_scale, picker.max_rank, editor.lane_px,
                  editor.band_h, editor.min_sprite, editor.size_scale, editor.max_rank);
    index += hb;

    std::vector<std::string> files;
    for (const Layout* l : {&picker, &editor}) {
      int sheet = 0;
      for (std::size_t off = 0; off < strip_ids.size(); off += kPerSheet, ++sheet) {
        const std::size_t end = std::min(strip_ids.size(), off + kPerSheet);
        const std::vector<int32_t> chunk(strip_ids.begin() + static_cast<std::ptrdiff_t>(off),
                                         strip_ids.begin() + static_cast<std::ptrdiff_t>(end));
        std::string file;
        index += write_sheet(dir, l == &picker ? "dump_picker" : "dump_editor", sheet, chunk, *l, true, file) + "\n";
        files.push_back(file);
      }
    }

    {
      // Ribbon head / tail landing: red = the split lines, yellow dot = strip head (vertex 0), cyan dot = strip tail.
      const std::vector<int32_t> ribbon_ids = {10820, 11230, 11791, 10650, 2010, 10380, 10630, 11320, 10660, 10770};
      index += "\nribbon_ends sheets (red = split lines, yellow = head, cyan = tail):\n";
      for (const Layout* l : {&picker, &editor}) {
        const std::string prefix = std::string("dump_ribbon_ends_") + l->name;
        std::string file;
        index += write_sheet(dir, prefix.c_str(), 0, ribbon_ids, *l, true, file, Bg::Plate, true) + "\n";
        files.push_back(file);
      }
    }

    {
      const std::vector<int32_t> controls ={2020, 11621, 10120, 11803, 10660, 10770, 10380, 10630, 10620};
      std::string file;
      index += write_sheet(dir, "dump_control", 0, controls, picker, true, file) + "\n";
      files.push_back(file);
    }

    {
      // Subtractive (cut-out) particles: butterfly kinds 627 (id 10620) and 980 (id 11070) have a negative baked
      // tint. Drawn over a mid-grey striped scene (the hole in the stripes is the cut-out) and over the dark plate,
      // at picker size and at the editor's halved size.
      const std::vector<int32_t> sub_ids = {10620, 11070};
      index += "\nsubtractive sheets (ids 10620, 11070; stripes = mid-grey scene, plate = dark):\n";
      for (const Layout* l : {&picker, &editor}) {
        for (Bg bg : {Bg::GreyStripes, Bg::Plate}) {
          const std::string prefix = std::string("dump_subtractive_") + (bg == Bg::GreyStripes ? "grey_" : "plate_") + l->name;
          std::string file;
          index += write_sheet(dir, prefix.c_str(), 0, sub_ids, *l, true, file, bg) + "\n";
          files.push_back(file);
        }
        for (int32_t id : sub_ids) {
          const SplitIndicatorCluster* c = wds::ui::split_effect_indicator_cluster(make_key(id, *l));
          char sb[200];
          std::snprintf(sb, sizeof sb, "  %s id %d: alpha %d add %d sub %d (1 = layer present)\n", l->name, id,
                        c && !c->alpha_layer.isNull() ? 1 : 0, c && !c->add_layer.isNull() ? 1 : 0,
                        c && !c->sub_layer.isNull() ? 1 : 0);
          index += sb;
        }
      }
    }

    index += "\nstrip entries per id (world units; dy extent includes half widths):\n";
    for (int32_t id : strip_ids) {
      index += "id " + std::to_string(id) + "\n";
      describe_strips(id, index);
    }
    index += "\n(band-fit dy compression factor not computed: needs private snapshot internals)\n";

    auto write_text = [&](const char* name, const std::string& body) {
      FILE* f = std::fopen(QDir(dir).filePath(QString::fromUtf8(name)).toUtf8().constData(), "wb");
      if (!f) {
        std::fprintf(stderr, "WARN: could not write %s\n", name);
        return;
      }
      std::fwrite(body.data(), 1, body.size(), f);
      std::fclose(f);
      files.push_back(name);
    };
    write_text("index.txt", index);

    // strips.tsv: raw vertices of every strip (all ids that have one).
    std::string tsv = "id\tline\tkind\tvert\tdx\tdy\twidth\tu\tr\tg\tb\ta\n";
    const SplitSnapStripPoint* pool = wds::ui::split_snap_strip_points();
    for (int32_t id : strip_ids) {
      for (int line = 0; line <= kSplitCount; ++line) {
        int n = 0;
        const SplitSnapParticle* ps = wds::ui::split_snap_particles(id, kSplitCount, line, &n);
        for (int i = 0; ps && i < n; ++i) {
          if (ps[i].shape != 5) continue;
          for (uint32_t k = 0; k < ps[i].strip_count; ++k) {
            const SplitSnapStripPoint& v = pool[ps[i].strip_first + k];
            char buf[256];
            std::snprintf(buf, sizeof buf, "%d\t%d\t%d\t%u\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t%g\n", id, line,
                          ps[i].catalog_index, k, v.dx, v.dy, v.width, v.u, v.r, v.g, v.b, v.a);
            tsv += buf;
          }
        }
      }
    }
    write_text("strips.tsv", tsv);

    std::printf("output dir: %s\n", dir.toUtf8().constData());
    for (const std::string& f : files) std::printf("  %s\n", f.c_str());

    // Regression gate (see file header).
    int blocked = 0;
    std::printf("\nstrip gate: coverage = (px with alpha>0.5 in add or alpha layer) / (12 lanes x band), BLOCK if > %.2f or "
                "bbox width > %.0f lanes\n", kMaxCoverage, kMaxWidthLanes);
    std::printf("%-7s %-7s %9s %11s  %s\n", "id", "layout", "coverage", "width[lane]", "status");
    for (const Layout* l : {&picker, &editor}) {
      for (int32_t id : strip_ids) {
        const GateRow g = measure_gate(id, *l);
        if (g.block) ++blocked;
        std::printf("%-7d %-7s %9.3f %11.2f  %s\n", id, l->name, g.coverage, g.width_lanes, g.block ? "BLOCK" : "ok");
      }
    }
    // Ribbon end gate (see measure_ribbon): every strip, after the transform, at picker and editor size.
    int ribbon_fail = 0;
    std::size_t ribbon_total = 0;
    std::printf("\nribbon end gate: |head x|, |tail x| <= %.2f world units; max turn in first/last %.0f%% <= %.0f deg\n",
                kMaxEndWorld, kEndZone * 100.0, kMaxEndTurnDeg);
    std::printf("%-7s %-7s %4s %6s %9s %9s %9s  %s\n", "id", "layout", "line", "verts", "head_x", "tail_x", "turn[deg]",
                "status");
    for (const Layout* l : {&picker, &editor}) {
      for (int32_t id : strip_ids) {
        for (const auto& s : wds::ui::split_effect_indicator_debug_strips(make_key(id, *l))) {
          const RibbonRow r = measure_ribbon(s);
          ++ribbon_total;
          if (r.fail) ++ribbon_fail;
          std::printf("%-7d %-7s %4d %6zu %9.4f %9.4f %9.1f  %s\n", id, l->name, r.line, r.verts, r.head_x, r.tail_x,
                      r.turn_deg, r.fail ? "FAIL" : "ok");
        }
      }
    }
    if (blocked > 0 || ribbon_fail > 0) {
      if (blocked > 0) std::fprintf(stderr, "FAIL: %d strip id/layout combination(s) flagged BLOCK\n", blocked);
      if (ribbon_fail > 0) {
        std::fprintf(stderr, "FAIL: %d of %zu strips fail the ribbon end gate\n", ribbon_fail, ribbon_total);
      }
      return 2;
    }
    std::printf("strip gate: all %zu ids ok at picker and editor size; ribbon end gate: all %zu strips ok\n",
                strip_ids.size(), ribbon_total);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: exception while rendering: %s\n", e.what());
    return 1;
  }
  return 0;
}
