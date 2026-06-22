# vnm_msdf_text

[![CI Linux](https://github.com/imakris/vnm_msdf_text/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/imakris/vnm_msdf_text/actions/workflows/ci-linux.yml) [![CI macOS](https://github.com/imakris/vnm_msdf_text/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/imakris/vnm_msdf_text/actions/workflows/ci-macos.yml) [![CI Windows](https://github.com/imakris/vnm_msdf_text/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/imakris/vnm_msdf_text/actions/workflows/ci-windows.yml) [![CI FreeBSD](https://github.com/imakris/vnm_msdf_text/actions/workflows/ci-freebsd.yml/badge.svg?branch=master)](https://github.com/imakris/vnm_msdf_text/actions/workflows/ci-freebsd.yml)

Small CPU-side MSDF text atlas builder shared by Varinomics plotting and editor
components.

The library builds a static C++ target:

```cmake
vnm_msdf_text::vnm_msdf_text
```

It uses FreeType and msdfgen. When configured as the top-level project, CMake
fetches those dependencies when compatible targets are not already available.
Parent projects should set `VNM_MSDF_TEXT_FETCH_DEPS` explicitly.

## Licensing

The source code is licensed under the BSD 2-Clause License. The bundled
`fonts/monospace.ttf` font is licensed separately under the Ubuntu Font Licence
1.0; see `THIRD_PARTY_NOTICES.md`.

## API contract

`build_font_atlas` builds a row-major linear RGBA8 MTSDF atlas from font bytes
and a requested set of Unicode scalar values. Requested codepoints are validated,
sorted, and deduplicated before glyph generation.

### Scale-independent atlas

The baked bitmap is generated at `msdf_bake_pixel_height(draw_pixel_height,
options)`, which is the requested `draw_pixel_height` clamped up to
`ceil(options_t::min_atlas_font_size)`. Glyph geometry, kerning, and font metrics
are stored in scale-independent font units (`glyph_t::bounds_*_units`,
`glyph_t::advance_units`, `kerning_units`, `font_metrics_units`), not in output
pixels. A single baked atlas therefore serves a range of draw pixel heights: two
requested heights that share a bake bucket produce a byte-identical bitmap and
identical glyph UVs, while their draw-size geometry differs.

Convert baked data to a specific draw pixel height with the scaling helpers:

- `scaled_glyph(atlas, glyph, draw_pixel_height)` returns a `scaled_glyph_t` with
  the output-pixel `advance_x`, baseline-relative `plane_*` rectangle, and UVs.
  Non-visible glyphs (for example U+0020) scale to a degenerate zero-area plane
  while still carrying `advance_x`.
- `px_range_for_pixel_height` is the shader distance range in output pixels,
  including `sharpness_bias`.
- `scaled_font_metrics` returns ascender, descender, line height, and em size in
  output pixels.

The layout and measurement entry points (`measure_text_advance_px`,
`for_each_positioned_glyph`, `measure_text_bounds_px`, `append_text_quads`) each
take a `draw_pixel_height` and apply this scaling internally.

Build results use `Build_status`:

- `SUCCESS`: every valid requested codepoint was emitted.
- `PARTIAL_SUCCESS`: the atlas contains at least one emitted glyph, and the
  diagnostic vectors describe skipped codepoints.
- `FAILURE`: no usable atlas was produced. The returned atlas is
  default-constructed and must not be rendered.

Diagnostics are split into invalid Unicode scalar values, missing font coverage,
glyph load failures, glyphs too large for the atlas, and glyphs skipped after
the atlas ran out of space.

`options_t::missing_glyph_policy` controls missing requested codepoints:

- `SKIP`: report missing coverage and omit those codepoints from the atlas.
- `USE_REPLACEMENT_CHARACTER`: when the font contains U+FFFD, alias missing
  codepoints to that replacement glyph while still reporting the original
  missing codepoints.
- `FAIL_BUILD`: fail the build when any valid requested codepoint is missing.

`atlas_t::font_metrics_units` exposes ascender, descender, line height, and em
size in font units; `scaled_font_metrics(atlas, draw_pixel_height)` returns the
same metrics in output pixels. A baseline-to-descender-bottom offset for a draw
height is `-scaled_font_metrics(atlas, draw_pixel_height).descender`.
`atlas_t::zero_advance_units` is the font-unit advance of glyph `0` when
available; it is a reference advance, not proof that the font is monospace.

`default_codepoints()` is a UI-oriented scalar set covering printable ASCII,
selected Latin, Greek, currency, and UI symbol codepoints. It includes U+FFFD so
callers can request a replacement glyph for invalid UTF-8 fallback. The bundled
font is a test fixture and license-noticed convenience asset; it does not cover
every codepoint in this default set.

## Layout and rendering

`append_text_quads` treats `x, y` as the baseline origin in output pixels. The
layout convention is screen-style Y-down coordinates. Glyph plane coordinates
are relative to the baseline; for normal visible glyphs, `plane_bottom` is the
smaller Y value and `plane_top` is the larger Y value after converting the
font's Y-up outline space.

Atlas data is row-major RGBA8. Row 0 is the first row in memory, and the UV `t`
coordinate increases downward to match that layout. Upload the texture as
linear data, not sRGB.

`options_t::atlas_px_range` is the baked distance range in atlas pixels, retained
on the atlas as `atlas_t::atlas_px_range`. The output-pixel distance range
intended for shader reconstruction is produced per draw height by
`px_range_for_pixel_height(atlas, draw_pixel_height)`, which applies the draw
scaling and `sharpness_bias`. `options_t::atlas_gutter_px` controls empty pixels
between packed glyph bitmaps. Shaders should still clamp sampling to the glyph UV
rectangle when using linear filtering.

A minimal OpenGL-style renderer setup looks like:

```cpp
// Linear RGBA8 upload; do not use an sRGB internal format for distance data.
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas.atlas_size, atlas.atlas_size,
             0, GL_RGBA, GL_UNSIGNED_BYTE, atlas.rgba.data());
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

const float shader_px_range =
    vnm::msdf_text::px_range_for_pixel_height(atlas, draw_pixel_height);

set_uniform("u_px_range", shader_px_range);
// atlas.atlas_px_range is the baked atlas-space range; shader_px_range is the
// draw-size output-pixel range used for coverage reconstruction below.
```

```glsl
// Inputs from text_vertex_t: v_uv = (s, t).
// v_uv_bounds = (s_min, t_min, s_max, t_max).
float median3(vec3 v) {
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

vec2 uv = clamp(v_uv, v_uv_bounds.xy, v_uv_bounds.zw);
vec4 mtsdf = texture(u_atlas, uv);
float signed_distance = median3(mtsdf.rgb) - 0.5;
float coverage = clamp(signed_distance * u_px_range + 0.5, 0.0, 1.0);
out_color = vec4(text_color.rgb, text_color.a * coverage);
```

Text layout is single-line, left-to-right codepoint layout with optional
kerning. It does not perform HarfBuzz shaping, bidirectional reordering,
ligature substitution, grapheme-cluster handling, or combining-mark placement.
`measure_text_advance_px` returns pen advance, not visual bounds. Invalid UTF-8
decodes to U+FFFD; missing glyphs are skipped during layout unless the atlas
contains a glyph entry for that decoded codepoint.

For custom renderers, `for_each_positioned_glyph` exposes the same decoded
layout stream used by `measure_text_advance_px`, `measure_text_bounds_px`, and
`append_text_quads`. `append_text_quads` can throw `std::length_error` if indexed
output would exceed `uint32_t` capacity; vector growth may throw allocation
exceptions.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To require system or pre-provided FreeType/msdfgen targets without network
fetching:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVNM_MSDF_TEXT_FETCH_DEPS=OFF
cmake --build build
```

For offline builds with fetched dependencies already checked out, pass
`FETCHCONTENT_SOURCE_DIR_FREETYPE` and `FETCHCONTENT_SOURCE_DIR_MSDFGEN`, or set
`VNM_MSDF_TEXT_DEP_OVERRIDES_FILE` to a CMake file that defines those cache
entries.

## Tests

When this project is configured as the top-level CMake project, tests are built
by default:

```bash
cmake --build build --target vnm_msdf_text_tests
ctest --test-dir build --output-on-failure
```

Set `-DVNM_MSDF_TEXT_BUILD_TESTS=OFF` to skip the test executable.
