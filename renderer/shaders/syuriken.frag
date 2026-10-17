#version 450

// OpenWDS SplitEffectSyuriken.shader (Vulkan Universal Forward
// path 777288701855927481 program 0). inner = uv.xxyx (iz on V, iw on U).
//
// Vertex colour: rgb = LineColor.rgb * LineColor.a, a = Unity world Y + 64
// (kOfficialSyurikenPulseWorldYBias). Pulse is sin(positionWS.y - 20 t):
// SpriteRenderer dynamic/sprite batching leaves ObjectToWorld identity
// (live dump darkband-orig-v1; not the unbatched GPU-probe wys=13.49).
// Blend is SrcAlpha One into a float target, so folding LineColor.a into
// rgb leaves rgb * alpha unchanged.

layout(push_constant) uniform PushConstants {
  mat4 mvp;
  vec4 params;  // time, unused, sprite_size, unused
} pc;

layout(location = 0) in vec3 v_uvq;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

float hermite(float x) {
  float s = clamp(x, 0.0, 1.0);
  return s * s * (3.0 - 2.0 * s);
}

float pow_sat(float x, float expn) {
  float a = abs(x);
  if (a <= 0.0) {
    return 0.0;
  }
  if (a >= 1.0) {
    return 1.0;
  }
  return pow(a, expn);
}

void main() {
  vec2 uv = v_uvq.xy / max(v_uvq.z, 1e-6);
  float u = uv.x;
  float v = uv.y;
  float time_sec = pc.params.x;
  float world_y = v_color.a - 64.0;

  float ox = hermite((u - 0.425000012) * 1.73913050);
  float oy = hermite((u - 0.574999988) * -1.73913050);
  float oz = hermite((v - 1.02500010) * 1.73913050);
  float ow = hermite((v - 1.17499995) * -1.73913050);
  float outer_pair_x = 1.0 - (ow + oz);
  float outer_pair_y = 1.0 - (oy + ox);
  float color_mask = 1.0 - outer_pair_x * outer_pair_y;
  color_mask = min(pow_sat(color_mask, 4.0), 1.0);

  float ix = hermite((u - 0.25) * 1.33333337);
  float iy = hermite((u - 0.75) * -1.33333337);
  float iz = hermite((v - 0.25) * 1.33333337);
  float iw = hermite((u - 0.544999957) * 2.19780207);
  float inner_pair = 1.0 - (iy + ix);
  float inner_reverse = 1.0 - iz;
  float checker = 1.0 - inner_reverse * inner_pair;
  checker = 1.0 - min(pow_sat(checker, 1200.0), 1.0);
  color_mask = (1.0 - color_mask) * checker;

  float cr = v_color.r;
  float cg = v_color.g;
  float cb = v_color.b;
  float max_c = max(cr, max(cg, cb));
  vec3 rgb = vec3(cr, cg, cb) + (vec3(max_c) - vec3(cr, cg, cb)) * color_mask;

  float pulse = min(sin(world_y - 20.0 * time_sec) + 1.7, 1.0);
  float edge = hermite((u - 0.455000013) * -2.19780207);
  edge = pow(max(edge + iw, 0.0), 0.4);
  float tail = (1.0 - edge) * (1.0 - min(pow_sat(iz, 80.0), 1.0));
  float alpha = pulse * tail + color_mask;
  out_color = vec4(rgb, alpha);
}
