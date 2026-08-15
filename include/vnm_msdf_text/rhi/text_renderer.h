#pragma once

#include <vnm_msdf_text/rhi/font_snapshot.h>
#include <vnm_msdf_text/rhi/status.h>
#include <vnm_msdf_text/rhi/text_batch.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

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
 * The colour is straight (non-premultiplied) RGBA in [0, 1]; the shader
 * premultiplies it by glyph coverage and the pipeline blends premultiplied
 * source over the target.
 */
struct draw_state_t
{
    std::array<float, 16> transform = k_identity_transform;
    std::array<float, 4>  color     = {1.0f, 1.0f, 1.0f, 1.0f};
    clip_rect_t           clip;
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
    std::uint64_t resource_generation   = 0;
    /// Increments whenever the graphics pipeline is created.
    std::uint64_t pipeline_builds       = 0;
    /**
     * @brief Increments whenever an atlas upload is put into a frame's batch.
     *
     * An enqueued upload is not an executed one, and this renderer never sees
     * the submission that would execute it. The count therefore says how often
     * the atlas was offered to a frame, which rises again whenever a frame that
     * carried an upload did not reach record().
     */
    std::uint64_t atlas_upload_enqueues = 0;

    std::size_t   vertex_buffer_bytes  = 0;
    std::size_t   index_buffer_bytes   = 0;
    std::size_t   uniform_buffer_bytes = 0;

    /// Cleared by begin_frame(), record(), and reset_frame().
    std::size_t   queued_draws   = 0;
    std::size_t   queued_indices = 0;
    /// Draws issued by the most recent record(); survives the frame reset.
    std::size_t   recorded_draws = 0;
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
     * Batches accumulate into one vertex and one index buffer, so the recorded
     * draw count follows the number of queued draw states, not the number of
     * glyphs. An empty batch is accepted and queues nothing.
     *
     * The frame's first batch with geometry fixes the font snapshot for the
     * whole frame. Every later batch must be laid out against that same
     * snapshot, and the atlas prepare() uploads and the smoothing data record()
     * draws with are that snapshot's, so a frame can never combine one
     * snapshot's geometry with another's atlas.
     *
     * Fails with NO_FONT when the batch has geometry and no snapshot is set,
     * with INVALID_ARGUMENT when the batch was laid out against a different
     * font than the frame's or the draw state enables a clip rectangle with a
     * negative width or height, with GEOMETRY_LIMIT_EXCEEDED when the frame's
     * accumulated geometry would exceed one QRhi buffer or the index range, and
     * with OUT_OF_MEMORY when the frame's geometry could not be allocated. A
     * failed queue leaves the frame exactly as it was.
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
     * that batch. An outstanding atlas upload is settled here and not before.
     * That is a statement about submission only; nothing in this API observes
     * GPU execution, presentation, or display.
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
