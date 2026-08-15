#include <vnm_msdf_text/rhi/font_snapshot.h>
#include <vnm_msdf_text/rhi/status.h>
#include <vnm_msdf_text/rhi/text_batch.h>
#include <vnm_msdf_text/rhi/text_renderer.h>

#include "rhi/sha256.h"

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QtGui/QColor>
#include <QtGui/QGuiApplication>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace msdf = vnm::msdf_text;
namespace mtr  = vnm::msdf_text::rhi;

namespace {

constexpr int k_draw_pixel_height = 24;
constexpr int k_atlas_size        = 512;

bool check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool check_status(const mtr::text_result_t& result, mtr::Text_status expected, std::string_view message)
{
    if (result.status == expected) {
        return true;
    }

    std::cerr << "FAIL: " << message
              << " (expected " << mtr::text_status_name(expected)
              << ", got " << mtr::text_status_name(result.status)
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

const std::vector<std::uint8_t>& test_font()
{
    static const std::vector<std::uint8_t> s_font = read_test_font();
    return s_font;
}

std::vector<char32_t> covered_codepoints()
{
    const std::string_view sample = "Varinomics 0123456789";

    std::vector<char32_t> codepoints;
    for (char c : sample) {
        codepoints.push_back(static_cast<char32_t>(c));
    }
    return codepoints;
}

msdf::options_t snapshot_options()
{
    msdf::options_t options;
    options.atlas_size          = k_atlas_size;
    options.min_atlas_font_size = 48.0;
    options.build_kerning_table = false;
    return options;
}

mtr::font_snapshot_result_t build_sample_snapshot(
    int                    draw_pixel_height = k_draw_pixel_height,
    const msdf::options_t& options           = snapshot_options())
{
    const std::vector<char32_t> codepoints = covered_codepoints();
    return mtr::build_font_snapshot(
        test_font(), draw_pixel_height, codepoints, options);
}

const mtr::Font_snapshot& shared_snapshot()
{
    static const std::shared_ptr<const mtr::Font_snapshot> s_snapshot =
        build_sample_snapshot().snapshot;
    if (!s_snapshot) {
        throw std::runtime_error("the shared test font snapshot could not be built");
    }
    return *s_snapshot;
}

std::string hex(std::span<const std::uint8_t> bytes)
{
    static constexpr char k_digits[] = "0123456789abcdef";

    std::string out;
    out.reserve(bytes.size() * 2u);
    for (std::uint8_t byte : bytes) {
        out.push_back(k_digits[(byte >> 4) & 0x0Fu]);
        out.push_back(k_digits[byte & 0x0Fu]);
    }
    return out;
}

// -----------------------------------------------------------------------------
// Font snapshot: build status, identity, and revision
// -----------------------------------------------------------------------------

bool test_sha256_known_answer()
{
    // FIPS 180-4 example: SHA-256("abc").
    const std::string_view input = "abc";

    mtr::detail::Sha256 hash;
    hash.update(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(input.data()), input.size()));

    return check(
        hex(hash.finish()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "the snapshot digest must be standard SHA-256");
}

bool test_snapshot_rejects_invalid_arguments()
{
    bool ok = true;

    const std::vector<char32_t> codepoints = covered_codepoints();

    const mtr::font_snapshot_result_t no_bytes = mtr::build_font_snapshot(
        std::span<const std::uint8_t>(), k_draw_pixel_height, codepoints);
    ok &= check_status(
        no_bytes.result, mtr::Text_status::INVALID_ARGUMENT, "empty font bytes must be rejected");
    ok &= check(no_bytes.snapshot == nullptr, "a rejected build must not return a snapshot");

    const mtr::font_snapshot_result_t no_height =
        mtr::build_font_snapshot(test_font(), 0, codepoints);
    ok &= check_status(
        no_height.result,
        mtr::Text_status::INVALID_ARGUMENT,
        "a non-positive draw pixel height must be rejected");
    ok &= check(no_height.snapshot == nullptr, "a rejected build must not return a snapshot");

    return ok;
}

bool test_snapshot_reports_build_failure()
{
    const std::vector<std::uint8_t> not_a_font(4096, 0x7Fu);
    const std::vector<char32_t>     codepoints = covered_codepoints();

    const mtr::font_snapshot_result_t failed =
        mtr::build_font_snapshot(not_a_font, k_draw_pixel_height, codepoints);

    bool ok = true;
    ok &= check_status(
        failed.result, mtr::Text_status::FONT_BUILD_FAILED, "unusable font bytes must fail the build");
    ok &= check(failed.snapshot == nullptr, "a failed build must not return a snapshot");
    ok &= check(
        failed.result.diagnostic[0] != '\0', "a failed build must carry a diagnostic message");
    return ok;
}

bool test_snapshot_reports_partial_build()
{
    // U+4E2D is outside the bundled font's coverage, so the atlas is built
    // without it and the snapshot stays usable.
    std::vector<char32_t> codepoints = covered_codepoints();
    codepoints.push_back(0x4E2D);

    const mtr::font_snapshot_result_t partial = mtr::build_font_snapshot(
        test_font(), k_draw_pixel_height, codepoints, snapshot_options());

    bool ok = true;
    ok &= check_status(partial.result, mtr::Text_status::OK, "a partial build must stay usable");
    if (!check(partial.snapshot != nullptr, "a partial build must return a snapshot")) {
        return false;
    }

    const msdf::build_result_t& build = partial.snapshot->build_result();
    ok &= check(
        build.status == msdf::Build_status::PARTIAL_SUCCESS,
        "a partial build must be visible through the composed CPU build status");
    ok &= check(
        std::find(build.missing_codepoints.begin(), build.missing_codepoints.end(), U'中') !=
            build.missing_codepoints.end(),
        "a partial build must name the codepoint it could not emit");
    ok &= check(
        !partial.snapshot->atlas().glyphs.empty(),
        "a partial build must still carry renderable glyphs");
    return ok;
}

bool test_identity_is_content_addressed()
{
    const mtr::font_snapshot_result_t first  = build_sample_snapshot();
    const mtr::font_snapshot_result_t second = build_sample_snapshot();

    bool ok = true;
    if (!check(
            first.snapshot != nullptr && second.snapshot != nullptr,
            "both sample snapshots must build"))
    {
        return false;
    }

    ok &= check(
        first.snapshot->identity() == second.snapshot->identity(),
        "equal build inputs must produce one identity");
    ok &= check(
        first.snapshot->atlas().rgba == second.snapshot->atlas().rgba,
        "equal identity must mean an identical baked bitmap");
    ok &= check(
        first.snapshot->revision() != second.snapshot->revision(),
        "two snapshot instances must carry distinct revisions");
    ok &= check(
        second.snapshot->revision() > first.snapshot->revision(),
        "revisions must increase within a process");
    ok &= check(
        first.snapshot->draw_pixel_height() == k_draw_pixel_height,
        "a snapshot must report the draw pixel height it was built for");

    const mtr::font_snapshot_result_t other_height =
        build_sample_snapshot(k_draw_pixel_height * 2);
    ok &= check(
        other_height.snapshot != nullptr &&
            other_height.snapshot->identity() != first.snapshot->identity(),
        "a different draw pixel height must change the identity");

    msdf::options_t sharper   = snapshot_options();
    sharper.sharpness_bias    = snapshot_options().sharpness_bias + 1.0f;
    const mtr::font_snapshot_result_t other_options =
        build_sample_snapshot(k_draw_pixel_height, sharper);
    ok &= check(
        other_options.snapshot != nullptr &&
            other_options.snapshot->identity() != first.snapshot->identity(),
        "a different atlas option must change the identity");

    std::vector<char32_t> fewer = covered_codepoints();
    fewer.pop_back();
    const mtr::font_snapshot_result_t other_codepoints = mtr::build_font_snapshot(
        test_font(), k_draw_pixel_height, fewer, snapshot_options());
    ok &= check(
        other_codepoints.snapshot != nullptr &&
            other_codepoints.snapshot->identity() != first.snapshot->identity(),
        "a different codepoint set must change the identity");

    return ok;
}

// -----------------------------------------------------------------------------
// Text batches: agreement with the CPU producers, and validation
// -----------------------------------------------------------------------------

bool test_batch_matches_cpu_quads()
{
    const mtr::Font_snapshot& font = shared_snapshot();
    const std::string_view    text = "Varinomics 42";
    constexpr float           k_baseline_x = 12.5f;
    constexpr float           k_baseline_y = 30.0f;

    mtr::Text_batch batch;
    const mtr::text_result_t appended =
        batch.append_run(font, text, k_baseline_x, k_baseline_y);

    bool ok = check_status(appended, mtr::Text_status::OK, "appending a run must succeed");

    std::vector<msdf::text_vertex_t> expected_vertices;
    std::vector<std::uint32_t>       expected_indices;
    msdf::append_text_quads(
        font.atlas(),
        font.draw_pixel_height(),
        text,
        k_baseline_x,
        k_baseline_y,
        expected_vertices,
        &expected_indices);

    ok &= check(
        batch.vertices().size() == expected_vertices.size() &&
            batch.indices().size() == expected_indices.size(),
        "a batch run must emit the same quad count as append_text_quads");
    if (batch.vertices().size() == expected_vertices.size()) {
        ok &= check(
            std::memcmp(
                batch.vertices().data(),
                expected_vertices.data(),
                expected_vertices.size() * sizeof(msdf::text_vertex_t)) == 0,
            "a batch run must emit the same vertices as append_text_quads");
    }
    if (batch.indices().size() == expected_indices.size()) {
        ok &= check(
            std::equal(
                batch.indices().begin(), batch.indices().end(), expected_indices.begin()),
            "a batch run must emit the same indices as append_text_quads");
    }

    ok &= check(
        batch.font_identity().has_value() && *batch.font_identity() == font.identity(),
        "a batch must record the font its geometry was laid out against");
    return ok;
}

bool test_batch_agrees_with_measurement_and_bounds()
{
    const mtr::Font_snapshot& font = shared_snapshot();
    const std::string_view    text = "Varinomics";
    constexpr float           k_baseline_x = 4.0f;
    constexpr float           k_baseline_y = 20.0f;

    mtr::Text_batch          batch;
    const mtr::text_result_t appended =
        batch.append_run(font, text, k_baseline_x, k_baseline_y);
    bool ok = check_status(appended, mtr::Text_status::OK, "appending a run must succeed");

    const msdf::text_bounds_t bounds =
        msdf::measure_text_bounds_px(font.atlas(), font.draw_pixel_height(), text);
    const float advance =
        msdf::measure_text_advance_px(font.atlas(), font.draw_pixel_height(), text);

    ok &= check(bounds.has_visible_glyphs, "the sample text must measure visible glyphs");
    ok &= check(advance > 0.0f, "the sample text must measure a positive advance");
    ok &= check(!batch.empty(), "the sample text must emit geometry");

    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_y = std::numeric_limits<float>::max();
    float max_y = std::numeric_limits<float>::lowest();
    for (const msdf::text_vertex_t& vertex : batch.vertices()) {
        min_x = std::min(min_x, vertex.x);
        max_x = std::max(max_x, vertex.x);
        min_y = std::min(min_y, vertex.y);
        max_y = std::max(max_y, vertex.y);
    }

    // A consumer clips against the measured bounds, so the drawn geometry must
    // sit exactly inside the rectangle those bounds describe at the same origin.
    constexpr float k_tolerance = 1.0f / 512.0f;
    ok &= check(
        min_x >= k_baseline_x + bounds.left   - k_tolerance &&
        max_x <= k_baseline_x + bounds.right  + k_tolerance,
        "emitted quads must stay inside the measured horizontal bounds");
    ok &= check(
        min_y >= k_baseline_y + bounds.top    - k_tolerance &&
        max_y <= k_baseline_y + bounds.bottom + k_tolerance,
        "emitted quads must stay inside the measured vertical bounds");
    ok &= check(
        max_x <= k_baseline_x + advance + k_tolerance,
        "emitted quads must stay within the measured pen advance");

    float pen_x = k_baseline_x;
    int   positioned = 0;
    const float end_x = msdf::for_each_positioned_glyph(
        font.atlas(),
        font.draw_pixel_height(),
        text,
        k_baseline_x,
        [&](const msdf::positioned_glyph_t& glyph) {
            pen_x = glyph.pen_x;
            ++positioned;
        });
    ok &= check(
        positioned == static_cast<int>(text.size()),
        "every sample codepoint must be positioned by the CPU layout stream");
    ok &= check(
        pen_x <= end_x && std::abs(end_x - (k_baseline_x + advance)) <= k_tolerance,
        "the positioned-glyph stream must end at the measured advance");
    return ok;
}

bool test_batch_rejects_invalid_geometry()
{
    const mtr::Font_snapshot& font = shared_snapshot();

    const std::vector<msdf::text_vertex_t> vertices(4, msdf::text_vertex_t{});
    const std::vector<std::uint32_t>       good_indices = {0, 1, 2, 0, 2, 3};
    const std::vector<std::uint32_t>       partial_triangle = {0, 1};
    const std::vector<std::uint32_t>       out_of_range = {0, 1, 9};

    bool ok = true;

    mtr::Text_batch batch;
    ok &= check_status(
        batch.append_quads(font, vertices, partial_triangle),
        mtr::Text_status::INVALID_ARGUMENT,
        "indices that do not form whole triangles must be rejected");
    ok &= check_status(
        batch.append_quads(font, vertices, out_of_range),
        mtr::Text_status::INVALID_ARGUMENT,
        "an index beyond the supplied vertices must be rejected");
    ok &= check(batch.empty(), "a rejected append must not change the batch");

    ok &= check_status(
        batch.append_quads(font, vertices, good_indices),
        mtr::Text_status::OK,
        "well-formed caller quads must be accepted");
    ok &= check(batch.indices().size() == good_indices.size(), "accepted quads must be kept");

    // Appending the same quads again must rebase onto the batch rather than
    // aliasing the first copy's vertices.
    ok &= check_status(
        batch.append_quads(font, vertices, good_indices),
        mtr::Text_status::OK,
        "a second append must be accepted");
    ok &= check(
        batch.indices()[good_indices.size()] == static_cast<std::uint32_t>(vertices.size()),
        "a second append must rebase its indices onto the batch");

    const mtr::font_snapshot_result_t other =
        build_sample_snapshot(k_draw_pixel_height * 2);
    if (check(other.snapshot != nullptr, "the second sample snapshot must build")) {
        ok &= check_status(
            batch.append_quads(*other.snapshot, vertices, good_indices),
            mtr::Text_status::INVALID_ARGUMENT,
            "a batch must refuse geometry from a second font");
    }

    batch.clear();
    ok &= check(
        batch.empty() && !batch.font_identity().has_value(),
        "clearing a batch must drop its geometry and its font identity");
    return ok;
}

// -----------------------------------------------------------------------------
// Null QRhi: frame status, command recording, and resource lifecycle
// -----------------------------------------------------------------------------

class Offscreen_target
{
public:
    Offscreen_target(QRhi& rhi, const QSize& size, int sample_count)
    {
        m_texture.reset(rhi.newTexture(
            QRhiTexture::RGBA8, size, sample_count, QRhiTexture::RenderTarget));
        if (!m_texture || !m_texture->create()) {
            m_texture.reset();
            return;
        }

        m_target.reset(rhi.newTextureRenderTarget({ m_texture.get() }));
        if (!m_target) {
            return;
        }
        m_render_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_render_pass.get());
        if (!m_target->create()) {
            m_target.reset();
        }
    }

    [[nodiscard]] bool valid() const { return m_target != nullptr; }
    [[nodiscard]] QRhiTextureRenderTarget* target() const { return m_target.get(); }

private:
    std::unique_ptr<QRhiTexture>                m_texture;
    std::unique_ptr<QRhiTextureRenderTarget>    m_target;
    std::unique_ptr<QRhiRenderPassDescriptor>   m_render_pass;
};

std::unique_ptr<QRhi> make_null_rhi()
{
    QRhiNullInitParams params;
    return std::unique_ptr<QRhi>(QRhi::create(QRhi::Null, &params));
}

mtr::text_result_t run_null_frame(
    QRhi&                rhi,
    Offscreen_target&    offscreen,
    mtr::Text_renderer&  renderer,
    const mtr::Text_batch& batch,
    const mtr::draw_state_t& state,
    mtr::text_result_t&  out_prepare)
{
    QRhiCommandBuffer* cb = nullptr;
    if (rhi.beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
        mtr::text_result_t no_frame;
        no_frame.status = mtr::Text_status::INVALID_FRAME;
        out_prepare     = no_frame;
        return no_frame;
    }

    mtr::frame_t frame;
    frame.rhi              = &rhi;
    frame.command_buffer   = cb;
    frame.render_target    = offscreen.target();
    frame.resource_updates = rhi.nextResourceUpdateBatch();

    renderer.begin_frame();
    const mtr::text_result_t queued = renderer.queue(batch, state);
    if (queued.status != mtr::Text_status::OK) {
        renderer.reset_frame();
        rhi.endOffscreenFrame();
        out_prepare = queued;
        return queued;
    }

    out_prepare = renderer.prepare(frame);

    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    const QSize size = frame.render_target->pixelSize();
    cb->setViewport(QRhiViewport(0.0f, 0.0f, float(size.width()), float(size.height())));
    const mtr::text_result_t recorded = renderer.record(frame);
    cb->endPass();

    rhi.endOffscreenFrame();
    return recorded;
}

bool test_null_records_prepared_text()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    if (!check(rhi != nullptr, "the Null QRhi backend must be available")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    if (!check(offscreen.valid(), "the Null offscreen render target must be created")) {
        return false;
    }

    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(held.snapshot != nullptr, "the renderer's snapshot must build")) {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::Text_batch batch;
    bool ok = check_status(
        batch.append_run(*held.snapshot, "Varinomics 01", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the sample run must be appended");

    mtr::draw_state_t state;
    state.color = { 1.0f, 1.0f, 1.0f, 1.0f };

    mtr::text_result_t prepared{};
    const mtr::text_result_t recorded =
        run_null_frame(*rhi, offscreen, renderer, batch, state, prepared);

    ok &= check_status(prepared, mtr::Text_status::OK, "the first frame must prepare");
    ok &= check_status(recorded, mtr::Text_status::OK, "the first frame must record");

    const mtr::renderer_diagnostics_t after = renderer.diagnostics();
    ok &= check(after.recorded_draws == 1, "one queued draw state must record one draw");
    ok &= check(after.atlas_uploads == 1, "the first frame must upload the atlas once");
    ok &= check(after.pipeline_builds == 1, "the first frame must build the pipeline once");
    ok &= check(after.vertex_buffer_bytes > 0, "the first frame must allocate a vertex buffer");
    ok &= check(after.queued_draws == 0, "recording must clear the queued frame");

    renderer.release_resources();
    return ok;
}

bool test_null_reports_frame_and_font_failures()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    if (!check(rhi != nullptr, "the Null QRhi backend must be available")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    if (!check(offscreen.valid(), "the Null offscreen render target must be created")) {
        return false;
    }

    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(held.snapshot != nullptr, "the renderer's snapshot must build")) {
        return false;
    }

    mtr::Text_batch batch;
    bool ok = check_status(
        batch.append_run(*held.snapshot, "Varinomics", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the sample run must be appended");

    mtr::Text_renderer renderer;
    renderer.begin_frame();
    ok &= check_status(
        renderer.queue(batch, mtr::draw_state_t{}),
        mtr::Text_status::NO_FONT,
        "queueing text without a font must report no font");

    const mtr::font_snapshot_result_t other = build_sample_snapshot(k_draw_pixel_height * 2);
    if (!check(other.snapshot != nullptr, "the second snapshot must build")) {
        return false;
    }
    renderer.set_font(other.snapshot);
    ok &= check_status(
        renderer.queue(batch, mtr::draw_state_t{}),
        mtr::Text_status::INVALID_ARGUMENT,
        "queueing a batch laid out against another font must be rejected");

    renderer.set_font(held.snapshot);
    ok &= check_status(
        renderer.queue(mtr::Text_batch{}, mtr::draw_state_t{}),
        mtr::Text_status::OK,
        "an empty batch must be accepted and queue nothing");
    ok &= check(
        renderer.diagnostics().queued_draws == 0, "an empty batch must not queue a draw");

    mtr::frame_t partial_frame;
    partial_frame.rhi = rhi.get();
    ok &= check_status(
        renderer.prepare(partial_frame),
        mtr::Text_status::INVALID_FRAME,
        "preparing without a render target must report an invalid frame");

    QRhiCommandBuffer* cb = nullptr;
    if (!check(
            rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
            "the offscreen frame must begin"))
    {
        return false;
    }

    mtr::frame_t frame;
    frame.rhi              = rhi.get();
    frame.command_buffer   = cb;
    frame.render_target    = offscreen.target();
    frame.resource_updates = rhi->nextResourceUpdateBatch();

    renderer.begin_frame();
    ok &= check_status(
        renderer.queue(batch, mtr::draw_state_t{}), mtr::Text_status::OK, "the run must queue");

    // Recording without preparing must never look like a recorded label.
    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    ok &= check_status(
        renderer.record(frame),
        mtr::Text_status::NOT_PREPARED,
        "recording unprepared text must fail");
    ok &= check(
        renderer.diagnostics().recorded_draws == 0,
        "a failed recording must not report recorded draws");
    cb->endPass();
    rhi->endOffscreenFrame();

    renderer.release_resources();
    return ok;
}

bool test_null_queue_after_prepare_is_not_recorded()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    Offscreen_target      offscreen(*rhi, QSize(320, 64), 1);
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(
            rhi != nullptr && offscreen.valid() && held.snapshot != nullptr,
            "the Null fixture must be available"))
    {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::Text_batch batch;
    bool ok = check_status(
        batch.append_run(*held.snapshot, "Varinomics", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the sample run must be appended");

    QRhiCommandBuffer* cb = nullptr;
    if (!check(
            rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
            "the offscreen frame must begin"))
    {
        return false;
    }

    mtr::frame_t frame;
    frame.rhi              = rhi.get();
    frame.command_buffer   = cb;
    frame.render_target    = offscreen.target();
    frame.resource_updates = rhi->nextResourceUpdateBatch();

    renderer.begin_frame();
    ok &= check_status(renderer.queue(batch, mtr::draw_state_t{}), mtr::Text_status::OK, "queue");
    ok &= check_status(renderer.prepare(frame), mtr::Text_status::OK, "prepare");
    ok &= check_status(
        renderer.queue(batch, mtr::draw_state_t{}),
        mtr::Text_status::OK,
        "a second queue after prepare must be accepted");

    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    ok &= check_status(
        renderer.record(frame),
        mtr::Text_status::NOT_PREPARED,
        "text queued after preparation must not be reported as recorded");
    cb->endPass();
    rhi->endOffscreenFrame();

    renderer.release_resources();
    return ok;
}

bool test_null_resource_recreation_causes()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    Offscreen_target      first_target(*rhi, QSize(320, 64), 1);
    Offscreen_target      second_target(*rhi, QSize(640, 128), 1);
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(
            rhi != nullptr && first_target.valid() && second_target.valid() &&
                held.snapshot != nullptr,
            "the Null fixture must be available"))
    {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::Text_batch short_batch;
    mtr::Text_batch long_batch;
    bool ok = check_status(
        short_batch.append_run(*held.snapshot, "V", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the short run must be appended");
    for (int line = 0; line < 64; ++line) {
        ok &= check_status(
            long_batch.append_run(
                *held.snapshot, "Varinomics 0123456789", 8.0f, 40.0f + float(line)),
            mtr::Text_status::OK,
            "the long run must be appended");
    }

    mtr::text_result_t prepared{};
    ok &= check_status(
        run_null_frame(*rhi, first_target, renderer, short_batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the first frame must record");
    ok &= check_status(prepared, mtr::Text_status::OK, "the first frame must prepare");

    const mtr::renderer_diagnostics_t first = renderer.diagnostics();

    ok &= check_status(
        run_null_frame(*rhi, first_target, renderer, short_batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the second frame must record");
    const mtr::renderer_diagnostics_t unchanged = renderer.diagnostics();
    ok &= check(
        unchanged.atlas_uploads == first.atlas_uploads &&
            unchanged.pipeline_builds == first.pipeline_builds &&
            unchanged.resource_generation == first.resource_generation,
        "an unchanged frame must not rebuild or re-upload anything");

    // A larger frame grows the geometry buffers.
    ok &= check_status(
        run_null_frame(*rhi, first_target, renderer, long_batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the growing frame must record");
    const mtr::renderer_diagnostics_t grown = renderer.diagnostics();
    ok &= check(
        grown.vertex_buffer_bytes > unchanged.vertex_buffer_bytes &&
            grown.index_buffer_bytes > unchanged.index_buffer_bytes,
        "a larger frame must grow the vertex and index buffers");

    // A different render target has a different render-pass descriptor.
    ok &= check_status(
        run_null_frame(*rhi, second_target, renderer, short_batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the retargeted frame must record");
    ok &= check(
        renderer.diagnostics().pipeline_builds > grown.pipeline_builds,
        "a different render pass must rebuild the pipeline");

    // A rebuilt snapshot has a new revision even at an identical identity.
    const mtr::font_snapshot_result_t rebuilt = build_sample_snapshot();
    if (!check(rebuilt.snapshot != nullptr, "the rebuilt snapshot must build")) {
        return false;
    }
    ok &= check(
        rebuilt.snapshot->identity() == held.snapshot->identity(),
        "the rebuilt snapshot must keep the identity");
    const mtr::renderer_diagnostics_t before_rebuild = renderer.diagnostics();
    renderer.set_font(rebuilt.snapshot);
    ok &= check_status(
        run_null_frame(*rhi, second_target, renderer, short_batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the frame after a font rebuild must record");
    ok &= check(
        renderer.diagnostics().atlas_uploads == before_rebuild.atlas_uploads + 1,
        "a new snapshot revision must re-upload the atlas");

    // An explicit release drops the device-local set and rebuilds it.
    const mtr::renderer_diagnostics_t before_release = renderer.diagnostics();
    renderer.release_resources();
    const mtr::renderer_diagnostics_t released = renderer.diagnostics();
    ok &= check(
        released.resource_generation == before_release.resource_generation + 1 &&
            released.vertex_buffer_bytes == 0,
        "releasing resources must drop the device-local set");
    ok &= check_status(
        run_null_frame(*rhi, second_target, renderer, short_batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the frame after a release must record");
    ok &= check(
        renderer.diagnostics().atlas_uploads == released.atlas_uploads + 1 &&
            renderer.diagnostics().pipeline_builds == released.pipeline_builds + 1,
        "a released renderer must upload and rebuild before recording again");

    renderer.release_resources();
    return ok;
}

bool test_null_resources_are_device_local()
{
    std::unique_ptr<QRhi> first_rhi  = make_null_rhi();
    std::unique_ptr<QRhi> second_rhi = make_null_rhi();
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(
            first_rhi != nullptr && second_rhi != nullptr && held.snapshot != nullptr,
            "two Null devices must be available"))
    {
        return false;
    }

    Offscreen_target first_target(*first_rhi, QSize(320, 64), 1);
    Offscreen_target second_target(*second_rhi, QSize(320, 64), 1);
    if (!check(
            first_target.valid() && second_target.valid(),
            "both offscreen render targets must be created"))
    {
        return false;
    }

    mtr::Text_batch batch;
    bool ok = check_status(
        batch.append_run(*held.snapshot, "Varinomics", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the sample run must be appended");

    // Two renderers sharing one snapshot keep separate device-local resources.
    mtr::Text_renderer first_renderer;
    mtr::Text_renderer second_renderer;
    first_renderer.set_font(held.snapshot);
    second_renderer.set_font(held.snapshot);

    mtr::text_result_t prepared{};
    ok &= check_status(
        run_null_frame(*first_rhi, first_target, first_renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the first device's frame must record");
    const mtr::renderer_diagnostics_t first_only = first_renderer.diagnostics();
    ok &= check(
        second_renderer.diagnostics().atlas_uploads == 0 &&
            second_renderer.diagnostics().pipeline_builds == 0,
        "one renderer's frame must not create the other renderer's resources");

    ok &= check_status(
        run_null_frame(
            *second_rhi, second_target, second_renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the second device's frame must record");
    ok &= check(
        second_renderer.diagnostics().atlas_uploads == 1 &&
            second_renderer.diagnostics().pipeline_builds == 1,
        "the second renderer must build its own device-local resources");
    ok &= check(
        first_renderer.diagnostics().atlas_uploads == first_only.atlas_uploads &&
            first_renderer.diagnostics().pipeline_builds == first_only.pipeline_builds,
        "the second device's frame must not disturb the first renderer");

    // Moving one renderer to another device rebuilds its whole resource set.
    first_renderer.release_resources();
    second_renderer.release_resources();
    return ok;
}

bool test_null_device_change_rebuilds_resources()
{
    std::unique_ptr<QRhi> first_rhi  = make_null_rhi();
    std::unique_ptr<QRhi> second_rhi = make_null_rhi();
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(
            first_rhi != nullptr && second_rhi != nullptr && held.snapshot != nullptr,
            "two Null devices must be available"))
    {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::Text_batch batch;
    bool ok = check_status(
        batch.append_run(*held.snapshot, "Varinomics", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the sample run must be appended");

    Offscreen_target first_target(*first_rhi, QSize(320, 64), 1);
    Offscreen_target second_target(*second_rhi, QSize(320, 64), 1);
    if (!check(
            first_target.valid() && second_target.valid(),
            "both offscreen render targets must be created"))
    {
        return false;
    }

    mtr::text_result_t prepared{};
    ok &= check_status(
        run_null_frame(*first_rhi, first_target, renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the first device's frame must record");
    const mtr::renderer_diagnostics_t after_first = renderer.diagnostics();

    // Both devices are alive here, so the renderer destroys the first device's
    // objects on that device, which is the sequence QRhi requires.
    ok &= check_status(
        run_null_frame(*second_rhi, second_target, renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the second device's frame must record");
    const mtr::renderer_diagnostics_t after_second = renderer.diagnostics();
    ok &= check(
        after_second.resource_generation == after_first.resource_generation + 1,
        "a new device must drop the previous device-local resource set");
    ok &= check(
        after_second.atlas_uploads == after_first.atlas_uploads + 1 &&
            after_second.pipeline_builds == after_first.pipeline_builds + 1,
        "a new device must rebuild and re-upload every device-local resource");

    renderer.release_resources();
    return ok;
}

bool test_null_multiple_draw_states_record_separately()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    Offscreen_target      offscreen(*rhi, QSize(320, 64), 1);
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(
            rhi != nullptr && offscreen.valid() && held.snapshot != nullptr,
            "the Null fixture must be available"))
    {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::Text_batch batch;
    bool ok = check_status(
        batch.append_run(*held.snapshot, "Varinomics", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the sample run must be appended");

    QRhiCommandBuffer* cb = nullptr;
    if (!check(
            rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
            "the offscreen frame must begin"))
    {
        return false;
    }

    mtr::frame_t frame;
    frame.rhi              = rhi.get();
    frame.command_buffer   = cb;
    frame.render_target    = offscreen.target();
    frame.resource_updates = rhi->nextResourceUpdateBatch();

    mtr::draw_state_t plain;
    plain.transform = mtr::pixel_ortho_transform(frame);

    mtr::draw_state_t clipped = plain;
    clipped.color = { 0.2f, 0.4f, 1.0f, 1.0f };
    clipped.clip  = { true, 0, 0, 64, 32 };

    renderer.begin_frame();
    ok &= check_status(renderer.queue(batch, plain), mtr::Text_status::OK, "the first draw queues");
    ok &= check_status(
        renderer.queue(batch, clipped), mtr::Text_status::OK, "the clipped draw queues");
    ok &= check(renderer.diagnostics().queued_draws == 2, "two draw states must queue two draws");
    ok &= check_status(renderer.prepare(frame), mtr::Text_status::OK, "the frame must prepare");

    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    cb->setViewport(QRhiViewport(0.0f, 0.0f, 320.0f, 64.0f));
    ok &= check_status(renderer.record(frame), mtr::Text_status::OK, "the frame must record");
    cb->endPass();
    rhi->endOffscreenFrame();

    ok &= check(
        renderer.diagnostics().recorded_draws == 2,
        "each queued draw state must record its own draw");
    ok &= check(
        renderer.diagnostics().pipeline_builds == 1,
        "several draw states must share one pipeline");

    renderer.release_resources();
    return ok;
}

bool run_test(const char* name, bool (*test)())
{
    try {
        if (test()) {
            std::cerr << "PASS: " << name << '\n';
            return true;
        }
        std::cerr << "FAIL: " << name << '\n';
        return false;
    }
    catch (const std::exception& e) {
        std::cerr << "FAIL: " << name << ": " << e.what() << '\n';
    }
    catch (...) {
        std::cerr << "FAIL: " << name << ": unknown exception\n";
    }

    return false;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    bool ok = true;
    ok &= run_test("snapshot digest is standard sha256", test_sha256_known_answer);
    ok &= run_test("snapshot rejects invalid arguments", test_snapshot_rejects_invalid_arguments);
    ok &= run_test("snapshot reports build failure", test_snapshot_reports_build_failure);
    ok &= run_test("snapshot reports partial build", test_snapshot_reports_partial_build);
    ok &= run_test("identity is content addressed", test_identity_is_content_addressed);
    ok &= run_test("batch matches cpu quads", test_batch_matches_cpu_quads);
    ok &= run_test("batch agrees with measurement and bounds", test_batch_agrees_with_measurement_and_bounds);
    ok &= run_test("batch rejects invalid geometry", test_batch_rejects_invalid_geometry);
    ok &= run_test("null records prepared text", test_null_records_prepared_text);
    ok &= run_test("null reports frame and font failures", test_null_reports_frame_and_font_failures);
    ok &= run_test("null queue after prepare is not recorded", test_null_queue_after_prepare_is_not_recorded);
    ok &= run_test("null resource recreation causes", test_null_resource_recreation_causes);
    ok &= run_test("null resources are device local", test_null_resources_are_device_local);
    ok &= run_test("null device change rebuilds resources", test_null_device_change_rebuilds_resources);
    ok &= run_test("null multiple draw states record separately", test_null_multiple_draw_states_record_separately);
    return ok ? 0 : 1;
}
