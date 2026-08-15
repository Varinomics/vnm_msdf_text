#include <vnm_msdf_text/rhi/text_batch.h>

#include <limits>
#include <new>
#include <stdexcept>

namespace vnm::msdf_text::rhi {
namespace {

constexpr std::size_t k_max_vertices =
    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

} // namespace

text_result_t Text_batch::adopt_font(const Font_snapshot& font)
{
    if (m_font_identity && *m_font_identity != font.identity()) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "a text batch holds geometry from one font only");
    }

    m_font_identity = font.identity();
    return {};
}

text_result_t Text_batch::append_run(
    const Font_snapshot& font,
    std::string_view     text,
    float                baseline_x,
    float                baseline_y)
{
    const text_result_t adopted = adopt_font(font);
    if (adopted.status != Text_status::OK) {
        return adopted;
    }

    const std::size_t vertex_mark = m_vertices.size();
    const std::size_t index_mark  = m_indices.size();

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
            Text_status::GEOMETRY_LIMIT_EXCEEDED,
            "text run geometry could not be allocated");
    }

    return {};
}

text_result_t Text_batch::append_quads(
    const Font_snapshot&           font,
    std::span<const text_vertex_t> vertices,
    std::span<const std::uint32_t> indices)
{
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

    const text_result_t adopted = adopt_font(font);
    if (adopted.status != Text_status::OK) {
        return adopted;
    }

    const auto base = static_cast<std::uint32_t>(m_vertices.size());
    m_vertices.insert(m_vertices.end(), vertices.begin(), vertices.end());
    m_indices.reserve(m_indices.size() + indices.size());
    for (std::uint32_t index : indices) {
        m_indices.push_back(index + base);
    }

    return {};
}

void Text_batch::clear()
{
    m_vertices.clear();
    m_indices.clear();
    m_font_identity.reset();
}

} // namespace vnm::msdf_text::rhi
