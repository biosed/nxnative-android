#version 450
// On-screen controls (nvn_vk.c ovl_sprites): one textured quad per draw, a triangle strip of 4 vertices placed by the push constants, in window pixels
layout(push_constant) uniform PC { vec4 rect; vec4 uv; vec4 tint; vec4 scr; } pc;   // rect: x y w h; uv: u v du dv in the atlas; tint: brightness, lift, opacity; scr: 1/w 1/h, turned half round
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 c = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vec2 p = pc.rect.xy + c * pc.rect.zw;
    if (pc.scr.z > 0.5) p = vec2(1.0 / pc.scr.x, 1.0 / pc.scr.y) - p;   // an upside-down swapchain (pre-rotation): the quad, its texture with it, turned half round
    v_uv = pc.uv.xy + c * pc.uv.zw;
    gl_Position = vec4(p * pc.scr.xy * 2.0 - 1.0, 0.0, 1.0);
}
