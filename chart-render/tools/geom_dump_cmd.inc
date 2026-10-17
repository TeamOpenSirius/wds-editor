// live dump geom-v1: native drawn-geometry stats for BakeMesh compare.
// Uses StageGeometry + playback_preview Cross/kettlewell. Not 2022.3.
// Include stage_geometry.hpp at file scope in the TU, not here.
using StageGeom = wds::renderer::StageGeometry;
using StageBounds2 = wds::renderer::ScreenBounds;
using StageVec2 = wds::renderer::Vec2;
using StagePreviewCfg = wds::renderer::PreviewVisualConfig;

struct GeomQuantiles {
  float p10 = 0;
  float p50 = 0;
  float p90 = 0;
  float minv = 0;
  float maxv = 0;
};

GeomQuantiles geom_quantiles(std::vector<float> xs) {
  GeomQuantiles q;
  if (xs.empty()) {
    return q;
  }
  std::sort(xs.begin(), xs.end());
  auto at = [&](float t) {
    const float idx = t * static_cast<float>(xs.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, xs.size() - 1);
    const float u = idx - static_cast<float>(lo);
    return xs[lo] * (1.0f - u) + xs[hi] * u;
  };
  q.minv = xs.front();
  q.maxv = xs.back();
  q.p10 = at(0.1f);
  q.p50 = at(0.5f);
  q.p90 = at(0.9f);
  return q;
}

float geom_dist3(float ax, float ay, float az, float bx, float by, float bz) {
  const float dx = ax - bx;
  const float dy = ay - by;
  const float dz = az - bz;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float geom_tri_area_px(float ax, float ay, float bx, float by, float cx, float cy) {
  return 0.5f * std::fabs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
}

struct GeomSysAcc {
  int n_particles = 0;
  int n_body_drawn = 0;
  int n_body_skip = 0;
  int vertex_count = 0;
  double area_px = 0;
  double alpha_sum = 0;
  float u_min = 1e9f;
  float u_max = -1e9f;
  float v_min = 1e9f;
  float v_max = -1e9f;
  std::vector<float> along;
  std::vector<float> across;
  std::vector<float> widths;
  std::vector<float> seglens;
  std::vector<float> centroid_off;
  int ribbon_chains = 0;
  int ribbon_pairs = 0;
  uint8_t render_mode = 0;
  uint8_t trails = 0;
  float maxp = 0;
};

void geom_note_uv(GeomSysAcc& a, float u, float v) {
  a.u_min = std::min(a.u_min, u);
  a.u_max = std::max(a.u_max, u);
  a.v_min = std::min(a.v_min, v);
  a.v_max = std::max(a.v_max, v);
}

void geom_add_quad_world(GeomSysAcc& acc, const float* qx, const float* qy, const float* qz,
                         float alpha, const StageGeom& geom, int fb_w,
                         int fb_h, float px, float py, float pz, bool have_pos) {
  acc.vertex_count += 4;
  acc.alpha_sum += 4.0 * static_cast<double>(alpha);
  geom_note_uv(acc, 0.0f, 0.0f);
  geom_note_uv(acc, 1.0f, 1.0f);
  const float e01 = geom_dist3(qx[0], qy[0], qz[0], qx[1], qy[1], qz[1]);
  const float e12 = geom_dist3(qx[1], qy[1], qz[1], qx[2], qy[2], qz[2]);
  const float e23 = geom_dist3(qx[2], qy[2], qz[2], qx[3], qy[3], qz[3]);
  const float e30 = geom_dist3(qx[3], qy[3], qz[3], qx[0], qy[0], qz[0]);
  float along = 0.5f * (e01 + e23);
  float across = 0.5f * (e12 + e30);
  if (along < across) {
    const float tmp = along;
    along = across;
    across = tmp;
  }
  acc.along.push_back(along);
  acc.across.push_back(across);
  const float cx = 0.25f * (qx[0] + qx[1] + qx[2] + qx[3]);
  const float cy = 0.25f * (qy[0] + qy[1] + qy[2] + qy[3]);
  const float cz = 0.25f * (qz[0] + qz[1] + qz[2] + qz[3]);
  if (have_pos) {
    acc.centroid_off.push_back(geom_dist3(cx, cy, cz, px, py, pz));
  }
  const StageBounds2& c = geom.content();
  auto to_px = [&](float wx, float wy, float wz, float& ox, float& oy) {
    const StageVec2 ndc = geom.project_main(wx, wy, wz);
    ox = (ndc.x - c.l) / std::max(c.w, 1e-6f) * static_cast<float>(fb_w);
    oy = (ndc.y - c.b) / std::max(c.h, 1e-6f) * static_cast<float>(fb_h);
  };
  float sx[4], sy[4];
  for (int i = 0; i < 4; ++i) {
    to_px(qx[i], qy[i], qz[i], sx[i], sy[i]);
  }
  acc.area_px += geom_tri_area_px(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
  acc.area_px += geom_tri_area_px(sx[0], sy[0], sx[2], sy[2], sx[3], sy[3]);
}

bool geom_skip_body(const SplitEffectLiveParticle& p) {
  const bool unnamed = p.uv_name_index == ::wds::chart_render::kSplitEffectNoUvName;
  const bool cube = (p.flags & ::wds::chart_render::kSplitEffectFlagCubeMesh) != 0;
  const bool ring = (p.flags & ::wds::chart_render::kSplitEffectFlagRingMesh) != 0;
  const bool unnamed_add_alpha_trails =
      unnamed && !cube && !ring &&
      (p.flags & ::wds::chart_render::kSplitEffectFlagTrails) != 0 &&
      p.blend != static_cast<uint8_t>(::wds::chart_render::SplitEffectBlend::Other);
  if (unnamed_add_alpha_trails) {
    return true;
  }
  if (p.a < 0.001f) {
    return true;
  }
  if (p.remaining_lifetime > 1e-5f) {
    return false;
  }
  if ((p.flags & ::wds::chart_render::kSplitEffectFlagTrails) == 0) {
    return false;
  }
  return p.trail_count < 2 && p.trail_points >= 2;
}

void geom_emit_body(GeomSysAcc& acc, const SplitEffectLiveParticle& p,
                    StageGeom& geom, int fb_w, int fb_h) {
  if (!::wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z)) {
    return;
  }
  if (geom_skip_body(p)) {
    acc.n_body_skip += 1;
    return;
  }
  const bool mesh_render = p.render_mode == ::wds::chart_render::kSplitEffectRenderMesh;
  const bool stretch_render = p.render_mode == ::wds::chart_render::kSplitEffectRenderStretch;
  float size_unity = p.size;
  if (!mesh_render && !stretch_render) {
    // live dump n7 + U1-6 sprite width-norm. Not 2022.3.
    const float view_z = ::wds::chart_editor::official_main_camera_z(p.main_y, p.z);
    const float aspect = std::fabs(geom.content().w) / std::max(std::fabs(geom.content().h), 1e-6f);
    const float tan_half = 1.0f / std::max(::wds::chart_editor::official_camera_focal(), 1e-6f);
    size_unity = ::wds::chart_render::split_effect_billboard_draw_width(
        p.current_size, p.size, p.max_particle_size, view_z, tan_half, aspect, p.render_mode);
  }
  if (!mesh_render && size_unity <= 1e-6f) {
    acc.n_body_skip += 1;
    return;
  }
  acc.n_body_drawn += 1;
  float qx[4], qy[4], qz[4];
  bool ok = false;
  if (stretch_render) {
    const float along = ::wds::chart_render::official_split_effect_stretch_height(
        size_unity, p.size > 1e-8f ? p.size_y * (size_unity / p.size) : p.size_y, p.size_3d);
    float svx = p.total_vx;
    float svy = p.total_vy;
    float svz = p.total_vz;
    if (svx * svx + svy * svy + svz * svz <= 1e-20f) {
      svx = p.vel_x;
      svy = p.vel_y;
      svz = p.vel_z;
    }
    const float applied_ls = ::wds::chart_render::official_split_effect_stretch_applied_length_scale(
        p.stored_vx, p.stored_vy, p.stored_vz, p.total_vx, p.total_vy, p.total_vz, p.length_scale,
        p.effect_id, p.render_mode, p.uv_name_index);
    ok = geom.split_particle_stretch_world_corners(
        p.world_x, p.main_y, p.z, svx, svy, svz, size_unity, along, p.pivot_y, p.roll, p.pivot_x,
        qx, qy, qz, applied_ls, p.velocity_scale, p.camera_velocity_scale, 0.0f, 0.0f, 0.0f,
        p.freeform_stretching, p.rotate_with_stretch);
    if (!ok) {
      ok = geom.split_particle_oriented_world_corners(
          p.world_x, p.main_y, std::max(size_unity, 1e-4f),
          along / std::max(size_unity, 1e-4f), p.pivot_y, p.roll, 0.0f, 0.0f, p.z, 0, p.pivot_x,
          0.0f, qx, qy, qz);
    }
  } else if (!mesh_render) {
    ok = geom.split_particle_oriented_world_corners(
        p.world_x, p.main_y, size_unity, p.sol_y, p.pivot_y, p.roll, p.tilt_x, p.tilt_y, p.z,
        p.render_alignment, p.pivot_x, p.pivot_z, qx, qy, qz);
  }
  if (ok) {
    geom_add_quad_world(acc, qx, qy, qz, p.a, geom, fb_w, fb_h, p.world_x, p.main_y, p.z, true);
  }
}

void geom_emit_ribbon(GeomSysAcc& acc, const std::vector<const SplitEffectLiveParticle*>& chain,
                      StageGeom& geom, int fb_w, int fb_h) {
  const int n = static_cast<int>(chain.size());
  if (n < 2) {
    return;
  }
  acc.ribbon_chains += 1;
  acc.ribbon_pairs += n;
  acc.vertex_count += n * 2;
  const float tilt = ::wds::chart_editor::official_deg_to_rad(::wds::chart_editor::kOfficialLaneTiltDeg);
  const float cam_fy = std::sin(tilt);
  const float cam_fz = std::cos(tilt);
  const float cam_fx = 0.0f;
  std::vector<float> lx(static_cast<size_t>(n));
  std::vector<float> ly(static_cast<size_t>(n));
  std::vector<float> lz(static_cast<size_t>(n));
  std::vector<float> rx(static_cast<size_t>(n));
  std::vector<float> ry(static_cast<size_t>(n));
  std::vector<float> rz(static_cast<size_t>(n));
  std::vector<float> along_v(static_cast<size_t>(n), 0.0f);
  std::vector<float> seglen(static_cast<size_t>(std::max(n - 1, 0)), 0.0f);
  float prev_px = 0, prev_py = 0, prev_pz = 0;
  float arc = 0.0f;
  for (int k = 0; k < n; ++k) {
    const auto& a = *chain[static_cast<size_t>(k)];
    float dx = 0.0f, dy = 1.0f, dz = 0.0f;
    if (k + 1 < n && k > 0) {
      dx = chain[static_cast<size_t>(k + 1)]->world_x - chain[static_cast<size_t>(k - 1)]->world_x;
      dy = chain[static_cast<size_t>(k + 1)]->main_y - chain[static_cast<size_t>(k - 1)]->main_y;
      dz = chain[static_cast<size_t>(k + 1)]->z - chain[static_cast<size_t>(k - 1)]->z;
    } else if (k + 1 < n) {
      dx = chain[static_cast<size_t>(k + 1)]->world_x - a.world_x;
      dy = chain[static_cast<size_t>(k + 1)]->main_y - a.main_y;
      dz = chain[static_cast<size_t>(k + 1)]->z - a.z;
    } else {
      dx = a.world_x - chain[static_cast<size_t>(k - 1)]->world_x;
      dy = a.main_y - chain[static_cast<size_t>(k - 1)]->main_y;
      dz = a.z - chain[static_cast<size_t>(k - 1)]->z;
    }
    float px = cam_fy * dz - cam_fz * dy;
    float py = cam_fz * dx - cam_fx * dz;
    float pz = cam_fx * dy - cam_fy * dx;
    const float plen = std::sqrt(px * px + py * py + pz * pz);
    if (plen < 1e-8f) {
      px = 1.0f;
      py = 0.0f;
      pz = 0.0f;
    } else {
      px /= plen;
      py /= plen;
      pz /= plen;
    }
    if (k > 0 && (prev_px * px + prev_py * py + prev_pz * pz) < 0.0f) {
      px = -px;
      py = -py;
      pz = -pz;
    }
    prev_px = px;
    prev_py = py;
    prev_pz = pz;
    const float w = a.trail_width;
    const float h = w * 0.5f;
    acc.widths.push_back(w);
    acc.alpha_sum += 2.0 * static_cast<double>(a.trail_a);
    lx[static_cast<size_t>(k)] = a.world_x + px * h;
    ly[static_cast<size_t>(k)] = a.main_y + py * h;
    lz[static_cast<size_t>(k)] = a.z + pz * h;
    rx[static_cast<size_t>(k)] = a.world_x - px * h;
    ry[static_cast<size_t>(k)] = a.main_y - py * h;
    rz[static_cast<size_t>(k)] = a.z - pz * h;
    if (k > 0) {
      const auto& b = *chain[static_cast<size_t>(k - 1)];
      const float step = geom_dist3(b.world_x, b.main_y, b.z, a.world_x, a.main_y, a.z);
      seglen[static_cast<size_t>(k - 1)] = step;
      acc.seglens.push_back(step);
      arc += step;
    }
  }
  // Ribbon Stretch UV: 3D arc / total, 0 = oldest. align2-n6 hop, not 2022.3.
  if (arc > 1e-8f) {
    float acc_arc = 0.0f;
    along_v[0] = 0.0f;
    for (int k = 1; k < n; ++k) {
      acc_arc += seglen[static_cast<size_t>(k - 1)];
      along_v[static_cast<size_t>(k)] = acc_arc / arc;
    }
  }
  for (int k = 0; k < n; ++k) {
    geom_note_uv(acc, 0.0f, along_v[static_cast<size_t>(k)]);
    geom_note_uv(acc, 1.0f, along_v[static_cast<size_t>(k)]);
  }
  const StageBounds2& c = geom.content();
  auto to_px = [&](float wx, float wy, float wz, float& ox, float& oy) {
    const StageVec2 ndc = geom.project_main(wx, wy, wz);
    ox = (ndc.x - c.l) / std::max(c.w, 1e-6f) * static_cast<float>(fb_w);
    oy = (ndc.y - c.b) / std::max(c.h, 1e-6f) * static_cast<float>(fb_h);
  };
  for (int k = 0; k + 1 < n; ++k) {
    const float w0 = chain[static_cast<size_t>(k)]->trail_width;
    const float w1 = chain[static_cast<size_t>(k + 1)]->trail_width;
    if (w0 < 1e-5f && w1 < 1e-5f) {
      continue;
    }
    float sx[4], sy[4];
    to_px(lx[static_cast<size_t>(k)], ly[static_cast<size_t>(k)], lz[static_cast<size_t>(k)], sx[0],
          sy[0]);
    to_px(rx[static_cast<size_t>(k)], ry[static_cast<size_t>(k)], rz[static_cast<size_t>(k)], sx[1],
          sy[1]);
    to_px(rx[static_cast<size_t>(k + 1)], ry[static_cast<size_t>(k + 1)], rz[static_cast<size_t>(k + 1)],
          sx[2], sy[2]);
    to_px(lx[static_cast<size_t>(k + 1)], ly[static_cast<size_t>(k + 1)], lz[static_cast<size_t>(k + 1)],
          sx[3], sy[3]);
    acc.area_px += geom_tri_area_px(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
    acc.area_px += geom_tri_area_px(sx[0], sy[0], sx[2], sy[2], sx[3], sy[3]);
  }
}

void geom_write_q(FILE* f, const char* name, const GeomQuantiles& q, bool comma) {
  std::fprintf(f, "%s\"%s\":{\"p10\":%.6g,\"p50\":%.6g,\"p90\":%.6g,\"min\":%.6g,\"max\":%.6g}",
               comma ? "," : "", name, q.p10, q.p50, q.p90, q.minv, q.maxv);
}

int cmd_geom_dump(const std::vector<int32_t>& effects, double now_t, const std::string& out_path,
                  int32_t line_index, int fb_w, int fb_h) {
  if (effects.empty()) {
    std::fprintf(stderr, "geom-dump requires --effect ID[,ID...]\n");
    return 1;
  }
  if (out_path.empty()) {
    std::fprintf(stderr, "geom-dump requires --out FILE.json\n");
    return 1;
  }
  StageGeom geom;
  StagePreviewCfg cfg;
  geom.configure(cfg);
  geom.resize(fb_w, fb_h);
  FILE* f = std::fopen(out_path.c_str(), "w");
  if (f == nullptr) {
    std::fprintf(stderr, "geom-dump cannot write %s\n", out_path.c_str());
    return 1;
  }
  std::fprintf(f,
               "{\"corpus\":\"geom-v1\",\"now_t\":%.6g,\"line_index\":%d,\"pixelW\":%d,\"pixelH\":%d,"
               "\"effects\":[",
               now_t, line_index, fb_w, fb_h);
  bool first_e = true;
  for (int32_t eid : effects) {
    if (::wds::chart_render::official_split_effect(eid) == nullptr) {
      std::fprintf(stderr, "unknown effect id %d\n", eid);
      std::fclose(f);
      return 1;
    }
    prehydrate_golden_controllers(eid);
    ::wds::chart_render::reset_split_effect_auto_seed_session();
    SplitEffectFrameCollect io;
    SplitEffectPlaybackState state;
    const std::vector<PreviewSplitLaneInstance> splits{
        make_split(eid, 6, SplitLaneType::Full, kDefaultStartMs)};
    const double now_sec = now_sec_from_now_t(kDefaultStartMs, now_t);
    ::wds::chart_render::collect_split_effect_frame(splits, 0, now_sec, false, state, io);
    std::map<uint32_t, GeomSysAcc> by;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<const SplitEffectLiveParticle*>> ribbons;
    for (size_t s = 0; s < io.split_count.size(); ++s) {
      const int begin = io.split_begin[s];
      const int count = io.split_count[s];
      for (int i = 0; i < count; ++i) {
        const auto& p = io.pool[static_cast<size_t>(begin + i)];
        if (p.effect_id != eid || p.line_index != line_index) {
          continue;
        }
        GeomSysAcc& acc = by[p.catalog_index];
        acc.n_particles += 1;
        acc.render_mode = p.render_mode;
        acc.trails = static_cast<uint8_t>((p.flags & ::wds::chart_render::kSplitEffectFlagTrails) != 0);
        acc.maxp = p.max_particle_size;
        geom_emit_body(acc, p, geom, fb_w, fb_h);
        if (p.trail_count >= 2) {
          ribbons[std::make_pair(p.catalog_index, p.trail_chain)].push_back(&p);
        }
      }
    }
    for (auto& kv : ribbons) {
      auto& chain = kv.second;
      std::sort(chain.begin(), chain.end(), [](const SplitEffectLiveParticle* a,
                                               const SplitEffectLiveParticle* b) {
        return a->trail_vertex < b->trail_vertex;
      });
      geom_emit_ribbon(by[kv.first.first], chain, geom, fb_w, fb_h);
    }
    if (!first_e) {
      std::fputc(',', f);
    }
    first_e = false;
    std::fprintf(f, "{\"effect\":%d,\"systems\":[", eid);
    bool first_s = true;
    for (auto& kv : by) {
      GeomSysAcc& a = kv.second;
      if (!first_s) {
        std::fputc(',', f);
      }
      first_s = false;
      const GeomQuantiles al = geom_quantiles(a.along);
      const GeomQuantiles ac = geom_quantiles(a.across);
      const GeomQuantiles wd = geom_quantiles(a.widths);
      const GeomQuantiles sg = geom_quantiles(a.seglens);
      const GeomQuantiles co = geom_quantiles(a.centroid_off);
      const float area_frac =
          (fb_w > 0 && fb_h > 0)
              ? static_cast<float>(a.area_px / (static_cast<double>(fb_w) * static_cast<double>(fb_h)))
              : 0.0f;
      std::fprintf(f,
                   "{\"catalog_index\":%u,\"n\":%d,\"n_body_drawn\":%d,\"n_body_skip\":%d,"
                   "\"vertexCount\":%d,\"areaPx\":%.6g,\"areaFrac\":%.6g,\"alphaSum\":%.6g,"
                   "\"renderMode\":%u,\"trails\":%u,\"maxP\":%.6g,\"ribbonChains\":%d,"
                   "\"ribbonPairs\":%d,\"uvUMin\":%.6g,\"uvUMax\":%.6g,\"uvVMin\":%.6g,\"uvVMax\":%.6g",
                   kv.first, a.n_particles, a.n_body_drawn, a.n_body_skip, a.vertex_count, a.area_px,
                   area_frac, a.alpha_sum, a.render_mode, a.trails, a.maxp, a.ribbon_chains,
                   a.ribbon_pairs, a.u_min > 1e8f ? 0.0f : a.u_min, a.u_max < -1e8f ? 0.0f : a.u_max,
                   a.v_min > 1e8f ? 0.0f : a.v_min, a.v_max < -1e8f ? 0.0f : a.v_max);
      geom_write_q(f, "quadAlong", al, true);
      geom_write_q(f, "quadAcross", ac, true);
      geom_write_q(f, "width", wd, true);
      geom_write_q(f, "segAlong", sg, true);
      geom_write_q(f, "centroidOff", co, true);
      std::fputc('}', f);
    }
    std::fprintf(f, "]}");
  }
  std::fprintf(f, "]}\n");
  std::fclose(f);
  std::fprintf(stdout, "geom-dump %s effects=%zu now_t=%.5f\n", out_path.c_str(), effects.size(),
               now_t);
  return 0;
}
