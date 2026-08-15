#version 440

// Vertex attributes are the fields of vnm::msdf_text::text_vertex_t, so the CPU
// producers in msdf_text.h feed this pipeline without an intermediate format.

layout(std140, binding = 0) uniform Block
{
    mat4  transform;
    vec4  color;
    float px_range;
    float padding_0;
    float padding_1;
    float padding_2;
} u;

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 uv_bounds;

layout(location = 0) smooth out vec2 vs_uv;
layout(location = 1) smooth out vec4 vs_uv_bounds;

void main()
{
    vs_uv        = uv;
    vs_uv_bounds = uv_bounds;
    gl_Position  = u.transform * vec4(position, 0.0, 1.0);
}
