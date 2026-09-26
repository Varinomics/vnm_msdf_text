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

using lcd_subpixel_order_t = Resolved_lcd_subpixel_order;

// The requested policy includes automatic detection; resolved orders never do.
enum class Lcd_subpixel_order_policy : std::uint8_t
{
    AUTO, NONE, RGB, BGR, VRGB, VBGR,
};

struct lcd_request_t
{
    bool automatic = true;
    lcd_subpixel_order_t resolved_order = lcd_subpixel_order_t::NONE;
};

constexpr lcd_subpixel_order_t lcd_sanitize_resolved_order(lcd_subpixel_order_t order)
{
    return is_display_specific(order) ? order : lcd_subpixel_order_t::NONE;
}

constexpr lcd_request_t lcd_auto_request() { return {}; }
constexpr lcd_request_t lcd_none_request() { return {false, lcd_subpixel_order_t::NONE}; }
constexpr lcd_request_t lcd_explicit_request(lcd_subpixel_order_t order)
{
    return {false, lcd_sanitize_resolved_order(order)};
}

constexpr lcd_request_t lcd_request_from_policy(Lcd_subpixel_order_policy policy)
{
    if (policy == Lcd_subpixel_order_policy::AUTO) { return lcd_auto_request(); }
    return lcd_explicit_request(static_cast<lcd_subpixel_order_t>(static_cast<int>(policy) - 1));
}

constexpr lcd_subpixel_order_t lcd_effective_order(lcd_request_t request, lcd_subpixel_order_t detected)
{
    return lcd_sanitize_resolved_order(request.automatic ? detected : request.resolved_order);
}

constexpr lcd_subpixel_order_t lcd_auto_order_from_detections(
    lcd_subpixel_order_t qt_order, lcd_subpixel_order_t os_order)
{
    return is_display_specific(qt_order) ? qt_order : lcd_sanitize_resolved_order(os_order);
}

} // namespace vnm::msdf_text::lcd
