#version 450

layout(push_constant) uniform PushConstants {
  mat4 mvp;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_uvq;  // (u*q, v*q, q)
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec3 v_uvq;
layout(location = 1) out vec4 v_color;

void main() {
  gl_Position = pc.mvp * vec4(in_pos, 1.0);
  v_uvq = in_uvq;
  v_color = in_color;
}
