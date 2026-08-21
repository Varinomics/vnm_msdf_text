#include <vnm_msdf_text/rhi/text_renderer.h>

#include <vnm_msdf_text/lcd_shader_reference.h>

#include <rhi/qrhi.h>

#include <QtCore/QFile>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtGui/QMatrix4x4>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vnm::msdf_text::rhi {
namespace {

/**
 * @brief The std140 uniform block both text shader stages read.
 *
 * One block is written per queued draw and addressed with a dynamic offset, so
 * the number of binding sets stays at one however many draw states a frame has.
 */
struct uniform_block_t
{
    float transform[16];
    float color[4];
    float px_range;
    float padding[3];
};

static_assert(sizeof(uniform_block_t) == 96, "text uniform block must match the shader's std140 layout");

/**
 * @brief The std140 uniform block of the optional-capability shader stages.
 *
 * Separate from the block above rather than an extension of it, so a frame that
 * queues no optional capability writes exactly the bytes it wrote before this
 * capability set existed.
 */
struct styled_uniform_block_t
{
    float transform[16];
    float color[4];
    float glow_color[4];
    float background_color[4];
    float px_range;
    float target_height;
    float glow_radius;
    float lcd_subpixel_order;
    std::int32_t framebuffer_y_up;
    std::int32_t sdf_mask_enabled;
    float padding[2];
};

static_assert(offsetof(styled_uniform_block_t, color)              ==  64, "styled UBO colour offset");
static_assert(offsetof(styled_uniform_block_t, glow_color)         ==  80, "styled UBO glow colour offset");
static_assert(offsetof(styled_uniform_block_t, background_color)   ==  96, "styled UBO background offset");
static_assert(offsetof(styled_uniform_block_t, px_range)           == 112, "styled UBO px range offset");
static_assert(offsetof(styled_uniform_block_t, framebuffer_y_up)   == 128, "styled UBO y-up offset");
static_assert(offsetof(styled_uniform_block_t, sdf_mask_enabled)   == 132, "styled UBO mask offset");
static_assert(sizeof(styled_uniform_block_t) == 144, "styled uniform block must match the shader's std140 layout");

/**
 * @brief One vertex of the optional-capability path.
 *
 * The base path's interpolated UV is not carried: that stage reconstructs its
 * sample position from the frame rectangle instead, which is what lets it step
 * in output pixels.
 */
struct styled_vertex_t
{
    float x;
    float y;
    float s_min;
    float t_min;
    float s_max;
    float t_max;
    float frame_x;
    float frame_y;
    float frame_width;
    float frame_height;
};

static_assert(sizeof(styled_vertex_t) == 40, "styled text vertex must stay tightly packed");

constexpr int         k_uniform_binding  = 0;
constexpr int         k_atlas_binding    = 1;
constexpr std::size_t k_min_buffer_bytes = 4096;

constexpr std::size_t k_max_qrhi_bytes =
    static_cast<std::size_t>(std::numeric_limits<quint32>::max());

constexpr std::size_t k_max_vertices =
    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

QShader load_shader(const char* file_name)
{
    QFile file(QStringLiteral(":/vnm_msdf_text/shaders/rhi/") + QString::fromLatin1(file_name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

[[nodiscard]] bool checked_byte_size(std::size_t count, std::size_t element_bytes, std::size_t& out)
{
    if (count > k_max_qrhi_bytes / element_bytes) {
        return false;
    }
    out = count * element_bytes;
    return out <= k_max_qrhi_bytes;
}

/// Doubles the current capacity until it holds the request, so a resizing frame
/// does not recreate its buffers again on the next frame of the same size.
[[nodiscard]] bool grown_capacity(std::size_t current, std::size_t needed, std::size_t& out)
{
    if (needed > k_max_qrhi_bytes) {
        return false;
    }

    std::size_t capacity = std::max(current, k_min_buffer_bytes);
    while (capacity < needed) {
        if (capacity > k_max_qrhi_bytes / 2u) {
            capacity = k_max_qrhi_bytes;
            break;
        }
        capacity *= 2u;
    }

    out = std::max(capacity, needed);
    return true;
}

[[nodiscard]] std::size_t aligned_up(std::size_t value, std::size_t alignment)
{
    if (alignment <= 1u) {
        return value;
    }
    return ((value + alignment - 1u) / alignment) * alignment;
}

template <typename Block>
[[nodiscard]] text_result_t stage_uniform_blocks(
    const std::vector<Block>&  blocks,
    std::size_t                stride,
    std::vector<std::uint8_t>& staging)
{
    // Keep the previous staging bytes until the complete replacement exists:
    // prepare() may be retried after an allocation failure with the same queue.
    try {
        std::vector<std::uint8_t> staged(blocks.size() * stride, 0);
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            std::memcpy(staged.data() + i * stride, &blocks[i], sizeof(Block));
        }
        staging.swap(staged);
    }
    catch (const std::bad_alloc&) {
        return detail::make_text_result(
            Text_status::OUT_OF_MEMORY,
            "text uniform blocks could not be staged");
    }
    catch (const std::length_error&) {
        return detail::make_text_result(
            Text_status::OUT_OF_MEMORY,
            "text uniform blocks could not be staged");
    }

    return {};
}

/// Which geometry, uniform, and pipeline set a queued draw belongs to.
enum class Draw_variant : std::uint8_t
{
    BASE,
    STYLED,
};

struct draw_op_t
{
    Draw_variant variant        = Draw_variant::BASE;
    quint32      index_start    = 0;
    quint32      index_count    = 0;
    /// Index into this variant's uniform blocks, resolved to bytes by prepare().
    std::size_t  uniform_index  = 0;
    quint32      uniform_offset = 0;
    clip_rect_t  clip;
};

[[nodiscard]] bool is_unit_interval_color(const std::array<float, 4>& color)
{
    return
        std::isfinite(color[0]) && color[0] >= 0.0f && color[0] <= 1.0f &&
        std::isfinite(color[1]) && color[1] >= 0.0f && color[1] <= 1.0f &&
        std::isfinite(color[2]) && color[2] >= 0.0f && color[2] <= 1.0f &&
        std::isfinite(color[3]) && color[3] >= 0.0f && color[3] <= 1.0f;
}

[[nodiscard]] bool is_styled(const draw_state_t& state)
{
    return state.lcd.has_value() || state.glow.has_value() || state.sdf_mask.has_value();
}

[[nodiscard]] bool is_known_lcd_order(lcd::Resolved_lcd_subpixel_order order)
{
    switch (order) {
        case lcd::Resolved_lcd_subpixel_order::NONE:
        case lcd::Resolved_lcd_subpixel_order::RGB:
        case lcd::Resolved_lcd_subpixel_order::BGR:
        case lcd::Resolved_lcd_subpixel_order::VRGB:
        case lcd::Resolved_lcd_subpixel_order::VBGR:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] text_result_t validate_draw_capability_versions(const draw_state_t& state)
{
    if (state.lcd && state.lcd->version != k_lcd_style_version) {
        return detail::make_text_result(
            Text_status::CAPABILITY_UNSUPPORTED,
            "this build does not implement the requested LCD style version");
    }
    if (state.glow && state.glow->version != k_glow_style_version) {
        return detail::make_text_result(
            Text_status::CAPABILITY_UNSUPPORTED,
            "this build does not implement the requested glow style version");
    }
    if (state.sdf_mask && state.sdf_mask->version != k_sdf_mask_version) {
        return detail::make_text_result(
            Text_status::CAPABILITY_UNSUPPORTED,
            "this build does not implement the requested SDF mask version");
    }

    return {};
}

/**
 * @brief Reject a draw whose optional records this build cannot draw as asked.
 *
 * Each record is checked at its own boundary and the whole draw is refused
 * rather than quietly drawn without the capability, so a caller never gets base
 * text back when it asked for something else. A refusal is local to this draw:
 * queue() changes nothing on failure, and the frame's other draws are untouched.
 */
[[nodiscard]] text_result_t validate_draw_capabilities(
    const draw_state_t& state,
    const Text_batch&   batch)
{
    if (!is_styled(state)) {
        return {};
    }

    if (!is_unit_interval_color(state.color)) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "a styled draw colour must be finite and within [0, 1]");
    }

    if (state.lcd && !is_known_lcd_order(state.lcd->order)) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "an LCD style must name a known subpixel order");
    }

    if (state.glow &&
        (!is_unit_interval_color(state.glow->color) ||
         !std::isfinite(state.glow->radius_px) ||
         state.glow->radius_px <= 0.0f ||
         state.glow->color[3] <= 0.0f))
    {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "a glow style needs a finite [0, 1] colour and a positive radius");
    }

    if (state.lcd && !is_unit_interval_color(state.lcd->background_color)) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "an LCD background colour must be finite and within [0, 1]");
    }

    if (state.lcd && lcd::is_display_specific(state.lcd->order)) {
        constexpr float k_opaque = lcd::shader_reference::k_lcd_opaque_alpha_cutoff;
        if (state.glow) {
            return detail::make_text_result(
                Text_status::INVALID_ARGUMENT,
                "a subpixel order and a glow cannot be drawn in one draw state");
        }
        if (!(state.color[3] >= k_opaque)) {
            return detail::make_text_result(
                Text_status::INVALID_ARGUMENT,
                "a subpixel order needs an opaque draw colour");
        }
        if (!(state.lcd->background_color[3] >= k_opaque)) {
            return detail::make_text_result(
                Text_status::INVALID_ARGUMENT,
                "a subpixel order needs an opaque background colour");
        }
    }

    if (!batch.has_glyph_frames()) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "a styled draw needs a batch that carries per-glyph frame rectangles");
    }

    return {};
}

} // namespace

struct Text_renderer::Impl
{
    std::shared_ptr<const Font_snapshot> font;

    // Captured from font on the frame's first batch with geometry. Everything
    // the frame does - the identity every later batch is checked against, the
    // atlas that is uploaded, the smoothing range in the uniform block - reads
    // this, so replacing font mid-frame cannot change work already queued.
    std::shared_ptr<const Font_snapshot> frame_font;

    // The snapshot whose bytes the atlas texture holds. Reuse is keyed on this
    // retained object rather than on a counter: the renderer owns a reference
    // to it, so no other live snapshot can share its address, and a snapshot is
    // immutable once built.
    std::shared_ptr<const Font_snapshot> committed_atlas;

    // The snapshot of an upload that has been put into a host batch but whose
    // submission this renderer has not seen yet. Retaining it keeps the bytes
    // that upload refers to alive for as long as the host may still submit it.
    std::shared_ptr<const Font_snapshot> outstanding_atlas;
    bool                                 atlas_enqueued_this_frame = false;

    std::vector<text_vertex_t>   vertices;
    std::vector<std::uint32_t>   indices;
    std::vector<uniform_block_t> uniforms;
    std::vector<std::uint8_t>    uniform_staging;

    std::vector<styled_vertex_t>        styled_vertices;
    std::vector<std::uint32_t>          styled_indices;
    std::vector<styled_uniform_block_t> styled_uniforms;
    std::vector<std::uint8_t>           styled_uniform_staging;

    // One list in queue order, so a styled draw composes over the base text
    // queued before it and under the base text queued after it.
    std::vector<draw_op_t> draws;

    QRhi*                                       rhi = nullptr;
    std::unique_ptr<QRhiTexture>                atlas_texture;
    std::unique_ptr<QRhiSampler>                sampler;
    std::unique_ptr<QRhiBuffer>                 vertex_buffer;
    std::unique_ptr<QRhiBuffer>                 index_buffer;
    std::unique_ptr<QRhiBuffer>                 uniform_buffer;
    std::unique_ptr<QRhiShaderResourceBindings> srb;
    std::unique_ptr<QRhiGraphicsPipeline>       pipeline;

    std::unique_ptr<QRhiBuffer>                 styled_vertex_buffer;
    std::unique_ptr<QRhiBuffer>                 styled_index_buffer;
    std::unique_ptr<QRhiBuffer>                 styled_uniform_buffer;
    std::unique_ptr<QRhiShaderResourceBindings> styled_srb;
    std::unique_ptr<QRhiGraphicsPipeline>       styled_pipeline;

    QRhiRenderPassDescriptor* pipeline_render_pass        = nullptr;
    int                       pipeline_samples            = 0;
    QRhiRenderPassDescriptor* styled_pipeline_render_pass = nullptr;
    int                       styled_pipeline_samples     = 0;

    int atlas_size = 0;

    std::size_t vertex_buffer_bytes  = 0;
    std::size_t index_buffer_bytes   = 0;
    std::size_t uniform_buffer_bytes = 0;

    std::size_t styled_vertex_buffer_bytes  = 0;
    std::size_t styled_index_buffer_bytes   = 0;
    std::size_t styled_uniform_buffer_bytes = 0;

    QShader vertex_shader;
    QShader fragment_shader;
    QShader styled_vertex_shader;
    QShader styled_fragment_shader;
    bool    shaders_loaded        = false;
    bool    styled_shaders_loaded = false;

    bool prepared = false;

    std::uint64_t resource_generation     = 0;
    std::uint64_t pipeline_builds         = 0;
    std::uint64_t styled_pipeline_builds  = 0;
    std::uint64_t atlas_upload_enqueues   = 0;
    std::uint64_t buffer_upload_enqueues  = 0;
    std::size_t   recorded_draws          = 0;
    std::size_t   recorded_pipeline_binds = 0;

    /// The snapshot this frame draws, or the current one before anything queues.
    [[nodiscard]] const std::shared_ptr<const Font_snapshot>& active_font() const
    {
        return frame_font ? frame_font : font;
    }

    void clear_frame()
    {
        vertices.clear();
        indices.clear();
        uniforms.clear();
        styled_vertices.clear();
        styled_indices.clear();
        styled_uniforms.clear();
        draws.clear();
        frame_font.reset();
        atlas_enqueued_this_frame = false;
        prepared                  = false;
    }

    /**
     * @brief Accept this frame's enqueued atlas upload as submitted.
     *
     * Called only from record(), which the host runs inside the pass it opened
     * with the batch prepare() filled. Only an upload this frame enqueued was
     * in that batch: one still outstanding from an earlier frame went into a
     * batch this pass says nothing about, so it stays outstanding for the next
     * prepare() to enqueue again. Until a frame carrying an upload gets here it
     * may still be released or abandoned unexecuted.
     */
    void settle_outstanding_atlas()
    {
        if (atlas_enqueued_this_frame && outstanding_atlas) {
            committed_atlas = std::move(outstanding_atlas);
        }
    }

    void release_resources()
    {
        styled_pipeline.reset();
        styled_srb.reset();
        styled_uniform_buffer.reset();
        styled_index_buffer.reset();
        styled_vertex_buffer.reset();
        pipeline.reset();
        srb.reset();
        uniform_buffer.reset();
        index_buffer.reset();
        vertex_buffer.reset();
        sampler.reset();
        atlas_texture.reset();
        committed_atlas.reset();
        outstanding_atlas.reset();

        pipeline_render_pass        = nullptr;
        pipeline_samples            = 0;
        styled_pipeline_render_pass = nullptr;
        styled_pipeline_samples     = 0;
        atlas_size                  = 0;
        vertex_buffer_bytes         = 0;
        index_buffer_bytes          = 0;
        uniform_buffer_bytes        = 0;
        styled_vertex_buffer_bytes  = 0;
        styled_index_buffer_bytes   = 0;
        styled_uniform_buffer_bytes = 0;
        atlas_enqueued_this_frame   = false;
        prepared                    = false;

        ++resource_generation;
    }

    [[nodiscard]] text_result_t ensure_atlas(
        QRhi*                                       device,
        QRhiResourceUpdateBatch*                    updates,
        const std::shared_ptr<const Font_snapshot>& snapshot)
    {
        // A snapshot only exists for a non-failed build, and such a build always
        // carries a sized bitmap with at least one glyph.
        const atlas_t& atlas = snapshot->atlas();

        if (atlas_texture && committed_atlas == snapshot) {
            return {};
        }
        // One enqueue per frame is enough: a second prepare() on the same frame
        // would only overwrite the same command in the same batch.
        if (atlas_enqueued_this_frame && outstanding_atlas == snapshot) {
            return {};
        }

        if (!atlas_texture || atlas_size != atlas.atlas_size) {
            atlas_texture.reset(device->newTexture(
                QRhiTexture::RGBA8,
                QSize(atlas.atlas_size, atlas.atlas_size)));
            if (!atlas_texture->create()) {
                atlas_texture.reset();
                atlas_size = 0;
                committed_atlas.reset();
                return detail::make_text_result(
                    Text_status::GPU_RESOURCE_FAILED,
                    "MSDF atlas texture could not be created");
            }
            atlas_size = atlas.atlas_size;
            // A new texture holds nothing yet, and the binding sets named the
            // old one, so both facts are recorded before the upload goes in.
            committed_atlas.reset();
            srb.reset();
            styled_srb.reset();
        }

        const QImage image(
            atlas.rgba.data(),
            atlas.atlas_size,
            atlas.atlas_size,
            atlas.atlas_size * 4,
            QImage::Format_RGBA8888);
        updates->uploadTexture(atlas_texture.get(), image);

        outstanding_atlas         = snapshot;
        atlas_enqueued_this_frame = true;
        ++atlas_upload_enqueues;
        return {};
    }

    /**
     * @brief Create or grow one dynamic buffer.
     *
     * The caller decides what a replacement invalidates, because only the
     * uniform buffer is named by the binding set: growing the vertex or index
     * buffer must not cost a binding-set and pipeline rebuild.
     */
    [[nodiscard]] text_result_t ensure_buffer(
        QRhi*                        device,
        std::unique_ptr<QRhiBuffer>& buffer,
        std::size_t&                 capacity_bytes,
        QRhiBuffer::UsageFlag        usage,
        std::size_t                  needed_bytes,
        const char*                  what)
    {
        if (buffer && capacity_bytes >= needed_bytes) {
            return {};
        }

        std::size_t capacity = 0;
        if (!grown_capacity(capacity_bytes, needed_bytes, capacity)) {
            return detail::make_text_result(
                Text_status::GEOMETRY_LIMIT_EXCEEDED,
                "prepared text geometry exceeds one QRhi buffer");
        }

        buffer.reset(device->newBuffer(
            QRhiBuffer::Dynamic, usage, static_cast<quint32>(capacity)));
        if (!buffer->create()) {
            buffer.reset();
            capacity_bytes = 0;
            return detail::make_text_result(Text_status::GPU_RESOURCE_FAILED, what);
        }
        capacity_bytes = capacity;
        return {};
    }

    [[nodiscard]] text_result_t ensure_bindings(
        QRhi*                                        device,
        std::unique_ptr<QRhiShaderResourceBindings>& bindings,
        const std::unique_ptr<QRhiBuffer>&           block_buffer,
        std::size_t                                  block_bytes,
        std::unique_ptr<QRhiGraphicsPipeline>&       dependent_pipeline)
    {
        if (bindings) {
            return {};
        }

        bindings.reset(device->newShaderResourceBindings());
        bindings->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                k_uniform_binding,
                QRhiShaderResourceBinding::VertexStage |
                    QRhiShaderResourceBinding::FragmentStage,
                block_buffer.get(),
                static_cast<quint32>(block_bytes)),
            QRhiShaderResourceBinding::sampledTexture(
                k_atlas_binding,
                QRhiShaderResourceBinding::FragmentStage,
                atlas_texture.get(),
                sampler.get()),
        });
        if (!bindings->create()) {
            bindings.reset();
            return detail::make_text_result(
                Text_status::GPU_RESOURCE_FAILED,
                "text shader resource bindings could not be created");
        }

        // A created pipeline keeps a raw pointer to the binding set it was made
        // from, and backends still read it - the Metal one does so from
        // setVertexInput() when no set has been bound for the pipeline yet - so
        // the pipeline goes with the set that is being replaced.
        dependent_pipeline.reset();
        return {};
    }

    [[nodiscard]] text_result_t ensure_pipeline(QRhi* device, QRhiRenderTarget* target)
    {
        QRhiRenderPassDescriptor* render_pass = target->renderPassDescriptor();
        const int                 samples     = target->sampleCount();
        if (pipeline &&
            pipeline_render_pass == render_pass &&
            pipeline_samples == samples)
        {
            return {};
        }

        if (!shaders_loaded) {
            vertex_shader   = load_shader("msdf_text.vert.qsb");
            fragment_shader = load_shader("msdf_text.frag.qsb");
            shaders_loaded  = true;
        }
        if (!vertex_shader.isValid() || !fragment_shader.isValid()) {
            return detail::make_text_result(
                Text_status::SHADER_UNAVAILABLE,
                "the compiled MSDF text shaders could not be loaded");
        }

        QRhiVertexInputLayout layout;
        layout.setBindings({
            QRhiVertexInputBinding(static_cast<quint32>(sizeof(text_vertex_t))),
        });
        layout.setAttributes({
            QRhiVertexInputAttribute(
                0, 0, QRhiVertexInputAttribute::Float2,
                static_cast<quint32>(offsetof(text_vertex_t, x))),
            QRhiVertexInputAttribute(
                0, 1, QRhiVertexInputAttribute::Float2,
                static_cast<quint32>(offsetof(text_vertex_t, s))),
            QRhiVertexInputAttribute(
                0, 2, QRhiVertexInputAttribute::Float4,
                static_cast<quint32>(offsetof(text_vertex_t, s_min))),
        });

        // The shader premultiplies the colour by glyph coverage, so the source
        // factor is One rather than SrcAlpha.
        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable   = true;
        blend.srcColor = QRhiGraphicsPipeline::One;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

        pipeline.reset(device->newGraphicsPipeline());
        pipeline->setShaderStages({
            { QRhiShaderStage::Vertex,   vertex_shader   },
            { QRhiShaderStage::Fragment, fragment_shader },
        });
        pipeline->setVertexInputLayout(layout);
        pipeline->setShaderResourceBindings(srb.get());
        pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        pipeline->setCullMode(QRhiGraphicsPipeline::None);
        pipeline->setTargetBlends({ blend });
        pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
        pipeline->setRenderPassDescriptor(render_pass);
        pipeline->setSampleCount(samples);

        if (!pipeline->create()) {
            pipeline.reset();
            return detail::make_text_result(
                Text_status::GPU_RESOURCE_FAILED,
                "text graphics pipeline could not be created");
        }

        pipeline_render_pass = render_pass;
        pipeline_samples     = samples;
        ++pipeline_builds;
        return {};
    }

    [[nodiscard]] text_result_t ensure_styled_pipeline(QRhi* device, QRhiRenderTarget* target)
    {
        QRhiRenderPassDescriptor* render_pass = target->renderPassDescriptor();
        const int                 samples     = target->sampleCount();
        if (styled_pipeline &&
            styled_pipeline_render_pass == render_pass &&
            styled_pipeline_samples == samples)
        {
            return {};
        }

        if (!styled_shaders_loaded) {
            styled_vertex_shader   = load_shader("msdf_text_styled.vert.qsb");
            styled_fragment_shader = load_shader("msdf_text_styled.frag.qsb");
            styled_shaders_loaded  = true;
        }
        if (!styled_vertex_shader.isValid() || !styled_fragment_shader.isValid()) {
            return detail::make_text_result(
                Text_status::SHADER_UNAVAILABLE,
                "the compiled styled MSDF text shaders could not be loaded");
        }

        QRhiVertexInputLayout layout;
        layout.setBindings({
            QRhiVertexInputBinding(static_cast<quint32>(sizeof(styled_vertex_t))),
        });
        layout.setAttributes({
            QRhiVertexInputAttribute(
                0, 0, QRhiVertexInputAttribute::Float2,
                static_cast<quint32>(offsetof(styled_vertex_t, x))),
            QRhiVertexInputAttribute(
                0, 1, QRhiVertexInputAttribute::Float4,
                static_cast<quint32>(offsetof(styled_vertex_t, s_min))),
            QRhiVertexInputAttribute(
                0, 2, QRhiVertexInputAttribute::Float4,
                static_cast<quint32>(offsetof(styled_vertex_t, frame_x))),
        });

        // This stage writes straight colour, because an LCD draw's one alpha
        // stands for three channel coverages and cannot premultiply them.
        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable   = true;
        blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

        styled_pipeline.reset(device->newGraphicsPipeline());
        styled_pipeline->setShaderStages({
            { QRhiShaderStage::Vertex,   styled_vertex_shader   },
            { QRhiShaderStage::Fragment, styled_fragment_shader },
        });
        styled_pipeline->setVertexInputLayout(layout);
        styled_pipeline->setShaderResourceBindings(styled_srb.get());
        styled_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        styled_pipeline->setCullMode(QRhiGraphicsPipeline::None);
        styled_pipeline->setTargetBlends({ blend });
        styled_pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
        styled_pipeline->setRenderPassDescriptor(render_pass);
        styled_pipeline->setSampleCount(samples);

        if (!styled_pipeline->create()) {
            styled_pipeline.reset();
            return detail::make_text_result(
                Text_status::GPU_RESOURCE_FAILED,
                "styled text graphics pipeline could not be created");
        }

        styled_pipeline_render_pass = render_pass;
        styled_pipeline_samples     = samples;
        ++styled_pipeline_builds;
        return {};
    }

    [[nodiscard]] text_result_t prepare_base(const frame_t& frame, std::size_t& out_stride)
    {
        std::size_t vertex_bytes = 0;
        std::size_t index_bytes  = 0;
        if (!checked_byte_size(vertices.size(), sizeof(text_vertex_t), vertex_bytes) ||
            !checked_byte_size(indices.size(), sizeof(std::uint32_t), index_bytes))
        {
            return detail::make_text_result(
                Text_status::GEOMETRY_LIMIT_EXCEEDED,
                "prepared text geometry exceeds one QRhi buffer");
        }

        const std::size_t stride =
            aligned_up(sizeof(uniform_block_t), static_cast<std::size_t>(frame.rhi->ubufAlignment()));
        std::size_t uniform_bytes = 0;
        if (!checked_byte_size(uniforms.size(), stride, uniform_bytes)) {
            return detail::make_text_result(
                Text_status::GEOMETRY_LIMIT_EXCEEDED,
                "prepared text draw states exceed one QRhi uniform buffer");
        }
        const text_result_t staged = stage_uniform_blocks(uniforms, stride, uniform_staging);
        if (staged.status != Text_status::OK) {
            return staged;
        }
        out_stride = stride;

        // The vertex and index buffers are named by the draw call, not by the
        // binding set, so growing them leaves the bindings and the pipeline alone.
        const text_result_t vertex_ready = ensure_buffer(
            frame.rhi, vertex_buffer, vertex_buffer_bytes,
            QRhiBuffer::VertexBuffer, vertex_bytes,
            "text vertex buffer could not be created");
        if (vertex_ready.status != Text_status::OK) {
            return vertex_ready;
        }

        const text_result_t index_ready = ensure_buffer(
            frame.rhi, index_buffer, index_buffer_bytes,
            QRhiBuffer::IndexBuffer, index_bytes,
            "text index buffer could not be created");
        if (index_ready.status != Text_status::OK) {
            return index_ready;
        }

        const QRhiBuffer* bound_uniform_buffer = uniform_buffer.get();

        const text_result_t uniform_ready = ensure_buffer(
            frame.rhi, uniform_buffer, uniform_buffer_bytes,
            QRhiBuffer::UniformBuffer, uniform_bytes,
            "text uniform buffer could not be created");
        if (uniform_ready.status != Text_status::OK) {
            return uniform_ready;
        }
        if (uniform_buffer.get() != bound_uniform_buffer) {
            // The binding set names this buffer, so a replacement invalidates it.
            srb.reset();
        }

        const text_result_t bindings_ready = ensure_bindings(
            frame.rhi, srb, uniform_buffer, sizeof(uniform_block_t), pipeline);
        if (bindings_ready.status != Text_status::OK) {
            return bindings_ready;
        }

        const text_result_t pipeline_ready = ensure_pipeline(frame.rhi, frame.render_target);
        if (pipeline_ready.status != Text_status::OK) {
            return pipeline_ready;
        }

        frame.resource_updates->updateDynamicBuffer(
            vertex_buffer.get(), 0, static_cast<quint32>(vertex_bytes), vertices.data());
        frame.resource_updates->updateDynamicBuffer(
            index_buffer.get(), 0, static_cast<quint32>(index_bytes), indices.data());
        frame.resource_updates->updateDynamicBuffer(
            uniform_buffer.get(), 0, static_cast<quint32>(uniform_bytes), uniform_staging.data());
        buffer_upload_enqueues += 3u;
        return {};
    }

    [[nodiscard]] text_result_t prepare_styled(const frame_t& frame, std::size_t& out_stride)
    {
        std::size_t vertex_bytes = 0;
        std::size_t index_bytes  = 0;
        if (!checked_byte_size(styled_vertices.size(), sizeof(styled_vertex_t), vertex_bytes) ||
            !checked_byte_size(styled_indices.size(), sizeof(std::uint32_t), index_bytes))
        {
            return detail::make_text_result(
                Text_status::GEOMETRY_LIMIT_EXCEEDED,
                "prepared styled text geometry exceeds one QRhi buffer");
        }

        const std::size_t stride = aligned_up(
            sizeof(styled_uniform_block_t), static_cast<std::size_t>(frame.rhi->ubufAlignment()));
        std::size_t uniform_bytes = 0;
        if (!checked_byte_size(styled_uniforms.size(), stride, uniform_bytes)) {
            return detail::make_text_result(
                Text_status::GEOMETRY_LIMIT_EXCEEDED,
                "prepared styled draw states exceed one QRhi uniform buffer");
        }
        // target_height and framebuffer_y_up describe this frame, so retain the
        // queued blocks unchanged and stage frame-local copies instead.
        std::vector<styled_uniform_block_t> frame_uniforms;
        try {
            frame_uniforms = styled_uniforms;
        }
        catch (const std::bad_alloc&) {
            return detail::make_text_result(
                Text_status::OUT_OF_MEMORY,
                "styled text uniform blocks could not be staged");
        }
        catch (const std::length_error&) {
            return detail::make_text_result(
                Text_status::OUT_OF_MEMORY,
                "styled text uniform blocks could not be staged");
        }

        const auto target_height =
            static_cast<float>(std::max(1, frame.render_target->pixelSize().height()));
        const std::int32_t y_up = frame.rhi->isYUpInFramebuffer() ? 1 : 0;
        for (styled_uniform_block_t& block : frame_uniforms) {
            block.target_height    = target_height;
            block.framebuffer_y_up = y_up;
        }

        const text_result_t staged =
            stage_uniform_blocks(frame_uniforms, stride, styled_uniform_staging);
        if (staged.status != Text_status::OK) {
            return staged;
        }
        out_stride = stride;

        const text_result_t vertex_ready = ensure_buffer(
            frame.rhi, styled_vertex_buffer, styled_vertex_buffer_bytes,
            QRhiBuffer::VertexBuffer, vertex_bytes,
            "styled text vertex buffer could not be created");
        if (vertex_ready.status != Text_status::OK) {
            return vertex_ready;
        }

        const text_result_t index_ready = ensure_buffer(
            frame.rhi, styled_index_buffer, styled_index_buffer_bytes,
            QRhiBuffer::IndexBuffer, index_bytes,
            "styled text index buffer could not be created");
        if (index_ready.status != Text_status::OK) {
            return index_ready;
        }

        const QRhiBuffer* bound_uniform_buffer = styled_uniform_buffer.get();

        const text_result_t uniform_ready = ensure_buffer(
            frame.rhi, styled_uniform_buffer, styled_uniform_buffer_bytes,
            QRhiBuffer::UniformBuffer, uniform_bytes,
            "styled text uniform buffer could not be created");
        if (uniform_ready.status != Text_status::OK) {
            return uniform_ready;
        }
        if (styled_uniform_buffer.get() != bound_uniform_buffer) {
            styled_srb.reset();
        }

        const text_result_t bindings_ready = ensure_bindings(
            frame.rhi, styled_srb, styled_uniform_buffer,
            sizeof(styled_uniform_block_t), styled_pipeline);
        if (bindings_ready.status != Text_status::OK) {
            return bindings_ready;
        }

        const text_result_t pipeline_ready =
            ensure_styled_pipeline(frame.rhi, frame.render_target);
        if (pipeline_ready.status != Text_status::OK) {
            return pipeline_ready;
        }

        frame.resource_updates->updateDynamicBuffer(
            styled_vertex_buffer.get(), 0,
            static_cast<quint32>(vertex_bytes), styled_vertices.data());
        frame.resource_updates->updateDynamicBuffer(
            styled_index_buffer.get(), 0,
            static_cast<quint32>(index_bytes), styled_indices.data());
        frame.resource_updates->updateDynamicBuffer(
            styled_uniform_buffer.get(), 0,
            static_cast<quint32>(uniform_bytes), styled_uniform_staging.data());
        buffer_upload_enqueues += 3u;
        return {};
    }
};

std::array<float, 16> pixel_ortho_transform(const frame_t& frame)
{
    if (!frame.rhi || !frame.render_target) {
        return k_identity_transform;
    }

    const QSize size   = frame.render_target->pixelSize();
    const auto  width  = static_cast<float>(std::max(1, size.width()));
    const auto  height = static_cast<float>(std::max(1, size.height()));

    // Text quads are laid out in screen-style Y-down pixels, so the top edge is
    // y = 0. clipSpaceCorrMatrix() carries the backend's own clip-space
    // convention on top of that.
    QMatrix4x4 matrix = frame.rhi->clipSpaceCorrMatrix();
    matrix.ortho(0.0f, width, height, 0.0f, -1.0f, 1.0f);

    std::array<float, 16> out{};
    std::memcpy(out.data(), matrix.constData(), sizeof(float) * out.size());
    return out;
}

Text_renderer::Text_renderer()
:
    d(std::make_unique<Impl>())
{}

Text_renderer::~Text_renderer() = default;

void Text_renderer::set_font(std::shared_ptr<const Font_snapshot> font)
{
    d->font = std::move(font);
}

const std::shared_ptr<const Font_snapshot>& Text_renderer::font() const
{
    return d->font;
}

void Text_renderer::begin_frame()
{
    d->clear_frame();
}

text_result_t Text_renderer::queue(const Text_batch& batch, const draw_state_t& state)
{
    const text_result_t version_ready = validate_draw_capability_versions(state);
    if (version_ready.status != Text_status::OK) {
        return version_ready;
    }
    if (batch.empty()) {
        return {};
    }
    if (state.clip.enabled && (state.clip.width < 0 || state.clip.height < 0)) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "an enabled clip rectangle cannot have a negative width or height");
    }

    const text_result_t capabilities_ready = validate_draw_capabilities(state, batch);
    if (capabilities_ready.status != Text_status::OK) {
        return capabilities_ready;
    }

    // The frame's first batch with geometry fixes its snapshot; a later
    // set_font() therefore cannot move this frame's work onto another atlas.
    const std::shared_ptr<const Font_snapshot>& snapshot = d->active_font();
    if (!snapshot) {
        return detail::make_text_result(
            Text_status::NO_FONT,
            "text was queued before a font snapshot was set");
    }
    if (!batch.font_identity() || *batch.font_identity() != snapshot->identity()) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "the batch was laid out against a different font than this frame's");
    }

    const std::span<const text_vertex_t> batch_vertices = batch.vertices();
    const std::span<const std::uint32_t> batch_indices  = batch.indices();
    const float px_range =
        px_range_for_pixel_height(snapshot->atlas(), snapshot->draw_pixel_height());

    const bool        styled      = is_styled(state);
    const std::size_t base_vertex = styled ? d->styled_vertices.size() : d->vertices.size();
    const std::size_t index_start = styled ? d->styled_indices.size()  : d->indices.size();
    if (batch_vertices.size() > k_max_vertices - base_vertex) {
        return detail::make_text_result(
            Text_status::GEOMETRY_LIMIT_EXCEEDED,
            "the frame's text geometry exceeds the addressable index range");
    }

    std::size_t vertex_bytes = 0;
    std::size_t index_bytes  = 0;
    if (!checked_byte_size(
            base_vertex + batch_vertices.size(),
            styled ? sizeof(styled_vertex_t) : sizeof(text_vertex_t),
            vertex_bytes) ||
        !checked_byte_size(
            index_start + batch_indices.size(), sizeof(std::uint32_t), index_bytes))
    {
        return detail::make_text_result(
            Text_status::GEOMETRY_LIMIT_EXCEEDED,
            "the frame's text geometry exceeds one QRhi buffer");
    }

    // A glow is drawn as its own pass under the glyphs, so the draw state that
    // asks for one contributes two uniform blocks and two draw ops.
    const std::size_t queued_ops = state.glow ? 2u : 1u;

    // Every container is grown before any of them is written, because every draw
    // op resolves its uniform_index against the matching variant's blocks. A
    // half-applied queue could otherwise leave an op with no uniform block.
    // Past this point each append fits reserved capacity and cannot throw.
    try {
        if (styled) {
            d->styled_vertices.reserve(base_vertex + batch_vertices.size());
            d->styled_indices.reserve(index_start + batch_indices.size());
            d->styled_uniforms.reserve(d->styled_uniforms.size() + queued_ops);
        }
        else {
            d->vertices.reserve(base_vertex + batch_vertices.size());
            d->indices.reserve(index_start + batch_indices.size());
            d->uniforms.reserve(d->uniforms.size() + queued_ops);
        }
        d->draws.reserve(d->draws.size() + queued_ops);
    }
    catch (const std::bad_alloc&) {
        return detail::make_text_result(
            Text_status::OUT_OF_MEMORY,
            "the frame's text geometry could not be allocated");
    }

    if (!d->frame_font) {
        d->frame_font = d->font;
    }

    draw_op_t op;
    op.variant     = styled ? Draw_variant::STYLED : Draw_variant::BASE;
    op.index_start = static_cast<quint32>(index_start);
    op.index_count = static_cast<quint32>(batch_indices.size());
    op.clip        = state.clip;

    const auto base = static_cast<std::uint32_t>(base_vertex);
    if (!styled) {
        uniform_block_t block{};
        std::memcpy(block.transform, state.transform.data(), sizeof(block.transform));
        std::memcpy(block.color, state.color.data(), sizeof(block.color));
        block.px_range = px_range;

        d->vertices.insert(d->vertices.end(), batch_vertices.begin(), batch_vertices.end());
        for (std::uint32_t index : batch_indices) {
            d->indices.push_back(index + base);
        }

        op.uniform_index = d->uniforms.size();
        d->uniforms.push_back(block);
        d->draws.push_back(op);

        d->prepared = false;
        return {};
    }

    // A styled draw needs frame rectangles, and a batch that has them holds one
    // per vertex, so the two spans are the same length.
    const std::span<const glyph_frame_t> batch_frames = batch.glyph_frames();
    for (std::size_t i = 0; i < batch_vertices.size(); ++i) {
        const text_vertex_t& vertex = batch_vertices[i];
        const glyph_frame_t& frame  = batch_frames[i];
        d->styled_vertices.push_back(styled_vertex_t{
            vertex.x,
            vertex.y,
            vertex.s_min,
            vertex.t_min,
            vertex.s_max,
            vertex.t_max,
            frame.x,
            frame.y,
            frame.width,
            frame.height});
    }
    for (std::uint32_t index : batch_indices) {
        d->styled_indices.push_back(index + base);
    }

    styled_uniform_block_t block{};
    std::memcpy(block.transform, state.transform.data(), sizeof(block.transform));
    std::memcpy(block.color, state.color.data(), sizeof(block.color));
    block.px_range           = px_range;
    block.sdf_mask_enabled   = state.sdf_mask ? 1 : 0;
    block.lcd_subpixel_order = state.lcd
        ? lcd::shader_uniform_value(state.lcd->order)
        : lcd::shader_uniform_value(lcd::Resolved_lcd_subpixel_order::NONE);
    if (state.lcd) {
        std::memcpy(
            block.background_color, state.lcd->background_color.data(),
            sizeof(block.background_color));
    }

    if (state.glow) {
        // The glow pass draws the same geometry with the glyphs turned fully
        // transparent, so what lands is the glow alone; the glyph pass then
        // draws over it with no glow of its own.
        styled_uniform_block_t glow_block = block;
        glow_block.color[3] = 0.0f;
        std::memcpy(
            glow_block.glow_color, state.glow->color.data(), sizeof(glow_block.glow_color));
        glow_block.glow_radius = state.glow->radius_px;

        draw_op_t glow_op     = op;
        glow_op.uniform_index = d->styled_uniforms.size();
        d->styled_uniforms.push_back(glow_block);
        d->draws.push_back(glow_op);
    }

    op.uniform_index = d->styled_uniforms.size();
    d->styled_uniforms.push_back(block);
    d->draws.push_back(op);

    d->prepared = false;
    return {};
}

text_result_t Text_renderer::prepare(const frame_t& frame)
{
    if (!frame.rhi || !frame.render_target || !frame.resource_updates) {
        return detail::make_text_result(
            Text_status::INVALID_FRAME,
            "text preparation needs a QRhi, a render target, and a resource-update batch");
    }

    if (!d->styled_uniforms.empty()) {
        const std::array<float, 16> expected = pixel_ortho_transform(frame);
        for (const styled_uniform_block_t& block : d->styled_uniforms) {
            if (!std::equal(
                    std::begin(block.transform), std::end(block.transform), expected.begin()))
            {
                return detail::make_text_result(
                    Text_status::INVALID_ARGUMENT,
                    "styled text must use pixel_ortho_transform() for this frame");
            }
        }
    }

    // A different QRhi means every object below belongs to a device that is
    // gone, so the whole resource set is rebuilt rather than reused.
    if (d->rhi != frame.rhi) {
        if (d->rhi != nullptr) {
            d->release_resources();
        }
        d->rhi = frame.rhi;
    }

    // queue() captures a snapshot for every batch with geometry, so no font
    // here means nothing was queued and there is no atlas to offer either.
    const std::shared_ptr<const Font_snapshot>& snapshot = d->active_font();
    if (!snapshot) {
        d->prepared = true;
        return {};
    }

    const text_result_t atlas_ready =
        d->ensure_atlas(frame.rhi, frame.resource_updates, snapshot);
    if (atlas_ready.status != Text_status::OK) {
        return atlas_ready;
    }

    if (d->draws.empty()) {
        d->prepared = true;
        return {};
    }

    if (!d->sampler) {
        d->sampler.reset(frame.rhi->newSampler(
            QRhiSampler::Linear,
            QRhiSampler::Linear,
            QRhiSampler::None,
            QRhiSampler::ClampToEdge,
            QRhiSampler::ClampToEdge));
        if (!d->sampler->create()) {
            d->sampler.reset();
            return detail::make_text_result(
                Text_status::GPU_RESOURCE_FAILED,
                "MSDF atlas sampler could not be created");
        }
        d->srb.reset();
        d->styled_srb.reset();
    }

    std::size_t base_stride   = 0;
    std::size_t styled_stride = 0;
    if (!d->uniforms.empty()) {
        const text_result_t base_ready = d->prepare_base(frame, base_stride);
        if (base_ready.status != Text_status::OK) {
            return base_ready;
        }
    }
    if (!d->styled_uniforms.empty()) {
        const text_result_t styled_ready = d->prepare_styled(frame, styled_stride);
        if (styled_ready.status != Text_status::OK) {
            return styled_ready;
        }
    }

    for (draw_op_t& op : d->draws) {
        const std::size_t stride =
            (op.variant == Draw_variant::STYLED) ? styled_stride : base_stride;
        op.uniform_offset = static_cast<quint32>(op.uniform_index * stride);
    }

    d->prepared = true;
    return {};
}

text_result_t Text_renderer::record(const frame_t& frame)
{
    d->recorded_draws          = 0;
    d->recorded_pipeline_binds = 0;

    if (!frame.command_buffer || !frame.render_target) {
        d->clear_frame();
        return detail::make_text_result(
            Text_status::INVALID_FRAME,
            "text recording needs a command buffer and a render target");
    }

    // The host opened the pass this call records into with the resource-update
    // batch prepare() filled, so getting here is where an upload this frame
    // enqueued stops being one this renderer may have to offer again. This runs
    // before the checks below because a frame can enqueue the atlas, fail a
    // later step, and still have had its batch submitted.
    d->settle_outstanding_atlas();

    if (d->draws.empty()) {
        d->clear_frame();
        return {};
    }

    const bool base_ready =
        d->uniforms.empty() ||
        (d->pipeline && d->srb && d->vertex_buffer && d->index_buffer);
    const bool styled_ready =
        d->styled_uniforms.empty() ||
        (d->styled_pipeline && d->styled_srb &&
         d->styled_vertex_buffer && d->styled_index_buffer);
    if (!d->prepared || !base_ready || !styled_ready) {
        d->clear_frame();
        return detail::make_text_result(
            Text_status::NOT_PREPARED,
            "queued text was never uploaded, so it cannot be recorded");
    }

    const QSize target_size = frame.render_target->pixelSize();

    QRhiCommandBuffer* cb            = frame.command_buffer;
    bool               bound         = false;
    Draw_variant       bound_variant = Draw_variant::BASE;

    // queue() refuses an empty batch, so every queued draw has geometry.
    for (const draw_op_t& op : d->draws) {
        if (!bound || bound_variant != op.variant) {
            const bool styled = op.variant == Draw_variant::STYLED;
            cb->setGraphicsPipeline(styled ? d->styled_pipeline.get() : d->pipeline.get());

            const QRhiCommandBuffer::VertexInput vertex_input(
                styled ? d->styled_vertex_buffer.get() : d->vertex_buffer.get(), 0);
            cb->setVertexInput(
                0, 1, &vertex_input,
                styled ? d->styled_index_buffer.get() : d->index_buffer.get(),
                0, QRhiCommandBuffer::IndexUInt32);

            bound         = true;
            bound_variant = op.variant;
            ++d->recorded_pipeline_binds;
        }

        const QRhiCommandBuffer::DynamicOffset uniform_offset(k_uniform_binding, op.uniform_offset);
        cb->setShaderResources(
            op.variant == Draw_variant::STYLED ? d->styled_srb.get() : d->srb.get(),
            1, &uniform_offset);

        if (op.clip.enabled) {
            cb->setScissor(QRhiScissor(op.clip.x, op.clip.y, op.clip.width, op.clip.height));
        }
        else {
            cb->setScissor(QRhiScissor(0, 0, target_size.width(), target_size.height()));
        }

        cb->drawIndexed(op.index_count, 1, op.index_start, 0, 0);
        ++d->recorded_draws;
    }

    d->clear_frame();
    return {};
}

void Text_renderer::reset_frame()
{
    d->clear_frame();
}

void Text_renderer::release_resources()
{
    d->release_resources();
    d->rhi = nullptr;
}

renderer_diagnostics_t Text_renderer::diagnostics() const
{
    renderer_diagnostics_t out;
    out.resource_generation    = d->resource_generation;
    out.pipeline_builds        = d->pipeline_builds;
    out.styled_pipeline_builds = d->styled_pipeline_builds;
    out.atlas_upload_enqueues  = d->atlas_upload_enqueues;
    out.buffer_upload_enqueues = d->buffer_upload_enqueues;

    out.vertex_buffer_bytes  = d->vertex_buffer_bytes;
    out.index_buffer_bytes   = d->index_buffer_bytes;
    out.uniform_buffer_bytes = d->uniform_buffer_bytes;

    out.styled_vertex_buffer_bytes  = d->styled_vertex_buffer_bytes;
    out.styled_index_buffer_bytes   = d->styled_index_buffer_bytes;
    out.styled_uniform_buffer_bytes = d->styled_uniform_buffer_bytes;

    out.queued_draws = d->draws.size();
    for (const draw_op_t& op : d->draws) {
        out.queued_indices += op.index_count;
    }
    out.recorded_draws          = d->recorded_draws;
    out.recorded_pipeline_binds = d->recorded_pipeline_binds;
    return out;
}

} // namespace vnm::msdf_text::rhi
