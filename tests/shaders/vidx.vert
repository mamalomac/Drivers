#version 450
// No vertex buffers: a quad built from gl_VertexIndex, which must include firstVertex.
layout(push_constant) uniform PC { vec4 color; vec2 origin; } pc;
layout(location = 0) out vec3 vColor;
void main() {
    int corner = gl_VertexIndex - 10; // drawn with firstVertex = 10
    vec2 p = vec2((corner == 1 || corner == 2 || corner == 4) ? 1.0 : 0.0, (corner == 2 || corner == 4 || corner == 5) ? 1.0 : 0.0);
    gl_Position = vec4(pc.origin + p - 1.0, 0.0, 1.0);
    vColor = pc.color.rgb;
}
