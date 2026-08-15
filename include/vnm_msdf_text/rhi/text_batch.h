#pragma once

#include <vnm_msdf_text/msdf_text.h>
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
 * A batch holds nothing but quads and the identity of the font they were laid
 * out against, so it can be built away from the render thread from a copy of an
 * immutable Font_snapshot and handed over when it is complete. Where the text
 * is placed, how it is wrapped, elided, or coloured, and which strings are worth
 * drawing all stay with the consumer.
 *
 * Both entry points end at the same geometry, and append_run() emits it through
 * append_text_quads(), so a batch always agrees with the CPU measurement and
 * layout helpers applied to the same snapshot.
 */
class Text_batch
{
public:
    /**
     * @brief Append one single-line run with x, y as the baseline origin.
     *
     * Coordinates are output pixels in the consumer's own space; the draw state
     * supplied at queue time maps that space to the render target.
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
     * batch. Fails with INVALID_ARGUMENT when the batch already holds geometry
     * from a different font, when the indices do not form whole triangles, or
     * when an index addresses a vertex that was not supplied; with
     * GEOMETRY_LIMIT_EXCEEDED past the addressable index range; and with
     * OUT_OF_MEMORY when the geometry could not be allocated. A failed append
     * leaves the batch exactly as it was, including its font identity.
     */
    [[nodiscard]] text_result_t append_quads(
        const Font_snapshot&           font,
        std::span<const text_vertex_t> vertices,
        std::span<const std::uint32_t> indices);

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

    std::vector<text_vertex_t>     m_vertices;
    std::vector<std::uint32_t>     m_indices;
    std::optional<font_identity_t> m_font_identity;
};

} // namespace vnm::msdf_text::rhi
