# vnm_msdf_text

[![CI Linux](https://github.com/Varinomics/vnm_msdf_text/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_msdf_text/actions/workflows/ci-linux.yml) [![CI macOS](https://github.com/Varinomics/vnm_msdf_text/actions/workflows/ci-macos.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_msdf_text/actions/workflows/ci-macos.yml) [![CI Windows](https://github.com/Varinomics/vnm_msdf_text/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_msdf_text/actions/workflows/ci-windows.yml) [![CI FreeBSD](https://github.com/Varinomics/vnm_msdf_text/actions/workflows/ci-freebsd.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_msdf_text/actions/workflows/ci-freebsd.yml)

A C++ library for building MSDF font atlases and laying out text from
caller-supplied font bytes. It provides scale-independent glyph data,
measurements and indexed quads for custom renderers, plus an optional Qt QRhi
renderer. Varinomics plotting and editor components share it.

Layout is single-line, left-to-right codepoint layout with optional kerning.
It does not perform shaping or bidirectional reordering. Qt is optional; the
CPU atlas uses FreeType and msdfgen.

## Choose a component

| Use | CMake target | Installed component |
| --- | --- | --- |
| Build atlases and CPU text geometry | `vnm_msdf_text::vnm_msdf_text` | `atlas` |
| Draw text with Qt QRhi | `vnm_msdf_text::rhi` | `rhi` |
| Share LCD order and mapping helpers | `vnm_msdf_text::lcd_contract` | `lcd_contract` |
| Check shared LCD shader constants | `vnm_msdf_text::lcd_shader_reference` | `lcd_shader_reference` |
| Resolve LCD order for Qt screens | `vnm_msdf_text::qt_lcd` | `qt_lcd` |

The LCD contract and shader-reference targets need neither FreeType, msdfgen
nor Qt. The Qt components are off by default.

## Get and build

The atlas and QRhi components require C++20. Source builds require CMake 3.22
or newer.

```sh
git clone https://github.com/Varinomics/vnm_msdf_text.git
cd vnm_msdf_text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

A top-level build fetches FreeType and msdfgen when compatible targets are
unavailable. See [building and testing](https://github.com/Varinomics/vnm_msdf_text/blob/master/docs/building.md)
for system dependencies, offline builds, optional components and installation.

## Integrate with CMake

With the checkout at `external/vnm_msdf_text` in a parent project:

```cmake
set(VNM_MSDF_TEXT_FETCH_DEPS ON CACHE BOOL "Fetch missing atlas dependencies")
add_subdirectory(external/vnm_msdf_text)
target_link_libraries(app PRIVATE vnm_msdf_text::vnm_msdf_text)
```

Set `VNM_MSDF_TEXT_FETCH_DEPS` explicitly in parent projects; use `OFF` when the
parent supplies FreeType and msdfgen. The library can also be added through
CMake `FetchContent`.

For an installed package that includes the atlas component:

```cmake
find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS atlas)
target_link_libraries(app PRIVATE vnm_msdf_text::vnm_msdf_text)
```

Request the component you need from the table above. Enable
`VNM_MSDF_TEXT_BUILD_RHI=ON` in source builds for GPU text; the
[QRhi usage guide](https://github.com/Varinomics/vnm_msdf_text/blob/master/docs/rhi_usage.md)
covers font snapshots, batching and the render-frame sequence.

## Build an atlas and lay out text

Load your font into a `std::vector<std::uint8_t> font_bytes`, then build an
atlas for the text you intend to draw:

```cpp
#include <vnm_msdf_text/msdf_text.h>

namespace text = vnm::msdf_text;

const std::string_view label = "Plot title";
const auto codepoints = text::utf8_to_codepoints(label);
const auto result = text::build_font_atlas(
    font_bytes.data(), font_bytes.size(), 24, codepoints);

if (result.status != text::Build_status::FAILURE)
{
    std::vector<text::text_vertex_t> vertices;
    std::vector<std::uint32_t> indices;
    text::append_text_quads(
        result.atlas, 24, label, 12.0f, 36.0f, vertices, &indices);
}
```

The quads use a baseline origin in output pixels. Upload the atlas as linear
RGBA8 distance data and render the geometry with a matching MSDF shader.
The [atlas usage guide](https://github.com/Varinomics/vnm_msdf_text/blob/master/docs/atlas_usage.md)
covers coverage diagnostics, scaling, measurements, texture coordinates and a
shader example.

## References and licensing

- [CPU atlas and layout](https://github.com/Varinomics/vnm_msdf_text/blob/master/docs/atlas_usage.md)
- [QRhi text rendering](https://github.com/Varinomics/vnm_msdf_text/blob/master/docs/rhi_usage.md)
- [LCD/MSDF contract and filter reference](https://github.com/Varinomics/vnm_msdf_text/blob/master/docs/lcd_msdf_commonization_contract.md)
- [Building, testing and package consumers](https://github.com/Varinomics/vnm_msdf_text/blob/master/docs/building.md)
- [Public headers](https://github.com/Varinomics/vnm_msdf_text/tree/master/include/vnm_msdf_text)

The source is licensed under the [BSD 2-Clause License](https://github.com/Varinomics/vnm_msdf_text/blob/master/LICENSE.txt).
This repository redistributes no third-party asset. Tests obtain their font from
[vnm_fonts](https://github.com/Varinomics/vnm_fonts), which carries the Ubuntu
Font Licence 1.0 text and notice. See the
[third-party notices](https://github.com/Varinomics/vnm_msdf_text/blob/master/THIRD_PARTY_NOTICES.md).
