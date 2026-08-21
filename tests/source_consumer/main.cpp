// Consumes vnm_msdf_text::rhi the way the README documents: public headers
// only, no Qt include of its own, no private source directory.
#include <vnm_msdf_text/rhi/draw_capabilities.h>
#include <vnm_msdf_text/rhi/font_snapshot.h>
#include <vnm_msdf_text/rhi/status.h>
#include <vnm_msdf_text/rhi/text_batch.h>
#include <vnm_msdf_text/rhi/text_renderer.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace mtr = vnm::msdf_text::rhi;

int main()
{
    std::ifstream file(VNM_MSDF_TEXT_CONSUMER_FONT_FILE, std::ios::binary);
    if (!file) {
        std::cerr << "FAIL: the consumer could not open its font\n";
        return 1;
    }
    const std::istreambuf_iterator<char> font_begin(file);
    const std::istreambuf_iterator<char> font_end;
    const std::vector<std::uint8_t>      font_bytes(font_begin, font_end);

    const std::vector<char32_t> codepoints = {U'V', U'n', U'm', U'0', U'1'};

    const mtr::font_snapshot_result_t built =
        mtr::build_font_snapshot(font_bytes, 24, codepoints);
    if (built.result.status != mtr::Text_status::OK || !built.snapshot) {
        std::cerr << "FAIL: the consumer could not build a snapshot ("
                  << mtr::text_status_name(built.result.status) << ": "
                  << built.result.diagnostic.data() << ")\n";
        return 1;
    }

    mtr::Text_batch          batch;
    const mtr::text_result_t framed = batch.enable_glyph_frames();
    if (framed.status != mtr::Text_status::OK || !batch.has_glyph_frames()) {
        std::cerr << "FAIL: the consumer could not ask for glyph frames ("
                  << mtr::text_status_name(framed.status) << ")\n";
        return 1;
    }

    const mtr::text_result_t appended =
        batch.append_run(*built.snapshot, "Vnm01", 4.0f, 20.0f);
    if (appended.status != mtr::Text_status::OK || batch.empty()) {
        std::cerr << "FAIL: the consumer could not append a run ("
                  << mtr::text_status_name(appended.status) << ")\n";
        return 1;
    }
    if (batch.glyph_frames().size() != batch.vertices().size()) {
        std::cerr << "FAIL: a framed batch must carry one rectangle per vertex\n";
        return 1;
    }

    // A renderer with no frame yet still has to link and report its state,
    // which is what proves the whole component came from the alias.
    mtr::Text_renderer renderer;
    renderer.set_font(built.snapshot);

    // An optional capability a consumer's build does not implement has to be
    // refused at that capability's own boundary, and the public constants are
    // what a consumer compares against before it gets there.
    mtr::draw_state_t future;
    future.lcd          = mtr::lcd_style_t{};
    future.lcd->version = mtr::k_lcd_style_version + 1u;
    const mtr::text_result_t refused = renderer.queue(batch, future);
    if (refused.status != mtr::Text_status::CAPABILITY_UNSUPPORTED) {
        std::cerr << "FAIL: an unknown capability version must be refused, not drawn ("
                  << mtr::text_status_name(refused.status) << ")\n";
        return 1;
    }

    const mtr::text_result_t no_frame = renderer.prepare(mtr::frame_t{});
    if (no_frame.status != mtr::Text_status::INVALID_FRAME) {
        std::cerr << "FAIL: preparing without a frame must report an invalid frame\n";
        return 1;
    }
    if (renderer.diagnostics().atlas_upload_enqueues != 0) {
        std::cerr << "FAIL: a rejected frame must not enqueue an atlas upload\n";
        return 1;
    }
    if (mtr::pixel_ortho_transform(mtr::frame_t{}) != mtr::k_identity_transform) {
        std::cerr << "FAIL: a frame without a render target must transform by identity\n";
        return 1;
    }

    std::cout << "source consumer linked vnm_msdf_text::rhi and measured "
              << batch.indices().size() << " indices\n";
    return 0;
}
