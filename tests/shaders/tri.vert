#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec3 inColor;
layout(set = 0, binding = 0) uniform UBO { vec4 tint; vec2 offset; } ubo;
layout(push_constant) uniform PC { vec4 scale; } pc;
layout(location = 0) out vec3 vColor;
layout(location = 1) out vec2 vUnused;
void main() {
    gl_Position = vec4(inPos * pc.scale.xy + ubo.offset, 0.5, 1.0);
    vColor = inColor * ubo.tint.rgb;
    vUnused = inPos;
}
