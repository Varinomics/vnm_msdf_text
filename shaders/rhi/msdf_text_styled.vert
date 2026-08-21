#version 440

// The styled path's vertex is the base text_vertex_t position and UV bounds
// plus that glyph's frame rectangle. The interpolated UV of the base pipeline
// is not carried: the fragment stage reconstructs its own sample position from
// the frame rectangle so it can step the subpixel filter in output pixels.

layout(std140, binding = 0) uniform Block
{
    mat4  transform;
    vec4  color;
    vec4  glow_color;
    vec4  background_color;
    float px_range;
    float target_height;
    float glow_radius;
    float lcd_subpixel_order;
    int   framebuffer_y_up;
    int   sdf_mask_enabled;
    float padding_0;
    float padding_1;
} u;

layout(location = 0) in vec2 position;
layout(location = 1) in vec4 uv_bounds;
layout(location = 2) in vec4 frame_rect;

layout(location = 0) smooth out vec4 vs_uv_bounds;
layout(location = 1) smooth out vec4 vs_frame_rect;

void main()
{
    vs_uv_bounds  = uv_bounds;
    vs_frame_rect = frame_rect;
    gl_Position   = u.transform * vec4(position, 0.0, 1.0);
}
