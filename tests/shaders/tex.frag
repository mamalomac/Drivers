#version 450
layout(location = 0) in vec2 vUV;
layout(location = 2) in vec4 vMul;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(tex, vUV) * vMul; }
