#version 450
layout(location = 0) in vec4 col;
layout(location = 1) in vec2 uv;
layout(set = 0, binding = 1) uniform sampler2D tex[1];   // the layout's binding holds --tex N descriptors (all written per set), the shader reads the first
layout(location = 0) out vec4 o;
void main() { o = col * texture(tex[0], uv); }
