#include <vnm_msdf_text/rhi/text_batch.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace vnm::msdf_text::rhi {
namespace {

constexpr std::size_t k_max_vertices =
    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

/// append_text_quads() emits one axis-aligned quad of four vertices per glyph.
constexpr std::size_t k_quad_vertices = 4u;

[[nodiscard]] bool glyph_frames_match_quads(
    std::span<const text_vertex_t> vertices,
    std::span<const glyph_frame_t> frames)
{
    if (vertices.size() % k_quad_vertices != 0u) {
        return false;
    }

    for (std::size_t quad = 0; quad < vertices.size(); quad += k_quad_vertices) {
        const glyph_frame_t& frame = frames[quad];
        if (!std::isfinite(frame.x) || !std::isfinite(frame.y) ||
            !std::isfinite(frame.width) || !std::isfinite(frame.height) ||
            frame.width <= 0.0f || frame.height <= 0.0f)
        {
            return false;
        }

        float left   = vertices[quad].x;
        float top    = vertices[quad].y;
        float right  = left;
        float bottom = top;
        if (!std::isfinite(left) || !std::isfinite(top)) {
            return false;
        }
        for (std::size_t corner = 1; corner < k_quad_vertices; ++corner) {
            const text_vertex_t& vertex = vertices[quad + corner];
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y)) {
                return false;
            }
            left   = std::min(left,   vertex.x);
            top    = std::min(top,    vertex.y);
            right  = std::max(right,  vertex.x);
            bottom = std::max(bottom, vertex.y);
        }

        for (std::size_t corner = 0; corner < k_quad_vertices; ++corner) {
            const glyph_frame_t& candidate = frames[quad + corner];
            if (candidate.x != frame.x || candidate.y != frame.y ||
                candidate.width != frame.width || candidate.height != frame.height)
            {
                return false;
            }
        }
        if (frame.x != left || frame.y != top ||
            frame.width != right - left || frame.height != bottom - top)
        {
            return false;
        }
    }

    return true;
}

/// The caller has already established whole, in-range triangles before this runs.
[[nodiscard]] bool glyph_frame_triangles_stay_within_quads(
    std::span<const std::uint32_t> indices)
{
    for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3u) {
        const std::uint32_t glyph_group =
            indices[triangle] / static_cast<std::uint32_t>(k_quad_vertices);
        if (indices[triangle + 1u] / static_cast<std::uint32_t>(k_quad_vertices) !=
                glyph_group ||
            indices[triangle + 2u] / static_cast<std::uint32_t>(k_quad_vertices) !=
                glyph_group)
        {
            return false;
        }
    }

    return true;
}

} // namespace

text_result_t Text_batch::validate_font(const Font_snapshot& font) const
{
    if (m_font_identity && *m_font_identity != font.identity()) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "a text batch holds geometry from one font only");
    }

    return {};
}

text_result_t Text_batch::enable_glyph_frames(const glyph_frames_t& capability)
{
    if (capability.version != k_glyph_frames_version) {
        return detail::make_text_result(
            Text_status::CAPABILITY_UNSUPPORTED,
            "this build does not implement the requested glyph-frame version");
    }
    if (!m_vertices.empty()) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "glyph frames are enabled on an empty batch, not part-way through one");
    }

    m_glyph_frames_enabled = true;
    return {};
}

bool Text_batch::append_quad_frames(std::size_t mark)
{
    // Reserving the whole result up front is what makes the inserts below
    // non-throwing, so a failure here is the only way this can fail and it
    // happens before anything has been written.
    try {
        m_glyph_frames.reserve(m_vertices.size());
    }
    catch (const std::bad_alloc&) {
        return false;
    }

    for (std::size_t i = mark; i + k_quad_vertices <= m_vertices.size(); i += k_quad_vertices) {
        float left   = m_vertices[i].x;
        float top    = m_vertices[i].y;
        float right  = left;
        float bottom = top;
        for (std::size_t corner = 1; corner < k_quad_vertices; ++corner) {
            const text_vertex_t& vertex = m_vertices[i + corner];
            left   = std::min(left,   vertex.x);
            top    = std::min(top,    vertex.y);
            right  = std::max(right,  vertex.x);
            bottom = std::max(bottom, vertex.y);
        }

        const glyph_frame_t frame{ left, top, right - left, bottom - top };
        m_glyph_frames.insert(m_glyph_frames.end(), k_quad_vertices, frame);
    }

    return true;
}

text_result_t Text_batch::append_run(
    const Font_snapshot& font,
    std::string_view     text,
    float                baseline_x,
    float                baseline_y)
{
    const text_result_t accepted = validate_font(font);
    if (accepted.status != Text_status::OK) {
        return accepted;
    }

    const std::size_t vertex_mark = m_vertices.size();
    const std::size_t index_mark  = m_indices.size();

    // append_text_quads() grows both vectors as it goes, so a failure part-way
    // through is rolled back to the marks above before it is reported. Shrinking
    // back cannot throw, and the font identity is only adopted once the whole
    // run is in, so a failed append leaves the batch exactly as it was.
    try {
        append_text_quads(
            font.atlas(),
            font.draw_pixel_height(),
            text,
            baseline_x,
            baseline_y,
            m_vertices,
            &m_indices);
    }
    catch (const std::length_error&) {
        m_vertices.resize(vertex_mark);
        m_indices.resize(index_mark);
        return detail::make_text_result(
            Text_status::GEOMETRY_LIMIT_EXCEEDED,
            "text run exceeds the addressable index range of one batch");
    }
    catch (const std::bad_alloc&) {
        m_vertices.resize(vertex_mark);
        m_indices.resize(index_mark);
        return detail::make_text_result(
            Text_status::OUT_OF_MEMORY,
            "text run geometry could not be allocated");
    }

    if (m_glyph_frames_enabled && !append_quad_frames(vertex_mark)) {
        m_vertices.resize(vertex_mark);
        m_indices.resize(index_mark);
        m_glyph_frames.resize(vertex_mark);
        return detail::make_text_result(
            Text_status::OUT_OF_MEMORY,
            "text run frame rectangles could not be allocated");
    }

    m_font_identity = font.identity();
    return {};
}

text_result_t Text_batch::append_quads(
    const Font_snapshot&           font,
    std::span<const text_vertex_t> vertices,
    std::span<const std::uint32_t> indices,
    std::span<const glyph_frame_t> frames)
{
    const std::size_t expected_frames = m_glyph_frames_enabled ? vertices.size() : 0u;
    if (frames.size() != expected_frames) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            m_glyph_frames_enabled
                ? "a batch with glyph frames needs one frame rectangle per vertex"
                : "a batch without glyph frames takes no frame rectangles");
    }
    if (indices.empty() && vertices.empty()) {
        return {};
    }
    if (indices.size() % 3u != 0u) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "text quad indices must form whole triangles");
    }
    if (vertices.size() > k_max_vertices - m_vertices.size()) {
        return detail::make_text_result(
            Text_status::GEOMETRY_LIMIT_EXCEEDED,
            "text quads exceed the addressable index range of one batch");
    }
    for (std::uint32_t index : indices) {
        if (index >= vertices.size()) {
            return detail::make_text_result(
                Text_status::INVALID_ARGUMENT,
                "a text quad index addresses a vertex that was not supplied");
        }
    }
    if (m_glyph_frames_enabled && !glyph_frames_match_quads(vertices, frames)) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "glyph frames must be finite positive per-quad bounds matching their vertices");
    }
    if (m_glyph_frames_enabled && !glyph_frame_triangles_stay_within_quads(indices)) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "a framed glyph triangle must stay inside one four-vertex group");
    }

    const text_result_t accepted = validate_font(font);
    if (accepted.status != Text_status::OK) {
        return accepted;
    }

    // Every vector is reserved before any of them is appended to, so a failed
    // allocation cannot leave the batch holding vertices without their indices
    // or without the frame rectangles the capability promises for them.
    try {
        m_vertices.reserve(m_vertices.size() + vertices.size());
        m_indices.reserve(m_indices.size() + indices.size());
        m_glyph_frames.reserve(m_glyph_frames.size() + frames.size());
    }
    catch (const std::bad_alloc&) {
        return detail::make_text_result(
            Text_status::OUT_OF_MEMORY,
            "text quad geometry could not be allocated");
    }

    const auto base = static_cast<std::uint32_t>(m_vertices.size());
    m_vertices.insert(m_vertices.end(), vertices.begin(), vertices.end());
    m_glyph_frames.insert(m_glyph_frames.end(), frames.begin(), frames.end());
    for (std::uint32_t index : indices) {
        m_indices.push_back(index + base);
    }

    m_font_identity = font.identity();
    return {};
}

void Text_batch::clear()
{
    m_vertices.clear();
    m_indices.clear();
    m_glyph_frames.clear();
    m_font_identity.reset();
    m_glyph_frames_enabled = false;
}

} // namespace vnm::msdf_text::rhi
