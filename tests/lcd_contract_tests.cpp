#include <vnm_msdf_text/lcd_contract.h>
#include <vnm_msdf_text/lcd_shader_reference.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>

namespace lcd = vnm::msdf_text::lcd;
namespace lcd_ref = vnm::msdf_text::lcd::shader_reference;

namespace {

static_assert(
    std::is_same_v<std::underlying_type_t<lcd::Resolved_lcd_subpixel_order>, std::uint8_t>);
static_assert(static_cast<int>(lcd::Resolved_lcd_subpixel_order::NONE) == 0);
static_assert(static_cast<int>(lcd::Resolved_lcd_subpixel_order::RGB) == 1);
static_assert(static_cast<int>(lcd::Resolved_lcd_subpixel_order::BGR) == 2);
static_assert(static_cast<int>(lcd::Resolved_lcd_subpixel_order::VRGB) == 3);
static_assert(static_cast<int>(lcd::Resolved_lcd_subpixel_order::VBGR) == 4);

bool check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool near_equal(float lhs, float rhs)
{
    return std::fabs(lhs - rhs) <= 0.000001f;
}

bool parse_float_literal(std::string_view text, float& parsed)
{
    try {
        std::size_t consumed = 0;
        parsed = std::stof(std::string(text), &consumed);
        return consumed == text.size();
    }
    catch (...) {
        return false;
    }
}

bool parses_to_float(std::string_view text, float expected)
{
    float parsed = 0.0f;
    return parse_float_literal(text, parsed) && near_equal(parsed, expected);
}

bool parse_decode_condition(
    std::string_view text,
    float&           min_exclusive,
    float&           max_exclusive)
{
    constexpr std::string_view min_prefix = "> ";
    constexpr std::string_view separator  = " && < ";

    if (text.substr(0, min_prefix.size()) != min_prefix) {
        return false;
    }

    const std::size_t separator_pos = text.find(separator, min_prefix.size());
    if (separator_pos == std::string_view::npos) {
        return false;
    }

    const std::string_view min_text =
        text.substr(min_prefix.size(), separator_pos - min_prefix.size());
    const std::string_view max_text =
        text.substr(separator_pos + separator.size());
    return
        parse_float_literal(min_text, min_exclusive) &&
        parse_float_literal(max_text, max_exclusive);
}

bool decode_condition_matches(
    std::string_view glsl_condition,
    float            expected_min_exclusive,
    float            expected_max_exclusive)
{
    float parsed_min = 0.0f;
    float parsed_max = 0.0f;
    return
        parse_decode_condition(glsl_condition, parsed_min, parsed_max) &&
        near_equal(parsed_min, expected_min_exclusive) &&
        near_equal(parsed_max, expected_max_exclusive);
}

bool test_lcd_contract_helpers()
{
    using order_t = lcd::Resolved_lcd_subpixel_order;

    constexpr order_t display_orders[] = {
        order_t::RGB,
        order_t::BGR,
        order_t::VRGB,
        order_t::VBGR,
    };
    constexpr int expected_values[] = {
        1,
        2,
        3,
        4,
    };

    bool ok = true;
    ok &= check(!lcd::is_display_specific(order_t::NONE), "NONE must not be display-specific");
    ok &= check(lcd::resolved_order_value(order_t::NONE) == 0, "NONE value must be 0");
    ok &= check(lcd::shader_uniform_value(order_t::NONE) == 0.0f, "NONE uniform must be 0.0");

    for (std::size_t i = 0; i < std::size(display_orders); ++i) {
        const order_t order = display_orders[i];
        ok &= check(lcd::is_display_specific(order), "display order must be display-specific");
        ok &= check(
            lcd::resolved_order_value(order) == expected_values[i],
            "display order integer value must match the public contract");
        ok &= check(
            lcd::shader_uniform_value(order) == static_cast<float>(expected_values[i]),
            "display order shader uniform must match the public contract");
    }

    struct order_reference_t
    {
        order_t order;
        int value;
        float uniform;
    };

    constexpr order_reference_t order_references[] = {
        {order_t::NONE, lcd_ref::k_lcd_order_none_value, lcd_ref::k_lcd_order_none_uniform},
        {order_t::RGB,  lcd_ref::k_lcd_order_rgb_value,  lcd_ref::k_lcd_order_rgb_uniform},
        {order_t::BGR,  lcd_ref::k_lcd_order_bgr_value,  lcd_ref::k_lcd_order_bgr_uniform},
        {order_t::VRGB, lcd_ref::k_lcd_order_vrgb_value, lcd_ref::k_lcd_order_vrgb_uniform},
        {order_t::VBGR, lcd_ref::k_lcd_order_vbgr_value, lcd_ref::k_lcd_order_vbgr_uniform},
    };
    for (const order_reference_t& reference : order_references) {
        ok &= check(
            lcd::resolved_order_value(reference.order) == reference.value,
            "helper resolved value must match shader reference value");
        ok &= check(
            lcd::shader_uniform_value(reference.order) == reference.uniform,
            "helper shader uniform must match shader reference uniform");
    }

    const order_t invalid_order = static_cast<order_t>(95);
    ok &= check(!lcd::is_display_specific(invalid_order), "invalid LCD order must fail closed");
    ok &= check(lcd::resolved_order_value(invalid_order) == 0, "invalid LCD order value must be 0");
    ok &= check(
        lcd::shader_uniform_value(invalid_order) == 0.0f,
        "invalid LCD order uniform must be 0.0");
    return ok;
}

bool test_lcd_shader_reference_values()
{
    using order_t = lcd::Resolved_lcd_subpixel_order;

    bool ok = true;
    ok &= check(lcd_ref::k_lcd_order_none_value == 0, "NONE integer reference must be 0");
    ok &= check(lcd_ref::k_lcd_order_rgb_value == 1, "RGB integer reference must be 1");
    ok &= check(lcd_ref::k_lcd_order_bgr_value == 2, "BGR integer reference must be 2");
    ok &= check(lcd_ref::k_lcd_order_vrgb_value == 3, "VRGB integer reference must be 3");
    ok &= check(lcd_ref::k_lcd_order_vbgr_value == 4, "VBGR integer reference must be 4");

    ok &= check(lcd_ref::k_lcd_order_none_uniform == 0.0f, "NONE uniform reference must be 0.0");
    ok &= check(lcd_ref::k_lcd_order_rgb_uniform == 1.0f, "RGB uniform reference must be 1.0");
    ok &= check(lcd_ref::k_lcd_order_bgr_uniform == 2.0f, "BGR uniform reference must be 2.0");
    ok &= check(lcd_ref::k_lcd_order_vrgb_uniform == 3.0f, "VRGB uniform reference must be 3.0");
    ok &= check(lcd_ref::k_lcd_order_vbgr_uniform == 4.0f, "VBGR uniform reference must be 4.0");
    ok &= check(
        lcd_ref::k_lcd_order_none_uniform ==
            static_cast<float>(lcd_ref::k_lcd_order_none_value),
        "NONE uniform reference must match its integer value");
    ok &= check(
        lcd_ref::k_lcd_order_rgb_uniform ==
            static_cast<float>(lcd_ref::k_lcd_order_rgb_value),
        "RGB uniform reference must match its integer value");
    ok &= check(
        lcd_ref::k_lcd_order_bgr_uniform ==
            static_cast<float>(lcd_ref::k_lcd_order_bgr_value),
        "BGR uniform reference must match its integer value");
    ok &= check(
        lcd_ref::k_lcd_order_vrgb_uniform ==
            static_cast<float>(lcd_ref::k_lcd_order_vrgb_value),
        "VRGB uniform reference must match its integer value");
    ok &= check(
        lcd_ref::k_lcd_order_vbgr_uniform ==
            static_cast<float>(lcd_ref::k_lcd_order_vbgr_value),
        "VBGR uniform reference must match its integer value");

    ok &= check(lcd_ref::k_lcd_decode_rgb_min == 0.5f, "RGB decode min must be 0.5");
    ok &= check(lcd_ref::k_lcd_decode_rgb_max == 1.5f, "RGB decode max must be 1.5");
    ok &= check(lcd_ref::k_lcd_decode_bgr_min == 1.5f, "BGR decode min must be 1.5");
    ok &= check(lcd_ref::k_lcd_decode_bgr_max == 2.5f, "BGR decode max must be 2.5");
    ok &= check(lcd_ref::k_lcd_decode_vrgb_min == 2.5f, "VRGB decode min must be 2.5");
    ok &= check(lcd_ref::k_lcd_decode_vrgb_max == 3.5f, "VRGB decode max must be 3.5");
    ok &= check(lcd_ref::k_lcd_decode_vbgr_min == 3.5f, "VBGR decode min must be 3.5");
    ok &= check(lcd_ref::k_lcd_decode_vbgr_max == 4.5f, "VBGR decode max must be 4.5");

    ok &= check(
        lcd_ref::k_lcd_decode_rgb_glsl == "> 0.5 && < 1.5",
        "RGB GLSL decode text must match");
    ok &= check(
        lcd_ref::k_lcd_decode_bgr_glsl == "> 1.5 && < 2.5",
        "BGR GLSL decode text must match");
    ok &= check(
        lcd_ref::k_lcd_decode_vrgb_glsl == "> 2.5 && < 3.5",
        "VRGB GLSL decode text must match");
    ok &= check(
        lcd_ref::k_lcd_decode_vbgr_glsl == "> 3.5 && < 4.5",
        "VBGR GLSL decode text must match");
    ok &= check(
        decode_condition_matches(
            lcd_ref::k_lcd_decode_rgb_glsl,
            lcd_ref::k_lcd_decode_rgb_min,
            lcd_ref::k_lcd_decode_rgb_max),
        "RGB GLSL decode thresholds must match numeric constants");
    ok &= check(
        decode_condition_matches(
            lcd_ref::k_lcd_decode_bgr_glsl,
            lcd_ref::k_lcd_decode_bgr_min,
            lcd_ref::k_lcd_decode_bgr_max),
        "BGR GLSL decode thresholds must match numeric constants");
    ok &= check(
        decode_condition_matches(
            lcd_ref::k_lcd_decode_vrgb_glsl,
            lcd_ref::k_lcd_decode_vrgb_min,
            lcd_ref::k_lcd_decode_vrgb_max),
        "VRGB GLSL decode thresholds must match numeric constants");
    ok &= check(
        decode_condition_matches(
            lcd_ref::k_lcd_decode_vbgr_glsl,
            lcd_ref::k_lcd_decode_vbgr_min,
            lcd_ref::k_lcd_decode_vbgr_max),
        "VBGR GLSL decode thresholds must match numeric constants");

    struct expected_decode_t
    {
        order_t order;
        float min_exclusive;
        float max_exclusive;
        std::string_view glsl_condition;
    };

    const expected_decode_t expected_decodes[] = {
        {
            order_t::RGB,
            lcd_ref::k_lcd_decode_rgb_min,
            lcd_ref::k_lcd_decode_rgb_max,
            lcd_ref::k_lcd_decode_rgb_glsl,
        },
        {
            order_t::BGR,
            lcd_ref::k_lcd_decode_bgr_min,
            lcd_ref::k_lcd_decode_bgr_max,
            lcd_ref::k_lcd_decode_bgr_glsl,
        },
        {
            order_t::VRGB,
            lcd_ref::k_lcd_decode_vrgb_min,
            lcd_ref::k_lcd_decode_vrgb_max,
            lcd_ref::k_lcd_decode_vrgb_glsl,
        },
        {
            order_t::VBGR,
            lcd_ref::k_lcd_decode_vbgr_min,
            lcd_ref::k_lcd_decode_vbgr_max,
            lcd_ref::k_lcd_decode_vbgr_glsl,
        },
    };
    ok &= check(
        std::size(lcd_ref::k_lcd_decode_thresholds) == std::size(expected_decodes),
        "LCD decode threshold manifest row count must match");
    for (std::size_t i = 0; i < std::size(expected_decodes); ++i) {
        const auto& row = lcd_ref::k_lcd_decode_thresholds[i];
        ok &= check(row.order == expected_decodes[i].order, "LCD decode row order must match");
        ok &= check(
            row.min_exclusive == expected_decodes[i].min_exclusive,
            "LCD decode row min must match");
        ok &= check(
            row.max_exclusive == expected_decodes[i].max_exclusive,
            "LCD decode row max must match");
        ok &= check(
            row.glsl_condition == expected_decodes[i].glsl_condition,
            "LCD decode row GLSL condition must match");
        ok &= check(
            decode_condition_matches(row.glsl_condition, row.min_exclusive, row.max_exclusive),
            "LCD decode row GLSL condition must match its numeric thresholds");
        const float uniform = lcd::shader_uniform_value(row.order);
        ok &= check(
            row.min_exclusive < uniform && uniform < row.max_exclusive,
            "LCD decode row must contain its shader uniform");
        ok &= check(
            near_equal(row.min_exclusive, uniform - 0.5f),
            "LCD decode row min must be centered around its shader uniform");
        ok &= check(
            near_equal(row.max_exclusive, uniform + 0.5f),
            "LCD decode row max must be centered around its shader uniform");
        if (i > 0) {
            ok &= check(
                near_equal(
                    lcd_ref::k_lcd_decode_thresholds[i - 1].max_exclusive,
                    row.min_exclusive),
                "LCD decode intervals must be contiguous");
        }
    }

    ok &= check(lcd_ref::k_lcd_filter_edge == 0.03125f, "LCD filter edge must match");
    ok &= check(lcd_ref::k_lcd_filter_side == 0.30078125f, "LCD filter side must match");
    ok &= check(lcd_ref::k_lcd_filter_center == 0.3359375f, "LCD filter center must match");
    ok &= check(
        lcd_ref::k_lcd_filter_edge_glsl == "0.03125",
        "edge filter GLSL literal must match");
    ok &= check(
        lcd_ref::k_lcd_filter_side_glsl == "0.30078125",
        "side filter GLSL literal must match");
    ok &= check(
        lcd_ref::k_lcd_filter_center_glsl == "0.3359375",
        "center filter GLSL literal must match");
    ok &= check(
        parses_to_float(lcd_ref::k_lcd_filter_edge_glsl, lcd_ref::k_lcd_filter_edge),
        "edge filter GLSL literal must parse to the filter constant");
    ok &= check(
        parses_to_float(lcd_ref::k_lcd_filter_side_glsl, lcd_ref::k_lcd_filter_side),
        "side filter GLSL literal must parse to the filter constant");
    ok &= check(
        parses_to_float(lcd_ref::k_lcd_filter_center_glsl, lcd_ref::k_lcd_filter_center),
        "center filter GLSL literal must parse to the filter constant");

    constexpr float expected_offsets[] = {
        -3.0f,
        -2.0f,
        -1.0f,
         0.0f,
         1.0f,
         2.0f,
         3.0f,
    };
    ok &= check(
        std::size(lcd_ref::k_lcd_tap_offsets) == std::size(expected_offsets),
        "LCD tap offset sequence size must match");
    for (std::size_t i = 0; i < std::size(expected_offsets); ++i) {
        ok &= check(
            lcd_ref::k_lcd_tap_offsets[i] == expected_offsets[i],
            "LCD tap offset sequence must match");
    }

    constexpr float expected_filter_weights[] = {
        lcd_ref::k_lcd_filter_edge,
        lcd_ref::k_lcd_filter_side,
        lcd_ref::k_lcd_filter_center,
        lcd_ref::k_lcd_filter_side,
        lcd_ref::k_lcd_filter_edge,
    };
    ok &= check(
        std::size(lcd_ref::k_lcd_filter_weights) == std::size(expected_filter_weights),
        "LCD filter weight sequence size must match");
    for (std::size_t i = 0; i < std::size(expected_filter_weights); ++i) {
        ok &= check(
            lcd_ref::k_lcd_filter_weights[i] == expected_filter_weights[i],
            "LCD filter weight sequence must match");
    }
    float weight_sum = 0.0f;
    for (float weight : lcd_ref::k_lcd_filter_weights) {
        weight_sum += weight;
    }
    ok &= check(near_equal(weight_sum, 1.0f), "LCD filter weights must sum to 1.0");

    constexpr std::string_view expected_window_names[] = {
        "first",
        "center",
        "last",
    };
    constexpr float expected_window_offsets[3][5] = {
        {-3.0f, -2.0f, -1.0f, 0.0f, 1.0f},
        {-2.0f, -1.0f,  0.0f, 1.0f, 2.0f},
        {-1.0f,  0.0f,  1.0f, 2.0f, 3.0f},
    };
    ok &= check(
        std::size(lcd_ref::k_lcd_filter_windows) == std::size(expected_window_names),
        "LCD filter window manifest row count must match");
    for (std::size_t window = 0; window < std::size(expected_window_names); ++window) {
        const auto& row = lcd_ref::k_lcd_filter_windows[window];
        ok &= check(
            row.channel_name == expected_window_names[window],
            "LCD filter window name must match");
        ok &= check(
            std::size(row.taps) == std::size(expected_filter_weights),
            "LCD filter window tap count must match");
        for (std::size_t tap = 0; tap < std::size(expected_filter_weights); ++tap) {
            ok &= check(
                row.taps[tap].offset == expected_window_offsets[window][tap],
                "LCD filter window tap offset must match");
            ok &= check(
                row.taps[tap].weight == expected_filter_weights[tap],
                "LCD filter window tap weight must match");
        }
        float window_weight_sum = 0.0f;
        for (const lcd_ref::filter_tap_t& tap : row.taps) {
            window_weight_sum += tap.weight;
        }
        ok &= check(
            near_equal(window_weight_sum, 1.0f),
            "LCD filter window weights must sum to 1.0");
    }

    ok &= check(
        lcd_ref::k_lcd_subpixel_divisor == 3.0f,
        "LCD subpixel divisor must be 3.0");
    ok &= check(
        lcd_ref::k_lcd_subpixel_divisor_glsl == "3.0",
        "LCD subpixel divisor GLSL literal must match");
    ok &= check(
        parses_to_float(lcd_ref::k_lcd_subpixel_divisor_glsl, lcd_ref::k_lcd_subpixel_divisor),
        "LCD subpixel divisor GLSL literal must parse to the divisor constant");
    ok &= check(
        lcd_ref::k_lcd_horizontal_step_glsl == "1.0 / (3.0 * frame_size.x)",
        "horizontal subpixel step literal must match");
    ok &= check(
        lcd_ref::k_lcd_vertical_step_glsl == "-1.0 / (3.0 * frame_size.y)",
        "vertical subpixel step literal must match");
    ok &= check(
        lcd_ref::k_lcd_opaque_alpha_cutoff == 0.999f,
        "opacity cutoff reference must match");
    ok &= check(
        lcd_ref::k_lcd_opaque_alpha_cutoff_glsl == "0.999",
        "opacity cutoff literal must match");
    ok &= check(
        parses_to_float(
            lcd_ref::k_lcd_opaque_alpha_cutoff_glsl,
            lcd_ref::k_lcd_opaque_alpha_cutoff),
        "opacity cutoff literal must parse to the cutoff constant");
    return ok;
}

bool run_test(const char* name, bool (*test)())
{
    try {
        if (test()) {
            std::cerr << "PASS: " << name << '\n';
            return true;
        }
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

int main()
{
    bool ok = true;
    ok &= run_test("lcd contract helpers", test_lcd_contract_helpers);
    ok &= run_test("lcd shader reference values", test_lcd_shader_reference_values);
    return ok ? 0 : 1;
}
