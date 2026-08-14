#include <vnm_msdf_text/msdf_text.h>

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace vnm {
namespace msdf_text {
namespace {

constexpr char32_t k_replacement_char = 0xFFFD;

bool is_continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

bool is_unicode_scalar_value(char32_t codepoint)
{
    return codepoint <= 0x10FFFF &&
        (codepoint < 0xD800 || codepoint > 0xDFFF);
}

bool is_finite_positive(double value)
{
    return std::isfinite(value) && value > 0.0;
}

struct Freetype_deleter
{
    void operator()(msdfgen::FreetypeHandle* handle) const noexcept
    {
        if (handle) {
            msdfgen::deinitializeFreetype(handle);
        }
    }
};

struct Font_deleter
{
    void operator()(msdfgen::FontHandle* handle) const noexcept
    {
        if (handle) {
            msdfgen::destroyFont(handle);
        }
    }
};

using freetype_ptr = std::unique_ptr<msdfgen::FreetypeHandle, Freetype_deleter>;
using font_ptr     = std::unique_ptr<msdfgen::FontHandle, Font_deleter>;

char32_t utf8_decode_one(const char*& it, const char* end)
{
    if (it >= end) {
        return k_replacement_char;
    }

    const auto* p = reinterpret_cast<const unsigned char*>(it);
    const unsigned char c0 = *p++;

    if ((c0 & 0x80) == 0) {
        it = reinterpret_cast<const char*>(p);
        return static_cast<char32_t>(c0);
    }

    std::size_t sequence_length = 0;
    char32_t codepoint = 0;
    if ((c0 & 0xE0) == 0xC0) {
        sequence_length = 2;
        codepoint = c0 & 0x1F;
    }
    else
    if ((c0 & 0xF0) == 0xE0) {
        sequence_length = 3;
        codepoint = c0 & 0x0F;
    }
    else
    if ((c0 & 0xF8) == 0xF0) {
        sequence_length = 4;
        codepoint = c0 & 0x07;
    }
    else {
        it = reinterpret_cast<const char*>(p);
        return k_replacement_char;
    }

    const auto* sequence_end = reinterpret_cast<const unsigned char*>(end);
    const std::size_t continuation_count = sequence_length - 1;
    if (static_cast<std::size_t>(sequence_end - p) < continuation_count) {
        it = end;
        return k_replacement_char;
    }

    for (std::size_t i = 1; i < sequence_length; ++i) {
        if (!is_continuation(*p)) {
            it = reinterpret_cast<const char*>(p);
            return k_replacement_char;
        }
        codepoint = (codepoint << 6) | (*p++ & 0x3F);
    }

    it = reinterpret_cast<const char*>(p);
    if ((sequence_length == 2 && codepoint < 0x80) ||
        (sequence_length == 3 && codepoint < 0x800) ||
        (sequence_length == 4 && codepoint < 0x10000) ||
        codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF))
    {
        return k_replacement_char;
    }

    return codepoint;
}

std::size_t codepoint_to_utf8(char32_t codepoint, char* out)
{
    auto* p = reinterpret_cast<unsigned char*>(out);

    if (codepoint < 0x80) {
        p[0] = static_cast<unsigned char>(codepoint);
        return 1;
    }
    if (codepoint < 0x800) {
        p[0] = static_cast<unsigned char>(0xC0 | (codepoint >> 6));
        p[1] = static_cast<unsigned char>(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            return 0;
        }
        p[0] = static_cast<unsigned char>(0xE0 | (codepoint >> 12));
        p[1] = static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3F));
        p[2] = static_cast<unsigned char>(0x80 | (codepoint & 0x3F));
        return 3;
    }
    if (codepoint <= 0x10FFFF) {
        p[0] = static_cast<unsigned char>(0xF0 | (codepoint >> 18));
        p[1] = static_cast<unsigned char>(0x80 | ((codepoint >> 12) & 0x3F));
        p[2] = static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3F));
        p[3] = static_cast<unsigned char>(0x80 | (codepoint & 0x3F));
        return 4;
    }

    return 0;
}

void append_range(std::vector<char32_t>& chars, char32_t first, char32_t last)
{
    for (char32_t codepoint = first; codepoint <= last; ++codepoint) {
        chars.push_back(codepoint);
    }
}

void append_codepoints(
    std::vector<char32_t>& chars,
    std::initializer_list<char32_t> codepoints)
{
    chars.insert(chars.end(), codepoints.begin(), codepoints.end());
}

build_result_t failure(const std::string& message)
{
    build_result_t result;
    result.status = Build_status::FAILURE;
    result.message = message;
    return result;
}

std::vector<char32_t> normalized_codepoints(
    std::span<const char32_t> codepoints,
    std::vector<char32_t>& invalid_codepoints)
{
    std::vector<char32_t> normalized;
    normalized.reserve(codepoints.size());

    for (char32_t codepoint : codepoints) {
        if (is_unicode_scalar_value(codepoint)) {
            normalized.push_back(codepoint);
        }
        else {
            invalid_codepoints.push_back(codepoint);
        }
    }

    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());

    std::sort(invalid_codepoints.begin(), invalid_codepoints.end());
    invalid_codepoints.erase(
        std::unique(invalid_codepoints.begin(), invalid_codepoints.end()),
        invalid_codepoints.end());

    return normalized;
}

bool has_partial_build_diagnostics(const build_result_t& result)
{
    return !result.invalid_codepoints.empty() ||
        !result.missing_codepoints.empty() ||
        !result.failed_codepoints.empty() ||
        !result.skipped_too_large.empty() ||
        !result.skipped_no_space.empty() ||
        result.atlas_full;
}

void sort_unique_codepoints(std::vector<char32_t>& codepoints)
{
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
}

void normalize_build_diagnostics(build_result_t& result)
{
    sort_unique_codepoints(result.invalid_codepoints);
    sort_unique_codepoints(result.missing_codepoints);
    sort_unique_codepoints(result.failed_codepoints);
    sort_unique_codepoints(result.skipped_too_large);
    sort_unique_codepoints(result.skipped_no_space);
}

void append_codepoints(
    std::vector<char32_t>& destination,
    const std::vector<char32_t>& source)
{
    destination.insert(destination.end(), source.begin(), source.end());
}

struct Glyph_group
{
    msdfgen::GlyphIndex glyph_index;
    std::vector<char32_t> codepoints;
};

struct Glyph_job
{
    msdfgen::GlyphIndex glyph_index;
    std::vector<char32_t> codepoints;
    msdfgen::Shape shape;
    msdfgen::Shape::Bounds bounds{};
    double advance = 0.0;
    int bitmap_w = 0;
    int bitmap_h = 0;
};

struct Emitted_glyph
{
    char32_t codepoint = 0;
    msdfgen::GlyphIndex glyph_index;
};

struct Draw_scaling
{
    double draw_scale = 0.0;
    // atlas_px_range * screen_to_atlas_ratio, in output pixels: the symmetric
    // padding added around the glyph outline bounds at the draw size.
    double pad = 0.0;
};

Draw_scaling draw_scaling_for(const atlas_t& atlas, int draw_pixel_height)
{
    const double ascender = atlas.font_metrics_units.ascender;
    const double draw_scale = (ascender > 0.0)
        ? static_cast<double>(draw_pixel_height) / ascender
        : 0.0;
    const double screen_to_atlas_ratio = (atlas.bitmap_scale > 0.0)
        ? draw_scale / atlas.bitmap_scale
        : 0.0;
    return Draw_scaling{draw_scale, atlas.atlas_px_range * screen_to_atlas_ratio};
}

scaled_glyph_t scale_glyph_with(const glyph_t& glyph, const Draw_scaling& scaling)
{
    scaled_glyph_t scaled;
    scaled.advance_x = static_cast<float>(glyph.advance_units * scaling.draw_scale);
    scaled.uv_left   = glyph.uv_left;
    scaled.uv_bottom = glyph.uv_bottom;
    scaled.uv_right  = glyph.uv_right;
    scaled.uv_top    = glyph.uv_top;
    // Only visible glyphs gain the padded plane rectangle. Advance-only glyphs
    // such as U+0020 stay degenerate so they emit no quad after scaling.
    if (glyph.visible) {
        scaled.plane_left = static_cast<float>(
            glyph.bounds_left_units * scaling.draw_scale - scaling.pad);
        scaled.plane_right = static_cast<float>(
            glyph.bounds_right_units * scaling.draw_scale + scaling.pad);
        scaled.plane_top = static_cast<float>(
            -glyph.bounds_bottom_units * scaling.draw_scale + scaling.pad);
        scaled.plane_bottom = static_cast<float>(
            -glyph.bounds_top_units * scaling.draw_scale - scaling.pad);
    }
    return scaled;
}

template <class Visitor>
float for_each_positioned_glyph_impl(
    const atlas_t& atlas,
    int draw_pixel_height,
    std::string_view text,
    float start_x,
    Visitor&& visit)
{
    const Draw_scaling scaling = draw_scaling_for(atlas, draw_pixel_height);
    float pen_x = start_x;
    char32_t previous = 0;
    const char* it = text.data();
    const char* end = it + text.size();
    while (it < end) {
        const char32_t codepoint = utf8_decode_one(it, end);
        const auto glyph_it = atlas.glyphs.find(codepoint);
        if (glyph_it == atlas.glyphs.end()) {
            continue;
        }
        if (previous != 0) {
            const auto kerning_it =
                atlas.kerning_units.find(make_kerning_key(previous, codepoint));
            if (kerning_it != atlas.kerning_units.end()) {
                pen_x += static_cast<float>(kerning_it->second * scaling.draw_scale);
            }
        }
        const scaled_glyph_t scaled = scale_glyph_with(glyph_it->second, scaling);
        visit(codepoint, pen_x, scaled);
        pen_x += scaled.advance_x;
        previous = codepoint;
    }
    return pen_x;
}

} // namespace

std::vector<char32_t> default_codepoints()
{
    static const std::vector<char32_t> codepoints = [] {
        std::vector<char32_t> chars;
        append_range(chars, U' ', U'~');

        append_range(chars, 0x00C0, 0x00D6);
        append_range(chars, 0x00D8, 0x00F6);
        append_range(chars, 0x00F8, 0x00FF);
        append_codepoints(chars, {
            0x0152, 0x0153, 0x0160, 0x0161, 0x0178, 0x0192,
        });

        append_range(chars, 0x0391, 0x03A1);
        append_range(chars, 0x03A3, 0x03A9);
        append_range(chars, 0x03B1, 0x03C1);
        append_range(chars, 0x03C3, 0x03C9);
        append_codepoints(chars, {
            0x0386, 0x0388, 0x0389, 0x038A, 0x038C, 0x038E, 0x038F,
            0x0390, 0x03AA, 0x03AB, 0x03AC, 0x03AD, 0x03AE, 0x03AF,
            0x03B0, 0x03C2, 0x03CA, 0x03CB, 0x03CC, 0x03CD, 0x03CE,
        });

        append_codepoints(chars, {
            0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00AB, 0x00BB,
            0x060B, 0x09F2, 0x09F3, 0x09FB, 0x0AF1, 0x0BF9, 0x0E3F,
            0x17DB, 0x20A0, 0x20A1, 0x20A2, 0x20A3, 0x20A4, 0x20A5,
            0x20A6, 0x20A7, 0x20A8, 0x20A9, 0x20AA, 0x20AB, 0x20AC,
            0x20AD, 0x20AE, 0x20AF, 0x20B0, 0x20B1, 0x20B2, 0x20B3,
            0x20B4, 0x20B5, 0x20B8, 0x20B9, 0x20BA, 0x20BC, 0x20BD,
            0x20BE, 0xFDFC, 0xFE69, 0xFF04, 0xFFE0, 0xFFE1, 0xFFE5,
            0xFFE6,
        });

        append_codepoints(chars, {
            0x2610, 0x2611, 0x2612, 0x2715, 0xFFFD,
            0x0387, 0x1F518, 0x1F5D5, 0x1F5D6, 0x1F5D7,
        });
        std::sort(chars.begin(), chars.end());
        chars.erase(std::unique(chars.begin(), chars.end()), chars.end());
        return chars;
    }();
    return codepoints;
}

std::vector<char32_t> utf8_to_codepoints(std::string_view text)
{
    std::vector<char32_t> result;
    if (text.empty()) {
        return result;
    }
    result.reserve(text.size());
    const char* it = text.data();
    const char* end = it + text.size();
    while (it < end) {
        result.push_back(utf8_decode_one(it, end));
    }
    return result;
}

std::string codepoints_to_utf8(std::span<const char32_t> codepoints)
{
    std::string result;
    result.reserve(codepoints.size() * 2);

    char buffer[4];
    for (char32_t codepoint : codepoints) {
        const std::size_t length = codepoint_to_utf8(codepoint, buffer);
        if (length > 0) {
            result.append(buffer, length);
        }
    }
    return result;
}

build_result_t build_font_atlas(
    const std::uint8_t* font_data,
    std::size_t font_size,
    int draw_pixel_height,
    std::span<const char32_t> codepoints,
    const options_t& options,
    const log_callback_t& log_debug_info)
{
    if (!font_data || font_size == 0) {
        return failure("No font data supplied");
    }
    if (font_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return failure("Font data is too large for msdfgen");
    }
    if (draw_pixel_height <= 0) {
        return failure("Font pixel height must be positive");
    }
    if (options.atlas_size <= 0) {
        return failure("MSDF atlas size must be positive");
    }
    if (!is_finite_positive(options.min_atlas_font_size)) {
        return failure("Minimum atlas font size must be finite and positive");
    }
    if (!is_finite_positive(options.atlas_px_range)) {
        return failure("MSDF atlas pixel range must be finite and positive");
    }
    if (!is_finite_positive(options.sharpness_bias)) {
        return failure("MSDF sharpness bias must be finite and positive");
    }
    if (options.atlas_gutter_px < 0 ||
        options.atlas_gutter_px > options.atlas_size)
    {
        return failure("MSDF atlas gutter must be between zero and atlas size");
    }

    std::vector<char32_t> invalid_codepoints;
    const std::vector<char32_t> requested_codepoints =
        normalized_codepoints(codepoints, invalid_codepoints);
    if (requested_codepoints.empty()) {
        build_result_t result = failure("No valid codepoints supplied");
        result.invalid_codepoints = std::move(invalid_codepoints);
        return result;
    }

    const std::size_t atlas_size = static_cast<std::size_t>(options.atlas_size);
    if (atlas_size > std::numeric_limits<std::size_t>::max() / atlas_size) {
        return failure("MSDF atlas pixel count overflows size_t");
    }
    const std::size_t atlas_pixel_count = atlas_size * atlas_size;
    if (atlas_pixel_count > std::numeric_limits<std::size_t>::max() / 4u) {
        return failure("MSDF atlas byte size overflows size_t");
    }
    const std::size_t atlas_byte_count = atlas_pixel_count * 4u;

    freetype_ptr ft(msdfgen::initializeFreetype());
    if (!ft) {
        return failure("Failed to initialize FreeType for msdfgen");
    }

    font_ptr font_handle(msdfgen::loadFontData(
        ft.get(),
        reinterpret_cast<const msdfgen::byte*>(font_data),
        static_cast<int>(font_size)));
    if (!font_handle) {
        return failure("Failed to load font data for msdfgen");
    }

    msdfgen::FontMetrics metrics{};
    // Keep the msdfgen default font-coordinate scaling: the atlas projection,
    // plane rectangles, and shader px_range are calibrated as one unit.
    if (!msdfgen::getFontMetrics(metrics, font_handle.get())) {
        return failure("Failed to query font metrics for msdfgen");
    }
    if (!is_finite_positive(metrics.ascenderY) ||
        !std::isfinite(metrics.descenderY) ||
        !std::isfinite(metrics.lineHeight) ||
        !std::isfinite(metrics.emSize))
    {
        return failure("Font metrics are not finite and usable");
    }

    build_result_t result;
    result.invalid_codepoints = std::move(invalid_codepoints);
    const auto fail_with_diagnostics = [&](const std::string& message) {
        result.status = Build_status::FAILURE;
        result.message = message;
        result.atlas = atlas_t{};
        return result;
    };

    // The bitmap is baked at bitmap_scale (font units -> atlas pixels). All
    // draw-size geometry (advances, plane rectangles, kerning, metrics, and the
    // shader px_range) is derived from the baked font-unit data at draw time by
    // the scaling helpers, so one baked atlas serves many draw pixel heights.
    const double atlas_px_range = options.atlas_px_range;
    const double bitmap_scale =
        std::max(static_cast<double>(draw_pixel_height), options.min_atlas_font_size) /
        metrics.ascenderY;
    if (!is_finite_positive(bitmap_scale)) {
        return failure("Font scaling values are not finite and usable");
    }

    std::vector<Glyph_group> glyph_groups;
    glyph_groups.reserve(requested_codepoints.size());
    std::unordered_map<unsigned, std::size_t> glyph_group_by_index;
    glyph_group_by_index.reserve(requested_codepoints.size());

    const auto add_codepoint_to_group =
        [&](char32_t codepoint, msdfgen::GlyphIndex glyph_index) {
            const unsigned glyph_index_value = glyph_index.getIndex();
            const auto [it, inserted] =
                glyph_group_by_index.emplace(glyph_index_value, glyph_groups.size());
            if (inserted) {
                Glyph_group group;
                group.glyph_index = glyph_index;
                glyph_groups.push_back(std::move(group));
            }
            glyph_groups[it->second].codepoints.push_back(codepoint);
        };

    msdfgen::GlyphIndex replacement_glyph_index;
    const bool replacement_glyph_available =
        msdfgen::getGlyphIndex(
            replacement_glyph_index,
            font_handle.get(),
            static_cast<msdfgen::unicode_t>(k_replacement_char));

    for (char32_t codepoint : requested_codepoints) {
        const auto unicode = static_cast<msdfgen::unicode_t>(codepoint);

        msdfgen::GlyphIndex glyph_index;
        if (!msdfgen::getGlyphIndex(glyph_index, font_handle.get(), unicode)) {
            result.missing_codepoints.push_back(codepoint);
            if (options.missing_glyph_policy == Missing_glyph_policy::USE_REPLACEMENT_CHARACTER &&
                replacement_glyph_available)
            {
                add_codepoint_to_group(codepoint, replacement_glyph_index);
            }
            continue;
        }

        add_codepoint_to_group(codepoint, glyph_index);
    }

    if (options.missing_glyph_policy == Missing_glyph_policy::FAIL_BUILD &&
        !result.missing_codepoints.empty())
    {
        normalize_build_diagnostics(result);
        return fail_with_diagnostics("Font is missing required glyphs");
    }

    atlas_t& atlas = result.atlas;
    atlas.baked_pixel_height = msdf_bake_pixel_height(draw_pixel_height, options);
    atlas.atlas_size = options.atlas_size;
    atlas.atlas_px_range = atlas_px_range;
    atlas.bitmap_scale = bitmap_scale;
    atlas.sharpness_bias = options.sharpness_bias;
    atlas.font_metrics_units.ascender = static_cast<float>(metrics.ascenderY);
    atlas.font_metrics_units.descender = static_cast<float>(metrics.descenderY);
    atlas.font_metrics_units.line_height = static_cast<float>(metrics.lineHeight);
    atlas.font_metrics_units.em_size = static_cast<float>(metrics.emSize);

    if (atlas_byte_count > atlas.rgba.max_size()) {
        return fail_with_diagnostics("MSDF atlas storage exceeds vector capacity");
    }
    try {
        atlas.rgba.assign(atlas_byte_count, 0);
    }
    catch (const std::bad_alloc&) {
        return fail_with_diagnostics("Failed to allocate MSDF atlas storage");
    }

    int pen_x = 0;
    int pen_y = 0;
    int row_h = 0;
    const int atlas_gutter_px = options.atlas_gutter_px;
    std::vector<Emitted_glyph> emitted_glyphs;
    emitted_glyphs.reserve(requested_codepoints.size());
    std::vector<Glyph_job> visible_jobs;
    visible_jobs.reserve(glyph_groups.size());

    for (const Glyph_group& group : glyph_groups) {
        msdfgen::Shape shape;
        double advance = 0.0;
        if (!msdfgen::loadGlyph(
                shape,
                font_handle.get(),
                group.glyph_index,
                &advance))
        {
            append_codepoints(result.failed_codepoints, group.codepoints);
            continue;
        }
        if (!std::isfinite(advance)) {
            append_codepoints(result.failed_codepoints, group.codepoints);
            continue;
        }

        if (shape.edgeCount() > 0) {
            msdfgen::edgeColoringSimple(shape, 3.0, 0);
        }

        const auto bounds = shape.getBounds();
        const double width_em = bounds.r - bounds.l;
        const double height_em = bounds.t - bounds.b;
        if (!std::isfinite(width_em) || !std::isfinite(height_em)) {
            append_codepoints(result.failed_codepoints, group.codepoints);
            continue;
        }

        if (height_em <= 0.0 || width_em <= 0.0) {
            if (advance > 0.0) {
                glyph_t glyph;
                glyph.advance_units = static_cast<float>(advance);
                glyph.visible = false;
                for (char32_t codepoint : group.codepoints) {
                    atlas.glyphs.emplace(codepoint, glyph);
                    emitted_glyphs.push_back(Emitted_glyph{codepoint, group.glyph_index});
                    if (codepoint == static_cast<char32_t>('0')) {
                        atlas.zero_advance_units = glyph.advance_units;
                        atlas.zero_advance_available = glyph.advance_units > 0.0f;
                    }
                }
            }
            continue;
        }

        const double bitmap_width_px  = width_em  * bitmap_scale + atlas_px_range * 2.0;
        const double bitmap_height_px = height_em * bitmap_scale + atlas_px_range * 2.0;
        if (!std::isfinite(bitmap_width_px) ||
            !std::isfinite(bitmap_height_px) ||
            bitmap_width_px > static_cast<double>(std::numeric_limits<int>::max()) ||
            bitmap_height_px > static_cast<double>(std::numeric_limits<int>::max()))
        {
            append_codepoints(result.skipped_too_large, group.codepoints);
            continue;
        }

        const int bitmap_w = static_cast<int>(std::ceil(bitmap_width_px));
        const int bitmap_h = static_cast<int>(std::ceil(bitmap_height_px));
        if (bitmap_w <= 0 ||
            bitmap_h <= 0 ||
            bitmap_w > options.atlas_size ||
            bitmap_h > options.atlas_size)
        {
            append_codepoints(result.skipped_too_large, group.codepoints);
            continue;
        }

        Glyph_job job;
        job.glyph_index = group.glyph_index;
        job.codepoints = group.codepoints;
        job.shape = std::move(shape);
        job.bounds = bounds;
        job.advance = advance;
        job.bitmap_w = bitmap_w;
        job.bitmap_h = bitmap_h;
        visible_jobs.push_back(std::move(job));
    }

    std::sort(
        visible_jobs.begin(),
        visible_jobs.end(),
        [](const Glyph_job& left, const Glyph_job& right) {
            if (left.bitmap_h != right.bitmap_h) {
                return left.bitmap_h > right.bitmap_h;
            }
            if (left.bitmap_w != right.bitmap_w) {
                return left.bitmap_w > right.bitmap_w;
            }
            const char32_t left_codepoint =
                left.codepoints.empty() ? 0 : left.codepoints.front();
            const char32_t right_codepoint =
                right.codepoints.empty() ? 0 : right.codepoints.front();
            return left_codepoint < right_codepoint;
        });

    for (std::size_t i = 0; i < visible_jobs.size(); ++i) {
        const Glyph_job& job = visible_jobs[i];
        const double width_em = job.bounds.r - job.bounds.l;
        const double height_em = job.bounds.t - job.bounds.b;

        if (pen_x > options.atlas_size - job.bitmap_w) {
            pen_x = 0;
            pen_y += row_h + atlas_gutter_px;
            row_h = 0;
        }
        if (pen_y > options.atlas_size - job.bitmap_h) {
            result.atlas_full = true;
            for (std::size_t j = i; j < visible_jobs.size(); ++j) {
                append_codepoints(result.skipped_no_space, visible_jobs[j].codepoints);
            }
            if (log_debug_info) {
                log_debug_info("MSDF atlas out of space, skipping remaining glyphs");
            }
            break;
        }

        msdfgen::Bitmap<float, 4> bitmap(job.bitmap_w, job.bitmap_h);
        const msdfgen::Vector2 msdf_scale(bitmap_scale, bitmap_scale);
        const msdfgen::Vector2 msdf_translate(
            -job.bounds.l + (atlas_px_range / bitmap_scale),
            -job.bounds.b + (atlas_px_range / bitmap_scale));
        const msdfgen::Projection projection(msdf_scale, msdf_translate);
        // msdfgen's range parameter is in shape units while atlas_px_range is
        // calibrated in atlas pixels (px_range_for_pixel_height() decodes the
        // texel slope as 1/atlas_px_range per atlas pixel), so convert through
        // bitmap_scale. Passing atlas pixels directly made the encoded distance
        // slope font-dependent: faces with a small font-unit ascender (legacy
        // CJK fonts with unitsPerEm 256, e.g. MS Gothic and SimSun) baked SDFs
        // so shallow that every edge decoded as a multi-pixel gray ramp.
        msdfgen::generateMTSDF(
            bitmap,
            job.shape,
            projection,
            options.atlas_px_range / bitmap_scale);

        for (int y = 0; y < job.bitmap_h; ++y) {
            for (int x = 0; x < job.bitmap_w; ++x) {
                const auto& pixel = bitmap(x, y);
                const std::size_t dst_idx =
                    (static_cast<std::size_t>(pen_y + y) * atlas_size +
                     static_cast<std::size_t>(pen_x + x)) * 4u;
                for (int c = 0; c < 4; ++c) {
                    const float normalized = std::max(0.0f, std::min(pixel[c], 1.0f));
                    atlas.rgba[dst_idx + static_cast<std::size_t>(c)] =
                        static_cast<std::uint8_t>(std::round(normalized * 255.0f));
                }
            }
        }

        glyph_t glyph;
        glyph.advance_units       = static_cast<float>(job.advance);
        glyph.bounds_left_units   = static_cast<float>(job.bounds.l);
        glyph.bounds_bottom_units = static_cast<float>(job.bounds.b);
        glyph.bounds_right_units  = static_cast<float>(job.bounds.r);
        glyph.bounds_top_units    = static_cast<float>(job.bounds.t);
        glyph.visible             = true;

        const double uv_width = width_em * bitmap_scale + 2.0 * atlas_px_range;
        const double uv_height = height_em * bitmap_scale + 2.0 * atlas_px_range;

        glyph.uv_left   = static_cast<float>(pen_x)            / options.atlas_size;
        glyph.uv_right  = static_cast<float>(pen_x + uv_width) / options.atlas_size;
        glyph.uv_top    = static_cast<float>(pen_y)             / options.atlas_size;
        glyph.uv_bottom = static_cast<float>(pen_y + uv_height) / options.atlas_size;

        for (char32_t codepoint : job.codepoints) {
            atlas.glyphs.emplace(codepoint, glyph);
            emitted_glyphs.push_back(Emitted_glyph{codepoint, job.glyph_index});
            if (codepoint == static_cast<char32_t>('0')) {
                atlas.zero_advance_units = glyph.advance_units;
                atlas.zero_advance_available = glyph.advance_units > 0.0f;
            }
        }

        row_h = std::max(row_h, job.bitmap_h);
        pen_x += job.bitmap_w + atlas_gutter_px;
    }

    if (options.build_kerning_table) {
        for (const Emitted_glyph& left : emitted_glyphs) {
            for (const Emitted_glyph& right : emitted_glyphs) {
                double k = 0.0;
                if (msdfgen::getKerning(
                        k,
                        font_handle.get(),
                        left.glyph_index,
                        right.glyph_index))
                {
                    const float kern_units = static_cast<float>(k);
                    if (kern_units != 0.0f) {
                        atlas.kerning_units.emplace(
                            make_kerning_key(left.codepoint, right.codepoint),
                            kern_units);
                    }
                }
            }
        }
    }

    normalize_build_diagnostics(result);
    if (atlas.glyphs.empty()) {
        return fail_with_diagnostics("No requested glyphs could be added to the MSDF atlas");
    }
    else
    if (has_partial_build_diagnostics(result)) {
        result.status = Build_status::PARTIAL_SUCCESS;
        result.message = "MSDF atlas built with skipped codepoints";
    }
    else {
        result.status = Build_status::SUCCESS;
        result.message = "MSDF atlas built";
    }

    return result;
}

int msdf_bake_pixel_height(int draw_pixel_height, const options_t& options)
{
    return std::max(
        draw_pixel_height,
        static_cast<int>(std::ceil(options.min_atlas_font_size)));
}

float px_range_for_pixel_height(const atlas_t& atlas, int draw_pixel_height)
{
    // draw_scaling_for() computes pad == atlas_px_range * screen_to_atlas_ratio,
    // so the shader range is the symmetric plane padding scaled by the sharpness
    // bias. Deriving it from the same draw-scaling core keeps one canonical
    // implementation of the draw-size projection.
    return static_cast<float>(
        draw_scaling_for(atlas, draw_pixel_height).pad *
        static_cast<double>(atlas.sharpness_bias));
}

scaled_glyph_t scaled_glyph(
    const atlas_t& atlas,
    const glyph_t& glyph,
    int draw_pixel_height)
{
    return scale_glyph_with(glyph, draw_scaling_for(atlas, draw_pixel_height));
}

font_metrics_px_t scaled_font_metrics(const atlas_t& atlas, int draw_pixel_height)
{
    const double draw_scale = draw_scaling_for(atlas, draw_pixel_height).draw_scale;
    font_metrics_px_t metrics;
    metrics.ascender =
        static_cast<float>(atlas.font_metrics_units.ascender * draw_scale);
    metrics.descender =
        static_cast<float>(atlas.font_metrics_units.descender * draw_scale);
    metrics.line_height =
        static_cast<float>(atlas.font_metrics_units.line_height * draw_scale);
    metrics.em_size =
        static_cast<float>(atlas.font_metrics_units.em_size * draw_scale);
    return metrics;
}

float measure_text_advance_px(
    const atlas_t& atlas,
    int draw_pixel_height,
    std::string_view text)
{
    return for_each_positioned_glyph_impl(
        atlas, draw_pixel_height, text, 0.0f,
        [](char32_t, float, const scaled_glyph_t&) {});
}

float for_each_positioned_glyph(
    const atlas_t& atlas,
    int draw_pixel_height,
    std::string_view text,
    float start_x,
    const layout_callback_t& callback)
{
    return for_each_positioned_glyph_impl(
        atlas,
        draw_pixel_height,
        text,
        start_x,
        [&](char32_t codepoint, float pen_x, const scaled_glyph_t& glyph) {
            if (callback) {
                callback(positioned_glyph_t{codepoint, glyph, pen_x});
            }
        });
}

text_bounds_t measure_text_bounds_px(
    const atlas_t& atlas,
    int draw_pixel_height,
    std::string_view text)
{
    text_bounds_t bounds;
    bounds.advance_x = for_each_positioned_glyph_impl(
        atlas,
        draw_pixel_height,
        text,
        0.0f,
        [&](char32_t, float pen_x, const scaled_glyph_t& glyph) {
            if (glyph.plane_left == glyph.plane_right ||
                glyph.plane_bottom == glyph.plane_top)
            {
                return;
            }

            const float left = pen_x + glyph.plane_left;
            const float right = pen_x + glyph.plane_right;
            const float top = std::min(glyph.plane_bottom, glyph.plane_top);
            const float bottom = std::max(glyph.plane_bottom, glyph.plane_top);

            if (!bounds.has_visible_glyphs) {
                bounds.left = left;
                bounds.right = right;
                bounds.top = top;
                bounds.bottom = bottom;
                bounds.has_visible_glyphs = true;
                return;
            }

            bounds.left = std::min(bounds.left, left);
            bounds.right = std::max(bounds.right, right);
            bounds.top = std::min(bounds.top, top);
            bounds.bottom = std::max(bounds.bottom, bottom);
        });
    return bounds;
}

void append_text_quads(
    const atlas_t& atlas,
    int draw_pixel_height,
    std::string_view text,
    float x,
    float y,
    std::vector<text_vertex_t>& vertices,
    std::vector<std::uint32_t>* indices)
{
    if (text.size() <= (vertices.max_size() - vertices.size()) / 4u) {
        vertices.reserve(vertices.size() + text.size() * 4u);
    }
    if (indices &&
        text.size() <= (indices->max_size() - indices->size()) / 6u)
    {
        indices->reserve(indices->size() + text.size() * 6u);
    }

    for_each_positioned_glyph_impl(
        atlas, draw_pixel_height, text, x,
        [&](char32_t, float pen_x, const scaled_glyph_t& glyph) {
            if (glyph.plane_left == glyph.plane_right ||
                glyph.plane_bottom == glyph.plane_top)
            {
                return;
            }

            const float x0 = pen_x + glyph.plane_left;
            const float x1 = pen_x + glyph.plane_right;
            const float y0 = y + glyph.plane_bottom;
            const float y1 = y + glyph.plane_top;
            const float s_min = std::min(glyph.uv_left, glyph.uv_right);
            const float s_max = std::max(glyph.uv_left, glyph.uv_right);
            const float t_min = std::min(glyph.uv_top, glyph.uv_bottom);
            const float t_max = std::max(glyph.uv_top, glyph.uv_bottom);

            std::uint32_t base = 0;
            if (indices) {
                if (vertices.size() >
                    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - 4u)
                {
                    throw std::length_error("MSDF text index buffer exceeds uint32_t capacity");
                }
                base = static_cast<std::uint32_t>(vertices.size());
            }

            vertices.push_back({x0, y0, glyph.uv_left,  glyph.uv_bottom, s_min, t_min, s_max, t_max});
            vertices.push_back({x0, y1, glyph.uv_left,  glyph.uv_top,    s_min, t_min, s_max, t_max});
            vertices.push_back({x1, y1, glyph.uv_right, glyph.uv_top,    s_min, t_min, s_max, t_max});
            vertices.push_back({x1, y0, glyph.uv_right, glyph.uv_bottom, s_min, t_min, s_max, t_max});

            if (indices) {
                indices->push_back(base);
                indices->push_back(base + 1);
                indices->push_back(base + 2);
                indices->push_back(base);
                indices->push_back(base + 2);
                indices->push_back(base + 3);
            }
        });
}

} // namespace msdf_text
} // namespace vnm
