#include <vnm_msdf_text/rhi/text_renderer.h>

#include <rhi/qrhi.h>

#include <QtCore/QFile>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtGui/QMatrix4x4>

#include <algorithm>
#include <cstring>
#include <limits>
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

struct draw_op_t
{
    quint32     index_start    = 0;
    quint32     index_count    = 0;
    quint32     uniform_offset = 0;
    clip_rect_t clip;
};

} // namespace

struct Text_renderer::Impl
{
    std::shared_ptr<const Font_snapshot> font;

    // Held from one upload to the next so the atlas bytes referenced by an
    // enqueued texture upload stay alive even if the caller replaces the font
    // before the host submits the frame's resource-update batch.
    std::shared_ptr<const Font_snapshot> uploaded_font;

    std::vector<text_vertex_t>   vertices;
    std::vector<std::uint32_t>   indices;
    std::vector<uniform_block_t> uniforms;
    std::vector<draw_op_t>       draws;
    std::vector<std::uint8_t>    uniform_staging;

    QRhi*                                       rhi = nullptr;
    std::unique_ptr<QRhiTexture>                atlas_texture;
    std::unique_ptr<QRhiSampler>                sampler;
    std::unique_ptr<QRhiBuffer>                 vertex_buffer;
    std::unique_ptr<QRhiBuffer>                 index_buffer;
    std::unique_ptr<QRhiBuffer>                 uniform_buffer;
    std::unique_ptr<QRhiShaderResourceBindings> srb;
    std::unique_ptr<QRhiGraphicsPipeline>       pipeline;

    QRhiRenderPassDescriptor* pipeline_render_pass = nullptr;
    int                       pipeline_samples     = 0;

    int           atlas_size        = 0;
    std::uint64_t uploaded_revision = 0;

    std::size_t vertex_buffer_bytes  = 0;
    std::size_t index_buffer_bytes   = 0;
    std::size_t uniform_buffer_bytes = 0;

    QShader vertex_shader;
    QShader fragment_shader;
    bool    shaders_loaded = false;

    bool prepared = false;

    std::uint64_t resource_generation = 0;
    std::uint64_t pipeline_builds     = 0;
    std::uint64_t atlas_uploads       = 0;
    std::size_t   recorded_draws      = 0;

    void clear_frame()
    {
        vertices.clear();
        indices.clear();
        uniforms.clear();
        draws.clear();
        prepared = false;
    }

    void release_resources()
    {
        pipeline.reset();
        srb.reset();
        uniform_buffer.reset();
        index_buffer.reset();
        vertex_buffer.reset();
        sampler.reset();
        atlas_texture.reset();
        uploaded_font.reset();

        pipeline_render_pass = nullptr;
        pipeline_samples     = 0;
        atlas_size           = 0;
        uploaded_revision    = 0;
        vertex_buffer_bytes  = 0;
        index_buffer_bytes   = 0;
        uniform_buffer_bytes = 0;
        prepared             = false;

        ++resource_generation;
    }

    [[nodiscard]] text_result_t ensure_atlas(QRhi* device, QRhiResourceUpdateBatch* updates)
    {
        // A snapshot only exists for a non-failed build, and such a build always
        // carries a sized bitmap with at least one glyph.
        const atlas_t& atlas = font->atlas();

        if (atlas_texture &&
            atlas_size == atlas.atlas_size &&
            uploaded_revision == font->revision())
        {
            return {};
        }

        if (!atlas_texture || atlas_size != atlas.atlas_size) {
            atlas_texture.reset(device->newTexture(
                QRhiTexture::RGBA8,
                QSize(atlas.atlas_size, atlas.atlas_size)));
            if (!atlas_texture->create()) {
                atlas_texture.reset();
                atlas_size = 0;
                return detail::make_text_result(
                    Text_status::GPU_RESOURCE_FAILED,
                    "MSDF atlas texture could not be created");
            }
            atlas_size = atlas.atlas_size;
            // The binding set names the old texture, so it is rebuilt below.
            srb.reset();
        }

        const QImage image(
            atlas.rgba.data(),
            atlas.atlas_size,
            atlas.atlas_size,
            atlas.atlas_size * 4,
            QImage::Format_RGBA8888);
        updates->uploadTexture(atlas_texture.get(), image);

        uploaded_font     = font;
        uploaded_revision = font->revision();
        ++atlas_uploads;
        return {};
    }

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
        // A recreated buffer invalidates the binding set that named it.
        srb.reset();
        return {};
    }

    [[nodiscard]] text_result_t ensure_bindings(QRhi* device)
    {
        if (srb) {
            return {};
        }

        srb.reset(device->newShaderResourceBindings());
        srb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                k_uniform_binding,
                QRhiShaderResourceBinding::VertexStage |
                    QRhiShaderResourceBinding::FragmentStage,
                uniform_buffer.get(),
                static_cast<quint32>(sizeof(uniform_block_t))),
            QRhiShaderResourceBinding::sampledTexture(
                k_atlas_binding,
                QRhiShaderResourceBinding::FragmentStage,
                atlas_texture.get(),
                sampler.get()),
        });
        if (!srb->create()) {
            srb.reset();
            return detail::make_text_result(
                Text_status::GPU_RESOURCE_FAILED,
                "text shader resource bindings could not be created");
        }

        // The pipeline was built against the previous layout's resources.
        pipeline.reset();
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
    if (batch.empty()) {
        return {};
    }
    if (!d->font) {
        return detail::make_text_result(
            Text_status::NO_FONT,
            "text was queued before a font snapshot was set");
    }
    if (!batch.font_identity() || *batch.font_identity() != d->font->identity()) {
        return detail::make_text_result(
            Text_status::INVALID_ARGUMENT,
            "the batch was laid out against a different font than the renderer's");
    }

    const std::span<const text_vertex_t> batch_vertices = batch.vertices();
    const std::span<const std::uint32_t> batch_indices  = batch.indices();

    const std::size_t base_vertex = d->vertices.size();
    const std::size_t index_start = d->indices.size();
    if (batch_vertices.size() > k_max_vertices - base_vertex) {
        return detail::make_text_result(
            Text_status::GEOMETRY_LIMIT_EXCEEDED,
            "the frame's text geometry exceeds the addressable index range");
    }

    std::size_t vertex_bytes = 0;
    std::size_t index_bytes  = 0;
    if (!checked_byte_size(
            base_vertex + batch_vertices.size(), sizeof(text_vertex_t), vertex_bytes) ||
        !checked_byte_size(
            index_start + batch_indices.size(), sizeof(std::uint32_t), index_bytes))
    {
        return detail::make_text_result(
            Text_status::GEOMETRY_LIMIT_EXCEEDED,
            "the frame's text geometry exceeds one QRhi buffer");
    }

    d->vertices.insert(d->vertices.end(), batch_vertices.begin(), batch_vertices.end());
    d->indices.reserve(index_start + batch_indices.size());
    const auto base = static_cast<std::uint32_t>(base_vertex);
    for (std::uint32_t index : batch_indices) {
        d->indices.push_back(index + base);
    }

    uniform_block_t block{};
    std::memcpy(block.transform, state.transform.data(), sizeof(block.transform));
    std::memcpy(block.color, state.color.data(), sizeof(block.color));
    block.px_range =
        px_range_for_pixel_height(d->font->atlas(), d->font->draw_pixel_height());
    d->uniforms.push_back(block);

    draw_op_t op;
    op.index_start = static_cast<quint32>(index_start);
    op.index_count = static_cast<quint32>(batch_indices.size());
    op.clip        = state.clip;
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

    // A different QRhi means every object below belongs to a device that is
    // gone, so the whole resource set is rebuilt rather than reused.
    if (d->rhi != frame.rhi) {
        if (d->rhi != nullptr) {
            d->release_resources();
        }
        d->rhi = frame.rhi;
    }

    if (!d->font) {
        if (d->draws.empty()) {
            d->prepared = true;
            return {};
        }
        return detail::make_text_result(
            Text_status::NO_FONT,
            "queued text cannot be prepared without a font snapshot");
    }

    const text_result_t atlas_ready = d->ensure_atlas(frame.rhi, frame.resource_updates);
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
    }

    std::size_t vertex_bytes = 0;
    std::size_t index_bytes  = 0;
    if (!checked_byte_size(d->vertices.size(), sizeof(text_vertex_t), vertex_bytes) ||
        !checked_byte_size(d->indices.size(), sizeof(std::uint32_t), index_bytes))
    {
        return detail::make_text_result(
            Text_status::GEOMETRY_LIMIT_EXCEEDED,
            "prepared text geometry exceeds one QRhi buffer");
    }

    const std::size_t stride =
        aligned_up(sizeof(uniform_block_t), static_cast<std::size_t>(frame.rhi->ubufAlignment()));
    std::size_t uniform_bytes = 0;
    if (!checked_byte_size(d->uniforms.size(), stride, uniform_bytes)) {
        return detail::make_text_result(
            Text_status::GEOMETRY_LIMIT_EXCEEDED,
            "prepared text draw states exceed one QRhi uniform buffer");
    }

    const text_result_t vertex_ready = d->ensure_buffer(
        frame.rhi, d->vertex_buffer, d->vertex_buffer_bytes,
        QRhiBuffer::VertexBuffer, vertex_bytes,
        "text vertex buffer could not be created");
    if (vertex_ready.status != Text_status::OK) {
        return vertex_ready;
    }

    const text_result_t index_ready = d->ensure_buffer(
        frame.rhi, d->index_buffer, d->index_buffer_bytes,
        QRhiBuffer::IndexBuffer, index_bytes,
        "text index buffer could not be created");
    if (index_ready.status != Text_status::OK) {
        return index_ready;
    }

    const text_result_t uniform_ready = d->ensure_buffer(
        frame.rhi, d->uniform_buffer, d->uniform_buffer_bytes,
        QRhiBuffer::UniformBuffer, uniform_bytes,
        "text uniform buffer could not be created");
    if (uniform_ready.status != Text_status::OK) {
        return uniform_ready;
    }

    const text_result_t bindings_ready = d->ensure_bindings(frame.rhi);
    if (bindings_ready.status != Text_status::OK) {
        return bindings_ready;
    }

    const text_result_t pipeline_ready = d->ensure_pipeline(frame.rhi, frame.render_target);
    if (pipeline_ready.status != Text_status::OK) {
        return pipeline_ready;
    }

    d->uniform_staging.assign(uniform_bytes, 0);
    for (std::size_t i = 0; i < d->uniforms.size(); ++i) {
        const std::size_t offset = i * stride;
        std::memcpy(d->uniform_staging.data() + offset, &d->uniforms[i], sizeof(uniform_block_t));
        d->draws[i].uniform_offset = static_cast<quint32>(offset);
    }

    frame.resource_updates->updateDynamicBuffer(
        d->vertex_buffer.get(), 0, static_cast<quint32>(vertex_bytes), d->vertices.data());
    frame.resource_updates->updateDynamicBuffer(
        d->index_buffer.get(), 0, static_cast<quint32>(index_bytes), d->indices.data());
    frame.resource_updates->updateDynamicBuffer(
        d->uniform_buffer.get(), 0, static_cast<quint32>(uniform_bytes), d->uniform_staging.data());

    d->prepared = true;
    return {};
}

text_result_t Text_renderer::record(const frame_t& frame)
{
    d->recorded_draws = 0;

    if (!frame.command_buffer || !frame.render_target) {
        d->clear_frame();
        return detail::make_text_result(
            Text_status::INVALID_FRAME,
            "text recording needs a command buffer and a render target");
    }

    if (d->draws.empty()) {
        d->clear_frame();
        return {};
    }

    if (!d->prepared || !d->pipeline || !d->srb || !d->vertex_buffer || !d->index_buffer) {
        d->clear_frame();
        return detail::make_text_result(
            Text_status::NOT_PREPARED,
            "queued text was never uploaded, so it cannot be recorded");
    }

    const QSize target_size = frame.render_target->pixelSize();

    QRhiCommandBuffer* cb = frame.command_buffer;
    cb->setGraphicsPipeline(d->pipeline.get());

    const QRhiCommandBuffer::VertexInput vertex_input(d->vertex_buffer.get(), 0);
    cb->setVertexInput(
        0, 1, &vertex_input, d->index_buffer.get(), 0, QRhiCommandBuffer::IndexUInt32);

    // queue() refuses an empty batch, so every queued draw has geometry.
    for (const draw_op_t& op : d->draws) {
        const QRhiCommandBuffer::DynamicOffset uniform_offset(k_uniform_binding, op.uniform_offset);
        cb->setShaderResources(d->srb.get(), 1, &uniform_offset);

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
    out.resource_generation  = d->resource_generation;
    out.pipeline_builds      = d->pipeline_builds;
    out.atlas_uploads        = d->atlas_uploads;
    out.vertex_buffer_bytes  = d->vertex_buffer_bytes;
    out.index_buffer_bytes   = d->index_buffer_bytes;
    out.uniform_buffer_bytes = d->uniform_buffer_bytes;
    out.queued_draws         = d->draws.size();
    out.queued_indices       = d->indices.size();
    out.recorded_draws       = d->recorded_draws;
    return out;
}

} // namespace vnm::msdf_text::rhi
