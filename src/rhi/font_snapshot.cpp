#include <vnm_msdf_text/rhi/font_snapshot.h>

#include "rhi/sha256.h"

#include <atomic>
#include <cstdint>
#include <utility>

namespace vnm::msdf_text::rhi {
namespace {

// A snapshot identity must change whenever anything that changes the baked
// atlas changes, so every build input feeds the digest, not just the bytes.
font_identity_t digest_build_inputs(
    std::span<const std::uint8_t> font_bytes,
    int                           draw_pixel_height,
    std::span<const char32_t>     codepoints,
    const options_t&              options)
{
    detail::Sha256 hash;

    hash.update(font_bytes);
    hash.update_u32(static_cast<std::uint32_t>(font_bytes.size()));
    hash.update_u32(static_cast<std::uint32_t>(draw_pixel_height));

    hash.update_u32(static_cast<std::uint32_t>(options.atlas_size));
    hash.update_f64(options.min_atlas_font_size);
    hash.update_f32(options.atlas_px_range);
    hash.update_f32(options.sharpness_bias);
    hash.update_u32(static_cast<std::uint32_t>(options.atlas_gutter_px));
    hash.update_u32(options.build_kerning_table ? 1u : 0u);
    hash.update_u32(static_cast<std::uint32_t>(options.missing_glyph_policy));

    hash.update_u32(static_cast<std::uint32_t>(codepoints.size()));
    for (char32_t codepoint : codepoints) {
        hash.update_u32(static_cast<std::uint32_t>(codepoint));
    }

    font_identity_t identity;
    identity.digest = hash.finish();
    return identity;
}

std::uint64_t next_revision()
{
    static std::atomic<std::uint64_t> s_next_revision{1};
    return s_next_revision.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

Font_snapshot::Font_snapshot(
    build_result_t         build,
    int                    draw_pixel_height,
    const font_identity_t& identity)
:
    m_build(std::move(build)),
    m_draw_pixel_height(draw_pixel_height),
    m_identity(identity),
    m_revision(next_revision())
{}

font_snapshot_result_t build_font_snapshot(
    std::span<const std::uint8_t> font_bytes,
    int                           draw_pixel_height,
    std::span<const char32_t>     codepoints,
    const options_t&              options,
    const log_callback_t&         log_debug_info)
{
    font_snapshot_result_t out;

    if (font_bytes.empty()) {
        out.result = detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "a font snapshot needs font bytes");
        return out;
    }
    if (draw_pixel_height <= 0) {
        out.result = detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "font draw pixel height must be positive");
        return out;
    }

    build_result_t build = build_font_atlas(
        font_bytes.data(),
        font_bytes.size(),
        draw_pixel_height,
        codepoints,
        options,
        log_debug_info);

    if (build.status == Build_status::FAILURE) {
        out.result = detail::make_text_result(
            Text_status::FONT_BUILD_FAILED,
            build.message.empty() ? "MSDF atlas build failed" : build.message);
        return out;
    }

    const font_identity_t identity =
        digest_build_inputs(font_bytes, draw_pixel_height, codepoints, options);

    out.snapshot = std::shared_ptr<const Font_snapshot>(
        new Font_snapshot(std::move(build), draw_pixel_height, identity));
    return out;
}

} // namespace vnm::msdf_text::rhi
