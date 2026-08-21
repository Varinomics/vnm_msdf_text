#pragma once

#include <vnm_msdf_text/rhi/draw_capabilities.h>
#include <vnm_msdf_text/rhi/font_snapshot.h>
#include <vnm_msdf_text/rhi/status.h>
#include <vnm_msdf_text/rhi/text_batch.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

class QRhi;
class QRhiCommandBuffer;
class QRhiRenderTarget;
class QRhiResourceUpdateBatch;

namespace vnm::msdf_text::rhi {

/**
 * @brief Everything this component needs from the frame the host has opened.
 *
 * The host owns every handle. Text fills the host's resource-update batch
 * before the pass and records draws once the pass is open, so it never calls
 * QRhiCommandBuffer::resourceUpdate() itself and never begins or ends a pass.
 * The render target must be the one the pass is opened on: its render-pass
 * descriptor, sample count, and pixel size decide the pipeline and the
 * unclipped scissor.
 *
 * The host must submit resource_updates for this frame, by passing it to
 * QRhiCommandBuffer::beginPass(), endPass(), or resourceUpdate(). QRhi runs the
 * commands in a batch only when the batch is submitted; a batch that is
 * released or abandoned instead executes nothing, which is why prepare() below
 * treats an upload as enqueued rather than done.
 */
struct frame_t
{
    QRhi*                    rhi              = nullptr;
    QRhiCommandBuffer*       command_buffer   = nullptr;
    QRhiRenderTarget*        render_target    = nullptr;
    QRhiResourceUpdateBatch* resource_updates = nullptr;
};

/**
 * @brief Scissor rectangle in framebuffer pixels.
 *
 * x and y are the bottom-left corner. QRhi takes OpenGL-style scissor
 * coordinates on every backend and itself flips them for the backends whose
 * native origin is the top-left, so a caller uses one convention everywhere -
 * and it is the opposite of the top-left pixel space the text is laid out in.
 * Negative x or y and partially out-of-bounds rectangles are clamped, and a
 * zero width or height clips the draw away entirely. A negative width or height
 * is not a rectangle QRhi can express: it drops such a scissor and leaves the
 * previous one in force, so queue() rejects it instead of recording a draw
 * under someone else's clip.
 */
struct clip_rect_t
{
    bool enabled = false;
    int  x       = 0;
    int  y       = 0;
    int  width   = 0;
    int  height  = 0;
};

/// Column-major 4x4 identity, the default draw transform.
inline constexpr std::array<float, 16> k_identity_transform = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};

/**
 * @brief How one queued batch is drawn.
 *
 * The transform maps the batch's own coordinates to clip space and is
 * column-major, matching QMatrix4x4::constData() and glm::value_ptr(). For text
 * laid out in framebuffer pixels, pixel_ortho_transform() below builds the
 * matching matrix for the frame's backend.
 *
 * The colour is straight (non-premultiplied) RGBA in [0, 1]. The base shader
 * premultiplies it by glyph coverage and blends premultiplied source over the
 * target; the styled shader writes straight colour and blends with SrcAlpha.
 *
 * The optional records below are absent by default. A draw state that carries
 * none of them is drawn exactly as a build without draw_capabilities.h would
 * draw it. A draw state that carries any of them is a styled draw: its batch
 * must carry per-glyph frame rectangles, and its coordinates must be
 * framebuffer pixels under pixel_ortho_transform(), because the styled shader
 * measures its filter steps in output pixels. Each record is validated at its
 * own boundary, so a rejected styled draw leaves the rest of the frame - base
 * text included - untouched.
 */
struct draw_state_t
{
    std::array<float, 16> transform = k_identity_transform;
    std::array<float, 4>  color     = {1.0f, 1.0f, 1.0f, 1.0f};
    clip_rect_t           clip;

    std::optional<lcd_style_t>  lcd;
    std::optional<glow_style_t> glow;
    std::optional<sdf_mask_t>   sdf_mask;
};

/**
 * @brief Column-major transform from top-left-origin framebuffer pixels to clip space.
 *
 * Text laid out with append_text_quads() uses screen-style Y-down pixels, so
 * this is the transform a consumer that draws text directly into the frame's
 * render target needs. It includes the backend's clip-space correction, which
 * is why it is derived from the frame rather than from a size alone. Returns
 * the identity when the frame has no QRhi or render target.
 */
[[nodiscard]] std::array<float, 16> pixel_ortho_transform(const frame_t& frame);

/// Observable resource and frame state, for diagnostics and gates.
struct renderer_diagnostics_t
{
    /// Increments whenever the whole device-local resource set is dropped.
    std::uint64_t resource_generation    = 0;
    /// Increments whenever the base-path graphics pipeline is created.
    std::uint64_t pipeline_builds        = 0;
    /**
     * @brief Increments whenever the styled graphics pipeline is created.
     *
     * A frame that queues no optional draw capability never builds it, which is
     * how a consumer can see that the base path really is the base path.
     */
    std::uint64_t styled_pipeline_builds = 0;
    /**
     * @brief Increments whenever an atlas upload is put into a frame's batch.
     *
     * An enqueued upload is not an executed one, and this renderer never sees
     * the submission that would execute it. The count therefore says how often
     * the atlas was offered to a frame, which rises again whenever a frame that
     * carried an upload did not reach record().
     */
    std::uint64_t atlas_upload_enqueues = 0;
    /**
     * @brief Increments per geometry or uniform upload put into a frame's batch.
     *
     * One per non-empty buffer per prepare(), so at most three for the base
     * path and three more once a frame also has styled draws. Like the atlas
     * count above, it says how often an upload was offered to a frame.
     */
    std::uint64_t buffer_upload_enqueues = 0;

    std::size_t   vertex_buffer_bytes  = 0;
    std::size_t   index_buffer_bytes   = 0;
    std::size_t   uniform_buffer_bytes = 0;

    /// Zero until a frame queues a styled draw, and separate buffers thereafter.
    std::size_t   styled_vertex_buffer_bytes  = 0;
    std::size_t   styled_index_buffer_bytes   = 0;
    std::size_t   styled_uniform_buffer_bytes = 0;

    /**
     * @brief Draw calls and index elements the frame will issue, over both paths.
     *
     * Cleared by begin_frame(), record(), and reset_frame(). A draw state with
     * a glow contributes two draw calls, because the glow is drawn under the
     * glyphs of that draw rather than composited into them.
     */
    std::size_t   queued_draws   = 0;
    std::size_t   queued_indices = 0;
    /// Draws issued by the most recent record(); survives the frame reset.
    std::size_t   recorded_draws = 0;
    /**
     * @brief Pipeline bindings the most recent record() issued.
     *
     * One per run of consecutive draws on the same pipeline, so a frame of base
     * text binds once and a frame that alternates paths binds once per change.
     */
    std::size_t   recorded_pipeline_binds = 0;
};

/**
 * @brief Device-local QRhi text resources and the per-frame text recording.
 *
 * One instance belongs to one renderer, one window, and one QRhi device, and
 * lives on the thread that drives that device's frames. Nothing here is shared
 * between devices or between threads, and there is no process-global QRhi
 * object: two renderers on two windows own two independent texture, buffer,
 * and pipeline sets even when they draw the same font snapshot.
 *
 * A frame is begin_frame(), one queue() per draw state, prepare() before the
 * host opens its pass, and record() inside the pass. The resource set is
 * rebuilt when the QRhi changes, and the pipeline when the render-pass
 * descriptor or sample count changes, even if the queued text did not change.
 *
 * Draws with no optional capability and draws with one are recorded from
 * separate geometry, uniform, and pipeline sets, in the order they were queued.
 * The base set is exactly what a build without draw_capabilities.h produces,
 * and the styled set is created only once a frame queues a styled draw.
 *
 * QRhi requires its resources to be destroyed before the device is, so the
 * owner calls release_resources() on the render thread while the device is
 * still alive, or destroys this object at that point.
 */
class Text_renderer
{
public:
    Text_renderer();
    ~Text_renderer();

    Text_renderer(const Text_renderer&)            = delete;
    Text_renderer& operator=(const Text_renderer&) = delete;
    Text_renderer(Text_renderer&&)                 = delete;
    Text_renderer& operator=(Text_renderer&&)      = delete;

    /**
     * @brief Set or replace the snapshot this renderer draws with.
     *
     * A replacement takes effect for frames that queue their first text after
     * this call. A frame that has already queued text keeps drawing the
     * snapshot that text was queued against, so replacing the font can never
     * move queued vertices, UVs, or smoothing data onto a different atlas; a
     * batch laid out against the replacement is rejected until the next
     * begin_frame().
     *
     * A different snapshot is uploaded by the next prepare() that draws it. The
     * renderer keeps a reference to the snapshot whose bytes the atlas texture
     * holds until it uploads another one, so the bytes an enqueued upload
     * refers to stay alive whether or not the host submits that frame.
     */
    void set_font(std::shared_ptr<const Font_snapshot> font);
    [[nodiscard]] const std::shared_ptr<const Font_snapshot>& font() const;

    /// Drop everything queued for the previous frame and start a new one.
    void begin_frame();

    /**
     * @brief Queue one prepared batch under one draw state.
     *
     * Batches accumulate into one vertex and one index buffer per path, so the
     * recorded draw count follows the number of queued draw states, not the
     * number of glyphs. An empty batch is accepted and queues nothing. A draw
     * state carrying a glow queues two draws: the glow under that same draw's
     * glyphs, so a later glyph's glow cannot darken an earlier glyph's body.
     * Draw states themselves stay in the order they were queued.
     *
     * The frame's first batch with geometry fixes the font snapshot for the
     * whole frame. Every later batch must be laid out against that same
     * snapshot, and the atlas prepare() uploads and the smoothing data record()
     * draws with are that snapshot's, so a frame can never combine one
     * snapshot's geometry with another's atlas.
     *
     * Fails with NO_FONT when the batch has geometry and no snapshot is set,
     * with CAPABILITY_UNSUPPORTED when an optional record names a version this
     * build does not implement, with INVALID_ARGUMENT when the batch was laid
     * out against a different font than the frame's, when the draw state
     * enables a clip rectangle with a negative width or height, or when an
     * optional record's values or combination cannot describe a drawable
     * result, with GEOMETRY_LIMIT_EXCEEDED when the frame's accumulated
     * geometry would exceed one QRhi buffer or the index range, and with
     * OUT_OF_MEMORY when the frame's geometry could not be allocated. A failed
     * queue leaves the frame exactly as it was, so the draws around it record
     * unchanged.
     *
     * The optional records reject these combinations. A subpixel order other
     * than NONE needs an opaque draw colour, an opaque background colour - both
     * measured against lcd::shader_reference::k_lcd_opaque_alpha_cutoff - and no
     * glow, because its output is only the intended image when it lands on the
     * background it was composed against. A glow must have a positive radius
     * and a visible colour. Any styled draw needs a batch that carries per-glyph
     * frame rectangles. prepare() rejects a styled transform other than
     * pixel_ortho_transform(frame) before it changes resources or enqueues
     * updates.
     */
    [[nodiscard]] text_result_t queue(const Text_batch& batch, const draw_state_t& state);

    /**
     * @brief Create or update the device-local resources and enqueue uploads.
     *
     * Must run before the host opens its render pass, because the atlas,
     * geometry, and uniform uploads all go into the frame's resource-update
     * batch. A frame with nothing queued still enqueues a changed atlas, so a
     * later frame that does queue text records against current data.
     *
     * Enqueueing is not uploading. QRhi executes a resource-update batch only
     * when the host submits it, and the host may release or abandon it instead,
     * so this call never concludes that the atlas reached the device. An
     * enqueued atlas upload stays outstanding until a frame carrying it reaches
     * record(); a frame that is cancelled, abandoned, or fails before then
     * leaves it outstanding and the next prepare() enqueues it again.
     */
    [[nodiscard]] text_result_t prepare(const frame_t& frame);

    /**
     * @brief Record the prepared draws; must run inside the host's render pass.
     *
     * Reports NOT_PREPARED when text was queued but prepare() did not run or
     * did not succeed, so a frame can never present as text-complete when its
     * text was dropped. The queued state is cleared either way; the recorded
     * draw count in diagnostics() describes what this call actually issued.
     *
     * Reaching this call with a usable frame is the one point in the sequence
     * at which the renderer knows the host got past submitting the batch
     * prepare() filled: the host opens the pass this call records into with
     * that batch. An atlas upload this frame enqueued is settled here and not
     * before; one an earlier frame left outstanding is not, because no batch
     * this frame submitted carried it. That is a statement about submission
     * only; nothing in this API observes GPU execution, presentation, or
     * display.
     *
     * The pipeline declares scissor use, so every draw sets a scissor: the
     * draw state's clip rectangle, or the render target's full pixel size. The
     * viewport stays as the host set it.
     */
    [[nodiscard]] text_result_t record(const frame_t& frame);

    /// Drop the queued frame without recording it.
    void reset_frame();

    /**
     * @brief Release every device-local QRhi object.
     *
     * Called on the render thread while the device is still alive, for example
     * when the scene graph is invalidated. The next prepare() rebuilds what it
     * needs and re-uploads the atlas.
     */
    void release_resources();

    [[nodiscard]] renderer_diagnostics_t diagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace vnm::msdf_text::rhi
