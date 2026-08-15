#include <vnm_msdf_text/rhi/font_snapshot.h>

#include <QtCore/QByteArrayView>
#include <QtCore/QCryptographicHash>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace vnm::msdf_text::rhi {
namespace {

/// Feeds one scalar in a fixed big-endian width, so the digest never depends on
/// the host's byte order or on where one field ends and the next begins.
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

// A snapshot identity must change whenever anything that changes the baked
// atlas changes, so every build input feeds the digest, not just the bytes. The
// order and framing below are the serialized identity contract: the same inputs
// must always produce the same byte stream, and no field may be dropped,
// reordered, or written at a different width.
font_identity_t digest_build_inputs(
    std::span<const std::uint8_t> font_bytes,
    int                           draw_pixel_height,
    std::span<const char32_t>     codepoints,
    const options_t&              options)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);

    hash.addData(QByteArrayView(
        reinterpret_cast<const char*>(font_bytes.data()),
        static_cast<qsizetype>(font_bytes.size())));
    hash_u32(hash, static_cast<std::uint32_t>(font_bytes.size()));
    hash_u32(hash, static_cast<std::uint32_t>(draw_pixel_height));

    hash_u32(hash, static_cast<std::uint32_t>(options.atlas_size));
    hash_f64(hash, options.min_atlas_font_size);
    hash_f32(hash, options.atlas_px_range);
    hash_f32(hash, options.sharpness_bias);
    hash_u32(hash, static_cast<std::uint32_t>(options.atlas_gutter_px));
    hash_u32(hash, options.build_kerning_table ? 1u : 0u);
    hash_u32(hash, static_cast<std::uint32_t>(options.missing_glyph_policy));

    hash_u32(hash, static_cast<std::uint32_t>(codepoints.size()));
    for (char32_t codepoint : codepoints) {
        hash_u32(hash, static_cast<std::uint32_t>(codepoint));
    }

    // SHA-256 is defined to produce exactly k_font_digest_bytes bytes.
    const QByteArray digest = hash.result();

    font_identity_t identity;
    std::memcpy(identity.digest.data(), digest.constData(), identity.digest.size());
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
