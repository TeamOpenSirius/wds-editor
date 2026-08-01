#version 450

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec3 v_uvq;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
  vec2 uv = v_uvq.xy / max(v_uvq.z, 1e-6);
  vec4 texel = texture(u_tex, uv);
  // Straight alpha (matches Sonolus Draw alpha). Premultiplying rgb*a made
  // flick/scratch arrows look far too faint vs opaque flick notes.
  out_color = texel * v_color;
  if (out_color.a < 0.01) {
    discard;
  }
}
