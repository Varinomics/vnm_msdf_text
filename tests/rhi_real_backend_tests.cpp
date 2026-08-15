// Mandatory real-backend gate: renders MSDF text with a real Windows QRhi
// backend and inspects the rasterized result. The Null backend proves resource
// lifecycle and command recording; only this gate proves the shaders, the atlas
// upload, and the pixels they produce.
//
// The default mode renders offscreen and reads the framebuffer back, which is
// what the assertions below are made of. --window additionally records frames
// against a swap chain on a shown window; that mode reports what the QRhi frame
// calls returned and does not claim anything about presentation or scanout.

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
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
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

std::shared_ptr<const mtr::Font_snapshot> build_snapshot()
{
    static const std::vector<std::uint8_t> s_font = read_test_font();

    std::vector<char32_t> codepoints;
    for (char c : k_sample_text) {
        codepoints.push_back(static_cast<char32_t>(c));
    }

    msdf::options_t options;
    options.atlas_size          = 512;
    options.build_kerning_table = false;

    const mtr::font_snapshot_result_t built = mtr::build_font_snapshot(
        s_font, k_draw_pixel_height, codepoints, options);
    if (!built.snapshot) {
        std::cerr << "FAIL: the sample font snapshot could not be built ("
                  << built.result.diagnostic.data() << ")\n";
    }
    return built.snapshot;
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

// Renders one offscreen frame of the sample text and returns the framebuffer as
// a top-down image, so image row 0 matches the Y-down layout the text uses.
bool render_offscreen(
    QRhi&                                     rhi,
    Offscreen_target&                         offscreen,
    mtr::Text_renderer&                       renderer,
    const std::shared_ptr<const mtr::Font_snapshot>& font,
    const mtr::clip_rect_t&                   clip,
    QImage&                                   out_image)
{
    mtr::Text_batch batch;
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
    state.color     = { 1.0f, 1.0f, 1.0f, 1.0f };
    state.clip      = clip;

    renderer.begin_frame();
    bool ok = check_status(renderer.queue(batch, state), "the sample text must queue");
    ok &= check_status(renderer.prepare(frame), "the sample text must prepare");

    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
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
    bool   ok = render_offscreen(rhi, offscreen, renderer, font, {}, unclipped);
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

    QImage clipped;
    const mtr::clip_rect_t clip = { true, 0, 0, k_clip_width, k_target_height };
    ok &= render_offscreen(rhi, offscreen, renderer, font, clip, clipped);
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

    QImage lower_half;
    const mtr::clip_rect_t lower_clip = { true, 0, 0, k_target_width, split_y };
    ok &= render_offscreen(rhi, offscreen, renderer, font, lower_clip, lower_half);

    QImage upper_half;
    const mtr::clip_rect_t upper_clip = { true, 0, split_y, k_target_width, split_y };
    ok &= render_offscreen(rhi, offscreen, renderer, font, upper_clip, upper_half);

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
