#version 450
layout(location = 0) in vec2 pos;
layout(set = 0, binding = 0) uniform U { vec4 off; vec4 col; } u;
layout(push_constant) uniform PC { float z; } pc;
layout(location = 0) out vec4 col;
layout(location = 1) out vec2 uv;
void main() { gl_Position = vec4(pos * u.off.zw + u.off.xy, pc.z, 1.0); col = u.col; uv = pos * 0.5 + 0.5; }
