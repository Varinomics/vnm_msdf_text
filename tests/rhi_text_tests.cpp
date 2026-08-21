#include <vnm_msdf_text/lcd_contract.h>
#include <vnm_msdf_text/lcd_shader_reference.h>
#include <vnm_msdf_text/rhi/draw_capabilities.h>
#include <vnm_msdf_text/rhi/font_snapshot.h>
#include <vnm_msdf_text/rhi/status.h>
#include <vnm_msdf_text/rhi/text_batch.h>
#include <vnm_msdf_text/rhi/text_renderer.h>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <rhi/qshader.h>

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace msdf = vnm::msdf_text;
namespace mtr  = vnm::msdf_text::rhi;

// -----------------------------------------------------------------------------
// Allocation fault seam
//
// The containers a queued frame and a batch grow are allocated by this
// executable, so replacing the global allocator is enough to make one chosen
// allocation inside one chosen call fail. The counter is armed only inside
// Failing_allocation's scope, which wraps a single component call that performs
// no Qt work of its own, so nothing else in the process is affected.
// -----------------------------------------------------------------------------

namespace {
int g_allocations_before_failure = -1;
}

void* operator new(std::size_t size)
{
    if (g_allocations_before_failure >= 0) {
        if (g_allocations_before_failure == 0) {
            g_allocations_before_failure = -1;
            throw std::bad_alloc();
        }
        --g_allocations_before_failure;
    }

    void* memory = std::malloc(size > 0 ? size : 1);
    if (!memory) {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new[](std::size_t size)                { return ::operator new(size); }
void  operator delete(void* memory) noexcept          { std::free(memory); }
void  operator delete[](void* memory) noexcept        { std::free(memory); }
void  operator delete(void* memory, std::size_t) noexcept   { std::free(memory); }
void  operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

/// Arms the allocation after @p survivors further allocations to fail.
class Failing_allocation
{
public:
    explicit Failing_allocation(int survivors) { g_allocations_before_failure = survivors; }
    ~Failing_allocation() { g_allocations_before_failure = -1; }

    Failing_allocation(const Failing_allocation&)            = delete;
    Failing_allocation& operator=(const Failing_allocation&) = delete;
};

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

void expected_u32(QCryptographicHash& hash, std::uint32_t value)
{
    const char bytes[4] = {
        static_cast<char>((value >> 24) & 0xFFu),
        static_cast<char>((value >> 16) & 0xFFu),
        static_cast<char>((value >>  8) & 0xFFu),
        static_cast<char>( value        & 0xFFu),
    };
    hash.addData(QByteArrayView(bytes, sizeof(bytes)));
}

void expected_f32(QCryptographicHash& hash, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    expected_u32(hash, bits);
}

void expected_f64(QCryptographicHash& hash, double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    expected_u32(hash, static_cast<std::uint32_t>(bits >> 32));
    expected_u32(hash, static_cast<std::uint32_t>(bits & 0xFFFFFFFFu));
}

// The identity is a SHA-256 over a fixed serialization of the build inputs.
// Rebuilding that byte stream here from the documented order pins the contract:
// a field dropped, reordered, or written at another width changes the digest,
// and so does swapping the digest for something that is not SHA-256.
bool test_identity_is_the_serialized_build_inputs()
{
    const msdf::options_t       options    = snapshot_options();
    const std::vector<char32_t> codepoints = covered_codepoints();

    const mtr::font_snapshot_result_t built = build_sample_snapshot();
    if (!check(built.snapshot != nullptr, "the sample snapshot must build")) {
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(
        reinterpret_cast<const char*>(test_font().data()),
        static_cast<qsizetype>(test_font().size())));
    expected_u32(hash, static_cast<std::uint32_t>(test_font().size()));
    expected_u32(hash, static_cast<std::uint32_t>(k_draw_pixel_height));

    expected_u32(hash, static_cast<std::uint32_t>(options.atlas_size));
    expected_f64(hash, options.min_atlas_font_size);
    expected_f32(hash, options.atlas_px_range);
    expected_f32(hash, options.sharpness_bias);
    expected_u32(hash, static_cast<std::uint32_t>(options.atlas_gutter_px));
    expected_u32(hash, options.build_kerning_table ? 1u : 0u);
    expected_u32(hash, static_cast<std::uint32_t>(options.missing_glyph_policy));

    expected_u32(hash, static_cast<std::uint32_t>(codepoints.size()));
    for (char32_t codepoint : codepoints) {
        expected_u32(hash, static_cast<std::uint32_t>(codepoint));
    }

    const QByteArray expected = hash.result();
    const std::string expected_hex = hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(expected.constData()),
        static_cast<std::size_t>(expected.size())));

    return check(
        hex(built.snapshot->identity().digest) == expected_hex,
        "the identity must be SHA-256 over the documented build-input serialization");
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

bool test_batch_survives_allocation_failure()
{
    const mtr::Font_snapshot& font = shared_snapshot();

    const std::vector<msdf::text_vertex_t> vertices(4, msdf::text_vertex_t{});
    const std::vector<std::uint32_t>       indices = {0, 1, 2, 0, 2, 3};

    bool ok       = true;
    int  failures = 0;

    // Which allocation a growing vector makes is an implementation detail, so
    // every early allocation of the call is armed in turn and each outcome has
    // to be either a clean success or a failure that changed nothing.
    for (int survivors = 0; survivors < 4; ++survivors) {
        mtr::Text_batch    run_batch;
        mtr::text_result_t appended{};
        {
            Failing_allocation fail(survivors);
            appended = run_batch.append_run(font, "Varinomics 0123456789", 4.0f, 20.0f);
        }
        if (appended.status == mtr::Text_status::OK) {
            continue;
        }

        ++failures;
        ok &= check_status(
            appended,
            mtr::Text_status::OUT_OF_MEMORY,
            "a run that could not be allocated must report out of memory");
        ok &= check(
            run_batch.empty() && !run_batch.font_identity().has_value(),
            "a failed run must leave the batch empty and without a font identity");

        // The same batch must still be usable afterwards.
        ok &= check_status(
            run_batch.append_run(font, "Varinomics", 4.0f, 20.0f),
            mtr::Text_status::OK,
            "a batch must still accept a run after an allocation failure");
        ok &= check(!run_batch.empty(), "the recovered run must be kept");

        mtr::Text_batch    quad_batch;
        mtr::text_result_t quads{};
        {
            Failing_allocation fail(survivors);
            quads = quad_batch.append_quads(font, vertices, indices);
        }
        if (quads.status != mtr::Text_status::OK) {
            ok &= check_status(
                quads,
                mtr::Text_status::OUT_OF_MEMORY,
                "quads that could not be allocated must report out of memory");
            ok &= check(
                quad_batch.empty() && quad_batch.vertices().empty() &&
                    !quad_batch.font_identity().has_value(),
                "a failed quad append must leave neither vertices nor an identity behind");
        }
    }

    ok &= check(failures > 0, "the allocation seam must actually fail an append");
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

    mtr::draw_state_t frame_state = state;
    if (frame_state.lcd || frame_state.glow || frame_state.sdf_mask) {
        frame_state.transform = mtr::pixel_ortho_transform(frame);
    }

    renderer.begin_frame();
    const mtr::text_result_t queued = renderer.queue(batch, frame_state);
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
    ok &= check(after.atlas_upload_enqueues == 1, "the first frame must upload the atlas once");
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
        unchanged.atlas_upload_enqueues == first.atlas_upload_enqueues &&
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

    // A rebuilt snapshot is a different object with its own storage, so it is
    // uploaded again even though it describes the same font.
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
        renderer.diagnostics().atlas_upload_enqueues == before_rebuild.atlas_upload_enqueues + 1,
        "a rebuilt snapshot must be uploaded again");

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
        renderer.diagnostics().atlas_upload_enqueues == released.atlas_upload_enqueues + 1 &&
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
        second_renderer.diagnostics().atlas_upload_enqueues == 0 &&
            second_renderer.diagnostics().pipeline_builds == 0,
        "one renderer's frame must not create the other renderer's resources");

    ok &= check_status(
        run_null_frame(
            *second_rhi, second_target, second_renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the second device's frame must record");
    ok &= check(
        second_renderer.diagnostics().atlas_upload_enqueues == 1 &&
            second_renderer.diagnostics().pipeline_builds == 1,
        "the second renderer must build its own device-local resources");
    ok &= check(
        first_renderer.diagnostics().atlas_upload_enqueues == first_only.atlas_upload_enqueues &&
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
        after_second.atlas_upload_enqueues == after_first.atlas_upload_enqueues + 1 &&
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

// A frame's font is fixed by its first batch with geometry. Replacing the
// renderer's font afterwards must leave that frame drawing what it queued, and
// the atlas the next frame reuses is the evidence: if the replacement had been
// uploaded instead, coming back to the first snapshot would have to upload again.
bool test_null_font_replacement_does_not_move_a_queued_frame()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    Offscreen_target      offscreen(*rhi, QSize(320, 64), 1);

    const mtr::font_snapshot_result_t first  = build_sample_snapshot();
    const mtr::font_snapshot_result_t second = build_sample_snapshot(k_draw_pixel_height * 2);
    if (!check(
            rhi != nullptr && offscreen.valid() &&
                first.snapshot != nullptr && second.snapshot != nullptr,
            "the Null fixture and both snapshots must be available"))
    {
        return false;
    }

    mtr::Text_batch first_batch;
    mtr::Text_batch second_batch;
    bool ok = check_status(
        first_batch.append_run(*first.snapshot, "Varinomics", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the first snapshot's run must be appended");
    ok &= check_status(
        second_batch.append_run(*second.snapshot, "Varinomics", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the second snapshot's run must be appended");

    mtr::Text_renderer renderer;
    renderer.set_font(first.snapshot);

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
        renderer.queue(first_batch, mtr::draw_state_t{}),
        mtr::Text_status::OK,
        "the first snapshot's batch must queue");

    // The replacement arrives after the frame already holds text.
    renderer.set_font(second.snapshot);
    ok &= check(
        renderer.font() == second.snapshot,
        "set_font must report the snapshot the next frame will draw");
    ok &= check_status(
        renderer.queue(second_batch, mtr::draw_state_t{}),
        mtr::Text_status::INVALID_ARGUMENT,
        "a batch from the replacement font must not join a frame queued against another");
    ok &= check(
        renderer.diagnostics().queued_draws == 1,
        "the rejected batch must not be queued");

    ok &= check_status(renderer.prepare(frame), mtr::Text_status::OK, "the frame must prepare");
    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    ok &= check_status(renderer.record(frame), mtr::Text_status::OK, "the frame must record");
    cb->endPass();
    rhi->endOffscreenFrame();

    const mtr::renderer_diagnostics_t replaced = renderer.diagnostics();
    ok &= check(replaced.recorded_draws == 1, "the frame must record the batch it queued");
    ok &= check(
        replaced.atlas_upload_enqueues == 1,
        "a frame queued against one snapshot must offer exactly that snapshot's atlas");

    // Going back to the first snapshot needs no upload, which is only true if
    // the frame above uploaded that snapshot rather than the replacement.
    renderer.set_font(first.snapshot);
    mtr::text_result_t prepared{};
    ok &= check_status(
        run_null_frame(*rhi, offscreen, renderer, first_batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the frame back on the first snapshot must record");
    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == replaced.atlas_upload_enqueues,
        "the atlas the replaced frame uploaded must be the one it was queued against");

    // The replacement itself takes effect for the next frame that queues it.
    renderer.set_font(second.snapshot);
    ok &= check_status(
        run_null_frame(*rhi, offscreen, renderer, second_batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the replacement snapshot must draw in a later frame");
    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == replaced.atlas_upload_enqueues + 1,
        "the replacement snapshot's atlas must be uploaded when it is first drawn");

    renderer.release_resources();
    return ok;
}

// An enqueued upload is not an executed one. A host that releases the batch and
// drops the frame has run nothing, so the renderer must still owe that upload.
bool test_null_cancelled_update_batch_is_uploaded_again()
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
            "the cancelled frame must begin"))
    {
        return false;
    }

    mtr::frame_t cancelled;
    cancelled.rhi              = rhi.get();
    cancelled.command_buffer   = cb;
    cancelled.render_target    = offscreen.target();
    cancelled.resource_updates = rhi->nextResourceUpdateBatch();

    renderer.begin_frame();
    ok &= check_status(
        renderer.queue(batch, mtr::draw_state_t{}), mtr::Text_status::OK, "the run must queue");
    ok &= check_status(renderer.prepare(cancelled), mtr::Text_status::OK, "the frame must prepare");
    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == 1,
        "preparation must put the atlas upload into the frame's batch");

    // The host returns the batch to the pool unsubmitted and abandons the frame.
    cancelled.resource_updates->release();
    renderer.reset_frame();
    rhi->endOffscreenFrame();

    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == 1,
        "abandoning a frame must not count as a second offer");

    mtr::text_result_t prepared{};
    ok &= check_status(
        run_null_frame(*rhi, offscreen, renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the frame after the cancelled one must record");
    ok &= check_status(prepared, mtr::Text_status::OK, "the frame after the cancelled one prepares");
    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == 2,
        "an upload whose batch was cancelled must be enqueued again");

    // Once a frame carrying the upload reached recording, it is settled.
    ok &= check_status(
        run_null_frame(*rhi, offscreen, renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the following frame must record");
    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == 2,
        "a submitted upload must not be offered again");

    renderer.release_resources();
    return ok;
}

// Reaching record() settles the upload the frame's own prepare() enqueued, not
// whatever is still outstanding. A frame that queues text and records without a
// successful prepare() opened its pass with a batch that carried no atlas, so an
// upload an earlier frame abandoned is still owed; committing it here would let
// the renderer treat an atlas texture nothing was ever uploaded into as current.
bool test_null_unprepared_record_does_not_settle_an_abandoned_upload()
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
            "the abandoned frame must begin"))
    {
        return false;
    }

    mtr::frame_t abandoned;
    abandoned.rhi              = rhi.get();
    abandoned.command_buffer   = cb;
    abandoned.render_target    = offscreen.target();
    abandoned.resource_updates = rhi->nextResourceUpdateBatch();

    renderer.begin_frame();
    ok &= check_status(
        renderer.queue(batch, mtr::draw_state_t{}), mtr::Text_status::OK, "the run must queue");
    ok &= check_status(
        renderer.prepare(abandoned), mtr::Text_status::OK, "the abandoned frame must prepare");
    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == 1,
        "preparation must put the atlas upload into the abandoned frame's batch");

    // The host returns that batch to the pool unsubmitted and drops the frame,
    // so the upload it holds executed nothing.
    abandoned.resource_updates->release();
    renderer.reset_frame();
    rhi->endOffscreenFrame();

    // The next frame queues text and reaches recording without preparing, so
    // the batch it opened its pass with carried no atlas upload at all.
    cb = nullptr;
    if (!check(
            rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
            "the unprepared frame must begin"))
    {
        return false;
    }

    mtr::frame_t unprepared;
    unprepared.rhi              = rhi.get();
    unprepared.command_buffer   = cb;
    unprepared.render_target    = offscreen.target();
    unprepared.resource_updates = rhi->nextResourceUpdateBatch();

    renderer.begin_frame();
    ok &= check_status(
        renderer.queue(batch, mtr::draw_state_t{}),
        mtr::Text_status::OK,
        "the unprepared frame's run must queue");

    cb->beginPass(
        unprepared.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, unprepared.resource_updates);
    ok &= check_status(
        renderer.record(unprepared),
        mtr::Text_status::NOT_PREPARED,
        "a frame whose text was never prepared must not record");
    cb->endPass();
    rhi->endOffscreenFrame();

    ok &= check(
        renderer.diagnostics().recorded_draws == 0,
        "an unprepared frame must issue no draws");
    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == 1,
        "a frame that did not prepare must not offer the atlas itself");

    // The abandoned upload is still owed, so the next preparation must enqueue
    // it again instead of finding it committed by the frame above.
    mtr::text_result_t prepared{};
    ok &= check_status(
        run_null_frame(*rhi, offscreen, renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the frame after the unprepared one must record");
    ok &= check_status(
        prepared, mtr::Text_status::OK, "the frame after the unprepared one prepares");
    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == 2,
        "an upload no recorded frame carried must be enqueued again");

    // That frame did carry its own upload into the pass it recorded in, so the
    // upload is settled and the frame after it draws the atlas as it stands.
    ok &= check_status(
        run_null_frame(*rhi, offscreen, renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the following frame must record");
    ok &= check(
        renderer.diagnostics().atlas_upload_enqueues == 2,
        "a settled upload must not be offered again");

    renderer.release_resources();
    return ok;
}

bool test_null_rejects_unusable_clip_rectangles()
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

    mtr::draw_state_t zero;
    zero.clip = { true, 0, 0, 0, 0 };

    mtr::draw_state_t partly_outside;
    partly_outside.clip = { true, -16, -8, 64, 32 };

    mtr::draw_state_t negative_width;
    negative_width.clip = { true, 0, 0, -4, 32 };

    mtr::draw_state_t negative_height;
    negative_height.clip = { true, 0, 0, 64, -4 };

    mtr::draw_state_t disabled_negative;
    disabled_negative.clip = { false, 0, 0, -4, -4 };

    renderer.begin_frame();
    ok &= check_status(
        renderer.queue(batch, zero),
        mtr::Text_status::OK,
        "a zero-sized clip is a rectangle QRhi can express and must be accepted");
    ok &= check_status(
        renderer.queue(batch, partly_outside),
        mtr::Text_status::OK,
        "a partly out-of-bounds clip must be accepted and left to QRhi to clamp");
    ok &= check_status(
        renderer.queue(batch, negative_width),
        mtr::Text_status::INVALID_ARGUMENT,
        "a negative clip width must be rejected");
    ok &= check_status(
        renderer.queue(batch, negative_height),
        mtr::Text_status::INVALID_ARGUMENT,
        "a negative clip height must be rejected");
    ok &= check_status(
        renderer.queue(batch, disabled_negative),
        mtr::Text_status::OK,
        "a disabled clip is not a scissor and its size does not matter");
    ok &= check(
        renderer.diagnostics().queued_draws == 3,
        "only the acceptable clips must be queued");

    ok &= check_status(renderer.prepare(frame), mtr::Text_status::OK, "the frame must prepare");
    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    cb->setViewport(QRhiViewport(0.0f, 0.0f, 320.0f, 64.0f));
    ok &= check_status(renderer.record(frame), mtr::Text_status::OK, "the frame must record");
    cb->endPass();
    rhi->endOffscreenFrame();

    ok &= check(
        renderer.diagnostics().recorded_draws == 3,
        "every accepted clip must record its draw");

    renderer.release_resources();
    return ok;
}

// Only the uniform buffer and the atlas texture are named by the binding set,
// so growing the vertex and index buffers must cost neither a binding rebuild
// nor a pipeline build.
bool test_null_geometry_growth_keeps_the_pipeline()
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

    mtr::Text_batch    batch;
    mtr::text_result_t prepared{};
    bool ok = check_status(
        batch.append_run(*held.snapshot, "V", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the first run must be appended");
    ok &= check_status(
        run_null_frame(*rhi, offscreen, renderer, batch, mtr::draw_state_t{}, prepared),
        mtr::Text_status::OK,
        "the first frame must record");

    const mtr::renderer_diagnostics_t first = renderer.diagnostics();
    ok &= check(first.pipeline_builds == 1, "the first frame must build one pipeline");

    // Four rounds of growth, each large enough to outgrow the doubling buffer.
    std::size_t previous_vertex_bytes = first.vertex_buffer_bytes;
    int         growths               = 0;
    for (int round = 0; round < 4; ++round) {
        for (int line = 0; line < 48; ++line) {
            ok &= check_status(
                batch.append_run(
                    *held.snapshot, "Varinomics 0123456789", 8.0f, 40.0f + float(line)),
                mtr::Text_status::OK,
                "the growing run must be appended");
        }
        ok &= check_status(
            run_null_frame(*rhi, offscreen, renderer, batch, mtr::draw_state_t{}, prepared),
            mtr::Text_status::OK,
            "the growing frame must record");

        const mtr::renderer_diagnostics_t grown = renderer.diagnostics();
        if (grown.vertex_buffer_bytes > previous_vertex_bytes) {
            ++growths;
            previous_vertex_bytes = grown.vertex_buffer_bytes;
        }
        ok &= check(
            grown.pipeline_builds == first.pipeline_builds,
            "growing the vertex and index buffers must not rebuild the pipeline");
        ok &= check(
            grown.resource_generation == first.resource_generation &&
                grown.atlas_upload_enqueues == first.atlas_upload_enqueues,
            "growing geometry must not drop resources or re-offer the atlas");
    }

    ok &= check(growths > 0, "the growing frames must actually have grown a buffer");
    ok &= check(
        renderer.diagnostics().index_buffer_bytes > first.index_buffer_bytes,
        "the index buffer must have grown as well");

    renderer.release_resources();
    return ok;
}

// A queued frame is walked as one array of uniform blocks and draw ops, so a
// failed allocation must leave both of them exactly as they were.
bool test_null_queue_survives_allocation_failure()
{
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(held.snapshot != nullptr, "the renderer's snapshot must build")) {
        return false;
    }

    mtr::Text_batch small;
    mtr::Text_batch large;
    bool ok = check_status(
        small.append_run(*held.snapshot, "V", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the small run must be appended");
    for (int line = 0; line < 64; ++line) {
        ok &= check_status(
            large.append_run(*held.snapshot, "Varinomics 0123456789", 8.0f, 40.0f + float(line)),
            mtr::Text_status::OK,
            "the large run must be appended");
    }

    int failures = 0;
    for (int survivors = 0; survivors < 4; ++survivors) {
        std::unique_ptr<QRhi> rhi = make_null_rhi();
        Offscreen_target      offscreen(*rhi, QSize(320, 64), 1);
        if (!check(
                rhi != nullptr && offscreen.valid(), "the Null fixture must be available"))
        {
            return false;
        }

        mtr::Text_renderer renderer;
        renderer.set_font(held.snapshot);

        renderer.begin_frame();
        ok &= check_status(
            renderer.queue(small, mtr::draw_state_t{}),
            mtr::Text_status::OK,
            "the first draw must queue");
        const std::size_t queued_indices = renderer.diagnostics().queued_indices;

        mtr::text_result_t queued{};
        {
            Failing_allocation fail(survivors);
            queued = renderer.queue(large, mtr::draw_state_t{});
        }
        if (queued.status == mtr::Text_status::OK) {
            continue;
        }

        ++failures;
        ok &= check_status(
            queued,
            mtr::Text_status::OUT_OF_MEMORY,
            "a queue that could not be allocated must report out of memory");
        ok &= check(
            renderer.diagnostics().queued_draws == 1 &&
                renderer.diagnostics().queued_indices == queued_indices,
            "a failed queue must leave the frame exactly as it was");

        // The frame must still be consistent enough to prepare and record, which
        // is what a leftover uniform block without its draw op would break.
        ok &= check_status(
            renderer.queue(small, mtr::draw_state_t{}),
            mtr::Text_status::OK,
            "the frame must still accept work after a failed queue");
        ok &= check(
            renderer.diagnostics().queued_draws == 2, "the retried draw must be queued");

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

        ok &= check_status(renderer.prepare(frame), mtr::Text_status::OK, "the frame must prepare");
        cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
        cb->setViewport(QRhiViewport(0.0f, 0.0f, 320.0f, 64.0f));
        ok &= check_status(renderer.record(frame), mtr::Text_status::OK, "the frame must record");
        cb->endPass();
        rhi->endOffscreenFrame();

        ok &= check(
            renderer.diagnostics().recorded_draws == 2,
            "the frame must record exactly the draws it kept");

        renderer.release_resources();
    }

    ok &= check(failures > 0, "the allocation seam must actually fail a queue");
    return ok;
}

// -----------------------------------------------------------------------------
// Optional draw capabilities
// -----------------------------------------------------------------------------

constexpr float k_opaque_cutoff = msdf::lcd::shader_reference::k_lcd_opaque_alpha_cutoff;

mtr::lcd_style_t opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order order)
{
    mtr::lcd_style_t style;
    style.order            = order;
    style.background_color = { 0.0f, 0.0f, 0.0f, 1.0f };
    return style;
}

mtr::glow_style_t visible_glow()
{
    mtr::glow_style_t style;
    style.color     = { 0.0f, 0.0f, 0.0f, 0.75f };
    style.radius_px = 3.0f;
    return style;
}

/// A batch of the shared sample text, with frame rectangles when asked for.
bool build_sample_batch(
    const mtr::Font_snapshot& font,
    bool                      with_frames,
    mtr::Text_batch&          out_batch)
{
    if (with_frames &&
        !check_status(
            out_batch.enable_glyph_frames(),
            mtr::Text_status::OK,
            "glyph frames must be enabled on an empty batch"))
    {
        return false;
    }

    return check_status(
        out_batch.append_run(font, "Varinomics 01", 8.0f, 40.0f),
        mtr::Text_status::OK,
        "the sample run must be appended");
}

bool test_batch_glyph_frames_bound_their_own_quads()
{
    const mtr::Font_snapshot& font = shared_snapshot();

    mtr::Text_batch plain;
    bool ok = check(!plain.has_glyph_frames(), "a batch carries no frame rectangles by default");
    ok &= check(plain.glyph_frames().empty(), "a batch without the capability exposes no frames");
    ok &= check_status(
        plain.append_run(font, "Varinomics", 4.0f, 20.0f),
        mtr::Text_status::OK,
        "a plain run must append");
    ok &= check(
        plain.glyph_frames().empty(),
        "a run appended without the capability must still carry no frames");
    ok &= check_status(
        plain.enable_glyph_frames(),
        mtr::Text_status::INVALID_ARGUMENT,
        "frame rectangles cannot be turned on part-way through a batch");

    mtr::glyph_frames_t unknown;
    unknown.version = mtr::k_glyph_frames_version + 1u;
    mtr::Text_batch future;
    ok &= check_status(
        future.enable_glyph_frames(unknown),
        mtr::Text_status::CAPABILITY_UNSUPPORTED,
        "an unknown glyph-frame version must be reported at its own boundary");
    ok &= check(
        !future.has_glyph_frames(),
        "a rejected capability must leave the batch without it");
    ok &= check_status(
        future.append_run(font, "Varinomics", 4.0f, 20.0f),
        mtr::Text_status::OK,
        "a batch that rejected an unknown version still appends base geometry");

    mtr::Text_batch framed;
    ok &= build_sample_batch(font, true, framed);
    ok &= check(framed.has_glyph_frames(), "the capability must be observable on the batch");
    ok &= check(
        framed.glyph_frames().size() == framed.vertices().size(),
        "a framed batch carries one rectangle per vertex");

    // Every quad's rectangle must be that quad's own bound, which is what makes
    // the fragment stage's reconstruction of a fragment's place in its glyph
    // agree with where the vertices actually put it.
    const std::span<const msdf::text_vertex_t> vertices = framed.vertices();
    const std::span<const mtr::glyph_frame_t>  frames   = framed.glyph_frames();
    ok &= check(vertices.size() % 4u == 0u, "a run emits whole quads");
    for (std::size_t i = 0; ok && i + 4u <= vertices.size(); i += 4u) {
        float left   = vertices[i].x;
        float top    = vertices[i].y;
        float right  = left;
        float bottom = top;
        for (std::size_t corner = 1; corner < 4u; ++corner) {
            left   = std::min(left,   vertices[i + corner].x);
            top    = std::min(top,    vertices[i + corner].y);
            right  = std::max(right,  vertices[i + corner].x);
            bottom = std::max(bottom, vertices[i + corner].y);
        }

        for (std::size_t corner = 0; corner < 4u; ++corner) {
            const mtr::glyph_frame_t& frame = frames[i + corner];
            ok &= check(
                frame.x == left && frame.y == top &&
                frame.width == right - left && frame.height == bottom - top,
                "every vertex of a quad must carry that quad's own rectangle");
        }
        ok &= check(
            frames[i].width > 0.0f && frames[i].height > 0.0f,
            "a visible glyph's rectangle must have a positive size");
    }

    framed.clear();
    ok &= check(
        !framed.has_glyph_frames() && framed.glyph_frames().empty(),
        "clear() returns a batch to its default state, frame rectangles included");

    return ok;
}

bool test_batch_quads_and_frames_must_agree()
{
    const mtr::Font_snapshot& font = shared_snapshot();

    mtr::Text_batch source;
    bool ok = build_sample_batch(font, true, source);
    if (!ok) {
        return false;
    }

    const std::vector<msdf::text_vertex_t> vertices(
        source.vertices().begin(), source.vertices().end());
    const std::vector<std::uint32_t> indices(source.indices().begin(), source.indices().end());
    const std::vector<mtr::glyph_frame_t> frames(
        source.glyph_frames().begin(), source.glyph_frames().end());

    mtr::Text_batch framed;
    ok &= check_status(framed.enable_glyph_frames(), mtr::Text_status::OK, "frames must enable");
    ok &= check_status(
        framed.append_quads(font, vertices, indices),
        mtr::Text_status::INVALID_ARGUMENT,
        "a framed batch must be given one rectangle per vertex");
    ok &= check(
        framed.empty() && framed.glyph_frames().empty(),
        "a rejected append must leave the batch untouched");
    ok &= check_status(
        framed.append_quads(
            font, vertices, indices,
            std::span<const mtr::glyph_frame_t>(frames).first(frames.size() - 4u)),
        mtr::Text_status::INVALID_ARGUMENT,
        "a short rectangle span must be rejected");
    const auto rejects_invalid_frames = [&](std::vector<mtr::glyph_frame_t> invalid, std::string_view message) {
        const mtr::text_result_t result = framed.append_quads(font, vertices, indices, invalid);
        return
            check_status(result, mtr::Text_status::INVALID_ARGUMENT, message) &&
            check(
                framed.empty() && framed.glyph_frames().empty(),
                "invalid frame rectangles must leave the batch untouched");
    };

    std::vector<mtr::glyph_frame_t> invalid = frames;
    invalid[0].width = std::numeric_limits<float>::quiet_NaN();
    ok &= rejects_invalid_frames(invalid, "a non-finite frame rectangle must be rejected");

    invalid = frames;
    invalid[0].height = -1.0f;
    ok &= rejects_invalid_frames(invalid, "a non-positive frame rectangle must be rejected");

    invalid = frames;
    invalid[1].x += 1.0f;
    ok &= rejects_invalid_frames(invalid, "a quad whose vertices disagree on its frame must be rejected");

    invalid = frames;
    invalid[0].width += 1.0f;
    invalid[1].width += 1.0f;
    invalid[2].width += 1.0f;
    invalid[3].width += 1.0f;
    ok &= rejects_invalid_frames(invalid, "a frame that is not its quad's AABB must be rejected");

    const std::vector<msdf::text_vertex_t> triangle(vertices.begin(), vertices.begin() + 3);
    const std::array<std::uint32_t, 3> triangle_indices = { 0u, 1u, 2u };
    const std::array<mtr::glyph_frame_t, 3> triangle_frames = {
        frames[0], frames[0], frames[0],
    };
    ok &= check_status(
        framed.append_quads(font, triangle, triangle_indices, triangle_frames),
        mtr::Text_status::INVALID_ARGUMENT,
        "frame rectangles must be grouped by complete quads");
    ok &= check(
        framed.empty() && framed.glyph_frames().empty(),
        "an incomplete frame group must leave the batch untouched");
    ok &= check_status(
        framed.append_quads(font, vertices, indices, frames),
        mtr::Text_status::OK,
        "matching vertices and rectangles must append");
    ok &= check(
        framed.glyph_frames().size() == framed.vertices().size(),
        "the appended rectangles must stay parallel to the vertices");

    mtr::Text_batch plain;
    ok &= check_status(
        plain.append_quads(font, vertices, indices, frames),
        mtr::Text_status::INVALID_ARGUMENT,
        "a batch without the capability must not accept rectangles");
    ok &= check(plain.empty(), "that rejection must leave the batch empty");
    ok &= check_status(
        plain.append_quads(font, vertices, indices),
        mtr::Text_status::OK,
        "the same quads without rectangles must append");

    return ok;
}

bool test_null_unknown_capability_versions_stay_local()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    if (!check(rhi != nullptr, "the Null QRhi backend must be available")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(offscreen.valid() && held.snapshot != nullptr, "the Null fixture must build")) {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::Text_batch framed;
    bool ok = build_sample_batch(*held.snapshot, true, framed);
    if (!ok) {
        return false;
    }

    struct case_t
    {
        const char*       name;
        mtr::draw_state_t state;
        mtr::Text_status  status;
    };

    std::vector<case_t> cases;
    {
        mtr::draw_state_t state;
        state.lcd          = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::RGB);
        state.lcd->version = mtr::k_lcd_style_version + 1u;
        cases.push_back({ "lcd", state, mtr::Text_status::CAPABILITY_UNSUPPORTED });
    }
    {
        mtr::draw_state_t state;
        state.glow          = visible_glow();
        state.glow->version = mtr::k_glow_style_version + 1u;
        cases.push_back({ "glow", state, mtr::Text_status::CAPABILITY_UNSUPPORTED });
    }
    {
        mtr::draw_state_t state;
        state.sdf_mask          = mtr::sdf_mask_t{};
        state.sdf_mask->version = mtr::k_sdf_mask_version + 1u;
        cases.push_back({ "sdf mask", state, mtr::Text_status::CAPABILITY_UNSUPPORTED });
    }
    {
        mtr::draw_state_t state;
        state.lcd = opaque_lcd(static_cast<msdf::lcd::Resolved_lcd_subpixel_order>(255u));
        cases.push_back({ "unknown LCD order", state, mtr::Text_status::INVALID_ARGUMENT });
    }

    QRhiCommandBuffer* cb = nullptr;
    ok &= check(
        rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
        "the offscreen frame must begin");

    mtr::frame_t frame;
    frame.rhi              = rhi.get();
    frame.command_buffer   = cb;
    frame.render_target    = offscreen.target();
    frame.resource_updates = rhi->nextResourceUpdateBatch();

    // The base draw is queued first, then every unknown version is refused, and
    // the base draw still records: an unknown optional version cannot take the
    // frame's ordinary text down with it.
    renderer.begin_frame();
    ok &= check_status(
        renderer.queue(framed, mtr::draw_state_t{}), mtr::Text_status::OK,
        "the frame's base text must queue");

    for (const case_t& item : cases) {
        ok &= check_status(
            renderer.queue(framed, item.state), item.status,
            item.name);
        ok &= check(
            renderer.diagnostics().queued_draws == 1,
            "a refused draw must leave the queued frame exactly as it was");
    }

    ok &= check_status(renderer.prepare(frame), mtr::Text_status::OK, "the frame must prepare");
    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    cb->setViewport(QRhiViewport(0.0f, 0.0f, 320.0f, 64.0f));
    ok &= check_status(renderer.record(frame), mtr::Text_status::OK, "the frame must record");
    cb->endPass();
    rhi->endOffscreenFrame();

    const mtr::renderer_diagnostics_t after = renderer.diagnostics();
    ok &= check(after.recorded_draws == 1, "the base draw must have recorded");
    ok &= check(
        after.styled_pipeline_builds == 0,
        "no refused draw may have built the styled pipeline");

    renderer.release_resources();
    return ok;
}

bool test_empty_batches_reject_unknown_capability_versions()
{
    mtr::Text_renderer renderer;
    mtr::Text_batch    empty;

    mtr::draw_state_t future;
    future.lcd          = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::RGB);
    future.lcd->version = mtr::k_lcd_style_version + 1u;

    bool ok = check_status(
        renderer.queue(empty, future), mtr::Text_status::CAPABILITY_UNSUPPORTED,
        "an empty batch must still reject an unknown optional capability version");
    ok &= check(
        renderer.diagnostics().queued_draws == 0 && renderer.diagnostics().queued_indices == 0,
        "an empty batch with an unknown capability must leave the queue empty");
    return ok;
}

bool test_invalid_optional_records_stay_local()
{
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(held.snapshot != nullptr, "the renderer's snapshot must build")) {
        return false;
    }

    mtr::Text_batch base;
    mtr::Text_batch framed;
    bool ok = build_sample_batch(*held.snapshot, false, base);
    ok     &= build_sample_batch(*held.snapshot, true, framed);
    if (!ok) {
        return false;
    }

    struct case_t
    {
        const char*       name;
        mtr::draw_state_t state;
    };

    std::vector<case_t> cases;
    {
        mtr::draw_state_t state;
        state.lcd = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::RGB);
        state.lcd->background_color[0] = -0.1f;
        cases.push_back({ "an LCD background below the unit interval", state });
    }
    {
        mtr::draw_state_t state;
        state.lcd = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::NONE);
        state.lcd->background_color[2] = std::numeric_limits<float>::infinity();
        cases.push_back({ "a non-finite LCD background", state });
    }
    {
        mtr::draw_state_t state;
        state.glow = visible_glow();
        state.glow->color[1] = 1.1f;
        cases.push_back({ "a glow colour above the unit interval", state });
    }
    {
        mtr::draw_state_t state;
        state.glow = visible_glow();
        state.glow->color[3] = std::numeric_limits<float>::quiet_NaN();
        cases.push_back({ "a non-finite glow colour", state });
    }
    {
        mtr::draw_state_t state;
        state.sdf_mask = mtr::sdf_mask_t{};
        state.color[0] = 1.1f;
        cases.push_back({ "a styled draw colour above the unit interval", state });
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);
    for (const case_t& item : cases) {
        renderer.begin_frame();
        ok &= check_status(
            renderer.queue(base, mtr::draw_state_t{}), mtr::Text_status::OK,
            "the comparison base draw must queue");
        const mtr::renderer_diagnostics_t before = renderer.diagnostics();
        ok &= check_status(
            renderer.queue(framed, item.state), mtr::Text_status::INVALID_ARGUMENT, item.name);
        const mtr::renderer_diagnostics_t after = renderer.diagnostics();
        ok &= check(
            after.queued_draws == before.queued_draws &&
                after.queued_indices == before.queued_indices,
            "an invalid optional record must leave an existing base draw unchanged");
    }

    renderer.reset_frame();
    return ok;
}

bool test_glow_reports_its_two_queued_index_ranges()
{
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(held.snapshot != nullptr, "the renderer's snapshot must build")) {
        return false;
    }

    mtr::Text_batch framed;
    if (!build_sample_batch(*held.snapshot, true, framed)) {
        return false;
    }

    mtr::draw_state_t glowing;
    glowing.glow = visible_glow();

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);
    renderer.begin_frame();
    bool ok = check_status(
        renderer.queue(framed, glowing), mtr::Text_status::OK, "a glowing draw must queue");
    const mtr::renderer_diagnostics_t queued = renderer.diagnostics();
    ok &= check(queued.queued_draws == 2, "a glowing draw queues two draw operations");
    ok &= check(
        queued.queued_indices == framed.indices().size() * 2u,
        "queued indices must include both of a glowing draw's index ranges");
    renderer.reset_frame();
    return ok;
}

bool test_styled_transform_rejects_before_preparation_mutates_resources()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(rhi != nullptr && held.snapshot != nullptr, "the Null fixture must build")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    mtr::Text_batch framed;
    if (!check(offscreen.valid(), "the Null target must be valid") ||
        !build_sample_batch(*held.snapshot, true, framed))
    {
        return false;
    }

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

    mtr::draw_state_t styled;
    styled.sdf_mask = mtr::sdf_mask_t{};

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);
    renderer.begin_frame();
    bool ok = check_status(
        renderer.queue(framed, styled), mtr::Text_status::OK, "the invalid-transform draw must queue");
    const mtr::renderer_diagnostics_t before = renderer.diagnostics();
    ok &= check_status(
        renderer.prepare(frame), mtr::Text_status::INVALID_ARGUMENT,
        "a styled draw without the frame pixel transform must be rejected");
    const mtr::renderer_diagnostics_t rejected = renderer.diagnostics();
    ok &= check(
        rejected.queued_draws == before.queued_draws &&
            rejected.queued_indices == before.queued_indices &&
            rejected.atlas_upload_enqueues == before.atlas_upload_enqueues &&
            rejected.buffer_upload_enqueues == before.buffer_upload_enqueues &&
            rejected.styled_pipeline_builds == before.styled_pipeline_builds,
        "a rejected styled transform must not mutate pending work or GPU resource state");

    renderer.reset_frame();
    styled.transform = mtr::pixel_ortho_transform(frame);
    ok &= check_status(
        renderer.queue(framed, styled), mtr::Text_status::OK,
        "a styled draw with the documented transform must queue");
    ok &= check_status(
        renderer.prepare(frame), mtr::Text_status::OK,
        "the corrected styled draw must prepare after the rejection");
    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    cb->setViewport(QRhiViewport(0.0f, 0.0f, 320.0f, 64.0f));
    ok &= check_status(renderer.record(frame), mtr::Text_status::OK, "the corrected styled draw must record");
    cb->endPass();
    rhi->endOffscreenFrame();
    renderer.release_resources();
    return ok;
}

bool test_null_prepare_base_survives_uniform_staging_failure()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(rhi != nullptr && held.snapshot != nullptr, "the Null fixture must build")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    mtr::Text_batch batch;
    if (!check(offscreen.valid(), "the Null target must be valid") ||
        !build_sample_batch(*held.snapshot, false, batch))
    {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);
    mtr::text_result_t prepared{};
    if (!check_status(
            run_null_frame(*rhi, offscreen, renderer, batch, mtr::draw_state_t{}, prepared),
            mtr::Text_status::OK,
            "the warm-up base frame must record"))
    {
        renderer.release_resources();
        return false;
    }

    QRhiCommandBuffer* cb = nullptr;
    if (!check(
            rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
            "the allocation-failure frame must begin"))
    {
        renderer.release_resources();
        return false;
    }

    mtr::frame_t frame;
    frame.rhi              = rhi.get();
    frame.command_buffer   = cb;
    frame.render_target    = offscreen.target();
    frame.resource_updates = rhi->nextResourceUpdateBatch();

    renderer.begin_frame();
    bool ok = check_status(renderer.queue(batch, {}), mtr::Text_status::OK, "the first base retry draw queues");
    ok &= check_status(renderer.queue(batch, {}), mtr::Text_status::OK, "the second base retry draw queues");
    const mtr::renderer_diagnostics_t before = renderer.diagnostics();

    mtr::text_result_t failed{};
    try {
        Failing_allocation fail(0);
        failed = renderer.prepare(frame);
    }
    catch (const std::exception&) {
        ok &= check(false, "base uniform staging must return a status instead of throwing");
    }
    ok &= check_status(failed, mtr::Text_status::OUT_OF_MEMORY, "base uniform staging allocation failure");
    const mtr::renderer_diagnostics_t rejected = renderer.diagnostics();
    ok &= check(
        rejected.queued_draws == before.queued_draws &&
            rejected.queued_indices == before.queued_indices &&
            rejected.buffer_upload_enqueues == before.buffer_upload_enqueues,
        "a base staging failure must leave the frame retry-safe");

    ok &= check_status(renderer.prepare(frame), mtr::Text_status::OK, "the base frame must prepare on retry");
    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    cb->setViewport(QRhiViewport(0.0f, 0.0f, 320.0f, 64.0f));
    ok &= check_status(renderer.record(frame), mtr::Text_status::OK, "the base frame must record on retry");
    cb->endPass();
    rhi->endOffscreenFrame();
    renderer.release_resources();
    return ok;
}

bool test_null_prepare_styled_survives_uniform_staging_failure()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(rhi != nullptr && held.snapshot != nullptr, "the Null fixture must build")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    mtr::Text_batch batch;
    if (!check(offscreen.valid(), "the Null target must be valid") ||
        !build_sample_batch(*held.snapshot, true, batch))
    {
        return false;
    }

    mtr::draw_state_t styled;
    styled.sdf_mask = mtr::sdf_mask_t{};

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);
    mtr::text_result_t prepared{};
    if (!check_status(
            run_null_frame(*rhi, offscreen, renderer, batch, styled, prepared),
            mtr::Text_status::OK,
            "the warm-up styled frame must record"))
    {
        renderer.release_resources();
        return false;
    }

    QRhiCommandBuffer* cb = nullptr;
    if (!check(
            rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
            "the allocation-failure frame must begin"))
    {
        renderer.release_resources();
        return false;
    }

    mtr::frame_t frame;
    frame.rhi              = rhi.get();
    frame.command_buffer   = cb;
    frame.render_target    = offscreen.target();
    frame.resource_updates = rhi->nextResourceUpdateBatch();
    styled.transform       = mtr::pixel_ortho_transform(frame);

    renderer.begin_frame();
    bool ok = check_status(renderer.queue(batch, styled), mtr::Text_status::OK, "the first styled retry draw queues");
    ok &= check_status(renderer.queue(batch, styled), mtr::Text_status::OK, "the second styled retry draw queues");
    const mtr::renderer_diagnostics_t before = renderer.diagnostics();

    mtr::text_result_t failed{};
    try {
        // The copied per-frame blocks survive this one allocation; staging the
        // byte buffer is the next allocation and must report OUT_OF_MEMORY.
        Failing_allocation fail(1);
        failed = renderer.prepare(frame);
    }
    catch (const std::exception&) {
        ok &= check(false, "styled uniform staging must return a status instead of throwing");
    }
    ok &= check_status(failed, mtr::Text_status::OUT_OF_MEMORY, "styled uniform staging allocation failure");
    const mtr::renderer_diagnostics_t rejected = renderer.diagnostics();
    ok &= check(
        rejected.queued_draws == before.queued_draws &&
            rejected.queued_indices == before.queued_indices &&
            rejected.buffer_upload_enqueues == before.buffer_upload_enqueues,
        "a styled staging failure must leave the frame retry-safe");

    ok &= check_status(renderer.prepare(frame), mtr::Text_status::OK, "the styled frame must prepare on retry");
    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    cb->setViewport(QRhiViewport(0.0f, 0.0f, 320.0f, 64.0f));
    ok &= check_status(renderer.record(frame), mtr::Text_status::OK, "the styled frame must record on retry");
    cb->endPass();
    rhi->endOffscreenFrame();
    renderer.release_resources();
    return ok;
}

bool test_null_rejects_undrawable_capability_combinations()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    if (!check(rhi != nullptr, "the Null QRhi backend must be available")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(offscreen.valid() && held.snapshot != nullptr, "the Null fixture must build")) {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::Text_batch framed;
    mtr::Text_batch frameless;
    bool ok = build_sample_batch(*held.snapshot, true,  framed);
    ok     &= build_sample_batch(*held.snapshot, false, frameless);
    if (!ok) {
        return false;
    }

    const auto queue_alone = [&](const mtr::Text_batch& batch, const mtr::draw_state_t& state) {
        renderer.begin_frame();
        return renderer.queue(batch, state);
    };

    {
        mtr::draw_state_t state;
        state.sdf_mask = mtr::sdf_mask_t{};
        ok &= check_status(
            queue_alone(frameless, state),
            mtr::Text_status::INVALID_ARGUMENT,
            "a styled draw needs a batch with frame rectangles");
    }
    {
        mtr::draw_state_t state;
        state.glow            = visible_glow();
        state.glow->radius_px = 0.0f;
        ok &= check_status(
            queue_alone(framed, state),
            mtr::Text_status::INVALID_ARGUMENT,
            "a glow with no radius must be refused rather than drawn as nothing");
    }
    {
        mtr::draw_state_t state;
        state.glow           = visible_glow();
        state.glow->color[3] = 0.0f;
        ok &= check_status(
            queue_alone(framed, state),
            mtr::Text_status::INVALID_ARGUMENT,
            "an invisible glow colour must be refused");
    }
    {
        mtr::draw_state_t state;
        state.lcd  = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::RGB);
        state.glow = visible_glow();
        ok &= check_status(
            queue_alone(framed, state),
            mtr::Text_status::INVALID_ARGUMENT,
            "a subpixel order and a glow cannot be drawn in one draw state");
    }
    {
        mtr::draw_state_t state;
        state.lcd      = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::BGR);
        state.color[3] = std::nextafter(k_opaque_cutoff, 0.0f);
        ok &= check_status(
            queue_alone(framed, state),
            mtr::Text_status::INVALID_ARGUMENT,
            "a subpixel order below the shared opacity cutoff must be refused");
    }
    {
        mtr::draw_state_t state;
        state.lcd                        = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::BGR);
        state.lcd->background_color[3]   = std::nextafter(k_opaque_cutoff, 0.0f);
        ok &= check_status(
            queue_alone(framed, state),
            mtr::Text_status::INVALID_ARGUMENT,
            "a background below the shared opacity cutoff must be refused");
    }

    // Exactly at the shared cutoff the same draw is accepted, which is what
    // binds this component's CPU decision to lcd_shader_reference rather than
    // to a threshold of its own.
    {
        mtr::draw_state_t state;
        state.lcd                      = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::BGR);
        state.color[3]                 = k_opaque_cutoff;
        state.lcd->background_color[3] = k_opaque_cutoff;
        ok &= check_status(
            queue_alone(framed, state),
            mtr::Text_status::OK,
            "a draw at the shared opacity cutoff must be accepted");
    }

    // NONE asks for no subpixel filtering, so none of the opacity conditions
    // apply to it and a translucent draw with a background is still valid.
    {
        mtr::draw_state_t state;
        state.lcd      = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::NONE);
        state.color[3] = 0.25f;
        ok &= check_status(
            queue_alone(framed, state),
            mtr::Text_status::OK,
            "an order of NONE places no opacity condition on the draw");
    }

    renderer.reset_frame();
    renderer.release_resources();
    return ok;
}

bool test_null_base_path_is_untouched_by_the_capability_set()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    if (!check(rhi != nullptr, "the Null QRhi backend must be available")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(offscreen.valid() && held.snapshot != nullptr, "the Null fixture must build")) {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    // The batch carries frame rectangles the draw state does not ask to use, so
    // this also shows that having them is not what moves a draw off the base path.
    mtr::Text_batch framed;
    bool ok = build_sample_batch(*held.snapshot, true, framed);
    if (!ok) {
        return false;
    }

    mtr::text_result_t prepared{};
    const mtr::text_result_t recorded =
        run_null_frame(*rhi, offscreen, renderer, framed, mtr::draw_state_t{}, prepared);
    ok &= check_status(prepared, mtr::Text_status::OK, "the base frame must prepare");
    ok &= check_status(recorded, mtr::Text_status::OK, "the base frame must record");

    const mtr::renderer_diagnostics_t after = renderer.diagnostics();
    ok &= check(after.pipeline_builds == 1, "the base pipeline must be the one that was built");
    ok &= check(
        after.styled_pipeline_builds == 0,
        "a frame with no optional capability must not build the styled pipeline");
    ok &= check(
        after.styled_vertex_buffer_bytes == 0 &&
        after.styled_index_buffer_bytes == 0 &&
        after.styled_uniform_buffer_bytes == 0,
        "a frame with no optional capability must allocate no styled buffers");
    ok &= check(
        after.buffer_upload_enqueues == 3,
        "the base path must still enqueue exactly its vertex, index, and uniform uploads");
    ok &= check(after.recorded_draws == 1, "one draw state must record one draw");
    ok &= check(
        after.recorded_pipeline_binds == 1,
        "a frame on one path must bind one pipeline");

    renderer.release_resources();
    return ok;
}

bool test_null_styled_draws_use_their_own_resources()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    if (!check(rhi != nullptr, "the Null QRhi backend must be available")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(offscreen.valid() && held.snapshot != nullptr, "the Null fixture must build")) {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::Text_batch framed;
    bool ok = build_sample_batch(*held.snapshot, true, framed);
    if (!ok) {
        return false;
    }

    mtr::draw_state_t styled;
    styled.lcd = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::RGB);

    mtr::text_result_t prepared{};
    mtr::text_result_t recorded =
        run_null_frame(*rhi, offscreen, renderer, framed, styled, prepared);
    ok &= check_status(prepared, mtr::Text_status::OK, "the styled frame must prepare");
    ok &= check_status(recorded, mtr::Text_status::OK, "the styled frame must record");

    mtr::renderer_diagnostics_t after = renderer.diagnostics();
    ok &= check(
        after.styled_pipeline_builds == 1,
        "a styled frame must build the styled pipeline once");
    ok &= check(after.pipeline_builds == 0, "a styled frame must not build the base pipeline");
    ok &= check(
        after.styled_vertex_buffer_bytes > 0 &&
        after.styled_index_buffer_bytes > 0 &&
        after.styled_uniform_buffer_bytes > 0,
        "a styled frame must allocate the styled buffers");
    ok &= check(
        after.vertex_buffer_bytes == 0 && after.index_buffer_bytes == 0,
        "a styled frame must allocate no base geometry buffers");
    ok &= check(
        after.buffer_upload_enqueues == 3,
        "a styled frame must enqueue exactly its own three uploads");
    ok &= check(after.recorded_draws == 1, "one styled draw state must record one draw");

    // A glow is drawn under the glyphs of its own draw, so it is a second draw
    // call rather than a second composite inside the same fragment.
    mtr::draw_state_t glowing;
    glowing.glow     = visible_glow();
    glowing.sdf_mask = mtr::sdf_mask_t{};

    const std::uint64_t builds_before = after.styled_pipeline_builds;
    recorded = run_null_frame(*rhi, offscreen, renderer, framed, glowing, prepared);
    ok &= check_status(prepared, mtr::Text_status::OK, "the glowing frame must prepare");
    ok &= check_status(recorded, mtr::Text_status::OK, "the glowing frame must record");

    after = renderer.diagnostics();
    ok &= check(after.recorded_draws == 2, "a glow adds one draw under the glyphs");
    ok &= check(
        after.queued_indices == 0,
        "queued index diagnostics clear after the glowing frame records");
    ok &= check(
        after.recorded_pipeline_binds == 1,
        "both of a glowing draw's calls run on one pipeline");
    ok &= check(
        after.styled_pipeline_builds == builds_before,
        "an unchanged target must not rebuild the styled pipeline");

    renderer.release_resources();
    ok &= check(
        renderer.diagnostics().styled_vertex_buffer_bytes == 0,
        "releasing resources must drop the styled buffers too");

    return ok;
}

bool test_null_styled_resources_recreate_and_stay_device_local()
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
    Offscreen_target wider_target(*first_rhi, QSize(640, 128), 1);
    Offscreen_target second_device_target(*second_rhi, QSize(320, 64), 1);
    if (!check(
            first_target.valid() && wider_target.valid() && second_device_target.valid(),
            "every offscreen render target must be created"))
    {
        return false;
    }

    mtr::Text_batch framed;
    mtr::Text_batch long_framed;
    bool ok = build_sample_batch(*held.snapshot, true, framed);
    ok     &= check_status(
        long_framed.enable_glyph_frames(), mtr::Text_status::OK, "frames must enable");
    for (int line = 0; line < 64; ++line) {
        ok &= check_status(
            long_framed.append_run(
                *held.snapshot, "Varinomics 0123456789", 8.0f, 40.0f + float(line)),
            mtr::Text_status::OK,
            "the long framed run must be appended");
    }
    if (!ok) {
        return false;
    }

    mtr::draw_state_t styled;
    styled.lcd = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::NONE);

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::text_result_t prepared{};
    ok &= check_status(
        run_null_frame(*first_rhi, first_target, renderer, framed, styled, prepared),
        mtr::Text_status::OK,
        "the first styled frame must record");
    const mtr::renderer_diagnostics_t first = renderer.diagnostics();
    ok &= check(first.styled_pipeline_builds == 1, "the first styled frame must build its pipeline");

    ok &= check_status(
        run_null_frame(*first_rhi, first_target, renderer, framed, styled, prepared),
        mtr::Text_status::OK,
        "the repeated styled frame must record");
    const mtr::renderer_diagnostics_t unchanged = renderer.diagnostics();
    ok &= check(
        unchanged.styled_pipeline_builds == first.styled_pipeline_builds &&
        unchanged.styled_vertex_buffer_bytes == first.styled_vertex_buffer_bytes,
        "an unchanged styled frame must rebuild nothing");

    ok &= check_status(
        run_null_frame(*first_rhi, first_target, renderer, long_framed, styled, prepared),
        mtr::Text_status::OK,
        "the growing styled frame must record");
    const mtr::renderer_diagnostics_t grown = renderer.diagnostics();
    ok &= check(
        grown.styled_vertex_buffer_bytes > unchanged.styled_vertex_buffer_bytes &&
        grown.styled_index_buffer_bytes > unchanged.styled_index_buffer_bytes,
        "a larger styled frame must grow the styled geometry buffers");
    ok &= check(
        grown.styled_pipeline_builds == unchanged.styled_pipeline_builds,
        "growing the styled buffers must not rebuild the styled pipeline");

    ok &= check_status(
        run_null_frame(*first_rhi, wider_target, renderer, framed, styled, prepared),
        mtr::Text_status::OK,
        "the retargeted styled frame must record");
    ok &= check(
        renderer.diagnostics().styled_pipeline_builds > grown.styled_pipeline_builds,
        "a different render pass must rebuild the styled pipeline");

    const mtr::renderer_diagnostics_t before_release = renderer.diagnostics();
    renderer.release_resources();
    const mtr::renderer_diagnostics_t released = renderer.diagnostics();
    ok &= check(
        released.resource_generation == before_release.resource_generation + 1 &&
        released.styled_vertex_buffer_bytes == 0 &&
        released.styled_index_buffer_bytes == 0 &&
        released.styled_uniform_buffer_bytes == 0,
        "releasing resources must drop the styled set with the rest");

    ok &= check_status(
        run_null_frame(*first_rhi, wider_target, renderer, framed, styled, prepared),
        mtr::Text_status::OK,
        "the styled frame after a release must record");
    ok &= check(
        renderer.diagnostics().styled_pipeline_builds == released.styled_pipeline_builds + 1,
        "a released renderer must rebuild the styled pipeline before recording again");

    // A second renderer on a second device owns its own styled resources: it
    // starts from nothing, and drawing on it moves no counter on the first.
    const mtr::renderer_diagnostics_t first_before_second = renderer.diagnostics();

    mtr::Text_renderer second_renderer;
    second_renderer.set_font(held.snapshot);
    ok &= check(
        second_renderer.diagnostics().styled_pipeline_builds == 0,
        "a fresh renderer owns no styled pipeline");
    ok &= check_status(
        run_null_frame(*second_rhi, second_device_target, second_renderer, framed, styled, prepared),
        mtr::Text_status::OK,
        "the second device's styled frame must record");
    ok &= check(
        second_renderer.diagnostics().styled_pipeline_builds == 1 &&
        second_renderer.diagnostics().styled_vertex_buffer_bytes > 0,
        "the second device must build its own styled resources");

    const mtr::renderer_diagnostics_t first_after_second = renderer.diagnostics();
    ok &= check(
        first_after_second.styled_pipeline_builds == first_before_second.styled_pipeline_builds &&
        first_after_second.resource_generation == first_before_second.resource_generation &&
        first_after_second.styled_vertex_buffer_bytes ==
            first_before_second.styled_vertex_buffer_bytes,
        "one device's styled work must not touch another device's resources");

    renderer.release_resources();
    second_renderer.release_resources();
    return ok;
}

bool test_null_mixed_paths_record_in_queue_order()
{
    std::unique_ptr<QRhi> rhi = make_null_rhi();
    if (!check(rhi != nullptr, "the Null QRhi backend must be available")) {
        return false;
    }

    Offscreen_target offscreen(*rhi, QSize(320, 64), 1);
    const mtr::font_snapshot_result_t held = build_sample_snapshot();
    if (!check(offscreen.valid() && held.snapshot != nullptr, "the Null fixture must build")) {
        return false;
    }

    mtr::Text_renderer renderer;
    renderer.set_font(held.snapshot);

    mtr::Text_batch framed;
    bool ok = build_sample_batch(*held.snapshot, true, framed);
    if (!ok) {
        return false;
    }

    mtr::draw_state_t styled;
    styled.lcd = opaque_lcd(msdf::lcd::Resolved_lcd_subpixel_order::VBGR);

    QRhiCommandBuffer* cb = nullptr;
    ok &= check(
        rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess,
        "the offscreen frame must begin");

    mtr::frame_t frame;
    frame.rhi              = rhi.get();
    frame.command_buffer   = cb;
    frame.render_target    = offscreen.target();
    frame.resource_updates = rhi->nextResourceUpdateBatch();
    styled.transform       = mtr::pixel_ortho_transform(frame);

    renderer.begin_frame();
    ok &= check_status(
        renderer.queue(framed, mtr::draw_state_t{}), mtr::Text_status::OK, "base text must queue");
    ok &= check_status(renderer.queue(framed, styled), mtr::Text_status::OK, "styled text must queue");
    ok &= check_status(
        renderer.queue(framed, mtr::draw_state_t{}), mtr::Text_status::OK, "base text must queue again");
    ok &= check(
        renderer.diagnostics().queued_draws == 3,
        "three draw states with no glow are three draws");

    ok &= check_status(renderer.prepare(frame), mtr::Text_status::OK, "the mixed frame must prepare");
    cb->beginPass(frame.render_target, QColor(0, 0, 0, 0), { 1.0f, 0 }, frame.resource_updates);
    cb->setViewport(QRhiViewport(0.0f, 0.0f, 320.0f, 64.0f));
    ok &= check_status(renderer.record(frame), mtr::Text_status::OK, "the mixed frame must record");
    cb->endPass();
    rhi->endOffscreenFrame();

    const mtr::renderer_diagnostics_t after = renderer.diagnostics();
    ok &= check(after.recorded_draws == 3, "every queued draw must record");
    // Base, styled, base is three runs, so the queue order was kept rather than
    // the draws being sorted by path.
    ok &= check(
        after.recorded_pipeline_binds == 3,
        "an alternating frame must bind once per change of path");
    ok &= check(
        after.pipeline_builds == 1 && after.styled_pipeline_builds == 1,
        "a mixed frame must build one pipeline of each kind");
    ok &= check(
        after.buffer_upload_enqueues == 6,
        "a mixed frame must enqueue three uploads for each path");

    renderer.release_resources();
    return ok;
}

bool test_shader_artifacts_cover_every_required_profile()
{
    struct profile_t
    {
        const char*       name;
        QShader::Source   source;
        int               version;
        QShaderVersion::Flags flags;
    };

    const profile_t profiles[] = {
        { "GLSL ES 3.0",  QShader::GlslShader, 300, QShaderVersion::GlslEs      },
        { "desktop GL 3.3", QShader::GlslShader, 330, QShaderVersion::Flags()   },
        { "desktop GL 4.1", QShader::GlslShader, 410, QShaderVersion::Flags()   },
        { "HLSL 5.0",     QShader::HlslShader,  50, QShaderVersion::Flags()     },
        { "Metal 1.2",    QShader::MslShader,   12, QShaderVersion::Flags()     },
        { "Metal 2.1",    QShader::MslShader,   21, QShaderVersion::Flags()     },
    };

    const char* artifacts[] = {
        ":/vnm_msdf_text/shaders/rhi/msdf_text.vert.qsb",
        ":/vnm_msdf_text/shaders/rhi/msdf_text.frag.qsb",
        ":/vnm_msdf_text/shaders/rhi/msdf_text_styled.vert.qsb",
        ":/vnm_msdf_text/shaders/rhi/msdf_text_styled.frag.qsb",
    };

    bool ok = true;
    for (const char* path : artifacts) {
        QFile file(QString::fromLatin1(path));
        if (!check(file.open(QIODevice::ReadOnly), std::string("shader artifact ") + path)) {
            ok = false;
            continue;
        }

        const QShader shader = QShader::fromSerialized(file.readAll());
        ok &= check(shader.isValid(), std::string("valid artifact for ") + path);

        const QList<QShaderKey> keys = shader.availableShaders();
        bool spirv = false;
        for (const QShaderKey& key : keys) {
            spirv = spirv || key.source() == QShader::SpirvShader;
        }
        ok &= check(spirv, std::string("SPIR-V for ") + path);

        for (const profile_t& profile : profiles) {
            bool present = false;
            for (const QShaderKey& key : keys) {
                present = present ||
                    (key.source() == profile.source &&
                     key.sourceVersion().version() == profile.version &&
                     key.sourceVersion().flags() == profile.flags);
            }
            ok &= check(present, std::string(profile.name) + " for " + path);
        }
    }

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
    ok &= run_test("identity is the serialized build inputs", test_identity_is_the_serialized_build_inputs);
    ok &= run_test("snapshot rejects invalid arguments", test_snapshot_rejects_invalid_arguments);
    ok &= run_test("snapshot reports build failure", test_snapshot_reports_build_failure);
    ok &= run_test("snapshot reports partial build", test_snapshot_reports_partial_build);
    ok &= run_test("identity is content addressed", test_identity_is_content_addressed);
    ok &= run_test("batch matches cpu quads", test_batch_matches_cpu_quads);
    ok &= run_test("batch agrees with measurement and bounds", test_batch_agrees_with_measurement_and_bounds);
    ok &= run_test("batch rejects invalid geometry", test_batch_rejects_invalid_geometry);
    ok &= run_test("batch survives allocation failure", test_batch_survives_allocation_failure);
    ok &= run_test("null records prepared text", test_null_records_prepared_text);
    ok &= run_test("null reports frame and font failures", test_null_reports_frame_and_font_failures);
    ok &= run_test("null queue after prepare is not recorded", test_null_queue_after_prepare_is_not_recorded);
    ok &= run_test("null resource recreation causes", test_null_resource_recreation_causes);
    ok &= run_test("null resources are device local", test_null_resources_are_device_local);
    ok &= run_test("null device change rebuilds resources", test_null_device_change_rebuilds_resources);
    ok &= run_test("null multiple draw states record separately", test_null_multiple_draw_states_record_separately);
    ok &= run_test("null font replacement does not move a queued frame", test_null_font_replacement_does_not_move_a_queued_frame);
    ok &= run_test("null cancelled update batch is uploaded again", test_null_cancelled_update_batch_is_uploaded_again);
    ok &= run_test("null unprepared record does not settle an abandoned upload", test_null_unprepared_record_does_not_settle_an_abandoned_upload);
    ok &= run_test("null rejects unusable clip rectangles", test_null_rejects_unusable_clip_rectangles);
    ok &= run_test("null geometry growth keeps the pipeline", test_null_geometry_growth_keeps_the_pipeline);
    ok &= run_test("null queue survives allocation failure", test_null_queue_survives_allocation_failure);
    ok &= run_test("batch glyph frames bound their own quads", test_batch_glyph_frames_bound_their_own_quads);
    ok &= run_test("batch quads and frames must agree", test_batch_quads_and_frames_must_agree);
    ok &= run_test("null unknown capability versions stay local", test_null_unknown_capability_versions_stay_local);
    ok &= run_test("empty batches reject unknown capability versions", test_empty_batches_reject_unknown_capability_versions);
    ok &= run_test("invalid optional records stay local", test_invalid_optional_records_stay_local);
    ok &= run_test("null rejects undrawable capability combinations", test_null_rejects_undrawable_capability_combinations);
    ok &= run_test("glow reports its two queued index ranges", test_glow_reports_its_two_queued_index_ranges);
    ok &= run_test("styled transform rejects before preparation mutates resources", test_styled_transform_rejects_before_preparation_mutates_resources);
    ok &= run_test("null base prepare survives uniform staging failure", test_null_prepare_base_survives_uniform_staging_failure);
    ok &= run_test("null styled prepare survives uniform staging failure", test_null_prepare_styled_survives_uniform_staging_failure);
    ok &= run_test("null base path is untouched by the capability set", test_null_base_path_is_untouched_by_the_capability_set);
    ok &= run_test("null styled draws use their own resources", test_null_styled_draws_use_their_own_resources);
    ok &= run_test("null styled resources recreate and stay device local", test_null_styled_resources_recreate_and_stay_device_local);
    ok &= run_test("null mixed paths record in queue order", test_null_mixed_paths_record_in_queue_order);
    ok &= run_test("shader artifacts cover every required profile", test_shader_artifacts_cover_every_required_profile);
    return ok ? 0 : 1;
}
