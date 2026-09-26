#pragma once

#include <vnm_msdf_text/msdf_text.h>
#include <vnm_msdf_text/rhi/status.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace vnm::msdf_text::rhi {

inline constexpr std::size_t k_font_digest_bytes = 32;

/// Content identity. Atlas identities exclude draw size; snapshot identities
/// include it so CPU layout caches never confuse different output sizes.
struct font_identity_t
{
    std::array<std::uint8_t, k_font_digest_bytes> digest{};
};

[[nodiscard]] inline bool operator==(const font_identity_t& lhs, const font_identity_t& rhs)
{
    return lhs.digest == rhs.digest;
}

[[nodiscard]] inline bool operator!=(const font_identity_t& lhs, const font_identity_t& rhs)
{
    return !(lhs == rhs);
}

struct baked_font_result_t;
struct font_snapshot_result_t;

/**
 * @brief Immutable baked font shared by draw-size snapshots and GPU uploads.
 *
 * Retains the complete build result, including partial-coverage diagnostics.
 * Its identity is derived from the atlas content, never supplied by a caller.
 * Sharing this object across draw sizes shares the bitmap and metrics storage.
 */
class Baked_font
{
public:
    [[nodiscard]] const build_result_t& build_result() const { return m_build; }
    [[nodiscard]] const atlas_t& atlas() const { return m_build.atlas; }
    [[nodiscard]] const font_identity_t& identity() const { return m_identity; }

private:
    Baked_font(build_result_t build, const font_identity_t& identity);
    friend baked_font_result_t adopt_baked_font(build_result_t build);

    build_result_t  m_build;
    font_identity_t m_identity;
};

struct baked_font_result_t
{
    text_result_t result;
    std::shared_ptr<const Baked_font> font;
};

/**
 * @brief Validate and own a complete build result, including a persisted one.
 *
 * The cache owner checks its producer compatibility key before calling this.
 * Invalid atlas data or an unusable build status is rejected. Cache containers
 * must retain the original status and diagnostics, not invent SUCCESS for an
 * atlas whose coverage is unknown. No bitmap is copied when ownership moves.
 */
[[nodiscard]] baked_font_result_t adopt_baked_font(build_result_t build);

[[nodiscard]] baked_font_result_t build_baked_font(
    std::span<const std::uint8_t> font_bytes,
    int                           draw_pixel_height,
    std::span<const char32_t>     codepoints,
    const options_t&              options        = options_t(),
    const log_callback_t&         log_debug_info = log_callback_t());

/**
 * @brief Immutable layout view over a shared baked font.
 *
 * Draw pixel height affects measurements and quads, but not the baked bitmap.
 * CPU preparation may retain the snapshot on any thread; QRhi resources remain
 * owned by the render thread. GPU reuse follows baked_font(), not this view.
 */
class Font_snapshot
{
public:
    Font_snapshot(const Font_snapshot&)            = delete;
    Font_snapshot& operator=(const Font_snapshot&) = delete;
    Font_snapshot(Font_snapshot&&)                 = delete;
    Font_snapshot& operator=(Font_snapshot&&)      = delete;

    [[nodiscard]] const build_result_t& build_result() const { return m_font->build_result(); }
    [[nodiscard]] const atlas_t& atlas() const { return m_font->atlas(); }
    [[nodiscard]] const std::shared_ptr<const Baked_font>& baked_font() const { return m_font; }
    [[nodiscard]] int draw_pixel_height() const { return m_draw_pixel_height; }
    [[nodiscard]] const font_identity_t& identity() const { return m_identity; }

    /// Monotonic instance identity within one loaded copy of this static library.
    /// Across module boundaries compare content identity instead.
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }

private:
    Font_snapshot(std::shared_ptr<const Baked_font> font, int draw_pixel_height);
    friend font_snapshot_result_t make_font_snapshot(std::shared_ptr<const Baked_font> font, int draw_pixel_height);

    std::shared_ptr<const Baked_font> m_font;
    int                              m_draw_pixel_height = 0;
    font_identity_t                  m_identity{};
    std::uint64_t                    m_revision = 0;
};

struct font_snapshot_result_t
{
    text_result_t result;

    /// Non-null exactly when result is OK. Partial coverage remains visible in
    /// snapshot->build_result(), including on a validated cache hit.
    std::shared_ptr<const Font_snapshot> snapshot;
};

/// Creates a draw-size view without copying or rebuilding the shared atlas.
[[nodiscard]] font_snapshot_result_t make_font_snapshot(
    std::shared_ptr<const Baked_font> font,
    int                               draw_pixel_height);

/// Acquires no assets: font bytes are borrowed only for this synchronous build.
[[nodiscard]] font_snapshot_result_t build_font_snapshot(
    std::span<const std::uint8_t> font_bytes,
    int                           draw_pixel_height,
    std::span<const char32_t>     codepoints,
    const options_t&              options        = options_t(),
    const log_callback_t&         log_debug_info = log_callback_t());

} // namespace vnm::msdf_text::rhi
