#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace vnm::msdf_text::rhi {

/**
 * @brief Outcome of a shared text call.
 *
 * Every value other than OK names a distinct reachable failure, so a consumer
 * that requires real text can tell "this font never built" apart from "this
 * frame could not be recorded" and react to each. No call reports success for
 * text it did not produce.
 */
enum class Text_status
{
    OK,
    /**
     * @brief A supplied value cannot describe the requested work.
     *
     * Reached by a non-positive draw pixel height, empty font bytes, caller
     * quads that do not form whole triangles or address vertices they did not
     * supply, a batch whose font identity differs from the renderer's, and an
     * optional draw capability whose values or combination cannot describe a
     * drawable result - an invisible glow, a subpixel order asked for over a
     * background that is not opaque, or a styled draw whose batch carries no
     * per-glyph frame rectangles.
     */
    INVALID_ARGUMENT,
    /// The font bytes were read but no MSDF atlas could be produced from them.
    FONT_BUILD_FAILED,
    /// The call needs a font snapshot that was never set on the renderer.
    NO_FONT,
    /// The frame is missing a QRhi handle the call needs.
    INVALID_FRAME,
    /// Text was queued but never uploaded, so there is nothing to record.
    NOT_PREPARED,
    /// The compiled shader artifacts could not be loaded.
    SHADER_UNAVAILABLE,
    /// A QRhi texture, sampler, buffer, binding set, or pipeline failed to build.
    GPU_RESOURCE_FAILED,
    /// The queued geometry exceeds what one QRhi buffer or index range can address.
    GEOMETRY_LIMIT_EXCEEDED,
    /**
     * @brief An optional capability record names a version this build has not.
     *
     * Reported at the boundary of the one record that carries the unknown
     * version, so the call that supplied it fails while everything else in the
     * frame - including base text queued before or after it - stays valid.
     */
    CAPABILITY_UNSUPPORTED,
    /**
     * @brief Host memory for the call's own containers could not be obtained.
     *
     * Distinct from GEOMETRY_LIMIT_EXCEEDED, which names geometry the contract
     * cannot express at all. A call that reports this one changed nothing: the
     * batch or the queued frame is exactly what it was before the call.
     */
    OUT_OF_MEMORY,
};

/// Stable spelling of a status for logs and test failure messages.
[[nodiscard]] constexpr const char* text_status_name(Text_status status)
{
    switch (status) {
        case Text_status::OK:                      return "ok";
        case Text_status::INVALID_ARGUMENT:        return "invalid_argument";
        case Text_status::FONT_BUILD_FAILED:       return "font_build_failed";
        case Text_status::NO_FONT:                 return "no_font";
        case Text_status::INVALID_FRAME:           return "invalid_frame";
        case Text_status::NOT_PREPARED:            return "not_prepared";
        case Text_status::SHADER_UNAVAILABLE:      return "shader_unavailable";
        case Text_status::GPU_RESOURCE_FAILED:     return "gpu_resource_failed";
        case Text_status::GEOMETRY_LIMIT_EXCEEDED: return "geometry_limit_exceeded";
        case Text_status::CAPABILITY_UNSUPPORTED:  return "capability_unsupported";
        case Text_status::OUT_OF_MEMORY:           return "out_of_memory";
        default:                                   return "unknown";
    }
}

/**
 * @brief Diagnostic capacity of text_result_t, including the terminator.
 *
 * Diagnostics live in a fixed buffer because these calls run on the render
 * thread once per frame: a failing frame neither allocates nor lets a message
 * grow without bound. A longer message is truncated, never reallocated.
 */
inline constexpr std::size_t k_text_diagnostic_capacity = 160;

struct text_result_t
{
    Text_status status = Text_status::OK;

    /// NUL-terminated, and empty while status is OK.
    std::array<char, k_text_diagnostic_capacity> diagnostic{};
};

namespace detail {

/// Builds a failing result with its message truncated into the fixed buffer.
[[nodiscard]] inline text_result_t make_text_result(
    Text_status      status,
    std::string_view message)
{
    text_result_t result;
    result.status = status;

    const std::size_t copied = std::min(message.size(), k_text_diagnostic_capacity - 1u);
    if (copied > 0u) {
        std::memcpy(result.diagnostic.data(), message.data(), copied);
    }
    result.diagnostic[copied] = '\0';
    return result;
}

} // namespace detail

} // namespace vnm::msdf_text::rhi
