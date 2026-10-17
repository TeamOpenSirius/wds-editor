#include "internal.hpp"

#include <algorithm>
#include <cmath>

namespace wds::chart_render::shuriken {
namespace {

void rotate_euler_deg(float rx, float ry, float rz, float& x, float& y, float& z) noexcept {
  if (std::fabs(rx) + std::fabs(ry) + std::fabs(rz) < 1e-8f) {
    return;
  }
  const float ax = rx * kDegToRad;
  const float ay = ry * kDegToRad;
  const float az = rz * kDegToRad;
  const float cx = std::cos(ax), sx = std::sin(ax);
  const float cy = std::cos(ay), sy = std::sin(ay);
  const float cz = std::cos(az), sz = std::sin(az);
  // Shape.rotation composed as Quaternion.Euler: z, then x, then y
  // (https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Quaternion.Euler.html).
  // ShapeModule.rotation page does not state the order. live dump refix-d
  // iso_euler_x0z (40,0,70) Circle plane matches this, not Z*Y*X. hop, not
  // a 2022.3 Shape official formula. Only this helper; PS localRotation
  // stays catalog quat / axis-angle.
  float x1 = x * cz - y * sz;
  float y1 = x * sz + y * cz;
  float y2 = y1 * cx - z * sx;
  float z2 = y1 * sx + z * cx;
  x = x1 * cy + z2 * sy;
  y = y2;
  z = -x1 * sy + z2 * cy;
}

void normalize(float& x, float& y, float& z) noexcept {
  const float len = std::sqrt(x * x + y * y + z * z);
  if (len <= 1e-8f) {
    x = 0.0f;
    y = 1.0f;
    z = 0.0f;
    return;
  }
  x /= len;
  y /= len;
  z /= len;
}

// Unity 2022.3 Quaternion * Vector3
// (ScriptReference/Quaternion-operator_multiply.html): rotates the point.
void rotate_by_quat(float qx, float qy, float qz, float qw, float& x, float& y,
                    float& z) noexcept {
  const float n2 = qx * qx + qy * qy + qz * qz + qw * qw;
  if (n2 <= 1e-16f) {
    return;
  }
  if (std::fabs(n2 - 1.0f) > 1e-6f) {
    const float inv = 1.0f / std::sqrt(n2);
    qx *= inv;
    qy *= inv;
    qz *= inv;
    qw *= inv;
  }
  const float tx = 2.0f * (qy * z - qz * y);
  const float ty = 2.0f * (qz * x - qx * z);
  const float tz = 2.0f * (qx * y - qy * x);
  x += qw * tx + (qy * tz - qz * ty);
  y += qw * ty + (qz * tx - qx * tz);
  z += qw * tz + (qx * ty - qy * tx);
}

void rotate_axis_angle_deg(float ax, float ay, float az, float angle_deg, float& x, float& y,
                           float& z) noexcept {
  if (std::fabs(angle_deg) < 1e-8f) {
    return;
  }
  float alen = std::sqrt(ax * ax + ay * ay + az * az);
  if (alen <= 1e-8f) {
    return;
  }
  ax /= alen;
  ay /= alen;
  az /= alen;
  const float ang = angle_deg * kDegToRad;
  const float c = std::cos(ang);
  const float s = std::sin(ang);
  const float dot = ax * x + ay * y + az * z;
  const float cx = ay * z - az * y;
  const float cy = az * x - ax * z;
  const float cz = ax * y - ay * x;
  x = x * c + cx * s + ax * dot * (1.0f - c);
  y = y * c + cy * s + ay * dot * (1.0f - c);
  z = z * c + cz * s + az * dot * (1.0f - c);
}

void rotate_ps_local(const SplitEffectParticle& p, float& x, float& y, float& z) noexcept {
  // catalog.json transform.localRotation: quat is the usable rotation;
  // angleDeg+axis is the extract's axis-angle of that same quat.
  const float qn = p.local_quat_x * p.local_quat_x + p.local_quat_y * p.local_quat_y +
                   p.local_quat_z * p.local_quat_z + p.local_quat_w * p.local_quat_w;
  if (qn > 1e-16f) {
    rotate_by_quat(p.local_quat_x, p.local_quat_y, p.local_quat_z, p.local_quat_w, x, y, z);
  } else {
    rotate_axis_angle_deg(p.local_axis_x, p.local_axis_y, p.local_axis_z, p.local_angle_deg, x, y,
                          z);
  }
}

void apply_ps_local_transform(const SplitEffectParticle& p, float& px, float& py, float& pz,
                              float& dx, float& dy, float& dz) noexcept {
  // Unity 2022.3 Transform on the ParticleSystem GameObject. Output stays
  // PS-local (parent Line is ignored here). Scaling Mode Local applies this
  // node's scale, not the parent Line 0.1
  // (Manual/PartSysMainModule.html Scaling Mode).
  // TransformPoint: rotation * (scale * point) + position
  // (ScriptReference/Transform.TransformPoint.html,
  //  ScriptReference/Matrix4x4.TRS.html). Not Facing.
  px *= p.local_scale_x;
  py *= p.local_scale_y;
  pz *= p.local_scale_z;
  rotate_ps_local(p, px, py, pz);
  px += p.local_pos_x;
  py += p.local_pos_y;
  pz += p.local_pos_z;
  // TransformDirection: rotation only — not scale or position
  // (ScriptReference/Transform.TransformDirection.html).
  rotate_ps_local(p, dx, dy, dz);
  normalize(dx, dy, dz);
}

void slerp_dir(float& dx, float& dy, float& dz, float tx, float ty, float tz, float amt) noexcept {
  if (amt <= 1e-6f) {
    return;
  }
  dx += (tx - dx) * amt;
  dy += (ty - dy) * amt;
  dz += (tz - dz) * amt;
  normalize(dx, dy, dz);
}

void unit_from_seed(uint32_t seed, uint32_t extra, uint32_t salt, float& x, float& y,
                    float& z) noexcept {
  // Marsaglia sphere. 2022.3 Shape Sphere: "Uniform particle emission in all
  // directions" — not the sampling method. Nearby mix3 salts were not
  // independent (same collapse as cube-reject 20/21/22). hop: stream-mix3
  // + Marsaglia.
  uint32_t rng = rng_start(seed, extra, salt);
  const float u = rng_next_u01(rng, seed, extra) * 2.0f - 1.0f;
  const float t = rng_next_u01(rng, seed, extra) * 2.0f * kPi;
  const float s = std::sqrt(std::max(0.0f, 1.0f - u * u));
  x = s * std::cos(t);
  y = s * std::sin(t);
  z = u;
}

float wrap_unit(float x) noexcept {
  x = std::fmod(x, 1.0f);
  return x < 0.0f ? x + 1.0f : x;
}

// live dump dump-pending-v1 hop (10170 / 11423_1), not 2022.3: arcSpeed Curve
// traveled(t) = 2π × ∫_0^t scalar×curve(system age/Duration) dτ. t is the
// same birth_t clock as the scalar hop. Curve wrap matches system_emit_u.
// Closed-form Hermite integral. Scalar-only path stays speed×t×2π.
float arc_speed_traveled(const SplitEffectParticle& p, float birth_t) noexcept {
  const float speed = p.shape_arc_speed;
  const float t = std::max(birth_t, 0.0f);
  if (p.shape_arc_speed_count == 0) {
    return speed * t * 2.0f * kPi;
  }
  const float delay = start_delay_wall_clock(p);
  const float sim = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  const bool looping = (p.flags & kSplitEffectFlagLooping) != 0;
  auto curve_at = [&](float u01) noexcept {
    return official_split_effect_eval_curve_keys(p.shape_arc_speed_begin, p.shape_arc_speed_count,
                                                 u01, speed);
  };
  auto integ0 = [&](float u01) noexcept {
    return official_split_effect_integrate_curve_keys(p.shape_arc_speed_begin,
                                                      p.shape_arc_speed_count, u01, speed);
  };
  float acc = 0.0f;
  const float delay_span = std::min(t, delay);
  if (delay_span > 0.0f) {
    acc += delay_span * curve_at(0.0f);
  }
  if (t <= delay) {
    return acc * 2.0f * kPi;
  }
  const float sigma = (t - delay) * sim;
  const float i_unit = integ0(1.0f);
  if (looping) {
    float rem = std::fmod(sigma, length);
    if (rem < 0.0f) {
      rem += length;
    }
    const float loops = std::floor(sigma / length);
    acc += (length / sim) * (loops * i_unit + integ0(rem / length));
  } else if (sigma <= length) {
    acc += (length / sim) * integ0(sigma / length);
  } else {
    acc += (length / sim) * i_unit + ((sigma - length) / sim) * curve_at(1.0f);
  }
  return acc * 2.0f * kPi;
}

float sample_shape_arc_u(const SplitEffectParticle& p, uint32_t seed, uint32_t extra, float birth_t,
                         int birth_index, float arc_rad) noexcept {
  // Unity 2022.3 ParticleSystemShapeMultiModeValue + ShapeModule.arcSpeed:
  // Random / Loop / PingPong / BurstSpread. Speed is complete 360° rotations
  // per second. Loop + speed 0 keeps the emit angle at the start of the arc.
  const uint8_t mode = p.shape_arc_mode;
  const float spread = std::clamp(p.shape_arc_spread, 0.0f, 1.0f);
  float u = 0.0f;
  if (mode == 1 || mode == 2) {
    const float traveled = arc_speed_traveled(p, birth_t);
    if (mode == 1) {
      if (arc_rad > 1e-8f) {
        float m = std::fmod(traveled, arc_rad);
        if (m < 0.0f) {
          m += arc_rad;
        }
        u = m / arc_rad;
      }
    } else if (arc_rad > 1e-8f) {
      const float period = 2.0f * arc_rad;
      const float dist = std::fabs(traveled);
      float m = std::fmod(dist, period);
      if (m < 0.0f) {
        m += period;
      }
      u = (m <= arc_rad ? m : period - m) / arc_rad;
    }
  } else if (mode == 3) {
    // BurstSpread: "Distribute new particles around the shape evenly."
    // Spread is the official interval. Burst count spaces a burst. RoT with
    // spread 0 and no burst has no published step — keep Random.
    float step = spread;
    if (step <= 1e-6f) {
      uint8_t bc = 0;
      const SplitEffectBurst* bursts = official_split_effect_bursts(p, &bc);
      if (bursts != nullptr && bc > 0) {
        const int n = std::max(1, static_cast<int>(std::lround(bursts[0].count)));
        step = n > 1 ? 1.0f / static_cast<float>(n) : 0.0f;
      }
    }
    if (step > 1e-6f) {
      u = wrap_unit(static_cast<float>(std::max(birth_index, 0)) * step);
    } else {
      u = u01(mix3(seed, extra, 20u));
    }
  } else {
    u = u01(mix3(seed, extra, 20u));
  }
  if (spread > 1e-6f && mode != 3) {
    u = std::round(u / spread) * spread;
    u = std::clamp(u, 0.0f, 1.0f);
  }
  return u;
}

}  // namespace

void sample_shape(const SplitEffectParticle& p, uint32_t seed, uint32_t extra, float birth_t,
                  int birth_index, float& px, float& py, float& pz, float& dx, float& dy,
                  float& dz) noexcept {
  // Unity 2022.3 ShapeModule.enabled
  // (ScriptReference/ParticleSystem.ShapeModule-enabled.html). Module off:
  // no Circle/Sphere sample — emit from the emitter origin, default +Z
  // (Manual/PartSysShapeModule.html Cone/Box forward). Then the PS
  // GameObject Transform still applies.
  if (p.shape_enabled == 0) {
    px = 0.0f;
    py = 0.0f;
    pz = 0.0f;
    dx = 0.0f;
    dy = 0.0f;
    dz = 1.0f;
    apply_ps_local_transform(p, px, py, pz, dx, dy, dz);
    return;
  }
  // Unity 2022.3 ShapeModule.scale / Manual Shape Scale: "Change the size
  // of the emitter shape." 0 on an axis is 0 extent, not a missing 1.
  const float sx = p.shape_scale_x;
  const float sy = p.shape_scale_y;
  const float sz = p.shape_scale_z;
  const float r = p.shape_radius;
  const float thick = std::clamp(p.shape_radius_thickness, 0.0f, 1.0f);
  // live dump v1 (synth-mesh-measure-v14 Wave 1 / Wave 4). Not 2022.3.
  // Same-frame young |p − emitter.world| ∈ [inner, outer] after PS transform
  // when scale is 1. Shared Sphere / Hemisphere / Circle radiusThickness.
  float shell_inner = 0.0f, shell_outer = 0.0f;
  split_effect_shape_radius_shell(r, thick, shell_inner, shell_outer);
  const float arc = (p.shape_arc > 1e-4f ? p.shape_arc : 360.0f) * kDegToRad;
  uint32_t urng = rng_start(seed, extra, 20u);
  auto unit = [&](uint32_t salt) {
    urng = mix3(urng, rng_or1(salt), rng_or1(extra));
    return u01(urng);
  };
  const float arc_u = sample_shape_arc_u(p, seed, extra, birth_t, birth_index, arc);
  const uint8_t t = p.shape_type;
  float ox = 0.0f, oy = 0.0f, oz = 0.0f;
  dx = 0.0f;
  dy = 0.0f;
  dz = 1.0f;

  if (t == 0 || t == 1 || t == 2 || t == 3) {
    // Unity 2022.3 Shape Sphere/Hemisphere: uniform emission in all directions
    // (Manual/PartSysShapeModule.html). Nearby-salt cube-reject was not
    // independent — 10220 Death sparks occupied two octants. hop: Marsaglia
    // direction; radiusThickness still the live-dump shell + u^(1/3) volume.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    unit_from_seed(seed, extra, 20u, x, y, z);
    if (t == 2 || t == 3) {
      z = std::fabs(z);
    }
    const bool shell = (t == 1 || t == 3);
    float rad = shell_outer;
    if (!shell) {
      const float u = std::pow(std::max(unit(40), 0.0f), 1.0f / 3.0f);
      rad = shell_inner + (shell_outer - shell_inner) * u;
    }
    ox = x * rad * sx;
    oy = y * rad * sy;
    oz = z * rad * sz;
    // Scale only changes the spawn volume, not startSpeed direction.
    dx = x;
    dy = y;
    dz = z;
    normalize(dx, dy, dz);
  } else if (t == 4 || t == 7 || t == 8) {
    const float ang = std::max(p.shape_angle, 0.0f) * kDegToRad;
    const float phi = arc_u * arc;
    // hop: live dump cone-dir-v1 (Unity 2022.3.62f3c1 Emit 5000, 12/13
    // checkable cases max angle error 0.0000°). Not 2022.3.
    // Manual PartSysShapeModule Cone: "The particles diverge in proportion
    // to their distance from the cone's center line." (semantic only; no
    // formula). Fitted: ρ² ~ U[1−thickness, 1], so
    // ρ = sqrt((1−t) + t·u22); rad = outer·ρ; azimuth = φ;
    // dir ∝ (ρ sinθ cosφ, ρ sinθ sinφ, cosθ), then × shape scale, then
    // normalize. tan(deflection) = tanθ · ρ. Independent ang·√u21 and
    // "deflection ∝ ρ" (θ·ρ) rejected (B 45° max 4.1°, C 80° 35.6°,
    // L 90° 86.8°). randomPositionAmount moves position only.
    // unit(21) no longer drives deflection; still consumed so chained
    // unit(22) radius samples match U6. thickness 0/1: rad equals the
    // previous inner+(outer−inner)·√u22 (catalog only 0 and 1).
    // t4 stays on the base disc (h=0, outer=R). t7/t8 share the same ρ
    // and direction; Volume untested (catalog 0). No t7 special case.
    // Does not replace Sphere / Circle shell hops. Non-Cone do not enter.
    (void)unit(21);
    const float length = std::max(p.shape_length, 0.0f);
    const float h = (t == 4) ? 0.0f : length * unit(24);
    float shell_inner = 0.0f, shell_outer = 0.0f;
    split_effect_cone_radial_shell(h, r, p.shape_angle, thick, shell_inner, shell_outer);
    (void)shell_inner;
    const float rho =
        std::sqrt((1.0f - thick) + thick * std::max(unit(22), 0.0f));
    const float rad = shell_outer * rho;
    const float sin_a = std::sin(ang);
    const float cos_a = std::cos(ang);
    dx = rho * sin_a * std::cos(phi);
    dy = rho * sin_a * std::sin(phi);
    dz = cos_a;
    ox = std::cos(phi) * rad * sx;
    oy = std::sin(phi) * rad * sy;
    oz = h * sz;
    dx *= sx;
    dy *= sy;
    dz *= sz;
    normalize(dx, dy, dz);
  } else if (t == 10 || t == 18) {
    const float phi = arc_u * arc;
    const float u = std::sqrt(std::max(unit(21), 0.0f));
    const float rad = shell_inner + (shell_outer - shell_inner) * u;
    ox = std::cos(phi) * rad * sx;
    oy = std::sin(phi) * rad * sy;
    oz = 0.0f;
    // Unity 2022.3 Circle: "The particles move only in the plane of the
    // circle." +Z is Cone/Box, not Circle. After Shape Rx(90°) the plane is
    // XZ — startSpeed leaves the split line instead of sliding along it.
    dx = ox;
    dy = oy;
    dz = 0.0f;
    if (dx * dx + dy * dy < 1e-16f) {
      dx = std::cos(phi) * sx;
      dy = std::sin(phi) * sy;
    }
  } else if (t == 12) {
    ox = (arc_u * 2.0f - 1.0f) * r * sx;
    oy = 0.0f;
    oz = 0.0f;
    dx = 0.0f;
    dy = 1.0f;
    dz = 0.0f;
  } else if (t == 16 || t == 15 || t == 5) {
    // Unity 2022.3 Shape Box (Manual/PartSysShapeModule.html): emit from
    // Edge / Shell / Volume; particles move in the emitter forward (Z).
    // Official 1.96.0 has type 16 BoxEdge only (6 systems). 12-edge position
    // topology is not on that page — keep edges, do not invent a mesh.
    // Scale sizes the box (same page). Type 5/15 are unused (0 / 1872).
    const float hx = 0.5f * sx;
    const float hy = 0.5f * sy;
    const float hz = 0.5f * sz;
    const int edge = static_cast<int>(unit(20) * 12.0f) % 12;
    const float u = unit(21) * 2.0f - 1.0f;
    switch (edge) {
      case 0:
        ox = u * hx;
        oy = hy;
        oz = hz;
        break;
      case 1:
        ox = u * hx;
        oy = hy;
        oz = -hz;
        break;
      case 2:
        ox = u * hx;
        oy = -hy;
        oz = hz;
        break;
      case 3:
        ox = u * hx;
        oy = -hy;
        oz = -hz;
        break;
      case 4:
        ox = hx;
        oy = u * hy;
        oz = hz;
        break;
      case 5:
        ox = hx;
        oy = u * hy;
        oz = -hz;
        break;
      case 6:
        ox = -hx;
        oy = u * hy;
        oz = hz;
        break;
      case 7:
        ox = -hx;
        oy = u * hy;
        oz = -hz;
        break;
      case 8:
        ox = hx;
        oy = hy;
        oz = u * hz;
        break;
      case 9:
        ox = hx;
        oy = -hy;
        oz = u * hz;
        break;
      case 10:
        ox = -hx;
        oy = hy;
        oz = u * hz;
        break;
      default:
        ox = -hx;
        oy = -hy;
        oz = u * hz;
        break;
    }
    if (t == 16) {
      dx = 0.0f;
      dy = 0.0f;
      dz = 1.0f;
    } else {
      dx = ox;
      dy = oy;
      dz = oz;
      normalize(dx, dy, dz);
    }
  } else {
    ox = (unit(20) * 2.0f - 1.0f) * r * sx;
    oy = (unit(21) * 2.0f - 1.0f) * r * sy;
    oz = (unit(22) * 2.0f - 1.0f) * r * sz;
    dx = ox;
    dy = oy;
    dz = oz;
    normalize(dx, dy, dz);
  }

  rotate_euler_deg(p.shape_rot_x, p.shape_rot_y, p.shape_rot_z, ox, oy, oz);
  rotate_euler_deg(p.shape_rot_x, p.shape_rot_y, p.shape_rot_z, dx, dy, dz);
  normalize(dx, dy, dz);

  if (p.random_direction_amount > 1e-6f) {
    float rx, ry, rz;
    unit_from_seed(seed, extra, 80u, rx, ry, rz);
    slerp_dir(dx, dy, dz, rx, ry, rz, p.random_direction_amount);
  }
  if (p.spherical_direction_amount > 1e-6f) {
    float rx, ry, rz;
    unit_from_seed(seed, extra, 90u, rx, ry, rz);
    slerp_dir(dx, dy, dz, rx, ry, rz, p.spherical_direction_amount);
  }
  // Unity 2022.3 Shape alignToDirection orients by initial travel, then
  // startRotation is added. Official catalog: 0 / 1872. The look-rotation
  // is not in the 2022.3 Shape manual — do not invent a quaternion here.

  px = p.shape_pos_x + ox;
  py = p.shape_pos_y + oy;
  pz = p.shape_pos_z + oz;

  // Unity 2022.3 Shape Randomize Position: "Move particles by a random
  // amount, up to the specified value." Cone: "The amount of randomization
  // is multiplied by the Scale property."
  if (p.random_position_amount > 1e-6f) {
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    unit_from_seed(seed, extra, 100u, rx, ry, rz);
    const float mag = u01(mix3(seed, extra, 101u)) * p.random_position_amount;
    rx *= mag;
    ry *= mag;
    rz *= mag;
    const bool cone = (t == 4 || t == 7 || t == 8);
    if (cone) {
      rx *= sx;
      ry *= sy;
      rz *= sz;
      rotate_euler_deg(p.shape_rot_x, p.shape_rot_y, p.shape_rot_z, rx, ry, rz);
    }
    px += rx;
    py += ry;
    pz += rz;
  }
  apply_ps_local_transform(p, px, py, pz, dx, dy, dz);
}

}  // namespace wds::chart_render::shuriken
