// Checks that the styled text fragment shader still spells the LCD filter the
// way vnm_msdf_text::lcd_shader_reference describes it.
//
// The reference is the oracle here: it already fixed the decode thresholds, the
// five-tap weights, the three channel windows, the seven tap offsets, and the
// third-of-a-pixel step before this component existed, and the live plot and
// timeline surfaces render through a shader bound to those same values. This
// test reads the shader source rather than the compiled artifact so a drift
// shows up as the statement that changed.
//
// It needs no Qt and no GPU: it is a text check on a text file.

#include <vnm_msdf_text/lcd_contract.h>
#include <vnm_msdf_text/lcd_shader_reference.h>

#include <cctype>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lcd = vnm::msdf_text::lcd;
namespace ref = vnm::msdf_text::lcd::shader_reference;

namespace {

using token_list_t = std::vector<std::string>;

bool check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

std::string read_text_file(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool is_identifier_start(unsigned char ch) { return std::isalpha(ch) || ch == '_'; }
bool is_identifier_char(unsigned char ch)  { return std::isalnum(ch) || ch == '_'; }

bool is_two_char_operator(std::string_view text)
{
    return
        text == "&&" || text == "||" || text == "<=" || text == ">=" ||
        text == "==" || text == "!=" || text == "+=" || text == "-=" ||
        text == "*=" || text == "/=";
}

/// Splits GLSL into identifiers, numbers, and punctuation, dropping comments
/// and whitespace, so a match compares statements rather than formatting.
token_list_t tokenize_glsl(std::string_view text)
{
    token_list_t tokens;

    for (std::size_t pos = 0; pos < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[pos]);
        if (std::isspace(ch)) {
            ++pos;
            continue;
        }

        if (text[pos] == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            pos += 2;
            while (pos < text.size() && text[pos] != '\n') {
                ++pos;
            }
            continue;
        }

        if (text[pos] == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < text.size() && !(text[pos] == '*' && text[pos + 1] == '/')) {
                ++pos;
            }
            pos = pos + 1 < text.size() ? pos + 2 : text.size();
            continue;
        }

        if (is_identifier_start(ch)) {
            const std::size_t start = pos++;
            while (pos < text.size() &&
                is_identifier_char(static_cast<unsigned char>(text[pos])))
            {
                ++pos;
            }
            tokens.emplace_back(text.substr(start, pos - start));
            continue;
        }

        if (std::isdigit(ch) ||
            (text[pos] == '.' && pos + 1 < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[pos + 1]))))
        {
            const std::size_t start = pos++;
            while (pos < text.size()) {
                const unsigned char next = static_cast<unsigned char>(text[pos]);
                if (std::isdigit(next) || text[pos] == '.') {
                    ++pos;
                    continue;
                }
                if (text[pos] == 'e' || text[pos] == 'E') {
                    ++pos;
                    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
                        ++pos;
                    }
                    continue;
                }
                break;
            }
            tokens.emplace_back(text.substr(start, pos - start));
            continue;
        }

        if (pos + 1 < text.size() && is_two_char_operator(text.substr(pos, 2))) {
            tokens.emplace_back(text.substr(pos, 2));
            pos += 2;
            continue;
        }

        tokens.emplace_back(1, static_cast<char>(ch));
        ++pos;
    }

    return tokens;
}

bool contains_statement(const token_list_t& shader, std::string_view statement)
{
    const token_list_t expected = tokenize_glsl(statement);
    if (expected.empty() || shader.size() < expected.size()) {
        return false;
    }

    for (std::size_t start = 0; start + expected.size() <= shader.size(); ++start) {
        bool matches = true;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (shader[start + i] != expected[i]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }

    return false;
}

std::string sample_name_for_offset(float offset)
{
    return "sample_" + std::to_string(static_cast<int>(offset + 3.0f));
}

std::string sample_expression_for_offset(float offset)
{
    if (offset == -3.0f) { return "glyph_ratio - subpixel_step * 3.0"; }
    if (offset == -2.0f) { return "glyph_ratio - subpixel_step * 2.0"; }
    if (offset == -1.0f) { return "glyph_ratio - subpixel_step";       }
    if (offset ==  0.0f) { return "glyph_ratio";                       }
    if (offset ==  1.0f) { return "glyph_ratio + subpixel_step";       }
    if (offset ==  2.0f) { return "glyph_ratio + subpixel_step * 2.0"; }
    if (offset ==  3.0f) { return "glyph_ratio + subpixel_step * 3.0"; }
    return {};
}

std::string weight_name_for_value(float weight)
{
    if (weight == ref::k_lcd_filter_edge)   { return "filter_edge";   }
    if (weight == ref::k_lcd_filter_side)   { return "filter_side";   }
    if (weight == ref::k_lcd_filter_center) { return "filter_center"; }
    return {};
}

std::string order_bool_name(lcd::Resolved_lcd_subpixel_order order)
{
    switch (order) {
        case lcd::Resolved_lcd_subpixel_order::RGB:  return "lcd_rgb";
        case lcd::Resolved_lcd_subpixel_order::BGR:  return "lcd_bgr";
        case lcd::Resolved_lcd_subpixel_order::VRGB: return "lcd_vrgb";
        case lcd::Resolved_lcd_subpixel_order::VBGR: return "lcd_vbgr";
        case lcd::Resolved_lcd_subpixel_order::NONE: break;
        default:                                    break;
    }

    return {};
}

std::string decode_threshold_statement(const ref::decode_threshold_t& threshold)
{
    const std::string condition(threshold.glsl_condition);
    const std::string separator     = " && ";
    const std::size_t separator_pos = condition.find(separator);
    const std::string bool_name     = order_bool_name(threshold.order);
    if (separator_pos == std::string::npos || bool_name.empty()) {
        return {};
    }

    return
        "bool " + bool_name + " = u.lcd_subpixel_order " +
        condition.substr(0, separator_pos) +
        " && u.lcd_subpixel_order " +
        condition.substr(separator_pos + separator.size()) + ";";
}

std::string filter_window_statement(const ref::filter_window_t& window)
{
    std::string statement = "float " + std::string(window.channel_name) + "_coverage =";

    for (std::size_t i = 0; i < window.taps.size(); ++i) {
        const std::string weight_name = weight_name_for_value(window.taps[i].weight);
        if (weight_name.empty()) {
            return {};
        }

        statement += " " + sample_name_for_offset(window.taps[i].offset) + " * " + weight_name;
        statement += (i + 1 == window.taps.size()) ? ";" : " +";
    }

    return statement;
}

std::string subpixel_step_statement()
{
    return
        "vec2 subpixel_step = lcd_horizontal ? vec2(" +
        std::string(ref::k_lcd_horizontal_step_glsl) + ", 0.0) : vec2(0.0, " +
        std::string(ref::k_lcd_vertical_step_glsl) + ");";
}

bool test_resolved_orders_match_the_shader_reference()
{
    struct case_t
    {
        lcd::Resolved_lcd_subpixel_order order;
        int                              value;
        float                            uniform;
    };

    constexpr case_t cases[] = {
        { lcd::Resolved_lcd_subpixel_order::NONE, ref::k_lcd_order_none_value, ref::k_lcd_order_none_uniform },
        { lcd::Resolved_lcd_subpixel_order::RGB,  ref::k_lcd_order_rgb_value,  ref::k_lcd_order_rgb_uniform  },
        { lcd::Resolved_lcd_subpixel_order::BGR,  ref::k_lcd_order_bgr_value,  ref::k_lcd_order_bgr_uniform  },
        { lcd::Resolved_lcd_subpixel_order::VRGB, ref::k_lcd_order_vrgb_value, ref::k_lcd_order_vrgb_uniform },
        { lcd::Resolved_lcd_subpixel_order::VBGR, ref::k_lcd_order_vbgr_value, ref::k_lcd_order_vbgr_uniform },
    };

    bool ok = true;
    for (const case_t& item : cases) {
        ok &= check(lcd::resolved_order_value(item.order) == item.value,
            "the resolved LCD order value must match the shader reference");
        ok &= check(lcd::shader_uniform_value(item.order) == item.uniform,
            "the LCD order uniform value must match the shader reference");
    }
    return ok;
}

bool test_styled_shader_binds_the_reference_filter()
{
    const std::string shader = read_text_file(VNM_MSDF_TEXT_STYLED_FRAG_PATH);
    if (!check(!shader.empty(), "the styled fragment shader source must be readable")) {
        return false;
    }
    const token_list_t tokens = tokenize_glsl(shader);

    bool ok = true;
    for (const ref::decode_threshold_t& threshold : ref::k_lcd_decode_thresholds) {
        const std::string statement = decode_threshold_statement(threshold);
        ok &= check(!statement.empty(),
            "every reference decode threshold must name a shader boolean");
        ok &= check(contains_statement(tokens, statement),
            "the shader must decode each subpixel order at the reference thresholds");
        ok &= check(
            lcd::shader_uniform_value(threshold.order) > threshold.min_exclusive &&
            lcd::shader_uniform_value(threshold.order) < threshold.max_exclusive,
            "each decode threshold must contain its own uniform value");
    }

    ok &= check(
        contains_statement(tokens, "float filter_edge = " + std::string(ref::k_lcd_filter_edge_glsl) + ";"),
        "the shader's edge filter weight must be the reference literal");
    ok &= check(
        contains_statement(tokens, "float filter_side = " + std::string(ref::k_lcd_filter_side_glsl) + ";"),
        "the shader's side filter weight must be the reference literal");
    ok &= check(
        contains_statement(tokens, "float filter_center = " + std::string(ref::k_lcd_filter_center_glsl) + ";"),
        "the shader's centre filter weight must be the reference literal");

    for (const ref::filter_window_t& window : ref::k_lcd_filter_windows) {
        const std::string statement = filter_window_statement(window);
        ok &= check(!statement.empty(),
            "every reference filter window must map onto shader symbols");
        ok &= check(contains_statement(tokens, statement),
            "each channel's coverage must weight the reference taps");
    }

    for (float offset : ref::k_lcd_tap_offsets) {
        const std::string expression = sample_expression_for_offset(offset);
        ok &= check(!expression.empty(),
            "every reference tap offset must map onto a shader expression");
        ok &= check(
            contains_statement(
                tokens,
                "float " + sample_name_for_offset(offset) + " = glyph_alpha_at_ratio(" +
                    expression + ", uv_min, uv_max);"),
            "each tap must sample at its reference offset");
    }

    ok &= check(contains_statement(tokens, subpixel_step_statement()),
        "the subpixel step must be a third of the glyph frame in the filtered axis");
    ok &= check(contains_statement(tokens, "bool lcd_horizontal = lcd_rgb || lcd_bgr;"),
        "the horizontal orders must be RGB and BGR");
    ok &= check(contains_statement(tokens, "bool lcd_vertical = lcd_vrgb || lcd_vbgr;"),
        "the vertical orders must be VRGB and VBGR");
    ok &= check(contains_statement(tokens, "bool forward_order = lcd_rgb || lcd_vrgb;"),
        "RGB and VRGB must be the forward channel orders");
    ok &= check(
        contains_statement(
            tokens,
            "return forward_order"
            " ? vec3(first_coverage, center_coverage, last_coverage)"
            " : vec3(last_coverage, center_coverage, first_coverage);"),
        "a reverse order must reverse the channel windows and nothing else");

    ok &= check(
        contains_statement(
            tokens,
            "if (u.sdf_mask_enabled != 0) {"
            "float sd_sdf = (texel.a - 0.5) * u.px_range;"
            "alpha = min(alpha, clamp(sd_sdf + 0.5, 0.0, 1.0));"
            "}"),
        "true-SDF masking must decode alpha for every MTSDF texel");
    ok &= check(
        contains_statement(tokens, "return (texel.a - 0.5) * u.px_range;"),
        "glow distance must always come from the MTSDF alpha channel");
    ok &= check(
        !contains_statement(tokens, "texel.a > 0.0"),
        "zero is a valid true-SDF value, never a request to fall back to RGB");

    return ok;
}

bool run_test(const char* name, bool (*test)())
{
    const bool ok = test();
    std::cout << (ok ? "PASS: " : "FAIL: ") << name << '\n';
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= run_test("resolved orders match the shader reference",
        test_resolved_orders_match_the_shader_reference);
    ok &= run_test("styled shader binds the reference filter",
        test_styled_shader_binds_the_reference_filter);

    std::cerr << (ok ? "PASS" : "FAIL") << ": styled shader reference\n";
    return ok ? 0 : 1;
}
