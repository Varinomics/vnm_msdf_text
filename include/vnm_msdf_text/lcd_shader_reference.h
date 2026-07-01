#pragma once

#include <vnm_msdf_text/lcd_contract.h>

#include <array>
#include <string_view>

namespace vnm::msdf_text::lcd::shader_reference {

struct decode_threshold_t
{
    Resolved_lcd_subpixel_order order = Resolved_lcd_subpixel_order::NONE;
    float min_exclusive = 0.0f;
    float max_exclusive = 0.0f;
    std::string_view glsl_condition;
};

struct filter_tap_t
{
    float offset = 0.0f;
    float weight = 0.0f;
};

struct filter_window_t
{
    std::string_view channel_name;
    std::array<filter_tap_t, 5> taps;
};

constexpr int k_lcd_order_none_value = 0;
constexpr int k_lcd_order_rgb_value  = 1;
constexpr int k_lcd_order_bgr_value  = 2;
constexpr int k_lcd_order_vrgb_value = 3;
constexpr int k_lcd_order_vbgr_value = 4;

constexpr float k_lcd_order_none_uniform = 0.0f;
constexpr float k_lcd_order_rgb_uniform  = 1.0f;
constexpr float k_lcd_order_bgr_uniform  = 2.0f;
constexpr float k_lcd_order_vrgb_uniform = 3.0f;
constexpr float k_lcd_order_vbgr_uniform = 4.0f;

constexpr float k_lcd_decode_rgb_min  = 0.5f;
constexpr float k_lcd_decode_rgb_max  = 1.5f;
constexpr float k_lcd_decode_bgr_min  = 1.5f;
constexpr float k_lcd_decode_bgr_max  = 2.5f;
constexpr float k_lcd_decode_vrgb_min = 2.5f;
constexpr float k_lcd_decode_vrgb_max = 3.5f;
constexpr float k_lcd_decode_vbgr_min = 3.5f;
constexpr float k_lcd_decode_vbgr_max = 4.5f;

constexpr std::string_view k_lcd_decode_rgb_glsl  = "> 0.5 && < 1.5";
constexpr std::string_view k_lcd_decode_bgr_glsl  = "> 1.5 && < 2.5";
constexpr std::string_view k_lcd_decode_vrgb_glsl = "> 2.5 && < 3.5";
constexpr std::string_view k_lcd_decode_vbgr_glsl = "> 3.5 && < 4.5";

constexpr float k_lcd_filter_edge   = 0.03125f;
constexpr float k_lcd_filter_side   = 0.30078125f;
constexpr float k_lcd_filter_center = 0.3359375f;

constexpr std::string_view k_lcd_filter_edge_glsl   = "0.03125";
constexpr std::string_view k_lcd_filter_side_glsl   = "0.30078125";
constexpr std::string_view k_lcd_filter_center_glsl = "0.3359375";

constexpr float k_lcd_subpixel_divisor = 3.0f;
constexpr std::string_view k_lcd_subpixel_divisor_glsl = "3.0";
constexpr std::string_view k_lcd_horizontal_step_glsl = "1.0 / (3.0 * frame_size.x)";
constexpr std::string_view k_lcd_vertical_step_glsl = "-1.0 / (3.0 * frame_size.y)";

constexpr float k_lcd_opaque_alpha_cutoff = 0.999f;
constexpr std::string_view k_lcd_opaque_alpha_cutoff_glsl = "0.999";

constexpr std::array<float, 7> k_lcd_tap_offsets = {
    -3.0f,
    -2.0f,
    -1.0f,
     0.0f,
     1.0f,
     2.0f,
     3.0f,
};

constexpr std::array<float, 5> k_lcd_filter_weights = {
    k_lcd_filter_edge,
    k_lcd_filter_side,
    k_lcd_filter_center,
    k_lcd_filter_side,
    k_lcd_filter_edge,
};

constexpr std::array<decode_threshold_t, 4> k_lcd_decode_thresholds = {{
    {
        Resolved_lcd_subpixel_order::RGB,
        k_lcd_decode_rgb_min,
        k_lcd_decode_rgb_max,
        k_lcd_decode_rgb_glsl,
    },
    {
        Resolved_lcd_subpixel_order::BGR,
        k_lcd_decode_bgr_min,
        k_lcd_decode_bgr_max,
        k_lcd_decode_bgr_glsl,
    },
    {
        Resolved_lcd_subpixel_order::VRGB,
        k_lcd_decode_vrgb_min,
        k_lcd_decode_vrgb_max,
        k_lcd_decode_vrgb_glsl,
    },
    {
        Resolved_lcd_subpixel_order::VBGR,
        k_lcd_decode_vbgr_min,
        k_lcd_decode_vbgr_max,
        k_lcd_decode_vbgr_glsl,
    },
}};

constexpr std::array<filter_window_t, 3> k_lcd_filter_windows = {{
    {
        "first",
        {{
            {-3.0f, k_lcd_filter_edge},
            {-2.0f, k_lcd_filter_side},
            {-1.0f, k_lcd_filter_center},
            { 0.0f, k_lcd_filter_side},
            { 1.0f, k_lcd_filter_edge},
        }},
    },
    {
        "center",
        {{
            {-2.0f, k_lcd_filter_edge},
            {-1.0f, k_lcd_filter_side},
            { 0.0f, k_lcd_filter_center},
            { 1.0f, k_lcd_filter_side},
            { 2.0f, k_lcd_filter_edge},
        }},
    },
    {
        "last",
        {{
            {-1.0f, k_lcd_filter_edge},
            { 0.0f, k_lcd_filter_side},
            { 1.0f, k_lcd_filter_center},
            { 2.0f, k_lcd_filter_side},
            { 3.0f, k_lcd_filter_edge},
        }},
    },
}};

} // namespace vnm::msdf_text::lcd::shader_reference
