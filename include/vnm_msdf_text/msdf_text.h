#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vnm {
namespace msdf_text {

/**
 * @brief Per-codepoint layout data in scale-independent font units and atlas UVs.
 *
 * The baked atlas stores glyph geometry in the font's own coordinate units, not
 * in output pixels, so a single baked atlas can produce layout for many draw
 * pixel heights without rebuilding the bitmap. Convert a glyph to output pixels
 * for a given draw pixel height with scaled_glyph().
 *
 * bounds_*_units are the glyph outline bounds in font units (Y-up, as reported
 * by the shape). advance_units is the horizontal advance in font units. visible
 * is true when the glyph has a rasterized bitmap; advance-only glyphs such as
 * U+0020 are not visible and scale to a degenerate (zero-area) plane rectangle.
 *
 * UVs address atlas_t::rgba, where row 0 is the first row in memory and T
 * increases downward. UVs are determined by the bake and do not change with the
 * draw pixel height.
 */
struct glyph_t
{
    float advance_units       = 0.0f;
    float bounds_left_units   = 0.0f;
    float bounds_bottom_units = 0.0f;
    float bounds_right_units  = 0.0f;
    float bounds_top_units    = 0.0f;
    bool  visible             = false;
    float uv_left             = 0.0f;
    float uv_bottom           = 0.0f;
    float uv_right            = 0.0f;
    float uv_top              = 0.0f;
};

/**
 * @brief Per-codepoint layout data scaled to a specific draw pixel height.
 *
 * Produced by scaled_glyph(). Plane coordinates are relative to the baseline
 * origin and use screen-style Y-down coordinates: for visible glyphs
 * plane_bottom is the smaller Y value and plane_top is the larger Y value. For
 * non-visible glyphs every plane coordinate is zero, so the quad is degenerate
 * and emits nothing while advance_x still advances the pen.
 */
struct scaled_glyph_t
{
    float advance_x    = 0.0f;
    float plane_left   = 0.0f;
    float plane_bottom = 0.0f;
    float plane_right  = 0.0f;
    float plane_top    = 0.0f;
    float uv_left      = 0.0f;
    float uv_bottom    = 0.0f;
    float uv_right     = 0.0f;
    float uv_top       = 0.0f;
};

using kerning_key_t = std::uint64_t;

constexpr kerning_key_t make_kerning_key(char32_t left, char32_t right)
{
    return (static_cast<std::uint64_t>(left) << 32u) |
        static_cast<std::uint32_t>(right);
}

using glyph_map_t         = std::unordered_map<char32_t, glyph_t>;
using kerning_units_map_t = std::unordered_map<kerning_key_t, float>;

/**
 * @brief Font metrics scaled to a draw pixel height (output pixels).
 */
struct font_metrics_px_t
{
    float ascender = 0.0f;
    float descender = 0.0f;
    float line_height = 0.0f;
    float em_size = 0.0f;
};

/**
 * @brief Font metrics in scale-independent font units.
 *
 * ascender is the font-unit ascender used as the draw-scale reference:
 * draw_scale = draw_pixel_height / ascender.
 */
struct font_metrics_units_t
{
    float ascender = 0.0f;
    float descender = 0.0f;
    float line_height = 0.0f;
    float em_size = 0.0f;
};

struct atlas_t
{
    /**
     * @brief Pixel height the bitmap was baked at (>= the requested draw height,
     * never below ceil(options_t::min_atlas_font_size)).
     */
    int baked_pixel_height = 0;
    int atlas_size = 0;
    /**
     * @brief Row-major linear RGBA8 MTSDF atlas data.
     *
     * The RGB channels contain the multi-channel distance field and alpha
     * contains true signed distance. Upload this data as linear, not sRGB.
     */
    std::vector<std::uint8_t> rgba;
    glyph_map_t glyphs;
    kerning_units_map_t kerning_units;
    font_metrics_units_t font_metrics_units;
    /**
     * @brief Baked distance range in atlas pixels (options_t::atlas_px_range).
     *
     * The draw-size shader range is produced by px_range_for_pixel_height().
     */
    double atlas_px_range = 0.0;
    /**
     * @brief Font-unit -> atlas-pixel scale used to bake the bitmap.
     */
    double bitmap_scale = 1.0;
    /**
     * @brief Multiplier applied to the draw-size px_range for shader sharpness.
     */
    float sharpness_bias = 1.0f;
    float zero_advance_units = 0.0f;
    bool zero_advance_available = false;
};

enum class Missing_glyph_policy
{
    SKIP,
    USE_REPLACEMENT_CHARACTER,
    FAIL_BUILD,
};

struct options_t
{
    int atlas_size = 2048;
    /**
     * @brief Lower bound for the atlas generation scale in output pixels.
     *
     * Text can be requested at smaller pixel heights while the baked atlas is
     * generated at this minimum scale to preserve distance-field quality.
     */
    double min_atlas_font_size = 48.0;
    /**
     * @brief Baked distance range passed to msdfgen in atlas pixels.
     */
    float atlas_px_range = 2.0f;
    /**
     * @brief Multiplier applied to the draw-size px_range for shader sharpness.
     *
     * 1.0 yields a one-output-pixel anti-aliasing ramp at every draw size;
     * larger values sharpen the edge proportionally.
     */
    float sharpness_bias = 1.0f;
    /**
     * @brief Empty atlas pixels left between packed glyph bitmaps.
     *
     * Shaders should still clamp samples to each glyph UV rectangle. Increase
     * this when a renderer uses mipmaps or nonstandard filtering.
     */
    int atlas_gutter_px = 1;
    bool build_kerning_table = true;
    /**
     * @brief Controls how missing requested codepoints are handled at build time.
     *
     * USE_REPLACEMENT_CHARACTER aliases missing glyphs to U+FFFD when the font
     * contains that glyph. FAIL_BUILD rejects missing requested codepoints before
     * atlas generation.
     */
    Missing_glyph_policy missing_glyph_policy = Missing_glyph_policy::SKIP;
};

/**
 * @brief Result state for build_font_atlas.
 *
 * PARTIAL_SUCCESS means an atlas was produced and contains at least one glyph,
 * but the diagnostic vectors describe requested codepoints that were not
 * emitted.
 */
enum class Build_status
{
    FAILURE,
    PARTIAL_SUCCESS,
    SUCCESS,
};

struct build_result_t
{
    Build_status status = Build_status::FAILURE;
    std::string message;
    atlas_t atlas;
    std::vector<char32_t> invalid_codepoints;
    std::vector<char32_t> missing_codepoints;
    std::vector<char32_t> failed_codepoints;
    std::vector<char32_t> skipped_too_large;
    std::vector<char32_t> skipped_no_space;
    bool atlas_full = false;
};

struct text_vertex_t
{
    float x;
    float y;
    float s;
    float t;
    float s_min;
    float t_min;
    float s_max;
    float t_max;
};

struct positioned_glyph_t
{
    char32_t codepoint = 0;
    /**
     * @brief The glyph scaled to the requested draw pixel height.
     */
    scaled_glyph_t glyph;
    float pen_x = 0.0f;
};

struct text_bounds_t
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float advance_x = 0.0f;
    bool has_visible_glyphs = false;
};

using log_callback_t = std::function<void(const std::string&)>;
using layout_callback_t = std::function<void(const positioned_glyph_t&)>;

[[nodiscard]]
std::vector<char32_t> default_codepoints();

[[nodiscard]]
std::vector<char32_t> utf8_to_codepoints(std::string_view text);

[[nodiscard]]
std::string codepoints_to_utf8(std::span<const char32_t> codepoints);

/**
 * @brief Bake pixel height derived from a requested draw pixel height.
 *
 * The bitmap is baked at least at ceil(options_t::min_atlas_font_size) so that
 * small draw sizes share one baked atlas. Larger draw sizes bake at the draw
 * size.
 */
[[nodiscard]]
int msdf_bake_pixel_height(int draw_pixel_height, const options_t& options);

/**
 * @brief Build an MTSDF atlas from font bytes and requested Unicode scalars.
 *
 * The bitmap is baked at msdf_bake_pixel_height(draw_pixel_height, options) and
 * glyph geometry, kerning, and font metrics are stored in scale-independent font
 * units. Produce draw-size layout for any draw pixel height with the scaling
 * helpers below; this lets one baked atlas serve a range of draw sizes.
 *
 * The input codepoint span is normalized before glyph work: invalid Unicode
 * scalar values are reported in build_result_t::invalid_codepoints and valid
 * duplicates are removed. Missing font coverage, load failures, oversized
 * glyphs, and atlas exhaustion are reported separately. FAILURE results have a
 * default-constructed atlas; callers may inspect diagnostics but must not render
 * from result.atlas.
 *
 * The function reports ordinary build problems through build_result_t. It may
 * still throw std::bad_alloc if an allocation fails outside the handled atlas
 * storage path.
 */
[[nodiscard]]
build_result_t build_font_atlas(
    const std::uint8_t* font_data,
    std::size_t font_size,
    int draw_pixel_height,
    std::span<const char32_t> codepoints,
    const options_t& options = options_t(),
    const log_callback_t& log_debug_info = log_callback_t());

/**
 * @brief Shader distance range in output pixels for a draw pixel height.
 *
 * Varies with the draw pixel height even when the baked bitmap does not, and
 * includes atlas_t::sharpness_bias.
 */
[[nodiscard]]
float px_range_for_pixel_height(const atlas_t& atlas, int draw_pixel_height);

/**
 * @brief Scale one baked glyph to output pixels for a draw pixel height.
 *
 * Non-visible glyphs scale to a degenerate (zero) plane rectangle while still
 * carrying advance_x.
 */
[[nodiscard]]
scaled_glyph_t scaled_glyph(
    const atlas_t& atlas,
    const glyph_t& glyph,
    int draw_pixel_height);

/**
 * @brief Font metrics scaled to a draw pixel height (output pixels).
 */
[[nodiscard]]
font_metrics_px_t scaled_font_metrics(const atlas_t& atlas, int draw_pixel_height);

/**
 * @brief Return the single-line pen advance for text in output pixels.
 *
 * This is advance measurement, not visual bounds. Invalid UTF-8 decodes to
 * U+FFFD; missing glyphs are skipped unless the atlas contains a fallback entry
 * for the decoded codepoint.
 */
[[nodiscard]]
float measure_text_advance_px(
    const atlas_t& atlas,
    int draw_pixel_height,
    std::string_view text);

/**
 * @brief Visit decoded glyphs in single-line layout order.
 *
 * The callback receives glyphs that exist in the atlas, scaled to the draw pixel
 * height, including invisible advance-only glyphs such as spaces. The returned
 * value is the final pen X.
 */
[[nodiscard]]
float for_each_positioned_glyph(
    const atlas_t& atlas,
    int draw_pixel_height,
    std::string_view text,
    float start_x,
    const layout_callback_t& callback);

/**
 * @brief Measure single-line visual bounds and pen advance in output pixels.
 *
 * Bounds use the same Y-down baseline-relative coordinate system as
 * scaled_glyph_t. For text without visible glyph rectangles, has_visible_glyphs
 * is false and the bounds are zero while advance_x still reports the pen advance.
 */
[[nodiscard]]
text_bounds_t measure_text_bounds_px(
    const atlas_t& atlas,
    int draw_pixel_height,
    std::string_view text);

/**
 * @brief Append one quad per visible glyph using x, y as the baseline origin.
 *
 * The function applies UTF-8 decoding, glyph lookup, kerning, advance, and draw
 * scaling. Glyphs with degenerate plane rectangles, such as spaces, advance the
 * pen but do not emit vertices or indices. Throws std::length_error if indexed
 * output would exceed uint32_t index capacity. Vector growth may throw
 * allocation exceptions.
 */
void append_text_quads(
    const atlas_t& atlas,
    int draw_pixel_height,
    std::string_view text,
    float x,
    float y,
    std::vector<text_vertex_t>& vertices,
    std::vector<std::uint32_t>* indices = nullptr);

} // namespace msdf_text
} // namespace vnm
