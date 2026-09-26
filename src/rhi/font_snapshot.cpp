#include <vnm_msdf_text/rhi/font_snapshot.h>

#include <QtCore/QByteArrayView>
#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace vnm::msdf_text::rhi {
namespace {

void hash_u32(QCryptographicHash& hash, std::uint32_t value)
{
    const char bytes[4] = {
        static_cast<char>((value >> 24) & 0xFFu),
        static_cast<char>((value >> 16) & 0xFFu),
        static_cast<char>((value >>  8) & 0xFFu),
        static_cast<char>( value        & 0xFFu),
    };
    hash.addData(QByteArrayView(bytes, sizeof(bytes)));
}

void hash_f32(QCryptographicHash& hash, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hash_u32(hash, bits);
}

void hash_f64(QCryptographicHash& hash, double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hash_u32(hash, static_cast<std::uint32_t>(bits >> 32));
    hash_u32(hash, static_cast<std::uint32_t>(bits & 0xFFFFFFFFu));
}

font_identity_t finish_digest(QCryptographicHash& hash)
{
    const QByteArray digest = hash.result();
    font_identity_t identity;
    std::memcpy(identity.digest.data(), digest.constData(), identity.digest.size());
    return identity;
}

bool valid_codepoint(char32_t codepoint)
{
    return codepoint <= 0x10ffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff);
}

bool valid_build(const build_result_t& build)
{
    if (build.status != Build_status::SUCCESS && build.status != Build_status::PARTIAL_SUCCESS) {
        return false;
    }
    const bool partial =
        !build.invalid_codepoints.empty() || !build.missing_codepoints.empty() ||
        !build.failed_codepoints.empty() || !build.skipped_too_large.empty() ||
        !build.skipped_no_space.empty() || build.atlas_full;
    if ((build.status == Build_status::PARTIAL_SUCCESS) != partial) {
        return false;
    }
    const atlas_t& atlas = build.atlas;
    if (atlas.atlas_size <= 0 || atlas.baked_pixel_height <= 0 || atlas.glyphs.empty()) {
        return false;
    }
    const auto size = static_cast<std::uint64_t>(atlas.atlas_size);
    if (size * size > UINT64_MAX / 4u || size * size * 4u != atlas.rgba.size()) {
        return false;
    }
    if (!std::isfinite(atlas.atlas_px_range) || atlas.atlas_px_range <= 0.0 ||
        !std::isfinite(atlas.bitmap_scale) || atlas.bitmap_scale <= 0.0 ||
        !std::isfinite(atlas.sharpness_bias) || atlas.sharpness_bias <= 0.0f ||
        !std::isfinite(atlas.font_metrics_units.ascender) || atlas.font_metrics_units.ascender <= 0.0f ||
        !std::isfinite(atlas.font_metrics_units.descender) ||
        !std::isfinite(atlas.font_metrics_units.line_height) ||
        !std::isfinite(atlas.font_metrics_units.em_size) ||
        !std::isfinite(atlas.zero_advance_units))
    {
        return false;
    }
    for (const auto& [codepoint, glyph] : atlas.glyphs) {
        if (!valid_codepoint(codepoint) ||
            !std::isfinite(glyph.advance_units) ||
            !std::isfinite(glyph.bounds_left_units) || !std::isfinite(glyph.bounds_right_units) ||
            !std::isfinite(glyph.bounds_bottom_units) || !std::isfinite(glyph.bounds_top_units) ||
            !std::isfinite(glyph.uv_left) || !std::isfinite(glyph.uv_right) ||
            !std::isfinite(glyph.uv_bottom) || !std::isfinite(glyph.uv_top))
        {
            return false;
        }
        if (glyph.visible &&
            (glyph.bounds_left_units > glyph.bounds_right_units ||
             glyph.bounds_bottom_units > glyph.bounds_top_units ||
             glyph.uv_left < 0.0f || glyph.uv_right > 1.0f || glyph.uv_left >= glyph.uv_right ||
             glyph.uv_top < 0.0f || glyph.uv_bottom > 1.0f || glyph.uv_top >= glyph.uv_bottom))
        {
            return false;
        }
    }
    for (const auto& [key, value] : atlas.kerning_units) {
        if (!valid_codepoint(static_cast<char32_t>(key >> 32)) ||
            !valid_codepoint(static_cast<char32_t>(key & 0xffffffffu)) || !std::isfinite(value))
        {
            return false;
        }
    }
    return true;
}

// Hash actual drawable data, so a restored atlas cannot claim an identity that
// describes unrelated bytes. Sorted map keys make cache round-trips independent
// of insertion order. Coverage diagnostics remain observable but do not affect
// the identity of the geometry that was successfully produced.
font_identity_t digest_atlas(const atlas_t& atlas)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash_u32(hash, k_font_bake_compatibility_version);
    hash_u32(hash, static_cast<std::uint32_t>(atlas.baked_pixel_height));
    hash_u32(hash, static_cast<std::uint32_t>(atlas.atlas_size));
    hash_f64(hash, atlas.atlas_px_range);
    hash_f64(hash, atlas.bitmap_scale);
    hash_f32(hash, atlas.sharpness_bias);
    hash_f32(hash, atlas.font_metrics_units.ascender);
    hash_f32(hash, atlas.font_metrics_units.descender);
    hash_f32(hash, atlas.font_metrics_units.line_height);
    hash_f32(hash, atlas.font_metrics_units.em_size);
    hash_f32(hash, atlas.zero_advance_units);
    hash_u32(hash, atlas.zero_advance_available ? 1u : 0u);
    hash.addData(QByteArrayView(
        reinterpret_cast<const char*>(atlas.rgba.data()),
        static_cast<qsizetype>(atlas.rgba.size())));

    std::vector<char32_t> codepoints;
    codepoints.reserve(atlas.glyphs.size());
    for (const auto& [codepoint, glyph] : atlas.glyphs) {
        codepoints.push_back(codepoint);
    }
    std::sort(codepoints.begin(), codepoints.end());
    hash_u32(hash, static_cast<std::uint32_t>(codepoints.size()));
    for (char32_t codepoint : codepoints) {
        const glyph_t& glyph = atlas.glyphs.at(codepoint);
        hash_u32(hash, static_cast<std::uint32_t>(codepoint));
        hash_f32(hash, glyph.advance_units);
        hash_f32(hash, glyph.bounds_left_units);
        hash_f32(hash, glyph.bounds_bottom_units);
        hash_f32(hash, glyph.bounds_right_units);
        hash_f32(hash, glyph.bounds_top_units);
        hash_u32(hash, glyph.visible ? 1u : 0u);
        hash_f32(hash, glyph.uv_left);
        hash_f32(hash, glyph.uv_bottom);
        hash_f32(hash, glyph.uv_right);
        hash_f32(hash, glyph.uv_top);
    }

    std::vector<kerning_key_t> pairs;
    pairs.reserve(atlas.kerning_units.size());
    for (const auto& [key, value] : atlas.kerning_units) {
        pairs.push_back(key);
    }
    std::sort(pairs.begin(), pairs.end());
    hash_u32(hash, static_cast<std::uint32_t>(pairs.size()));
    for (kerning_key_t key : pairs) {
        hash_u32(hash, static_cast<std::uint32_t>(key >> 32));
        hash_u32(hash, static_cast<std::uint32_t>(key & 0xffffffffu));
        hash_f32(hash, atlas.kerning_units.at(key));
    }
    return finish_digest(hash);
}

font_identity_t digest_layout(const Baked_font& font, int draw_pixel_height)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(
        reinterpret_cast<const char*>(font.identity().digest.data()),
        static_cast<qsizetype>(font.identity().digest.size())));
    hash_u32(hash, static_cast<std::uint32_t>(draw_pixel_height));
    return finish_digest(hash);
}

std::uint64_t next_revision()
{
    static std::atomic<std::uint64_t> s_next_revision{1};
    return s_next_revision.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

Baked_font::Baked_font(build_result_t build, const font_identity_t& identity)
:
    m_build(std::move(build)),
    m_identity(identity)
{}

baked_font_result_t adopt_baked_font(build_result_t build)
{
    baked_font_result_t out;
    if (!valid_build(build)) {
        out.result = detail::make_text_result(
            Text_status::INVALID_ARGUMENT, "a baked font needs a complete usable build result");
        return out;
    }
    try {
        const font_identity_t identity = digest_atlas(build.atlas);
        out.font = std::shared_ptr<const Baked_font>(new Baked_font(std::move(build), identity));
    }
    catch (const std::bad_alloc&) {
        out.result = detail::make_text_result(Text_status::OUT_OF_MEMORY, "baked font allocation failed");
    }
    return out;
}

baked_font_result_t build_baked_font(
    std::span<const std::uint8_t> font_bytes,
    int                           draw_pixel_height,
    std::span<const char32_t>     codepoints,
    const options_t&              options,
    const log_callback_t&         log_debug_info)
{
    baked_font_result_t out;
    if (font_bytes.empty() || draw_pixel_height <= 0) {
        out.result = detail::make_text_result(
            Text_status::INVALID_ARGUMENT, "a baked font needs font bytes and a positive draw height");
        return out;
    }
    build_result_t build = build_font_atlas(
        font_bytes.data(), font_bytes.size(), draw_pixel_height, codepoints, options, log_debug_info);
    if (build.status == Build_status::FAILURE) {
        out.result = detail::make_text_result(
            Text_status::FONT_BUILD_FAILED,
            build.message.empty() ? "MSDF atlas build failed" : build.message);
        return out;
    }
    return adopt_baked_font(std::move(build));
}

Font_snapshot::Font_snapshot(std::shared_ptr<const Baked_font> font, int draw_pixel_height)
:
    m_font(std::move(font)),
    m_draw_pixel_height(draw_pixel_height),
    m_identity(digest_layout(*m_font, draw_pixel_height)),
    m_revision(next_revision())
{}

font_snapshot_result_t make_font_snapshot(std::shared_ptr<const Baked_font> font, int draw_pixel_height)
{
    font_snapshot_result_t out;
    if (!font || draw_pixel_height <= 0) {
        out.result = detail::make_text_result(
            Text_status::INVALID_ARGUMENT, "a font snapshot needs a baked font and a positive draw height");
        return out;
    }
    try {
        out.snapshot = std::shared_ptr<const Font_snapshot>(new Font_snapshot(std::move(font), draw_pixel_height));
    }
    catch (const std::bad_alloc&) {
        out.result = detail::make_text_result(Text_status::OUT_OF_MEMORY, "font snapshot allocation failed");
    }
    return out;
}

font_snapshot_result_t build_font_snapshot(
    std::span<const std::uint8_t> font_bytes,
    int                           draw_pixel_height,
    std::span<const char32_t>     codepoints,
    const options_t&              options,
    const log_callback_t&         log_debug_info)
{
    baked_font_result_t built =
        build_baked_font(font_bytes, draw_pixel_height, codepoints, options, log_debug_info);
    if (built.result.status != Text_status::OK) {
        return {std::move(built.result), {}};
    }
    return make_font_snapshot(std::move(built.font), draw_pixel_height);
}

} // namespace vnm::msdf_text::rhi
