// Consumes the installed vnm_msdf_text::lcd_contract component: the header has
// to be complete on its own, and the imported target has to be enough to build
// and link against.
#include <vnm_msdf_text/lcd_contract.h>

#include <iostream>

namespace lcd = vnm::msdf_text::lcd;

int main()
{
    using order_t = lcd::Resolved_lcd_subpixel_order;

    if (lcd::resolved_order_value(order_t::RGB) != 1 ||
        lcd::resolved_order_value(order_t::BGR) != 2) {
        std::cerr << "FAIL: the installed lcd_contract header reports the wrong order values\n";
        return 1;
    }
    if (!lcd::is_display_specific(order_t::VRGB)) {
        std::cerr << "FAIL: the installed lcd_contract header must classify VRGB as display specific\n";
        return 1;
    }
    if (lcd::shader_uniform_value(order_t::VBGR) != 4.0f) {
        std::cerr << "FAIL: the installed lcd_contract header reports the wrong VBGR uniform\n";
        return 1;
    }

    std::cout << "package consumer linked vnm_msdf_text::lcd_contract\n";
    return 0;
}
