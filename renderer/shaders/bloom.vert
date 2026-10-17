#version 450

layout(location = 0) out vec2 v_uv;

void main() {
  // Fullscreen triangle. Normal (unflipped) viewport — UVs match the
  // stored swapchain image after the main pass's Y-flip write.
  vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  v_uv = pos;
  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
