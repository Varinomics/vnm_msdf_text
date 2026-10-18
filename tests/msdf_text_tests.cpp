#include <vnm_msdf_text/msdf_text.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace msdf = vnm::msdf_text;

namespace {

constexpr char32_t k_replacement_char = 0xFFFD;
constexpr char32_t k_invalid_surrogate = 0xD800;
constexpr char32_t k_invalid_too_large = 0x110000;

bool check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool contains_codepoint(const std::vector<char32_t>& codepoints, char32_t codepoint)
{
    return std::find(codepoints.begin(), codepoints.end(), codepoint) != codepoints.end();
}

std::vector<std::uint8_t> read_test_font()
{
    std::ifstream file(VNM_MSDF_TEXT_TEST_FONT_FILE, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open test font");
    }

    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

msdf::options_t atlas_options(int atlas_size)
{
    msdf::options_t options;
    options.atlas_size          = atlas_size;
    options.min_atlas_font_size = 48.0;
    options.atlas_px_range      = 2.0f;
    options.sharpness_bias      = 2.5f;
    options.build_kerning_table = false;
    return options;
}

constexpr int k_default_draw_pixel_height = 32;

msdf::build_result_t build_test_atlas(
    const std::vector<std::uint8_t>& font_data,
    std::span<const char32_t> codepoints,
    int atlas_size = 256,
    msdf::Missing_glyph_policy missing_glyph_policy = msdf::Missing_glyph_policy::SKIP,
    int draw_pixel_height = k_default_draw_pixel_height)
{
    msdf::options_t options = atlas_options(atlas_size);
    options.missing_glyph_policy = missing_glyph_policy;

    return msdf::build_font_atlas(
        font_data.data(),
        font_data.size(),
        draw_pixel_height,
        codepoints,
        options);
}

bool near_equal(float lhs, float rhs, float relative_tolerance = 1e-4f)
{
    const float scale = std::max({1.0f, std::abs(lhs), std::abs(rhs)});
    return std::abs(lhs - rhs) <= relative_tolerance * scale;
}

char32_t missing_codepoint_for_font(const std::vector<std::uint8_t>& font_data)
{
    const std::vector<char32_t> candidates = {
        0x1F518,
        0x10FFFD,
        0xE000,
        0x2FA1D,
    };

    for (char32_t candidate : candidates) {
        const std::vector<char32_t> codepoints = {candidate};
        const msdf::build_result_t result = build_test_atlas(font_data, codepoints);
        if (contains_codepoint(result.missing_codepoints, candidate)) {
            return candidate;
        }
    }

    throw std::runtime_error("test font unexpectedly covers all missing-glyph candidates");
}

struct utf8_case_t
{
    std::string_view bytes;
    std::u32string_view codepoints;
};

// Unicode 3.9.5 requires preserving well-formed successors of an invalid prefix.
constexpr utf8_case_t k_utf8_prefix_cases[] = {
    {"\xC2",                         U"\uFFFD"},
    {"\xE2",                         U"\uFFFD"},
    {"\xE2\x82",                     U"\uFFFD"},
    {"\xF0",                         U"\uFFFD"},
    {"\xF0\x90",                     U"\uFFFD"},
    {"\xF0\x90\x91",                 U"\uFFFD"},
    {"\xE2" "A",                     U"\uFFFDA"},
    {"\xF0" "AB",                    U"\uFFFDAB"},
    {"\xF0\x90" "A",                 U"\uFFFDA"},
    {"\xE2" "AB",                    U"\uFFFDAB"},
    {"\xF0" "ABC",                   U"\uFFFDABC"},
    {"\xE2\x82" "A",                 U"\uFFFDA"},
    {"\xF0\x90\x91" "A",             U"\uFFFDA"},
    {"\xF0\xC2\xA2",                 U"\uFFFD\u00A2"},
    {"\xF0\x90\xC2\xA2",             U"\uFFFD\u00A2"},
    {"\xF0\xE2\x82\xAC",             U"\uFFFD\u20AC"},

    {"\xC2\xA2\xE2\x82\xAC\xF0\x9F\x98\x80", U"\u00A2\u20AC\U0001F600"},
};

bool test_utf8_prefix_decoding()
{
    bool ok = true;
    for (const auto& test_case : k_utf8_prefix_cases) {
        const auto decoded = msdf::utf8_to_codepoints(test_case.bytes);
        const std::vector<char32_t> expected(
            test_case.codepoints.begin(), test_case.codepoints.end());
        ok &= check(decoded == expected, "UTF-8 decoding must preserve valid suffixes and replace invalid prefixes");
    }
    return ok;
}

bool test_default_codepoints_are_explicit_scalars()
{
    const std::vector<char32_t> codepoints = msdf::default_codepoints();

    bool ok = true;
    ok &= check(codepoints.size() == 291, "default codepoints must keep expected coverage count");
    ok &= check(
        std::is_sorted(codepoints.begin(), codepoints.end()),
        "default codepoints must be sorted");
    ok &= check(
        std::adjacent_find(codepoints.begin(), codepoints.end()) == codepoints.end(),
        "default codepoints must be unique");
    ok &= check(contains_codepoint(codepoints, U' '), "default codepoints must include space");
    ok &= check(contains_codepoint(codepoints, U'~'), "default codepoints must include ASCII");
    ok &= check(contains_codepoint(codepoints, 0x20AC), "default codepoints must include Euro sign");
    ok &= check(
        contains_codepoint(codepoints, k_replacement_char),
        "default codepoints must include U+FFFD replacement glyph");
    return ok;
}

bool test_invalid_and_duplicate_codepoints()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const std::vector<char32_t> codepoints = {
        U'W',
        U'W',
        k_invalid_surrogate,
        k_invalid_too_large,
    };
    const msdf::build_result_t result = build_test_atlas(font_data, codepoints);

    bool ok = true;
    ok &= check(
        result.status == msdf::Build_status::PARTIAL_SUCCESS,
        "invalid codepoints with a usable glyph must produce partial success");
    ok &= check(result.atlas.glyphs.count(U'W') == 1, "duplicate codepoints must build one glyph");
    ok &= check(result.invalid_codepoints.size() == 2, "invalid codepoints must be reported once");
    ok &= check(
        contains_codepoint(result.invalid_codepoints, k_invalid_surrogate),
        "invalid surrogate must be reported");
    ok &= check(
        contains_codepoint(result.invalid_codepoints, k_invalid_too_large),
        "codepoints above Unicode range must be reported");
    return ok;
}

bool test_missing_glyph_failure()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const char32_t missing_codepoint = missing_codepoint_for_font(font_data);
    const std::vector<char32_t> codepoints = {missing_codepoint};
    const msdf::build_result_t result = build_test_atlas(font_data, codepoints);

    bool ok = true;
    ok &= check(
        result.status == msdf::Build_status::FAILURE,
        "only missing glyphs must fail the atlas build");
    ok &= check(result.atlas.glyphs.empty(), "missing glyph build must not emit glyphs");
    ok &= check(result.atlas.rgba.empty(), "failure result must not retain atlas texture storage");
    ok &= check(result.atlas.atlas_size == 0, "failure result must expose a default atlas");
    ok &= check(
        contains_codepoint(result.missing_codepoints, missing_codepoint),
        "missing glyph codepoint must be reported");
    return ok;
}

bool test_too_small_atlas_failure()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const std::vector<char32_t> codepoints = {U'W'};
    const msdf::build_result_t result = build_test_atlas(font_data, codepoints, 1);

    bool ok = true;
    ok &= check(
        result.status == msdf::Build_status::FAILURE,
        "atlas that cannot fit the requested glyph must fail");
    ok &= check(result.atlas.glyphs.empty(), "too-small atlas must not emit glyphs");
    ok &= check(result.atlas.rgba.empty(), "too-small atlas failure must clear texture storage");
    ok &= check(
        contains_codepoint(result.skipped_too_large, U'W'),
        "too-large glyph must be reported");
    return ok;
}

bool test_partial_success_keeps_usable_atlas()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const char32_t missing_codepoint = missing_codepoint_for_font(font_data);
    const std::vector<char32_t> codepoints = {U'W', missing_codepoint};
    const msdf::build_result_t result = build_test_atlas(font_data, codepoints);

    bool ok = true;
    ok &= check(
        result.status == msdf::Build_status::PARTIAL_SUCCESS,
        "emitted glyph plus missing glyph must produce partial success");
    ok &= check(result.atlas.glyphs.count(U'W') == 1, "partial atlas must retain emitted glyphs");
    ok &= check(
        contains_codepoint(result.missing_codepoints, missing_codepoint),
        "partial atlas must report missing glyphs");
    ok &= check(!result.atlas.rgba.empty(), "partial atlas must retain texture storage");

    std::vector<msdf::text_vertex_t> vertices;
    std::vector<std::uint32_t> indices;
    msdf::append_text_quads(
        result.atlas, k_default_draw_pixel_height, "W", 0.0f, 0.0f, vertices, &indices);
    ok &= check(vertices.size() == 4, "partial atlas must remain usable for emitted glyphs");
    ok &= check(indices.size() == 6, "partial atlas must emit indexed quads");
    return ok;
}

bool test_replacement_missing_glyph_policy()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const char32_t missing_codepoint = missing_codepoint_for_font(font_data);
    const std::vector<char32_t> codepoints = {U'W', missing_codepoint};
    const msdf::build_result_t result = build_test_atlas(
        font_data,
        codepoints,
        256,
        msdf::Missing_glyph_policy::USE_REPLACEMENT_CHARACTER);

    bool ok = true;
    ok &= check(
        result.status == msdf::Build_status::PARTIAL_SUCCESS,
        "replacement policy must report partial success for missing coverage");
    ok &= check(result.atlas.glyphs.count(U'W') == 1, "replacement atlas must retain covered glyphs");
    ok &= check(
        result.atlas.glyphs.count(missing_codepoint) == 1,
        "replacement policy must alias missing codepoints when U+FFFD is available");
    ok &= check(
        contains_codepoint(result.missing_codepoints, missing_codepoint),
        "replacement policy must still report missing original codepoints");

    const std::vector<char32_t> missing_text = {missing_codepoint};
    std::vector<msdf::text_vertex_t> vertices;
    msdf::append_text_quads(
        result.atlas,
        k_default_draw_pixel_height,
        msdf::codepoints_to_utf8(missing_text),
        0.0f,
        0.0f,
        vertices);
    ok &= check(vertices.size() == 4, "replacement glyph must emit a visible quad");
    return ok;
}

bool test_fail_missing_glyph_policy()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const char32_t missing_codepoint = missing_codepoint_for_font(font_data);
    const std::vector<char32_t> codepoints = {U'W', missing_codepoint};
    const msdf::build_result_t result = build_test_atlas(
        font_data,
        codepoints,
        256,
        msdf::Missing_glyph_policy::FAIL_BUILD);

    bool ok = true;
    ok &= check(
        result.status == msdf::Build_status::FAILURE,
        "fail policy must reject missing requested glyphs");
    ok &= check(result.atlas.glyphs.empty(), "fail policy must not emit partial glyphs");
    ok &= check(result.atlas.rgba.empty(), "fail policy must not allocate atlas storage");
    ok &= check(
        contains_codepoint(result.missing_codepoints, missing_codepoint),
        "fail policy must report the missing codepoint");
    return ok;
}

bool test_atlas_generation_smoke()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const std::vector<char32_t> codepoints = {U'W'};
    const msdf::build_result_t result = build_test_atlas(font_data, codepoints);

    const msdf::atlas_t& atlas = result.atlas;
    const msdf::font_metrics_units_t& metrics = atlas.font_metrics_units;

    bool ok = true;
    ok &= check(result.status == msdf::Build_status::SUCCESS, "single covered glyph must build");
    ok &= check(atlas.atlas_px_range > 0.0, "baked atlas pixel range must be positive");
    ok &= check(
        msdf::px_range_for_pixel_height(atlas, k_default_draw_pixel_height) > 0.0f,
        "draw-size shader pixel range must be positive");
    ok &= check(metrics.ascender > 0.0f, "font ascender metric must be positive");
    ok &= check(metrics.descender < 0.0f, "font descender metric must be negative");
    ok &= check(metrics.line_height > 0.0f, "font line height metric must be positive");
    ok &= check(metrics.em_size > 0.0f, "font em size metric must be positive");
    ok &= check(!atlas.rgba.empty(), "atlas texture storage must exist");
    ok &= check(
        std::any_of(
            atlas.rgba.begin(),
            atlas.rgba.end(),
            [](std::uint8_t value) { return value != 0; }),
        "generated atlas must contain non-zero distance data");

    const auto glyph_it = atlas.glyphs.find(U'W');
    ok &= check(glyph_it != atlas.glyphs.end(), "atlas must contain W glyph");
    if (glyph_it == atlas.glyphs.end()) {
        return false;
    }

    const msdf::glyph_t& glyph = glyph_it->second;
    ok &= check(glyph.visible, "W must be a visible glyph");
    ok &= check(glyph.advance_units > 0.0f, "glyph advance must be positive");
    ok &= check(
        glyph.bounds_left_units < glyph.bounds_right_units,
        "glyph outline X bounds must be ordered");
    ok &= check(
        glyph.bounds_bottom_units < glyph.bounds_top_units,
        "glyph outline Y bounds must be ordered (font units are Y-up)");
    ok &= check(glyph.uv_left < glyph.uv_right, "glyph UV S range must be ordered");
    ok &= check(glyph.uv_top < glyph.uv_bottom, "glyph UV T range must be ordered");

    const msdf::scaled_glyph_t scaled =
        msdf::scaled_glyph(atlas, glyph, k_default_draw_pixel_height);
    ok &= check(scaled.advance_x > 0.0f, "scaled glyph advance must be positive");
    ok &= check(scaled.plane_left < scaled.plane_right, "scaled plane X range must be ordered");
    ok &= check(
        scaled.plane_bottom < scaled.plane_top,
        "scaled plane Y range must be ordered (screen units are Y-down)");
    return ok;
}

// A hand-built atlas whose ascender equals the draw pixel height and whose baked
// distance range is zero, so draw_scale == 1 and the symmetric plane padding is
// zero. Font-unit geometry then maps 1:1 to output pixels, which keeps the layout
// arithmetic below exact and independent of any font file.
msdf::atlas_t unit_scale_atlas(int draw_pixel_height)
{
    msdf::atlas_t atlas;
    atlas.font_metrics_units.ascender = static_cast<float>(draw_pixel_height);
    atlas.bitmap_scale = 1.0;
    atlas.atlas_px_range = 0.0;
    atlas.sharpness_bias = 1.0f;
    return atlas;
}

bool test_utf8_prefix_layout()
{
    constexpr int draw_height = 32;
    constexpr float advance = 5.0f;
    msdf::atlas_t atlas = unit_scale_atlas(draw_height);
    msdf::glyph_t glyph;
    glyph.advance_units       = advance;
    glyph.bounds_left_units   = 1.0f;
    glyph.bounds_right_units  = 4.0f;
    glyph.bounds_bottom_units = -2.0f;
    glyph.bounds_top_units    = 7.0f;
    glyph.visible             = true;
    for (char32_t codepoint : std::u32string_view(U"\uFFFDABC\u00A2\u20AC\U0001F600")) {
        atlas.glyphs.emplace(codepoint, glyph);
    }

    bool ok = true;
    for (const auto& test_case : k_utf8_prefix_cases) {
        const std::vector<char32_t> expected(
            test_case.codepoints.begin(), test_case.codepoints.end());
        const float expected_advance = advance * (float)expected.size();
        std::vector<char32_t> visited;
        std::vector<float> positions;
        const auto record_glyph = [
                &visited,
                &positions
            ](
                const msdf::positioned_glyph_t& positioned)
            {
                visited.push_back(positioned.codepoint);
                positions.push_back(positioned.pen_x);
            };
        const float final_pen_x = msdf::for_each_positioned_glyph(
            atlas, draw_height, test_case.bytes, 10.0f, record_glyph);
        ok &= check(visited == expected, "layout must visit each replacement and valid successor");
        for (std::size_t i = 0; i < positions.size(); ++i) {
            ok &= check(positions[i] == 10.0f + advance * (float)i,
                "layout must position each decoded glyph at its advance");
        }
        ok &= check(final_pen_x == 10.0f + expected_advance,
            "layout must retain the advance of valid UTF-8 successors");
        ok &= check(msdf::measure_text_advance_px(atlas, draw_height, test_case.bytes) == expected_advance,
            "measurement must retain the advance of valid UTF-8 successors");

        const auto bounds = msdf::measure_text_bounds_px(atlas, draw_height, test_case.bytes);
        ok &= check(bounds.has_visible_glyphs, "decoded UTF-8 must retain visible glyphs");
        ok &= check(bounds.advance_x == expected_advance,
            "bounds must retain the advance of valid UTF-8 successors");
        ok &= check(bounds.left == 1.0f && bounds.right == expected_advance - advance + 4.0f &&
            bounds.top == -7.0f && bounds.bottom == 2.0f,
            "bounds must include every decoded glyph");

        std::vector<msdf::text_vertex_t> vertices;
        std::vector<std::uint32_t> indices;
        msdf::append_text_quads(
            atlas, draw_height, test_case.bytes, 10.0f, 20.0f, vertices, &indices);
        ok &= check(vertices.size() == expected.size() * 4,
            "geometry must retain quads for valid UTF-8 successors");
        ok &= check(indices.size() == expected.size() * 6,
            "geometry must retain indices for valid UTF-8 successors");
    }
    return ok;
}

bool test_whitespace_skips_degenerate_quads()
{
    constexpr int draw_height = 32;
    msdf::atlas_t atlas = unit_scale_atlas(draw_height);

    msdf::glyph_t space;
    space.advance_units = 5.0f;
    space.visible       = false;
    atlas.glyphs.emplace(U' ', space);

    // Outline bounds in Y-up font units. With draw_scale == 1 and zero padding the
    // scaled plane becomes left/right = bounds_left/right and top/bottom =
    // -bounds_bottom/-bounds_top, i.e. plane_top = 2, plane_bottom = -8.
    msdf::glyph_t glyph;
    glyph.advance_units       = 7.0f;
    glyph.bounds_left_units   = 0.0f;
    glyph.bounds_right_units  = 6.0f;
    glyph.bounds_bottom_units = -2.0f;
    glyph.bounds_top_units    = 8.0f;
    glyph.visible             = true;
    glyph.uv_left             = 0.1f;
    glyph.uv_right            = 0.2f;
    glyph.uv_top              = 0.3f;
    glyph.uv_bottom           = 0.4f;
    atlas.glyphs.emplace(U'A', glyph);

    std::vector<msdf::text_vertex_t> vertices;
    std::vector<std::uint32_t> indices;
    msdf::append_text_quads(atlas, draw_height, " A ", 10.0f, 20.0f, vertices, &indices);

    int visited_glyphs = 0;
    float a_pen_x = 0.0f;
    const float final_pen_x = msdf::for_each_positioned_glyph(
        atlas,
        draw_height,
        " A ",
        0.0f,
        [&](const msdf::positioned_glyph_t& positioned) {
            ++visited_glyphs;
            if (positioned.codepoint == U'A') {
                a_pen_x = positioned.pen_x;
            }
        });
    const msdf::text_bounds_t bounds = msdf::measure_text_bounds_px(atlas, draw_height, " A ");

    bool ok = true;
    ok &= check(
        msdf::measure_text_advance_px(atlas, draw_height, " A ") == 17.0f,
        "measurement must include spaces");
    ok &= check(final_pen_x == 17.0f, "layout callback must return final pen advance");
    ok &= check(visited_glyphs == 3, "layout callback must visit invisible advance glyphs");
    ok &= check(a_pen_x == 5.0f, "layout callback must position visible glyph after leading space");
    ok &= check(bounds.has_visible_glyphs, "visual bounds must report visible glyphs");
    ok &= check(bounds.advance_x == 17.0f, "visual bounds must include final advance");
    ok &= check(bounds.left == 5.0f, "visual bounds left edge must ignore leading space");
    ok &= check(bounds.right == 11.0f, "visual bounds right edge must include visible glyph");
    ok &= check(bounds.top == -8.0f, "visual bounds top must be the minimum glyph Y");
    ok &= check(bounds.bottom == 2.0f, "visual bounds bottom must be the maximum glyph Y");
    ok &= check(vertices.size() == 4, "only visible glyphs must emit vertices");
    ok &= check(indices.size() == 6, "only visible glyphs must emit indices");
    if (vertices.size() == 4) {
        ok &= check(vertices[0].x == 15.0f, "visible glyph quad must be positioned after leading space");
    }
    return ok;
}

bool test_two_heights_share_baked_bitmap()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const std::vector<char32_t> codepoints = {U'W'};

    // Both draw heights are below min_atlas_font_size (48), so both bake at the
    // same bitmap scale and must produce a byte-identical bitmap and identical UVs.
    // Only the derived draw-size geometry differs between them.
    const msdf::build_result_t small = build_test_atlas(
        font_data, codepoints, 256, msdf::Missing_glyph_policy::SKIP, 16);
    const msdf::build_result_t large = build_test_atlas(
        font_data, codepoints, 256, msdf::Missing_glyph_policy::SKIP, 32);

    bool ok = true;
    ok &= check(small.status == msdf::Build_status::SUCCESS, "small draw height must build");
    ok &= check(large.status == msdf::Build_status::SUCCESS, "large draw height must build");
    ok &= check(
        small.atlas.baked_pixel_height == 48,
        "sub-minimum draw height must bake at ceil(min_atlas_font_size)");
    ok &= check(
        large.atlas.baked_pixel_height == 48,
        "second sub-minimum draw height must bake at the same height");
    ok &= check(
        small.atlas.bitmap_scale == large.atlas.bitmap_scale,
        "a shared bake bucket must share the bitmap scale");
    ok &= check(
        small.atlas.rgba == large.atlas.rgba,
        "a shared bake bucket must produce byte-identical bitmaps");

    const auto small_w = small.atlas.glyphs.find(U'W');
    const auto large_w = large.atlas.glyphs.find(U'W');
    ok &= check(
        small_w != small.atlas.glyphs.end() && large_w != large.atlas.glyphs.end(),
        "both atlases must contain W");
    if (small_w == small.atlas.glyphs.end() || large_w == large.atlas.glyphs.end()) {
        return false;
    }
    ok &= check(
        small_w->second.uv_left == large_w->second.uv_left &&
            small_w->second.uv_right == large_w->second.uv_right &&
            small_w->second.uv_top == large_w->second.uv_top &&
            small_w->second.uv_bottom == large_w->second.uv_bottom,
        "a shared bake bucket must produce identical glyph UVs");

    // The draw-size geometry still differs: doubling the draw height doubles the
    // scaled advance derived from the same baked data.
    const float advance_small = msdf::scaled_glyph(small.atlas, small_w->second, 16).advance_x;
    const float advance_large = msdf::scaled_glyph(large.atlas, large_w->second, 32).advance_x;
    ok &= check(
        near_equal(advance_large, 2.0f * advance_small),
        "draw-size advance must double when the draw height doubles");
    return ok;
}

bool test_scaled_geometry_is_linear_in_draw_height()
{
    // ascender 10 units, bitmap baked at scale 1, distance range 2, bias 1. Then
    // draw_scale(h) = h / 10 and both the plane padding and shader px_range are
    // linear in h, so doubling the draw height must double every scaled quantity.
    msdf::atlas_t atlas;
    atlas.font_metrics_units.ascender    = 10.0f;
    atlas.font_metrics_units.descender   = -3.0f;
    atlas.font_metrics_units.line_height = 13.0f;
    atlas.font_metrics_units.em_size     = 16.0f;
    atlas.bitmap_scale   = 1.0;
    atlas.atlas_px_range = 2.0;
    atlas.sharpness_bias = 1.0f;

    msdf::glyph_t a;
    a.advance_units       = 9.0f;
    a.bounds_left_units   = 1.0f;
    a.bounds_right_units  = 7.0f;
    a.bounds_bottom_units = -1.0f;
    a.bounds_top_units    = 8.0f;
    a.visible             = true;
    a.uv_left = 0.0f; a.uv_right = 0.1f; a.uv_top = 0.0f; a.uv_bottom = 0.1f;
    atlas.glyphs.emplace(U'A', a);

    msdf::glyph_t b = a;
    b.advance_units = 11.0f;
    atlas.glyphs.emplace(U'B', b);

    atlas.kerning_units.emplace(msdf::make_kerning_key(U'A', U'B'), -2.0f);

    constexpr int h  = 16;
    constexpr int h2 = 32;

    const msdf::scaled_glyph_t sa  = msdf::scaled_glyph(atlas, a, h);
    const msdf::scaled_glyph_t sa2 = msdf::scaled_glyph(atlas, a, h2);

    bool ok = true;
    ok &= check(near_equal(sa2.advance_x, 2.0f * sa.advance_x), "advance must scale linearly");
    ok &= check(near_equal(sa2.plane_left, 2.0f * sa.plane_left), "plane left must scale linearly");
    ok &= check(near_equal(sa2.plane_right, 2.0f * sa.plane_right), "plane right must scale linearly");
    ok &= check(near_equal(sa2.plane_top, 2.0f * sa.plane_top), "plane top must scale linearly");
    ok &= check(near_equal(sa2.plane_bottom, 2.0f * sa.plane_bottom), "plane bottom must scale linearly");
    ok &= check(
        sa2.uv_left == sa.uv_left && sa2.uv_right == sa.uv_right &&
            sa2.uv_top == sa.uv_top && sa2.uv_bottom == sa.uv_bottom,
        "scaling must not change UVs");

    ok &= check(
        near_equal(
            msdf::px_range_for_pixel_height(atlas, h2),
            2.0f * msdf::px_range_for_pixel_height(atlas, h)),
        "shader pixel range must scale linearly with draw height");

    const msdf::font_metrics_px_t m  = msdf::scaled_font_metrics(atlas, h);
    const msdf::font_metrics_px_t m2 = msdf::scaled_font_metrics(atlas, h2);
    ok &= check(near_equal(m2.ascender, 2.0f * m.ascender), "ascender must scale linearly");
    ok &= check(near_equal(m2.descender, 2.0f * m.descender), "descender must scale linearly");
    ok &= check(near_equal(m2.line_height, 2.0f * m.line_height), "line height must scale linearly");
    ok &= check(near_equal(m2.em_size, 2.0f * m.em_size), "em size must scale linearly");

    // Kerning is folded into the pen advance; the kerned "AB" advance must scale
    // linearly too, and the negative kerning must actually shorten it.
    const float advance_h  = msdf::measure_text_advance_px(atlas, h, "AB");
    const float advance_h2 = msdf::measure_text_advance_px(atlas, h2, "AB");
    ok &= check(near_equal(advance_h2, 2.0f * advance_h), "kerned advance must scale linearly");
    const float draw_scale_h = static_cast<float>(h) / 10.0f;
    const float unkerned = (9.0f + 11.0f) * draw_scale_h;
    ok &= check(advance_h < unkerned, "negative kerning must shorten the measured advance");
    return ok;
}

bool test_space_is_advance_only_after_scaling()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const std::vector<char32_t> codepoints = {U' ', U'W'};
    const msdf::build_result_t result = build_test_atlas(font_data, codepoints);

    bool ok = true;
    ok &= check(result.status == msdf::Build_status::SUCCESS, "space plus W must build");
    const auto space_it = result.atlas.glyphs.find(U' ');
    ok &= check(space_it != result.atlas.glyphs.end(), "atlas must contain the space glyph");
    if (space_it == result.atlas.glyphs.end()) {
        return false;
    }

    const msdf::glyph_t& space = space_it->second;
    ok &= check(!space.visible, "space must be an advance-only (non-visible) glyph");
    ok &= check(space.advance_units > 0.0f, "space must carry a positive advance");

    const msdf::scaled_glyph_t scaled =
        msdf::scaled_glyph(result.atlas, space, k_default_draw_pixel_height);
    ok &= check(scaled.advance_x > 0.0f, "scaled space must still advance the pen");
    ok &= check(
        scaled.plane_left == 0.0f && scaled.plane_right == 0.0f &&
            scaled.plane_top == 0.0f && scaled.plane_bottom == 0.0f,
        "scaled space must stay a degenerate (non-drawable) quad");

    std::vector<msdf::text_vertex_t> vertices;
    msdf::append_text_quads(
        result.atlas, k_default_draw_pixel_height, " ", 0.0f, 0.0f, vertices);
    ok &= check(vertices.empty(), "drawing a lone space must emit no vertices");
    return ok;
}

bool test_visible_glyph_has_coverage()
{
    const std::vector<std::uint8_t> font_data = read_test_font();
    const std::vector<char32_t> codepoints = {U'W'};
    const msdf::build_result_t result = build_test_atlas(font_data, codepoints);

    bool ok = true;
    ok &= check(result.status == msdf::Build_status::SUCCESS, "W must build");
    const auto glyph_it = result.atlas.glyphs.find(U'W');
    ok &= check(glyph_it != result.atlas.glyphs.end(), "atlas must contain W");
    if (glyph_it == result.atlas.glyphs.end()) {
        return false;
    }

    const msdf::glyph_t& glyph = glyph_it->second;
    const int atlas_size = result.atlas.atlas_size;
    ok &= check(atlas_size > 0, "atlas must have a positive size");
    if (atlas_size <= 0) {
        return false;
    }

    // Map the glyph UV rectangle back to atlas pixels and inspect the MTSDF alpha
    // channel (true signed distance). A real rasterized glyph crosses its outline
    // inside this rectangle, so the alpha must contain both clearly-inside
    // (> 0.5) and clearly-outside (< 0.5) samples.
    const auto to_pixel = [atlas_size](float uv) {
        const int pixel = static_cast<int>(uv * static_cast<float>(atlas_size));
        return std::clamp(pixel, 0, atlas_size - 1);
    };
    const int x0 = to_pixel(glyph.uv_left);
    const int x1 = to_pixel(glyph.uv_right);
    const int y0 = to_pixel(glyph.uv_top);
    const int y1 = to_pixel(glyph.uv_bottom);
    ok &= check(x1 > x0 && y1 > y0, "glyph UV rectangle must cover at least one pixel");

    std::uint8_t min_alpha = 255;
    std::uint8_t max_alpha = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const std::size_t idx =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(atlas_size) +
                 static_cast<std::size_t>(x)) * 4u + 3u;
            if (idx < result.atlas.rgba.size()) {
                const std::uint8_t alpha = result.atlas.rgba[idx];
                min_alpha = std::min(min_alpha, alpha);
                max_alpha = std::max(max_alpha, alpha);
            }
        }
    }
    ok &= check(max_alpha > 160, "glyph interior must produce high MTSDF alpha coverage");
    ok &= check(min_alpha < 96, "glyph exterior padding must produce low MTSDF alpha coverage");
    return ok;
}

bool test_bake_bucket_rule()
{
    msdf::options_t options;
    options.min_atlas_font_size = 48.0;

    bool ok = true;
    // Draw heights at or below the floor share one bake bucket. A renderer keys
    // its baked atlas on this height, so equal results are a same-bucket cache hit
    // (reuse across zoom) and unequal results are a cross-bucket miss (rebuild).
    ok &= check(
        msdf::msdf_bake_pixel_height(12, options) == 48,
        "a sub-floor draw height bakes at ceil(min_atlas_font_size)");
    ok &= check(
        msdf::msdf_bake_pixel_height(48, options) == 48,
        "a draw height at the floor bakes at the floor");
    ok &= check(
        msdf::msdf_bake_pixel_height(12, options) ==
            msdf::msdf_bake_pixel_height(24, options),
        "two sub-floor draw heights map to one bake bucket (same-bucket hit)");
    ok &= check(
        msdf::msdf_bake_pixel_height(72, options) == 72,
        "an above-floor draw height bakes at the draw size");
    ok &= check(
        msdf::msdf_bake_pixel_height(48, options) !=
            msdf::msdf_bake_pixel_height(72, options),
        "crossing the floor maps to different bake buckets (cross-bucket miss)");
    ok &= check(
        msdf::msdf_bake_pixel_height(72, options) !=
            msdf::msdf_bake_pixel_height(96, options),
        "distinct above-floor draw heights map to distinct bake buckets");
    return ok;
}

bool run_test(const char* name, bool (*test)())
{
    try {
        if (test()) {
            std::cerr << "PASS: " << name << '\n';
            return true;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "FAIL: " << name << ": " << e.what() << '\n';
    }
    catch (...) {
        std::cerr << "FAIL: " << name << ": unknown exception\n";
    }

    return false;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= run_test("utf8 prefix decoding", test_utf8_prefix_decoding);
    ok &= run_test("utf8 prefix layout", test_utf8_prefix_layout);
    ok &= run_test("default codepoints are explicit scalars", test_default_codepoints_are_explicit_scalars);
    ok &= run_test("invalid and duplicate codepoints", test_invalid_and_duplicate_codepoints);
    ok &= run_test("missing glyph failure", test_missing_glyph_failure);
    ok &= run_test("too-small atlas failure", test_too_small_atlas_failure);
    ok &= run_test("partial success keeps usable atlas", test_partial_success_keeps_usable_atlas);
    ok &= run_test("replacement missing glyph policy", test_replacement_missing_glyph_policy);
    ok &= run_test("fail missing glyph policy", test_fail_missing_glyph_policy);
    ok &= run_test("atlas generation smoke", test_atlas_generation_smoke);
    ok &= run_test("whitespace skips degenerate quads", test_whitespace_skips_degenerate_quads);
    ok &= run_test("two heights share baked bitmap", test_two_heights_share_baked_bitmap);
    ok &= run_test("scaled geometry is linear in draw height", test_scaled_geometry_is_linear_in_draw_height);
    ok &= run_test("space is advance-only after scaling", test_space_is_advance_only_after_scaling);
    ok &= run_test("visible glyph has coverage", test_visible_glyph_has_coverage);
    ok &= run_test("bake bucket rule", test_bake_bucket_rule);
    return ok ? 0 : 1;
}
