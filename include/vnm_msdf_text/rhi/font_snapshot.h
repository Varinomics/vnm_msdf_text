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

/**
 * @brief Content identity of one built font snapshot.
 *
 * The digest covers every input that determines the baked atlas: the font
 * bytes, the draw pixel height, the atlas options, and the requested
 * codepoints. Two snapshots with equal identity therefore measure text
 * identically and emit identical quads, which is what lets a consumer put the
 * identity in a presentation key and reuse CPU measurements across a rebuild.
 * A different font, size, option, or codepoint set yields a different identity.
 */
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

struct font_snapshot_result_t;

/**
 * @brief One immutable font/atlas snapshot, shared through a shared_ptr.
 *
 * A snapshot composes the CPU build result rather than restating it: callers
 * measure, clip, and lay out with the existing free functions applied to
 * atlas() and draw_pixel_height(), and read build diagnostics from
 * build_result(). Nothing here mutates after construction, so CPU preparation
 * on any thread may hold a copy of the shared pointer while a render thread
 * draws from the same snapshot.
 *
 * Only build_font_snapshot() constructs one, because the identity below is a
 * digest of the build inputs and would be a lie if it could be supplied
 * separately from the atlas it describes.
 */
class Font_snapshot
{
public:
    Font_snapshot(const Font_snapshot&)            = delete;
    Font_snapshot& operator=(const Font_snapshot&) = delete;
    Font_snapshot(Font_snapshot&&)                 = delete;
    Font_snapshot& operator=(Font_snapshot&&)      = delete;

    /// The complete CPU build result, including status, message, and diagnostics.
    [[nodiscard]] const build_result_t& build_result() const { return m_build; }

    /// The baked atlas to measure and render from.
    [[nodiscard]] const atlas_t& atlas() const { return m_build.atlas; }

    /// The draw pixel height every scaling helper must be called with.
    [[nodiscard]] int draw_pixel_height() const { return m_draw_pixel_height; }

    [[nodiscard]] const font_identity_t& identity() const { return m_identity; }

    /**
     * @brief Distinguishes snapshot instances that share an identity.
     *
     * Two snapshots built from the same inputs describe the same font, but they
     * are still separate objects with separate atlas storage. A renderer keys
     * its GPU atlas upload on the revision so a rebuilt snapshot is uploaded
     * again, while CPU measurement caches can key on the identity alone.
     * Revisions are unique and increasing within a process.
     */
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }

private:
    Font_snapshot(build_result_t build, int draw_pixel_height, const font_identity_t& identity);

    friend font_snapshot_result_t build_font_snapshot(
        std::span<const std::uint8_t> font_bytes,
        int                           draw_pixel_height,
        std::span<const char32_t>     codepoints,
        const options_t&              options,
        const log_callback_t&         log_debug_info);

    build_result_t  m_build;
    int             m_draw_pixel_height = 0;
    font_identity_t m_identity{};
    std::uint64_t   m_revision          = 0;
};

struct font_snapshot_result_t
{
    text_result_t result;

    /**
     * @brief The built snapshot, non-null exactly when result.status is OK.
     *
     * A partially built atlas is a usable snapshot: result.status is OK and
     * snapshot->build_result().status is PARTIAL_SUCCESS with the diagnostics
     * describing the codepoints that were not emitted. A caller that requires
     * complete coverage inspects that build status; a caller that only needs
     * renderable text does not have to.
     */
    std::shared_ptr<const Font_snapshot> snapshot;
};

/**
 * @brief Build an immutable font snapshot from caller-supplied font bytes.
 *
 * The caller owns font acquisition: this component never reads a file, resolves
 * an asset, or retains the bytes, which are only read during the call.
 *
 * Fails with INVALID_ARGUMENT for empty bytes or a non-positive draw pixel
 * height, and with FONT_BUILD_FAILED when no usable atlas could be produced.
 */
[[nodiscard]] font_snapshot_result_t build_font_snapshot(
    std::span<const std::uint8_t> font_bytes,
    int                           draw_pixel_height,
    std::span<const char32_t>     codepoints,
    const options_t&              options        = options_t(),
    const log_callback_t&         log_debug_info = log_callback_t());

} // namespace vnm::msdf_text::rhi
