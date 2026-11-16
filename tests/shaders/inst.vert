#version 450
// No vertex buffers: positions come from gl_VertexIndex / gl_InstanceIndex,
// which must include firstVertex / firstInstance exactly as Vulkan defines.
layout(location = 0) out vec3 vColor;
void main() {
    int corner = gl_VertexIndex - 10;          // drawn with firstVertex = 10
    int cell = gl_InstanceIndex - 5;           // drawn with firstInstance = 5
    vec2 p = vec2((corner == 1 || corner == 2 || corner == 4) ? 1.0 : 0.0, (corner == 2 || corner == 4 || corner == 5) ? 1.0 : 0.0);
    vec2 origin = vec2(float(cell % 2), float(cell / 2));
    gl_Position = vec4((origin + p) - 1.0, 0.0, 1.0);
    vColor = vec3(float(cell & 1), float((cell >> 1) & 1), 1.0 - float(cell) / 3.0);
}
