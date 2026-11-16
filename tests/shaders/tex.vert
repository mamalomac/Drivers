#version 450
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec2 vUV;
layout(location = 1) out float vPad;
layout(location = 2) out vec4 vMul;
layout(push_constant) uniform PC { vec4 mul; float depth; } pc;
void main() {
    gl_Position = vec4(inPos.xy, pc.depth, 1.0);
    vUV = inUV;
    vPad = 1.0;
    vMul = pc.mul;
}
