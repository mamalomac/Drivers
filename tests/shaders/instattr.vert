#version 450
layout(location = 0) in vec2 inOrigin; // per instance
layout(location = 1) in vec3 inColor;  // per instance
layout(location = 0) out vec3 vColor;
void main() {
    int corner = gl_VertexIndex;
    vec2 p = vec2((corner == 1 || corner == 2 || corner == 4) ? 1.0 : 0.0, (corner == 2 || corner == 4 || corner == 5) ? 1.0 : 0.0);
    gl_Position = vec4(inOrigin + p - 1.0, 0.0, 1.0);
    vColor = inColor;
}
