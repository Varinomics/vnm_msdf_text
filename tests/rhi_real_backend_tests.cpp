// Mandatory real-backend gate: renders MSDF text with a real Windows QRhi
// backend and inspects the rasterized result. The Null backend proves resource
// lifecycle and command recording; only this gate proves the shaders, the atlas
// upload, and the pixels they produce.
//
// The default mode renders offscreen and reads the framebuffer back, which is
// what the assertions below are made of. --window additionally records frames
// against a swap chain on a shown window; that mode reports what the QRhi frame
// calls returned and does not claim anything about presentation or scanout.

#include <vnm_msdf_text/lcd_contract.h>
#include <vnm_msdf_text/rhi/draw_capabilities.h>
#include <vnm_msdf_text/rhi/font_snapshot.h>
#include <vnm_msdf_text/rhi/status.h>
#include <vnm_msdf_text/rhi/text_batch.h>
#include <vnm_msdf_text/rhi/text_renderer.h>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QWindow>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace msdf = vnm::msdf_text;
namespace mtr  = vnm::msdf_text::rhi;

namespace {

constexpr int   k_draw_pixel_height = 28;
constexpr int   k_target_width      = 384;
constexpr int   k_target_height     = 72;
constexpr float k_baseline_x        = 10.0f;
constexpr float k_baseline_y        = 48.0f;
constexpr int   k_clip_width        = 96;

const std::string_view k_sample_text = "Varinomics 0123";

bool check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool check_status(const mtr::text_result_t& result, std::string_view message)
{
    if (result.status == mtr::Text_status::OK) {
        return true;
    }

    std::cerr << "FAIL: " << message << " (" << mtr::text_status_name(result.status)
              << ": " << result.diagnostic.data() << ")\n";
    return false;
}

std::vector<std::uint8_t> read_test_font()
{
    std::ifstream file(VNM_MSDF_TEXT_TEST_FONT_FILE, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open test font");
    }

    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::shared_ptr<const mtr::Font_snapshot> build_snapshot(const msdf::options_t& options)
{
    static const std::vector<std::uint8_t> s_font = read_test_font();

    std::vector<char32_t> codepoints;
    for (char c : k_sample_text) {
        codepoints.push_back(static_cast<char32_t>(c));
    }

    const mtr::font_snapshot_result_t built = mtr::build_font_snapshot(
        s_font, k_draw_pixel_height, codepoints, options);
    if (!built.snapshot) {
        std::cerr << "FAIL: the sample font snapshot could not be built ("
                  << built.result.diagnostic.data() << ")\n";
    }
    return built.snapshot;
}

std::shared_ptr<const mtr::Font_snapshot> build_snapshot()
{
    msdf::options_t options;
    options.atlas_size          = 512;
    options.build_kerning_table = false;
    return build_snapshot(options);
}

/// The same text baked with a wider distance range, which pads each glyph quad
/// far enough for an outer glow to have somewhere to fall off into.
std::shared_ptr<const mtr::Font_snapshot> build_padded_snapshot()
{
    msdf::options_t options;
    options.atlas_size          = 512;
    options.build_kerning_table = false;
    options.atlas_px_range      = 10.0f;
    return build_snapshot(options);
}

QRhi::Implementation backend_from_name(std::string_view name)
{
    if (name == "vulkan") { return QRhi::Vulkan; }
    if (name == "d3d12")  { return QRhi::D3D12; }
    if (name == "opengl") { return QRhi::OpenGLES2; }
    return QRhi::D3D11;
}

const char* backend_name(QRhi::Implementation backend)
{
    switch (backend) {
        case QRhi::Null:       return "Null";
        case QRhi::Vulkan:     return "Vulkan";
        case QRhi::OpenGLES2:  return "OpenGL";
        case QRhi::D3D11:      return "D3D11";
        case QRhi::Metal:      return "Metal";
        case QRhi::D3D12:      return "D3D12";
        default:               return "unknown";
    }
}

std::unique_ptr<QRhi> create_backend(QRhi::Implementation backend)
{
    switch (backend) {
        case QRhi::D3D11: {
            QRhiD3D11InitParams params;
            return std::unique_ptr<QRhi>(QRhi::create(backend, &params));
        }
        case QRhi::D3D12: {
            QRhiD3D12InitParams params;
            return std::unique_ptr<QRhi>(QRhi::create(backend, &params));
        }
        default:
            return {};
    }
}

void report_device(const QRhi& rhi)
{
    const QRhiDriverInfo info = rhi.driverInfo();
    std::cout << "backend=" << rhi.backendName()
              << " device=" << info.deviceName.constData()
              << " vendor_id=0x" << std::hex << info.vendorId
              << " device_id=0x" << info.deviceId << std::dec
              << " device_type=" << int(info.deviceType)
              << " frames_in_flight=" << rhi.resourceLimit(QRhi::FramesInFlight)
              << '\n';
}

class Offscreen_target
{
public:
    Offscreen_target(QRhi& rhi, const QSize& size, int sample_count, bool readable)
    {
        QRhiTexture::Flags flags = QRhiTexture::RenderTarget;
        if (readable) {
            flags |= QRhiTexture::UsedAsTransferSource;
        }

        m_texture.reset(rhi.newTexture(QRhiTexture::RGBA8, size, sample_count, flags));
        if (!m_texture || !m_texture->create()) {
            m_texture.reset();
            return;
        }

        m_target.reset(rhi.newTextureRenderTarget({ m_texture.get() }));
        m_render_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_render_pass.get());
        if (!m_target->create()) {
            m_target.reset();
        }
    }

    [[nodiscard]] bool valid() const { return m_target != nullptr; }
    [[nodiscard]] QRhiTextureRenderTarget* target()  const { return m_target.get(); }
    [[nodiscard]] QRhiTexture*             texture() const { return m_texture.get(); }

private:
    std::unique_ptr<QRhiTexture>              m_texture;
    std::unique_ptr<QRhiTextureRenderTarget>  m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_render_pass;
};

/// One rendered frame's variables: what to clip it to, what to clear it to, and
/// which optional draw capabilities the draw state carries.
struct render_options_t
{
    mtr::clip_rect_t                 clip;
    QColor                           clear_color = QColor(0, 0, 0, 0);
    std::array<float, 4>             color       = {1.0f, 1.0f, 1.0f, 1.0f};
    std::optional<mtr::lcd_style_t>  lcd;
    std::optional<mtr::glow_style_t> glow;
    std::optional<mtr::sdf_mask_t>   sdf_mask;

    [[nodiscard]] bool styled() const
    {
        return lcd.has_value() || glow.has_value() || sdf_mask.has_value();
    }
};

// Renders one offscreen frame of the sample text and returns the framebuffer as
// a top-down image, so image row 0 matches the Y-down layout the text uses.
bool render_offscreen(
    QRhi&                                     rhi,
    Offscreen_target&                         offscreen,
    mtr::Text_renderer&                       renderer,
    const std::shared_ptr<const mtr::Font_snapshot>& font,
    const render_options_t&                   options,
    QImage&                                   out_image)
{
    mtr::Text_batch batch;
    if (options.styled() &&
        !check_status(batch.enable_glyph_frames(), "a styled draw's batch must carry frames"))
    {
        return false;
    }
    if (!check_status(
            batch.append_run(*font, k_sample_text, k_baseline_x, k_baseline_y),
            "the sample run must be appended"))
    {
        return false;
    }

    QRhiCommandBuffer* cb = nullptr;
    if (!check(
            rhi.beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
            "the offscreen frame must begin"))
    {
        return false;
    }

    mtr::frame_t frame;
    frame.rhi              = &rhi;
    frame.command_buffer   = cb;
    frame.render_target    = offscreen.target();
    frame.resource_updates = rhi.nextResourceUpdateBatch();

    mtr::draw_state_t state;
    state.transform = mtr::pixel_ortho_transform(frame);
    state.color     = options.color;
    state.clip      = options.clip;
    state.lcd       = options.lcd;
    state.glow      = options.glow;
    state.sdf_mask  = options.sdf_mask;

    renderer.begin_frame();
    bool ok = check_status(renderer.queue(batch, state), "the sample text must queue");
    ok &= check_status(renderer.prepare(frame), "the sample text must prepare");

    cb->beginPass(frame.render_target, options.clear_color, { 1.0f, 0 }, frame.resource_updates);
    cb->setViewport(QRhiViewport(
        0.0f, 0.0f, float(k_target_width), float(k_target_height)));
    ok &= check_status(renderer.record(frame), "the sample text must record");

    QRhiReadbackResult readback;
    QRhiResourceUpdateBatch* readback_batch = rhi.nextResourceUpdateBatch();
    readback_batch->readBackTexture({ offscreen.texture() }, &readback);
    cb->endPass(readback_batch);

    ok &= check(
        rhi.endOffscreenFrame() == QRhi::FrameOpSuccess, "the offscreen frame must end");
    ok &= check(!readback.data.isEmpty(), "the framebuffer readback must return data");
    if (!ok) {
        return false;
    }

    QImage image(
        reinterpret_cast<const uchar*>(readback.data.constData()),
        readback.pixelSize.width(),
        readback.pixelSize.height(),
        QImage::Format_RGBA8888);
    // One operation, two spellings across a Qt generation: flipped() arrived in
    // Qt 6.9 and deprecates mirrored(), and this project supports Qt 6.7 up.
    if (rhi.isYUpInFramebuffer()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        out_image = image.flipped(Qt::Vertical);
#else
        out_image = image.mirrored(false, true);
#endif
    }
    else {
        out_image = image.copy();
    }
    return true;
}

struct coverage_t
{
    int  count        = 0;
    int  opaque_count = 0;
    int  min_x        = 0;
    int  max_x        = 0;
    int  min_y        = 0;
    int  max_y        = 0;
    bool empty        = true;
};

coverage_t measure_coverage(const QImage& image)
{
    coverage_t out;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int alpha = qAlpha(image.pixel(x, y));
            if (alpha <= 32) {
                continue;
            }
            if (out.empty) {
                out.min_x = out.max_x = x;
                out.min_y = out.max_y = y;
                out.empty = false;
            }
            out.min_x = std::min(out.min_x, x);
            out.max_x = std::max(out.max_x, x);
            out.min_y = std::min(out.min_y, y);
            out.max_y = std::max(out.max_y, y);
            ++out.count;
            if (alpha >= 240) {
                ++out.opaque_count;
            }
        }
    }
    return out;
}

// The gate renders light text on a transparent target so the alpha channel is
// the coverage evidence. Composite it over an opaque background for the saved
// image, which is what a human reads when accepting the render.
QImage composite_over_background(const QImage& rendered)
{
    QImage preview(rendered.size(), QImage::Format_RGBA8888);
    preview.fill(QColor(20, 24, 34));

    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            const QRgb source     = rendered.pixel(x, y);
            const QRgb background = preview.pixel(x, y);
            const int  alpha      = qAlpha(source);

            // The shader writes premultiplied colour, so source over is a plain
            // add of the destination scaled by the remaining coverage.
            const auto blend = [&](int source_channel, int background_channel) {
                return std::clamp(
                    source_channel + background_channel * (255 - alpha) / 255, 0, 255);
            };
            preview.setPixel(x, y, qRgb(
                blend(qRed(source),   qRed(background)),
                blend(qGreen(source), qGreen(background)),
                blend(qBlue(source),  qBlue(background))));
        }
    }
    return preview;
}

bool run_offscreen_gate(QRhi& rhi, const QString& image_path)
{
    const std::shared_ptr<const mtr::Font_snapshot> font = build_snapshot();
    if (!check(font != nullptr, "the sample font snapshot must build")) {
        return false;
    }

    Offscreen_target offscreen(rhi, QSize(k_target_width, k_target_height), 1, true);
    if (!check(offscreen.valid(), "the offscreen render target must be created")) {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(font);

    QImage unclipped;
    bool   ok = render_offscreen(rhi, offscreen, renderer, font, render_options_t{}, unclipped);
    if (!ok) {
        renderer.release_resources();
        return false;
    }

    if (!image_path.isEmpty()) {
        ok &= check(
            composite_over_background(unclipped).save(image_path, "PNG"),
            "the rendered text image must be saved");
        std::cout << "rendered_image=" << image_path.toStdString() << '\n';
    }

    const coverage_t drawn = measure_coverage(unclipped);
    std::cout << "coverage_pixels=" << drawn.count
              << " opaque_pixels=" << drawn.opaque_count
              << " x=[" << drawn.min_x << "," << drawn.max_x << "]"
              << " y=[" << drawn.min_y << "," << drawn.max_y << "]\n";

    ok &= check(!drawn.empty, "the real backend must rasterize glyph coverage");
    ok &= check(drawn.count > 200, "the sample text must cover a text-sized area");

    // Glyphs must be filled, not edge-only: a distance field reconstructed with
    // the wrong range or channel produces outlines with almost no solid pixels.
    ok &= check(
        drawn.opaque_count * 4 > drawn.count,
        "rasterized glyphs must be filled rather than outlined");

    // The rasterized extent must agree with what the CPU measurement promised,
    // which is what lets a consumer clip and lay out against those numbers.
    const msdf::text_bounds_t bounds =
        msdf::measure_text_bounds_px(font->atlas(), font->draw_pixel_height(), k_sample_text);
    ok &= check(bounds.has_visible_glyphs, "the sample text must measure visible glyphs");

    constexpr int k_slack = 2;
    ok &= check(
        drawn.min_x >= int(k_baseline_x + bounds.left)   - k_slack &&
        drawn.max_x <= int(k_baseline_x + bounds.right)  + k_slack,
        "rasterized coverage must stay inside the measured horizontal bounds");
    ok &= check(
        drawn.min_y >= int(k_baseline_y + bounds.top)    - k_slack &&
        drawn.max_y <= int(k_baseline_y + bounds.bottom) + k_slack,
        "rasterized coverage must stay inside the measured vertical bounds");

    QImage           clipped;
    render_options_t clipped_options;
    clipped_options.clip = { true, 0, 0, k_clip_width, k_target_height };
    ok &= render_offscreen(rhi, offscreen, renderer, font, clipped_options, clipped);
    if (ok) {
        const coverage_t clipped_coverage = measure_coverage(clipped);
        std::cout << "clipped_coverage_pixels=" << clipped_coverage.count
                  << " x=[" << clipped_coverage.min_x << "," << clipped_coverage.max_x << "]\n";
        ok &= check(!clipped_coverage.empty, "a clipped draw must still rasterize text");
        ok &= check(
            clipped_coverage.max_x < k_clip_width,
            "a clipped draw must not rasterize outside its scissor");
        ok &= check(
            clipped_coverage.count < drawn.count,
            "a clipped draw must rasterize less than the unclipped draw");
    }

    // Which half of the target a scissor keeps is the only direct evidence of
    // the origin clip_rect_t documents. A full-height rectangle cannot show it,
    // because it is the same rectangle either way up; a half-height one is not.
    // Read-back images are top-left origin, so a scissor at y = 0 that keeps the
    // lower half of the image is a bottom-left scissor.
    const int split_y = k_target_height / 2;
    ok &= check(
        drawn.min_y < split_y && drawn.max_y > split_y,
        "the clip fixture needs text crossing the target's middle row");

    QImage           lower_half;
    render_options_t lower_options;
    lower_options.clip = { true, 0, 0, k_target_width, split_y };
    ok &= render_offscreen(rhi, offscreen, renderer, font, lower_options, lower_half);

    QImage           upper_half;
    render_options_t upper_options;
    upper_options.clip = { true, 0, split_y, k_target_width, split_y };
    ok &= render_offscreen(rhi, offscreen, renderer, font, upper_options, upper_half);

    if (ok) {
        const coverage_t lower = measure_coverage(lower_half);
        const coverage_t upper = measure_coverage(upper_half);
        std::cout << "clip_origin_lower_y=[" << lower.min_y << "," << lower.max_y << "]"
                  << " clip_origin_upper_y=[" << upper.min_y << "," << upper.max_y << "]\n";

        ok &= check(
            !lower.empty && !upper.empty,
            "both half-target clips must still rasterize part of the text");
        ok &= check(
            lower.min_y >= split_y,
            "a scissor at y = 0 must keep the lower half, as bottom-left origin means");
        ok &= check(
            upper.max_y < split_y,
            "a scissor at y = height / 2 must keep the upper half");
        ok &= check(
            lower.count + upper.count == drawn.count,
            "the two half-target clips must partition the unclipped coverage");
    }

    // A changed sample count must rebuild the pipeline even though the text did
    // not change. Backends that offer only one sample count report that here.
    const QList<int> samples = rhi.supportedSampleCounts();
    if (samples.contains(4)) {
        Offscreen_target multisampled(rhi, QSize(k_target_width, k_target_height), 4, false);
        if (check(multisampled.valid(), "the multisampled render target must be created")) {
            const std::uint64_t before = renderer.diagnostics().pipeline_builds;

            mtr::Text_batch batch;
            ok &= check_status(
                batch.append_run(*font, k_sample_text, k_baseline_x, k_baseline_y),
                "the multisampled run must be appended");

            QRhiCommandBuffer* cb = nullptr;
            ok &= check(
                rhi.beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
                "the multisampled frame must begin");

            mtr::frame_t frame;
            frame.rhi              = &rhi;
            frame.command_buffer   = cb;
            frame.render_target    = multisampled.target();
            frame.resource_updates = rhi.nextResourceUpdateBatch();

            mtr::draw_state_t state;
            state.transform = mtr::pixel_ortho_transform(frame);

            renderer.begin_frame();
            ok &= check_status(renderer.queue(batch, state), "the multisampled text must queue");
            ok &= check_status(renderer.prepare(frame), "the multisampled text must prepare");
            cb->beginPass(
                frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
            cb->setViewport(QRhiViewport(
                0.0f, 0.0f, float(k_target_width), float(k_target_height)));
            ok &= check_status(renderer.record(frame), "the multisampled text must record");
            cb->endPass();
            ok &= check(
                rhi.endOffscreenFrame() == QRhi::FrameOpSuccess,
                "the multisampled frame must end");

            ok &= check(
                renderer.diagnostics().pipeline_builds > before,
                "a changed sample count must rebuild the text pipeline");
            std::cout << "sample_count_pipeline_builds="
                      << renderer.diagnostics().pipeline_builds << '\n';
        }
    }
    else {
        std::cout << "sample_count_gate=skipped (backend offers one sample count)\n";
    }

    renderer.release_resources();
    return ok;
}

// -----------------------------------------------------------------------------
// Optional draw capabilities
//
// These render onto an opaque background, because that is the destination an
// LCD draw is composed against, so coverage is read from luminance rather than
// from alpha. Every assertion below follows from what the capability is defined
// to compute - a reversed channel order swaps R and B, a minimum cannot exceed
// either operand, an outer glow can only add coverage - so none of them needs a
// stored image to compare against, and none is minted from a first run.
// -----------------------------------------------------------------------------

constexpr float k_styled_glow_radius = 4.0f;

/// The opaque destination an LCD draw is composed against in these gates.
QColor styled_background() { return QColor(0, 0, 0, 255); }

struct luma_coverage_t
{
    int       count = 0;
    long long total = 0;
    int       min_x = 0;
    int       max_x = 0;
    int       min_y = 0;
    int       max_y = 0;
    bool      empty = true;
};

int pixel_luma(QRgb pixel)
{
    return std::max(qRed(pixel), std::max(qGreen(pixel), qBlue(pixel)));
}

luma_coverage_t measure_luma_coverage(const QImage& image, int threshold)
{
    luma_coverage_t out;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int luma = pixel_luma(image.pixel(x, y));
            out.total += luma;
            if (luma <= threshold) {
                continue;
            }
            if (out.empty) {
                out.min_x = out.max_x = x;
                out.min_y = out.max_y = y;
                out.empty = false;
            }
            out.min_x = std::min(out.min_x, x);
            out.max_x = std::max(out.max_x, x);
            out.min_y = std::min(out.min_y, y);
            out.max_y = std::max(out.max_y, y);
            ++out.count;
        }
    }
    return out;
}

render_options_t opaque_options()
{
    render_options_t options;
    options.clear_color = styled_background();
    return options;
}

mtr::lcd_style_t lcd_on_background(msdf::lcd::Resolved_lcd_subpixel_order order)
{
    const QColor background = styled_background();

    mtr::lcd_style_t style;
    style.order            = order;
    style.background_color = {
        float(background.redF()),
        float(background.greenF()),
        float(background.blueF()),
        1.0f };
    return style;
}

mtr::glow_style_t grey_glow()
{
    mtr::glow_style_t style;
    style.color     = { 0.35f, 0.35f, 0.35f, 1.0f };
    style.radius_px = k_styled_glow_radius;
    return style;
}

/// Saves a rendered frame beside the gate's own image, for a human to look at.
/// Nothing asserts against these files: they are evidence, not an oracle.
void save_beside(const QImage& image, const QString& base_path, const char* suffix)
{
    if (base_path.isEmpty()) {
        return;
    }

    QString         path = base_path;
    const qsizetype dot  = path.lastIndexOf(QLatin1Char('.'));
    path.insert(dot >= 0 ? dot : path.size(), QStringLiteral("_") + QString::fromLatin1(suffix));
    if (image.save(path, "PNG")) {
        std::cout << "rendered_image=" << path.toStdString() << '\n';
    }
}

// This test-only fixture changes a just-built, otherwise unobserved snapshot
// before its first upload, then restores it before the snapshot leaves scope.
// It creates the adversarial MTSDF texel RGB > 0, alpha == 0 exactly; public
// snapshots remain immutable to component consumers.
bool run_alpha_zero_mtsdf_adversarial_gate(QRhi& rhi, const QString& image_path)
{
    const std::shared_ptr<const mtr::Font_snapshot> font = build_snapshot();
    if (!check(font != nullptr, "the adversarial MTSDF snapshot must build")) {
        return false;
    }

    msdf::atlas_t& atlas = const_cast<msdf::atlas_t&>(font->atlas());
    const std::vector<std::uint8_t> original = atlas.rgba;
    for (std::size_t byte = 0; byte < atlas.rgba.size(); byte += 4u) {
        atlas.rgba[byte + 0u] = 255u;
        atlas.rgba[byte + 1u] = 255u;
        atlas.rgba[byte + 2u] = 255u;
        atlas.rgba[byte + 3u] = 0u;
    }

    Offscreen_target offscreen(rhi, QSize(k_target_width, k_target_height), 1, true);
    if (!check(offscreen.valid(), "the adversarial MTSDF target must be valid")) {
        atlas.rgba = original;
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(font);

    render_options_t unmasked;
    unmasked.lcd = lcd_on_background(msdf::lcd::Resolved_lcd_subpixel_order::NONE);
    render_options_t masked;
    masked.sdf_mask = mtr::sdf_mask_t{};
    render_options_t glow;
    glow.color = { 1.0f, 1.0f, 1.0f, 0.0f };
    mtr::glow_style_t alpha_zero_glow;
    alpha_zero_glow.color     = { 0.5f, 0.5f, 0.5f, 1.0f };
    alpha_zero_glow.radius_px = 0.25f;
    glow.glow                 = alpha_zero_glow;

    QImage unmasked_image;
    QImage masked_image;
    QImage glow_image;
    bool ok = render_offscreen(rhi, offscreen, renderer, font, unmasked, unmasked_image);
    ok     &= render_offscreen(rhi, offscreen, renderer, font, masked, masked_image);
    ok     &= render_offscreen(rhi, offscreen, renderer, font, glow, glow_image);

    if (ok) {
        save_beside(unmasked_image, image_path, "styled_rgb_positive_alpha_zero_unmasked");
        save_beside(masked_image, image_path, "styled_rgb_positive_alpha_zero_masked");
        save_beside(glow_image, image_path, "styled_rgb_positive_alpha_zero_glow");
        const coverage_t unmasked_coverage = measure_coverage(unmasked_image);
        const coverage_t masked_coverage   = measure_coverage(masked_image);
        const coverage_t glow_coverage     = measure_coverage(glow_image);
        std::cout << "alpha_zero_unmasked=" << unmasked_coverage.count
                  << " alpha_zero_masked=" << masked_coverage.count
                  << " alpha_zero_glow=" << glow_coverage.count << '\n';
        ok &= check(
            !unmasked_coverage.empty,
            "positive MTSDF RGB must make the adversarial unmasked control visible");
        ok &= check(
            masked_coverage.empty,
            "MTSDF alpha zero must mask positive RGB coverage completely");
        ok &= check(
            glow_coverage.empty,
            "MTSDF alpha zero must keep positive RGB from creating a glow");
    }

    renderer.release_resources();
    atlas.rgba = original;
    return ok;
}

bool channels_are_uniform(const QImage& image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) != qGreen(pixel) || qGreen(pixel) != qBlue(pixel)) {
                return false;
            }
        }
    }
    return true;
}

/// True when @p left equals @p right after swapping the red and blue channels.
bool is_red_blue_mirror(const QImage& left, const QImage& right, int tolerance)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const QRgb a = left.pixel(x, y);
            const QRgb b = right.pixel(x, y);
            if (std::abs(qRed(a)   - qBlue(b))  > tolerance ||
                std::abs(qBlue(a)  - qRed(b))   > tolerance ||
                std::abs(qGreen(a) - qGreen(b)) > tolerance)
            {
                return false;
            }
        }
    }
    return true;
}

int max_channel_difference(const QImage& left, const QImage& right)
{
    if (left.size() != right.size()) {
        return 255;
    }

    int worst = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const QRgb a = left.pixel(x, y);
            const QRgb b = right.pixel(x, y);
            worst = std::max(worst, std::abs(qRed(a)   - qRed(b)));
            worst = std::max(worst, std::abs(qGreen(a) - qGreen(b)));
            worst = std::max(worst, std::abs(qBlue(a)  - qBlue(b)));
        }
    }
    return worst;
}

bool run_styled_gates(QRhi& rhi, const QString& image_path)
{
    const std::shared_ptr<const mtr::Font_snapshot> font = build_snapshot();
    if (!check(font != nullptr, "the sample font snapshot must build")) {
        return false;
    }

    Offscreen_target offscreen(rhi, QSize(k_target_width, k_target_height), 1, true);
    if (!check(offscreen.valid(), "the offscreen render target must be created")) {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(font);

    const auto render = [&](const render_options_t& options, QImage& out) {
        return render_offscreen(rhi, offscreen, renderer, font, options, out);
    };

    QImage base_image;
    bool   ok = render(opaque_options(), base_image);

    // The styled path reconstructs where each fragment sits inside its glyph
    // from that glyph's frame rectangle instead of from an interpolated UV, so
    // an order of NONE has to land on the same glyphs as the base path. A wrong
    // rectangle would shift, scale, or shear them, and the comparison below is
    // what makes that visible.
    render_options_t none_options = opaque_options();
    none_options.lcd              = lcd_on_background(msdf::lcd::Resolved_lcd_subpixel_order::NONE);

    QImage none_image;
    ok &= render(none_options, none_image);
    if (!ok) {
        renderer.release_resources();
        return false;
    }
    save_beside(base_image, image_path, "styled_base");
    save_beside(none_image, image_path, "styled_none");

    const int none_difference = max_channel_difference(base_image, none_image);
    const luma_coverage_t base_coverage = measure_luma_coverage(base_image, 32);
    const luma_coverage_t none_coverage = measure_luma_coverage(none_image, 32);
    std::cout << "styled_none_max_channel_difference=" << none_difference
              << " base_coverage=" << base_coverage.count
              << " none_coverage=" << none_coverage.count << '\n';

    // Two paths of the same arithmetic, one writing premultiplied colour and one
    // writing straight colour for the pipeline to multiply, so the rounding of
    // one 8-bit write can differ. Anything beyond that is a real difference.
    ok &= check(none_difference <= 2, "an order of NONE must render what the base path renders");
    ok &= check(!base_coverage.empty, "the base path must rasterize the sample text");
    ok &= check(
        base_coverage.min_x == none_coverage.min_x &&
        base_coverage.max_x == none_coverage.max_x &&
        base_coverage.min_y == none_coverage.min_y &&
        base_coverage.max_y == none_coverage.max_y,
        "frame-rectangle sampling must place the glyphs where the base path does");
    ok &= check(
        channels_are_uniform(none_image),
        "an order of NONE must leave white text grey-scale across the channels");

    // Horizontal and vertical subpixel orders. A reversed order reverses the
    // channel windows and nothing else, so BGR is RGB with red and blue swapped.
    struct order_pair_t
    {
        const char*                              name;
        msdf::lcd::Resolved_lcd_subpixel_order   forward;
        msdf::lcd::Resolved_lcd_subpixel_order   reverse;
        const char*                              forward_suffix;
        const char*                              reverse_suffix;
    };

    const order_pair_t pairs[] = {
        {
            "horizontal",
            msdf::lcd::Resolved_lcd_subpixel_order::RGB,
            msdf::lcd::Resolved_lcd_subpixel_order::BGR,
            "styled_lcd_rgb", "styled_lcd_bgr",
        },
        {
            "vertical",
            msdf::lcd::Resolved_lcd_subpixel_order::VRGB,
            msdf::lcd::Resolved_lcd_subpixel_order::VBGR,
            "styled_lcd_vrgb", "styled_lcd_vbgr",
        },
    };

    for (const order_pair_t& pair : pairs) {
        render_options_t forward_options = opaque_options();
        forward_options.lcd              = lcd_on_background(pair.forward);
        render_options_t reverse_options = opaque_options();
        reverse_options.lcd              = lcd_on_background(pair.reverse);

        QImage forward_image;
        QImage reverse_image;
        bool   rendered = render(forward_options, forward_image);
        rendered       &= render(reverse_options, reverse_image);
        ok             &= rendered;
        if (!rendered) {
            continue;
        }
        save_beside(forward_image, image_path, pair.forward_suffix);
        save_beside(reverse_image, image_path, pair.reverse_suffix);

        const luma_coverage_t filtered = measure_luma_coverage(forward_image, 32);
        std::cout << "lcd_" << pair.name << "_coverage=" << filtered.count
                  << " grayscale_difference=" << max_channel_difference(none_image, forward_image)
                  << '\n';

        ok &= check(!filtered.empty, "a subpixel order must still rasterize the text");
        ok &= check(
            !channels_are_uniform(forward_image),
            "a subpixel order must produce per-channel coverage");
        ok &= check(
            is_red_blue_mirror(forward_image, reverse_image, 1),
            "a reversed subpixel order must swap red and blue and change nothing else");
        ok &= check(
            max_channel_difference(none_image, forward_image) > 2,
            "a subpixel order must differ from the unfiltered draw");
    }

    // An outer glow is drawn on the glyph quads, so how far it can reach is the
    // atlas padding at the draw size. This pair therefore renders against a
    // snapshot baked with a wider distance range - the way a consumer that wants
    // a glow bakes one - and compares the same text with and without it.
    const std::shared_ptr<const mtr::Font_snapshot> padded = build_padded_snapshot();
    if (check(padded != nullptr, "the wider-range font snapshot must build")) {
        mtr::Text_renderer glow_renderer;
        glow_renderer.set_font(padded);

        render_options_t plain_options = opaque_options();
        plain_options.lcd = lcd_on_background(msdf::lcd::Resolved_lcd_subpixel_order::NONE);
        render_options_t glow_options = plain_options;
        glow_options.glow             = grey_glow();
        glow_options.lcd.reset();

        QImage plain_image;
        QImage glow_image;
        bool   rendered =
            render_offscreen(rhi, offscreen, glow_renderer, padded, plain_options, plain_image);
        rendered &=
            render_offscreen(rhi, offscreen, glow_renderer, padded, glow_options, glow_image);
        ok &= rendered;

        if (rendered) {
            save_beside(plain_image, image_path, "styled_glow_absent");
            save_beside(glow_image, image_path, "styled_glow");
            const luma_coverage_t plain = measure_luma_coverage(plain_image, 32);
            const luma_coverage_t glow  = measure_luma_coverage(glow_image, 32);
            std::cout << "glow_coverage=" << glow.count
                      << " plain_coverage=" << plain.count
                      << " glow_x=[" << glow.min_x << "," << glow.max_x << "]"
                      << " plain_x=[" << plain.min_x << "," << plain.max_x << "]"
                      << " glow_y=[" << glow.min_y << "," << glow.max_y << "]"
                      << " plain_y=[" << plain.min_y << "," << plain.max_y << "]\n";

            ok &= check(!plain.empty, "the wider-range snapshot must rasterize the text");
            ok &= check(glow.count > plain.count, "a glow must cover more than the glyphs alone");
            ok &= check(
                glow.min_x <= plain.min_x && glow.max_x >= plain.max_x &&
                glow.min_y <= plain.min_y && glow.max_y >= plain.max_y,
                "a glow must reach at least as far as the glyphs it surrounds");
            ok &= check(
                glow.min_x < plain.min_x || glow.max_x > plain.max_x ||
                glow.min_y < plain.min_y || glow.max_y > plain.max_y,
                "a glow must reach outside the glyphs it surrounds");
            ok &= check(
                glow_renderer.diagnostics().recorded_draws == 2,
                "a glowing draw must record the glow under the glyphs");
        }

        glow_renderer.release_resources();
    }
    else {
        ok = false;
    }

    // Masking with the true signed distance takes the smaller of two coverages,
    // so no pixel can gain and the picture as a whole has to lose.
    render_options_t masked_options = opaque_options();
    masked_options.sdf_mask         = mtr::sdf_mask_t{};

    QImage masked_image;
    if (render(masked_options, masked_image)) {
        save_beside(masked_image, image_path, "styled_sdf_mask");
        const luma_coverage_t masked = measure_luma_coverage(masked_image, 32);
        std::cout << "sdf_mask_total=" << masked.total
                  << " unmasked_total=" << none_coverage.total << '\n';

        bool never_gains = true;
        for (int y = 0; never_gains && y < masked_image.height(); ++y) {
            for (int x = 0; x < masked_image.width(); ++x) {
                if (pixel_luma(masked_image.pixel(x, y)) >
                    pixel_luma(none_image.pixel(x, y)) + 1)
                {
                    never_gains = false;
                    break;
                }
            }
        }
        ok &= check(never_gains, "the true-SDF mask must never add coverage to a pixel");
        ok &= check(
            masked.total < none_coverage.total,
            "the true-SDF mask must remove the coverage the two fields disagree on");
    }
    else {
        ok = false;
    }

    ok &= run_alpha_zero_mtsdf_adversarial_gate(rhi, image_path);

    // Supported combinations, and a styled draw under a scissor.
    render_options_t combined_lcd = opaque_options();
    combined_lcd.lcd              = lcd_on_background(msdf::lcd::Resolved_lcd_subpixel_order::RGB);
    combined_lcd.sdf_mask         = mtr::sdf_mask_t{};

    QImage combined_lcd_image;
    if (render(combined_lcd, combined_lcd_image)) {
        save_beside(combined_lcd_image, image_path, "styled_lcd_rgb_masked");
        ok &= check(
            !measure_luma_coverage(combined_lcd_image, 32).empty,
            "a subpixel order and a mask together must still rasterize text");
        ok &= check(
            !channels_are_uniform(combined_lcd_image),
            "a masked subpixel draw must keep its per-channel coverage");
    }
    else {
        ok = false;
    }

    render_options_t combined_glow = opaque_options();
    combined_glow.glow             = grey_glow();
    combined_glow.sdf_mask         = mtr::sdf_mask_t{};
    combined_glow.clip             = { true, 0, 0, k_clip_width, k_target_height };

    QImage combined_glow_image;
    if (render(combined_glow, combined_glow_image)) {
        save_beside(combined_glow_image, image_path, "styled_glow_masked_clipped");
        const luma_coverage_t clipped = measure_luma_coverage(combined_glow_image, 32);
        std::cout << "styled_clipped_coverage=" << clipped.count
                  << " x=[" << clipped.min_x << "," << clipped.max_x << "]\n";
        ok &= check(!clipped.empty, "a clipped styled draw must still rasterize text");
        ok &= check(
            clipped.max_x < k_clip_width,
            "a styled draw must not rasterize outside its scissor");
        ok &= check(
            renderer.diagnostics().recorded_draws == 2,
            "a glowing styled draw must record the glow and the glyphs");
    }
    else {
        ok = false;
    }

    const mtr::renderer_diagnostics_t after = renderer.diagnostics();
    std::cout << "styled_pipeline_builds=" << after.styled_pipeline_builds
              << " base_pipeline_builds=" << after.pipeline_builds
              << " styled_vertex_bytes=" << after.styled_vertex_buffer_bytes << '\n';
    ok &= check(
        after.styled_pipeline_builds == 1,
        "one target must need the styled pipeline built exactly once");

    renderer.release_resources();
    return ok;
}

bool run_window_frames(QRhi& rhi, QRhi::Implementation backend, int frame_count)
{
    const std::shared_ptr<const mtr::Font_snapshot> font = build_snapshot();
    if (!check(font != nullptr, "the sample font snapshot must build")) {
        return false;
    }

    QWindow window;
    window.setTitle(QStringLiteral("vnm_msdf_text::rhi real backend text"));
    window.resize(k_target_width * 2, k_target_height * 2);
    switch (backend) {
        case QRhi::Vulkan:    window.setSurfaceType(QSurface::VulkanSurface);    break;
        case QRhi::OpenGLES2: window.setSurfaceType(QSurface::OpenGLSurface);    break;
        default:              window.setSurfaceType(QSurface::Direct3DSurface);  break;
    }
    window.show();

    std::unique_ptr<QRhiSwapChain> swap_chain(rhi.newSwapChain());
    swap_chain->setWindow(&window);
    std::unique_ptr<QRhiRenderPassDescriptor> render_pass(
        swap_chain->newCompatibleRenderPassDescriptor());
    swap_chain->setRenderPassDescriptor(render_pass.get());
    if (!check(swap_chain->createOrResize(), "the swap chain must be created")) {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(font);

    bool ok             = true;
    int  recorded_frames = 0;
    for (int i = 0; i < frame_count; ++i) {
        QGuiApplication::processEvents();

        if (swap_chain->currentPixelSize() != swap_chain->surfacePixelSize()) {
            swap_chain->createOrResize();
        }
        if (rhi.beginFrame(swap_chain.get()) != QRhi::FrameOpSuccess) {
            continue;
        }

        mtr::frame_t frame;
        frame.rhi              = &rhi;
        frame.command_buffer   = swap_chain->currentFrameCommandBuffer();
        frame.render_target    = swap_chain->currentFrameRenderTarget();
        frame.resource_updates = rhi.nextResourceUpdateBatch();

        const QSize size = frame.render_target->pixelSize();

        mtr::Text_batch batch;
        ok &= check_status(
            batch.append_run(
                *font,
                k_sample_text,
                k_baseline_x,
                float(size.height()) * 0.5f),
            "the window run must be appended");

        mtr::draw_state_t state;
        state.transform = mtr::pixel_ortho_transform(frame);
        state.color     = { 0.95f, 0.95f, 1.0f, 1.0f };

        renderer.begin_frame();
        ok &= check_status(renderer.queue(batch, state), "the window text must queue");
        ok &= check_status(renderer.prepare(frame), "the window text must prepare");

        frame.command_buffer->beginPass(
            frame.render_target,
            QColor::fromRgbF(0.08f, 0.09f, 0.12f, 1.0f),
            { 1.0f, 0 },
            frame.resource_updates);
        frame.command_buffer->setViewport(QRhiViewport(
            0.0f, 0.0f, float(size.width()), float(size.height())));
        const mtr::text_result_t recorded = renderer.record(frame);
        ok &= check_status(recorded, "the window text must record");
        frame.command_buffer->endPass();

        // endFrame() returning success means the frame's commands were
        // submitted; it is not evidence of presentation or scanout.
        ok &= check(
            rhi.endFrame(swap_chain.get()) == QRhi::FrameOpSuccess,
            "the window frame must end");
        if (recorded.status == mtr::Text_status::OK &&
            renderer.diagnostics().recorded_draws > 0)
        {
            ++recorded_frames;
        }
    }

    std::cout << "window_frames_with_recorded_text=" << recorded_frames
              << "/" << frame_count << '\n';
    ok &= check(recorded_frames == frame_count, "every window frame must record its text");

    renderer.release_resources();
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    QRhi::Implementation backend    = QRhi::D3D11;
    QString              image_path;
    bool                 want_window = false;
    int                  frame_count = 120;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--window") {
            want_window = true;
        }
        else
        if (argument == "--backend" && i + 1 < argc) {
            backend = backend_from_name(argv[++i]);
        }
        else
        if (argument == "--image" && i + 1 < argc) {
            image_path = QString::fromLocal8Bit(argv[++i]);
        }
        else
        if (argument == "--frames" && i + 1 < argc) {
            frame_count = std::max(1, std::atoi(argv[++i]));
        }
    }

    std::unique_ptr<QRhi> rhi = create_backend(backend);
    if (!rhi) {
        std::cerr << "FAIL: the real QRhi backend " << backend_name(backend)
                  << " could not be created\n";
        return 1;
    }
    report_device(*rhi);

    bool ok = true;
    try {
        ok &= run_offscreen_gate(*rhi, image_path);
        ok &= run_styled_gates(*rhi, image_path);
        if (want_window) {
            ok &= run_window_frames(*rhi, backend, frame_count);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "FAIL: real backend gate: " << e.what() << '\n';
        ok = false;
    }

    std::cerr << (ok ? "PASS" : "FAIL") << ": real windows qrhi text\n";
    return ok ? 0 : 1;
}
