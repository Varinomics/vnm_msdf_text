// Consumes the installed vnm_msdf_text::lcd_shader_reference component, which
// is what a shader author reads the LCD decode and filter constants from.
#include <vnm_msdf_text/lcd_shader_reference.h>

#include <iostream>

namespace lcd_ref = vnm::msdf_text::lcd::shader_reference;

int main()
{
    if (lcd_ref::k_lcd_decode_rgb_min != 0.5f ||
        lcd_ref::k_lcd_decode_rgb_max != 1.5f) {
        std::cerr << "FAIL: the installed shader reference header reports the wrong decode range\n";
        return 1;
    }
    if (lcd_ref::k_lcd_order_vrgb_uniform != 3.0f) {
        std::cerr << "FAIL: the installed shader reference header reports the wrong VRGB uniform\n";
        return 1;
    }
    if (lcd_ref::k_lcd_filter_center != 0.3359375f) {
        std::cerr << "FAIL: the installed shader reference header reports the wrong filter center\n";
        return 1;
    }

    std::cout << "package consumer linked vnm_msdf_text::lcd_shader_reference\n";
    return 0;
}
