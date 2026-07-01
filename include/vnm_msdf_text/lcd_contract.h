#pragma once

#include <cstdint>

namespace vnm::msdf_text::lcd {

enum class Resolved_lcd_subpixel_order : std::uint8_t
{
    NONE = 0,
    RGB  = 1,
    BGR  = 2,
    VRGB = 3,
    VBGR = 4,
};

constexpr bool is_display_specific(Resolved_lcd_subpixel_order order)
{
    switch (order) {
        case Resolved_lcd_subpixel_order::RGB:
        case Resolved_lcd_subpixel_order::BGR:
        case Resolved_lcd_subpixel_order::VRGB:
        case Resolved_lcd_subpixel_order::VBGR:
            return true;
        case Resolved_lcd_subpixel_order::NONE:
        default:
            return false;
    }
}

constexpr int resolved_order_value(Resolved_lcd_subpixel_order order)
{
    switch (order) {
        case Resolved_lcd_subpixel_order::RGB:  return 1;
        case Resolved_lcd_subpixel_order::BGR:  return 2;
        case Resolved_lcd_subpixel_order::VRGB: return 3;
        case Resolved_lcd_subpixel_order::VBGR: return 4;
        case Resolved_lcd_subpixel_order::NONE:
        default:                                return 0;
    }
}

constexpr float shader_uniform_value(Resolved_lcd_subpixel_order order)
{
    return static_cast<float>(resolved_order_value(order));
}

} // namespace vnm::msdf_text::lcd
