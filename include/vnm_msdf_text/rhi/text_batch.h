#pragma once

#include <vnm_msdf_text/msdf_text.h>
#include <vnm_msdf_text/rhi/draw_capabilities.h>
#include <vnm_msdf_text/rhi/font_snapshot.h>
#include <vnm_msdf_text/rhi/status.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vnm::msdf_text::rhi {

/**
 * @brief Caller-prepared text geometry for one draw state.
 *
 * A batch holds nothing but quads, optionally their per-glyph frame rectangles,
 * and the identity of the font they were laid out against, so it can be built
 * away from the render thread from a copy of an immutable Font_snapshot and
 * handed over when it is complete. Where the text is placed, how it is wrapped,
 * elided, or coloured, and which strings are worth drawing all stay with the
 * consumer.
 *
 * Both entry points end at the same geometry, and append_run() emits it through
 * append_text_quads(), so a batch always agrees with the CPU measurement and
 * layout helpers applied to the same snapshot.
 */
class Text_batch
{
public:
    /**
     * @brief Record a frame rectangle for every vertex appended from here on.
     *
     * A batch either carries frame rectangles for all of its geometry or for
     * none of it, so this is set on an empty batch and applies to everything
     * appended afterwards. A styled draw needs them; a draw with no optional
     * style ignores them, and a batch may carry them either way.
     *
     * append_run() derives each rectangle from the quad it emits. A caller
     * supplying its own quads supplies the matching rectangles alongside them.
     *
     * Fails with CAPABILITY_UNSUPPORTED when the envelope names a version this
     * build does not implement, and with INVALID_ARGUMENT when the batch
     * already holds geometry. clear() returns the batch to its default state,
     * frame rectangles included.
     */
    [[nodiscard]] text_result_t enable_glyph_frames(const glyph_frames_t& capability = {});

    [[nodiscard]] bool has_glyph_frames() const { return m_glyph_frames_enabled; }

    /// One rectangle per vertex, and empty unless the capability was enabled.
    [[nodiscard]] std::span<const glyph_frame_t> glyph_frames() const { return m_glyph_frames; }

    /**
     * @brief Append one single-line run with x, y as the baseline origin.
     *
     * Coordinates are output pixels in the consumer's own space; the draw state
     * supplied at queue time maps that space to the render target.
     *
     * With frame rectangles enabled, each emitted quad's rectangle is derived
     * from that quad's own four positions, so a run needs no extra caller data.
     *
     * Fails with INVALID_ARGUMENT when the batch already holds geometry from a
     * different font, with GEOMETRY_LIMIT_EXCEEDED when the run would push the
     * batch past the addressable index range, and with OUT_OF_MEMORY when the
     * geometry could not be allocated. A failed append leaves the batch exactly
     * as it was, including its font identity.
     */
    [[nodiscard]] text_result_t append_run(
        const Font_snapshot& font,
        std::string_view     text,
        float                baseline_x,
        float                baseline_y);

    /**
     * @brief Append quads a consumer produced itself from the same snapshot.
     *
     * Indices are relative to the supplied vertices and are rebased onto the
     * batch. One frame rectangle per vertex is required exactly when the batch
     * has frame rectangles enabled, and the four vertices of a quad are
     * must form four-vertex groups, and every group must carry one finite,
     * positive rectangle equal to that group's axis-aligned vertex bound.
     *
     * Fails with INVALID_ARGUMENT when the batch already holds geometry from a
     * different font, when the indices do not form whole triangles, when an
     * index addresses a vertex that was not supplied, or when the frame
     * rectangles do not match the batch's capability and vertex count; with
     * GEOMETRY_LIMIT_EXCEEDED past the addressable index range; and with
     * OUT_OF_MEMORY when the geometry could not be allocated. A failed append
     * leaves the batch exactly as it was, including its font identity.
     */
    [[nodiscard]] text_result_t append_quads(
        const Font_snapshot&           font,
        std::span<const text_vertex_t> vertices,
        std::span<const std::uint32_t> indices,
        std::span<const glyph_frame_t> frames = {});

    void clear();

    [[nodiscard]] bool empty() const { return m_indices.empty(); }

    /// The font the batch was laid out against; unset until the first append.
    [[nodiscard]] const std::optional<font_identity_t>& font_identity() const
    {
        return m_font_identity;
    }

    [[nodiscard]] std::span<const text_vertex_t> vertices() const { return m_vertices; }
    [[nodiscard]] std::span<const std::uint32_t> indices()  const { return m_indices; }

private:
    /// Rejects a font that would mix with the geometry the batch already holds.
    [[nodiscard]] text_result_t validate_font(const Font_snapshot& font) const;

    /// Derives one rectangle per vertex for the quads appended since @p mark.
    [[nodiscard]] bool append_quad_frames(std::size_t mark);

    std::vector<text_vertex_t>     m_vertices;
    std::vector<std::uint32_t>     m_indices;
    std::vector<glyph_frame_t>     m_glyph_frames;
    std::optional<font_identity_t> m_font_identity;
    bool                           m_glyph_frames_enabled = false;
};

} // namespace vnm::msdf_text::rhi
