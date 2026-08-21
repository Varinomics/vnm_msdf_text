#version 440

// The optional draw capabilities of vnm_msdf_text/rhi/draw_capabilities.h. The
// LCD decode thresholds, filter weights, tap windows, and subpixel step below
// are the ones vnm_msdf_text::lcd_shader_reference describes, and the
// shader-reference test reads this file to check they still are.
//
// The opacity and glow conditions the reference also states are enforced where
// the draw is queued, not here: a subpixel order arrives only with an opaque
// colour, an opaque background, and no glow, so this stage does not re-test
// what cannot reach it.

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

layout(binding = 1) uniform sampler2D atlas;

layout(location = 0) smooth in vec4 vs_uv_bounds;
layout(location = 1) smooth in vec4 vs_frame_rect;

layout(location = 0) out vec4 out_color;

float median3(vec3 v)
{
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

float smootherstep(float edge0, float edge1, float x)
{
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float glyph_alpha_from_texel(vec4 texel)
{
    float sd = (median3(texel.rgb) - 0.5) * u.px_range;
    float alpha = clamp(sd + 0.5, 0.0, 1.0);

    // The alpha channel is the true SDF of the MTSDF atlas. Masking with it
    // removes what the multi-channel median reconstructs where its channels
    // disagree, while the median keeps the corners the true field rounds off.
    if (u.sdf_mask_enabled != 0) {
        float sd_sdf = (texel.a - 0.5) * u.px_range;
        alpha = min(alpha, clamp(sd_sdf + 0.5, 0.0, 1.0));
    }

    return alpha;
}

float signed_distance_from_texel(vec4 texel)
{
    // The MTSDF atlas always stores the true signed distance in alpha. The glow
    // follows that field rather than the corner reconstruction in RGB.
    return (texel.a - 0.5) * u.px_range;
}

float glow_alpha_from_texel(vec4 texel)
{
    float radius = max(u.glow_radius, 0.0);
    float sd = signed_distance_from_texel(texel);
    return smootherstep(-radius, 0.5, sd);
}

vec4 glyph_texel_at_ratio(vec2 glyph_ratio, vec2 uv_min, vec2 uv_max)
{
    vec2 glyph_span = max(vs_uv_bounds.zw - vs_uv_bounds.xy, vec2(0.000001));
    vec2 glyph_uv   = vs_uv_bounds.xy + glyph_ratio * glyph_span;
    return texture(atlas, clamp(glyph_uv, uv_min, uv_max));
}

float glyph_alpha_at_ratio(vec2 glyph_ratio, vec2 uv_min, vec2 uv_max)
{
    return glyph_alpha_from_texel(glyph_texel_at_ratio(glyph_ratio, uv_min, uv_max));
}

vec3 filtered_lcd_coverage(
    vec2 glyph_ratio,
    vec2 subpixel_step,
    bool forward_order,
    vec2 uv_min,
    vec2 uv_max)
{
    float sample_0 = glyph_alpha_at_ratio(glyph_ratio - subpixel_step * 3.0, uv_min, uv_max);
    float sample_1 = glyph_alpha_at_ratio(glyph_ratio - subpixel_step * 2.0, uv_min, uv_max);
    float sample_2 = glyph_alpha_at_ratio(glyph_ratio - subpixel_step, uv_min, uv_max);
    float sample_3 = glyph_alpha_at_ratio(glyph_ratio, uv_min, uv_max);
    float sample_4 = glyph_alpha_at_ratio(glyph_ratio + subpixel_step, uv_min, uv_max);
    float sample_5 = glyph_alpha_at_ratio(glyph_ratio + subpixel_step * 2.0, uv_min, uv_max);
    float sample_6 = glyph_alpha_at_ratio(glyph_ratio + subpixel_step * 3.0, uv_min, uv_max);

    float filter_edge = 0.03125;
    float filter_side = 0.30078125;
    float filter_center = 0.3359375;
    float first_coverage =
        sample_0 * filter_edge +
        sample_1 * filter_side +
        sample_2 * filter_center +
        sample_3 * filter_side +
        sample_4 * filter_edge;
    float center_coverage =
        sample_1 * filter_edge +
        sample_2 * filter_side +
        sample_3 * filter_center +
        sample_4 * filter_side +
        sample_5 * filter_edge;
    float last_coverage =
        sample_2 * filter_edge +
        sample_3 * filter_side +
        sample_4 * filter_center +
        sample_5 * filter_side +
        sample_6 * filter_edge;

    return forward_order
        ? vec3(first_coverage, center_coverage, last_coverage)
        : vec3(last_coverage, center_coverage, first_coverage);
}

void main()
{
    // Clamp the lookup inside the glyph box so neighbouring atlas glyphs cannot
    // bleed across the half-texel edge under bilinear filtering. The margin
    // never crosses the middle of a thin glyph.
    vec2 atlas_size   = vec2(textureSize(atlas, 0));
    vec2 glyph_span   = max(vs_uv_bounds.zw - vs_uv_bounds.xy, vec2(0.0));
    vec2 half_texel   = vec2(0.5) / atlas_size;
    vec2 clamp_margin = min(half_texel, glyph_span * 0.499);
    vec2 uv_min       = vs_uv_bounds.xy + clamp_margin;
    vec2 uv_max       = vs_uv_bounds.zw - clamp_margin;

    // Where this fragment sits inside its own glyph, in output pixels. The
    // frame rectangle is top-left origin like the text, so gl_FragCoord.y is
    // brought into that space first; a filter step of a third of frame_size is
    // then a third of an output pixel.
    vec2 frame_origin = vs_frame_rect.xy;
    vec2 frame_size = max(vec2(1.0), vs_frame_rect.zw);
    float frag_y = (u.framebuffer_y_up != 0)
        ? (u.target_height - gl_FragCoord.y)
        : gl_FragCoord.y;
    vec2 glyph_pixel = vec2(gl_FragCoord.x, frag_y) - frame_origin;
    vec2 glyph_ratio = vec2(
        glyph_pixel.x / frame_size.x,
        1.0 - glyph_pixel.y / frame_size.y);

    bool lcd_rgb = u.lcd_subpixel_order > 0.5 && u.lcd_subpixel_order < 1.5;
    bool lcd_bgr = u.lcd_subpixel_order > 1.5 && u.lcd_subpixel_order < 2.5;
    bool lcd_vrgb = u.lcd_subpixel_order > 2.5 && u.lcd_subpixel_order < 3.5;
    bool lcd_vbgr = u.lcd_subpixel_order > 3.5 && u.lcd_subpixel_order < 4.5;
    bool lcd_horizontal = lcd_rgb || lcd_bgr;
    bool lcd_vertical = lcd_vrgb || lcd_vbgr;
    bool lcd_enabled = lcd_horizontal || lcd_vertical;
    if (lcd_enabled) {
        vec2 subpixel_step = lcd_horizontal
            ? vec2(1.0 / (3.0 * frame_size.x), 0.0)
            : vec2(0.0, -1.0 / (3.0 * frame_size.y));
        bool forward_order = lcd_rgb || lcd_vrgb;
        vec3 lcd_coverage =
            filtered_lcd_coverage(glyph_ratio, subpixel_step, forward_order, uv_min, uv_max);
        float alpha = max(lcd_coverage.r, max(lcd_coverage.g, lcd_coverage.b));
        if (alpha <= 0.0) {
            out_color = vec4(u.color.rgb, 0.0);
            return;
        }

        // One alpha has to stand for three channel coverages, so the colour is
        // solved back out of the per-channel mix: blending this straight colour
        // over the background it was composed against reproduces that mix.
        vec3 precomposed_rgb = mix(
            u.background_color.rgb,
            u.color.rgb,
            lcd_coverage);
        vec3 straight_rgb =
            (precomposed_rgb -
                u.background_color.rgb * (1.0 - alpha)) /
            alpha;
        out_color = vec4(clamp(straight_rgb, 0.0, 1.0), alpha);
        return;
    }

    vec4 texel = glyph_texel_at_ratio(glyph_ratio, uv_min, uv_max);
    float glyph_alpha = glyph_alpha_from_texel(texel);
    float glow_alpha = 0.0;
    if (u.glow_radius > 0.0 && u.glow_color.a > 0.0) {
        glow_alpha = glow_alpha_from_texel(texel);
    }

    float glyph_a = u.color.a * glyph_alpha;
    float glow_a = u.glow_color.a * glow_alpha;
    float out_a = glyph_a + glow_a * (1.0 - glyph_a);
    if (out_a <= 0.0) {
        out_color = vec4(0.0);
        return;
    }

    // Straight colour, so the pipeline blends SrcAlpha against OneMinusSrcAlpha.
    vec3 out_rgb =
        (u.color.rgb * glyph_a + u.glow_color.rgb * glow_a * (1.0 - glyph_a)) /
        out_a;
    out_color = vec4(out_rgb, out_a);
}
