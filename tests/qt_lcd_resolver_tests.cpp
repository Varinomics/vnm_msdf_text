#include <vnm_msdf_text/qt/lcd_resolver.h>

#include <iostream>

namespace lcd = vnm::msdf_text::lcd;

int main()
{
    using Order = lcd::Resolved_lcd_subpixel_order;
    bool success = true;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; success = false; }
    };
    // Explicit policy and absent-display behavior are independent of the host monitor.
    for (int value = 1; value <= 5; ++value) {
        const auto request = lcd::lcd_request_from_policy(static_cast<lcd::Lcd_subpixel_order_policy>(value));
        check(lcd::resolve_lcd_subpixel_order_for_screen(request, nullptr) == static_cast<Order>(value - 1),
            "explicit policy requires no display");
    }
    check(lcd::resolve_lcd_subpixel_order_for_screen(lcd::lcd_auto_request(), nullptr) == Order::NONE,
        "automatic detection without a display fails closed");
    check(lcd::lcd_from_windows_font_smoothing_settings(false, 2, 1) == Order::NONE,
        "disabled Windows font smoothing disables automatic LCD fallback");
    check(lcd::lcd_from_windows_font_smoothing_settings(true, 1, 1) == Order::NONE,
        "standard font smoothing is not ClearType");
    check(lcd::lcd_from_windows_font_smoothing_settings(true, 2, 0) == Order::BGR,
        "Windows orientation zero means BGR");
    // DEVMODE rotations are counter-clockwise physical rotations.
    const Order rotated_rgb[] = {Order::RGB, Order::VBGR, Order::BGR, Order::VRGB};
    const Order rotated_bgr[] = {Order::BGR, Order::VRGB, Order::RGB, Order::VBGR};
    for (unsigned int rotation = 0; rotation != 4; ++rotation) {
        check(lcd::lcd_from_windows_display_settings(1, rotation, Order::NONE) == rotated_rgb[rotation],
            "RGB physical monitor rotation");
        check(lcd::lcd_from_windows_display_settings(2, rotation, Order::NONE) == rotated_bgr[rotation],
            "BGR physical monitor rotation");
        check(lcd::lcd_from_windows_display_settings(0, rotation, Order::RGB) == Order::NONE,
            "flat monitor must not inherit a global LCD fallback");
    }
    check(lcd::lcd_from_windows_display_settings(std::nullopt, 0, Order::BGR) == Order::BGR,
        "identified monitor without layout uses the fallback");
    check(lcd::lcd_from_windows_display_settings(7, 0, Order::RGB) == Order::NONE,
        "invalid monitor layout fails closed");
    return success ? 0 : 1;
}
