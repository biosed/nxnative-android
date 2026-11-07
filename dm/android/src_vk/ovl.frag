#version 450
// On-screen controls: the atlas's premultiplied sprite, brightened while pressed (tint.x, tint.y), at the layout's opacity (tint.z)
layout(push_constant) uniform PC { vec4 rect; vec4 uv; vec4 tint; vec4 scr; } pc;
layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
// 1 on an sRGB swapchain (the game presents sRGB, nvn_vk.c g_want_srgb): the atlas holds display values, which the attachment would encode once more
layout(constant_id = 0) const int SRGB_OUT = 0;
void main() {
    vec4 c = texture(atlas, v_uv);
    c.rgb = min(c.rgb * pc.tint.x + pc.tint.y * c.a, vec3(c.a));
    o = c * pc.tint.z;
    if (SRGB_OUT != 0) o.rgb = pow(o.rgb, vec3(2.2));   // close to the sRGB decode; premultiplied, so applied after the opacity
}
