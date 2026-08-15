// Consumes the installed atlas component. It is built only when the package
// exports it and its FreeType and msdfgen dependencies resolved, which is the
// only configuration in which the atlas target can be linked at all.
#include <vnm_msdf_text/msdf_text.h>

#include <iostream>

int main()
{
    const vnm::msdf_text::options_t options;
    if (options.atlas_size <= 0 || options.atlas_px_range <= 0.0f) {
        std::cerr << "FAIL: the installed atlas header defaults to an unusable atlas\n";
        return 1;
    }

    const vnm::msdf_text::glyph_t glyph;
    if (glyph.visible || glyph.advance_units != 0.0f) {
        std::cerr << "FAIL: the installed atlas header defaults a glyph to a drawn one\n";
        return 1;
    }

    std::cout << "package consumer linked vnm_msdf_text::vnm_msdf_text\n";
    return 0;
}
