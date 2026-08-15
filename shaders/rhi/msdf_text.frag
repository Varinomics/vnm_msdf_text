#version 440

layout(std140, binding = 0) uniform Block
{
    mat4  transform;
    vec4  color;
    float px_range;
    float padding_0;
    float padding_1;
    float padding_2;
} u;

layout(binding = 1) uniform sampler2D atlas;

layout(location = 0) smooth in vec2 vs_uv;
layout(location = 1) smooth in vec4 vs_uv_bounds;

layout(location = 0) out vec4 out_color;

float median3(vec3 v)
{
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

void main()
{
    // Clamp inside the glyph's own UV rectangle, inset by half a texel, so
    // bilinear filtering cannot pull a neighbouring packed glyph across the
    // atlas gutter. The inset never crosses the middle of a thin glyph.
    vec2 texel_size = vec2(1.0) / vec2(textureSize(atlas, 0));
    vec2 glyph_span = max(vs_uv_bounds.zw - vs_uv_bounds.xy, vec2(0.0));
    vec2 inset      = min(texel_size * 0.5, glyph_span * 0.499);
    vec2 uv         = clamp(vs_uv, vs_uv_bounds.xy + inset, vs_uv_bounds.zw - inset);

    // MTSDF: the RGB channels carry the multi-channel distance field, which
    // keeps corners sharper than the true signed distance in alpha.
    vec4  mtsdf           = texture(atlas, uv);
    float signed_distance = median3(mtsdf.rgb) - 0.5;
    float coverage        = clamp(signed_distance * u.px_range + 0.5, 0.0, 1.0);

    // Premultiplied output: the pipeline blends One / OneMinusSrcAlpha.
    float alpha = u.color.a * coverage;
    out_color   = vec4(u.color.rgb * alpha, alpha);
}
