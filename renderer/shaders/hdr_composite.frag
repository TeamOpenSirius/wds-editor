#version 450

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// Official Add is SrcAlpha+One of texel*v_color on one HDR camera
// (OfflineRhythmPreview.unity m_HDR=1). ButterflyEffect Custom1.x+1 can
// go negative and carves dest (notes, background, fog) in the FP16 scene
// stack. This pass presents that result onto the UNORM swapchain — clamp
// leftover negatives after the carve, do not invent bloom/filmic crush.
void main() {
  out_color = vec4(max(texture(u_tex, v_uv).rgb, vec3(0.0)), 1.0);
}
