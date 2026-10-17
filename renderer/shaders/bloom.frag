#version 450

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// Soften the extracted Add stack. No official bloom threshold/radius in
// repo — keep a small isotropic kernel, not Y-streak or invented weights.
void main() {
  const vec2 px = vec2(0.0022, 0.0039);
  vec3 acc = texture(u_tex, v_uv).rgb * 0.36;
  acc += texture(u_tex, v_uv + vec2(px.x, 0.0)).rgb * 0.12;
  acc += texture(u_tex, v_uv - vec2(px.x, 0.0)).rgb * 0.12;
  acc += texture(u_tex, v_uv + vec2(0.0, px.y)).rgb * 0.12;
  acc += texture(u_tex, v_uv - vec2(0.0, px.y)).rgb * 0.12;
  acc += texture(u_tex, v_uv + px).rgb * 0.04;
  acc += texture(u_tex, v_uv - px).rgb * 0.04;
  acc += texture(u_tex, v_uv + vec2(px.x, -px.y)).rgb * 0.04;
  acc += texture(u_tex, v_uv + vec2(-px.x, px.y)).rgb * 0.04;
  out_color = vec4(max(acc, vec3(0.0)), 1.0);
}
