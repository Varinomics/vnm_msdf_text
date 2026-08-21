#pragma once

#include <vnm_msdf_text/lcd_contract.h>
#include <vnm_msdf_text/lcd_shader_reference.h>

#include <array>
#include <cstdint>

namespace vnm::msdf_text::rhi {

/**
 * @brief Optional draw capabilities, each with its own presence and version.
 *
 * Every record below is absent by default, and a frame that supplies none of
 * them draws through the same geometry, uniform block, shader, and pipeline as
 * a build without this header: absence is not a mode, it is the base path.
 * Supplying one moves that single draw onto the styled pipeline; the other
 * draws of the same frame are unaffected either way.
 *
 * Each record carries its own version because each evolves on its own. A
 * version this component does not implement is reported at that record's own
 * boundary - the draw carrying it is rejected - and never invalidates the base
 * text of the same frame. The version is not decoration: this is a static
 * library, so a process can hold two copies of it (see Font_snapshot::revision),
 * and a record built by a module compiled against a different revision of this
 * header would otherwise be read with the wrong field meanings. Compare a
 * record's version with the matching k_*_version below to decide before
 * queueing.
 *
 * The provider supplies rendering semantics only. Which text gets a subpixel
 * order, a glow, or a mask, what background it sits on, and when any of that is
 * worth doing stay with the consumer.
 */

/// Per-glyph frame rectangles carried by a batch. See Text_batch.
inline constexpr std::uint32_t k_glyph_frames_version = 1;
/// LCD subpixel order and the opaque background it is composed against.
inline constexpr std::uint32_t k_lcd_style_version    = 1;
/// Outer glow around the glyph outline.
inline constexpr std::uint32_t k_glow_style_version   = 1;
/// True-SDF alpha masking of the multi-channel coverage.
inline constexpr std::uint32_t k_sdf_mask_version     = 1;

/**
 * @brief One glyph quad's rectangle in the space its vertices are laid out in.
 *
 * x, y is the top-left corner in the batch's own top-left-origin pixel space,
 * and the rectangle is the axis-aligned bound of the quad's four positions. All
 * four vertices of a quad carry the same rectangle, which is what makes it a
 * per-glyph value rather than a per-vertex one.
 *
 * The styled fragment shader reconstructs where a fragment sits inside its
 * glyph from this rectangle, which is how a subpixel filter can step by a third
 * of an output pixel: the step is width and height divided into thirds. That
 * only holds while the rectangle is in framebuffer pixels, so a styled draw is
 * laid out in framebuffer pixels and transformed by pixel_ortho_transform().
 */
struct glyph_frame_t
{
    float x      = 0.0f;
    float y      = 0.0f;
    float width  = 0.0f;
    float height = 0.0f;
};

/**
 * @brief Asks a batch to record a frame rectangle for every vertex it appends.
 *
 * The envelope carries nothing but its version today. It exists so the geometry
 * capability can gain fields without touching the batch's mandatory geometry,
 * and so a batch built against an unknown version is rejected rather than read
 * with the wrong meaning.
 */
struct glyph_frames_t
{
    std::uint32_t version = k_glyph_frames_version;
};

/**
 * @brief Subpixel order and the opaque background the coverage is composed on.
 *
 * The order is the existing vnm_msdf_text::lcd_contract resolution, and the
 * filter is the one vnm_msdf_text::lcd_shader_reference describes: three
 * five-tap windows a third of a pixel apart, weighted by the reference's edge,
 * side, and centre coefficients, laid along X for RGB and BGR and along Y for
 * VRGB and VBGR, in reverse channel order for BGR and VBGR.
 *
 * A display-specific order composes each channel's coverage against
 * background_color and writes the straight colour that reproduces that mix once
 * the pipeline has blended it. That is only the intended image where the
 * destination under the text really is background_color, which is why the draw
 * is rejected unless both the draw colour and this background are opaque within
 * lcd::shader_reference::k_lcd_opaque_alpha_cutoff, and why a glow may not be
 * combined with it. A consumer that cannot promise an opaque known background
 * for a run passes Resolved_lcd_subpixel_order::NONE for it.
 *
 * NONE is a valid order and means no subpixel filtering: coverage stays
 * grayscale, background_color is unused, and the draw composes exactly as the
 * base path does.
 */
struct lcd_style_t
{
    std::uint32_t                    version = k_lcd_style_version;
    lcd::Resolved_lcd_subpixel_order order   = lcd::Resolved_lcd_subpixel_order::NONE;

    /// Straight RGBA in [0, 1]; the destination the coverage is composed onto.
    std::array<float, 4>             background_color = {0.0f, 0.0f, 0.0f, 1.0f};
};

/**
 * @brief An outer glow that falls off outside the glyph outline.
 *
 * Coverage ramps smoothly from the outline outwards over radius_px output
 * pixels, from the glyph's signed distance, so the glow follows the letter
 * shapes rather than their quads. It is drawn under the glyphs of the same
 * draw, so a later glyph's glow never darkens an earlier glyph's body.
 *
 * The glow is centred on the text. A consumer that wants an offset drop shadow
 * draws a second, displaced run with the glow colour.
 *
 * It is drawn on the glyph quads, so how far it can actually reach is the
 * padding a glyph quad has around its outline: options_t::atlas_px_range at the
 * bake scale, scaled to the draw pixel height. A radius beyond that is clipped
 * to the quad rather than rejected, because the reach depends on the snapshot
 * and the draw size rather than on the record. A consumer that wants a wide
 * glow bakes its snapshot with a wider range.
 *
 * Presence means the glow is drawn, so radius_px must be positive and the
 * colour visible; a consumer that wants no glow omits the record instead of
 * supplying an invisible one.
 */
struct glow_style_t
{
    std::uint32_t        version   = k_glow_style_version;

    /// Straight RGBA in [0, 1].
    std::array<float, 4> color     = {0.0f, 0.0f, 0.0f, 1.0f};
    float                radius_px = 0.0f;
};

/**
 * @brief Masks the multi-channel coverage with the atlas's true signed distance.
 *
 * The atlas is an MTSDF: the RGB channels carry the multi-channel field, whose
 * median keeps corners sharp, and the alpha channel carries the plain signed
 * distance, which has no corner reconstruction and therefore no corner
 * artifacts either. Presence takes the smaller of the two coverages, which
 * keeps the sharp outline while removing the stray fragments the multi-channel
 * median reconstructs where three channels disagree.
 *
 * The mask applies where the atlas carries true-SDF data. It is a coverage
 * capability with no parameters, so the envelope carries only its version.
 */
struct sdf_mask_t
{
    std::uint32_t version = k_sdf_mask_version;
};

} // namespace vnm::msdf_text::rhi
