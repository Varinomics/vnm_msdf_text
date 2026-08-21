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

An opt-in Qt QRhi text component builds alongside it when a consumer asks for
it, adding the shared GPU text substrate described under
[QRhi text component](#qrhi-text-component):

```cmake
vnm_msdf_text::rhi
```

The package also exports dependency-light LCD/MSDF support targets:

```cmake
vnm_msdf_text::lcd_contract
vnm_msdf_text::lcd_shader_reference
```

Use `find_package(vnm_msdf_text CONFIG COMPONENTS lcd_contract)` or
`lcd_shader_reference` when a consumer only needs the resolved LCD enum, mapping
helpers, or shader drift-reference constants. Request `COMPONENTS atlas` for the
full atlas-builder target and its FreeType/msdfgen dependencies. A no-component
package lookup succeeds with the dependency-light LCD targets when the installed
package does not export the atlas target. See
`docs/lcd_msdf_commonization_contract.md` for the LCD contract.
Versioned package lookup requires the exact current project version.

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

## QRhi text component

`vnm_msdf_text::rhi` is the shared GPU substrate that draws the atlas above with
Qt's QRhi. It is a compositional layer over the CPU API: it owns device-local
texture, buffer, sampler, pipeline, and binding resources, the per-frame upload
and recording, and the status of both. Where text goes, what it says, how it is
coloured, and when it is worth drawing stay with the consumer.

The component is off by default. Configure with `-DVNM_MSDF_TEXT_BUILD_RHI=ON`
to build it; Qt is located only when that option is on, so a CPU-only or
editor-style consumer neither finds nor links Qt through this project. A
consumer that requires GPU text can read the `VNM_MSDF_TEXT_HAS_RHI` cache
entry and fail its own configure instead of linking a build without it. The
component requires C++20, Qt 6.7 or newer, and Qt Shader Tools at build time
only.

It is a source-tree target, consumed through `add_subdirectory` or
`FetchContent` and linked as `vnm_msdf_text::rhi`. It is not part of the
installed package: the installed tree contains neither an RHI target nor the
`vnm_msdf_text/rhi/` headers, and `find_package(vnm_msdf_text COMPONENTS rhi)`
is rejected as an unsupported component. Installing headers a package cannot
satisfy would be a contract the package does not have.

Public headers live under `vnm_msdf_text/rhi/` and everything is in namespace
`vnm::msdf_text::rhi`. The public headers only forward-declare the QRhi types
they take pointers to, so Qt's private QRhi headers stay inside the
implementation.

### Font snapshots

`build_font_snapshot(font_bytes, draw_pixel_height, codepoints, options)` builds
an immutable `Font_snapshot` from caller-supplied bytes. The component never
reads a file or resolves an asset: font acquisition belongs to the consumer.

The result carries a status and, on success, a `shared_ptr` to the snapshot:

- `INVALID_ARGUMENT` for empty bytes or a non-positive draw pixel height.
- `FONT_BUILD_FAILED` when no usable atlas could be produced; no snapshot.
- `OK` otherwise. A partially built atlas is an `OK` result with a usable
  snapshot whose `build_result().status` is `PARTIAL_SUCCESS` and whose
  diagnostic vectors name the codepoints that were not emitted.

A snapshot exposes the CPU data it was built from rather than restating it:
`atlas()`, `draw_pixel_height()`, and the full `build_result()`. Measurement,
bounds, positioned glyphs, and quad emission are the existing free functions in
`msdf_text.h` applied to those two values, so there is one set of metrics.

`identity()` is a digest of every input that determines the bake: the font
bytes, the draw pixel height, the atlas options, and the requested codepoints.
Two snapshots with equal identity measure identically and emit identical quads,
so a consumer can key cached CPU measurements or a presentation key on it and
keep them across a rebuild.

`revision()` distinguishes snapshot instances that share an identity, so a
consumer can tell that state it derived from a snapshot came from an older one.
Revisions are unique and increasing among the snapshots built by one loaded copy
of this static library; two modules that each link it count independently, so a
revision compares snapshots from one producer and does not identify a snapshot
across a module boundary. Compare content with `identity()`, and decide whether
a device resource built from a snapshot can be reused from the retained snapshot
object, which is what `Text_renderer` does.

### Batches and draw states

A `Text_batch` holds quads and the identity of the font they were laid out
against. `append_run(font, text, baseline_x, baseline_y)` emits them through
`append_text_quads`, so a batch always agrees with the measurement helpers;
`append_quads(font, vertices, indices)` accepts geometry a consumer produced
itself and rebases the indices. A batch holds geometry from one font only, and a
renderer rejects a batch whose font is not its own. Because a batch needs
nothing but an immutable snapshot, CPU preparation can build one away from the
render thread and hand it over when it is complete.

A `draw_state_t` carries the column-major transform from the batch's own
coordinates to clip space, a straight (non-premultiplied) RGBA colour, an
optional scissor rectangle, and the optional draw capabilities below.
`pixel_ortho_transform(frame)` builds the transform for text laid out in
top-left-origin framebuffer pixels, including the backend's clip-space
correction.

`clip_rect_t` is in framebuffer pixels with `x`, `y` at the bottom-left corner.
QRhi takes OpenGL-style scissor coordinates on every backend and itself flips
them for the backends whose native origin is the top-left, so a caller uses one
convention everywhere - and it is the opposite of the Y-down pixel space the
text itself is laid out in. The real-backend gate pins that origin rather than
asserting it: it clips the lower and the upper half of a target the text crosses
and checks which half survives. Negative `x` or `y` and partially out-of-bounds
rectangles are clamped by QRhi, and a zero width or height clips the draw away.
A negative width or height is not a rectangle QRhi can express -
it drops such a scissor and leaves the previous one in force - so `queue()`
rejects an enabled clip with one instead of recording a draw under someone
else's clip.

### Optional draw capabilities

`draw_capabilities.h` adds four optional capabilities. Each is a presence-tagged
record with a version of its own, and each is absent by default. A draw state
that carries none of them takes the base path described above: the same
geometry, uniform block, shader, and pipeline a build without that header uses.
A draw state that carries any of them is a *styled* draw and takes a second
path, created only once a frame queues one.

| Capability | Record | What it does |
| --- | --- | --- |
| Per-glyph frame rectangles | `glyph_frames_t` on `Text_batch` | Records each quad's rectangle so the fragment stage can measure in output pixels |
| LCD subpixel order | `lcd_style_t` on `draw_state_t` | Filters coverage per channel and composes it against an opaque background |
| Outer glow | `glow_style_t` on `draw_state_t` | Ramps coverage outwards from the glyph outline |
| True-SDF alpha masking | `sdf_mask_t` on `draw_state_t` | Takes the smaller of the multi-channel and true-SDF coverages |

The provider supplies rendering semantics only. Which text gets a subpixel
order, a glow, or a mask, what background it sits on, and when any of that is
worth doing stay with the consumer.

A styled draw is laid out in framebuffer pixels under
`pixel_ortho_transform(frame)`, and its batch carries frame rectangles:
`enable_glyph_frames()` on an empty batch, after which `append_run()` derives
each quad's rectangle itself and `append_quads()` takes one rectangle per
vertex. Caller-supplied frames must be finite, positive, identical across each
four-vertex quad, and equal that quad's axis-aligned bound. `prepare()` rejects
a styled transform other than `pixel_ortho_transform(frame)` before it changes
resources or enqueues updates. The fragment stage reconstructs where a fragment
sits inside its glyph from that rectangle, which is what lets a subpixel filter
step by a third of an output pixel.

`lcd_style_t::order` is the existing `vnm_msdf_text::lcd_contract` resolution and
the filter is the one `vnm_msdf_text::lcd_shader_reference` describes: three
five-tap windows a third of a pixel apart, along X for `RGB` and `BGR` and along
Y for `VRGB` and `VBGR`, in reverse channel order for `BGR` and `VBGR`. A
display-specific order composes each channel's coverage against
`background_color` and writes the straight colour that reproduces that mix once
the pipeline has blended it, so it is only the intended image where the
destination really is that background. `queue()` therefore rejects a
display-specific order unless the draw colour and the background are both opaque
within `lcd::shader_reference::k_lcd_opaque_alpha_cutoff` and no glow is present.
`NONE` is a valid order meaning no subpixel filtering: it composes exactly as the
base path does, and none of those conditions applies to it.

`glow_style_t` ramps coverage outwards from the glyph outline over `radius_px`
output pixels and is drawn under the glyphs of that same draw, so a later
glyph's glow never darkens an earlier glyph's body - which is why a glowing draw
records two draw calls. Draw states themselves stay in the order they were
queued. It is drawn on the glyph quads, so its reach is the padding
`options_t::atlas_px_range` leaves around each outline at the draw size; a
consumer that wants a wide glow bakes a snapshot with a wider range. The glow is
centred, so an offset drop shadow is a second displaced run.

`sdf_mask_t` takes the smaller of the multi-channel coverage and the true signed
distance in the MTSDF alpha channel, which keeps the sharp corners of the median
while dropping what it reconstructs where its channels disagree.

Absence is not a mode. A frame that queues no styled draw builds no styled
pipeline and allocates no styled buffers, which `diagnostics()` reports through
`styled_pipeline_builds` and the `styled_*_buffer_bytes` counters. A record
naming a version this build does not implement is refused at that record's own
boundary with `CAPABILITY_UNSUPPORTED`, and a record whose values or combination
cannot describe a drawable result with `INVALID_ARGUMENT`; either way the call
changes nothing, so the frame's other draws - base text included - record
unchanged. Nothing is ever drawn without a capability that was asked for.

### Frames

`Text_renderer` belongs to one renderer, one window, and one QRhi device, and
lives on the thread that drives that device's frames. There is no process-global
or cross-device QRhi object: two renderers own two independent resource sets
even when they draw the same snapshot. QRhi requires its resources to be
destroyed before the device, so the owner calls `release_resources()` on the
render thread while the device is still alive.

A frame is:

1. `begin_frame()`.
2. `queue(batch, state)` once per draw state. Batches accumulate into one vertex
   and one index buffer per active base or styled path. Each ordinary draw state
   produces one draw call, while a glow-enabled styled state produces two, so
   draw count follows draw states rather than glyphs.
3. `prepare(frame)` before the host opens its render pass, because the atlas,
   geometry, and uniform uploads go into the frame's resource-update batch.
4. `record(frame)` inside the pass the host opened with that batch.

Every step returns a `text_result_t`. `record()` reports `NOT_PREPARED` when
text was queued but preparation did not run or did not succeed, so a frame can
never present as text-complete when its text was dropped; `diagnostics()`
reports what the call actually issued. A `queue()` that fails - a rejected clip,
a batch from another font, a geometry limit, or `OUT_OF_MEMORY` - leaves the
frame exactly as it was rather than half-queued. The renderer sets a scissor for
every draw and leaves the viewport as the host set it.

The frame's first batch with geometry fixes the snapshot the whole frame uses.
Every later batch must be laid out against that same snapshot, and the atlas
`prepare()` offers and the smoothing data `record()` draws with come from it, so
a frame can never combine one snapshot's geometry with another's atlas. A
`set_font()` during a frame that already holds text therefore applies to the
next frame that queues its first batch, not to the one in flight.

The resource set is rebuilt when the QRhi changes, the pipeline when the
render-pass descriptor or sample count changes, and the atlas is uploaded again
when the renderer draws a different snapshot, even if the queued text did not
change. Growing the vertex and index buffers rebuilds neither the shader
bindings nor the pipeline, because neither buffer is named by the binding set.
`diagnostics()` exposes those causes as counters.

`prepare()` enqueues its uploads; it does not perform them. QRhi runs the
commands in a `QRhiResourceUpdateBatch` only when the host submits the batch
through `beginPass()`, `endPass()`, or `QRhiCommandBuffer::resourceUpdate()`,
and a host may release or abandon a batch instead, in which case nothing runs.
So an enqueued atlas upload stays outstanding until a frame carrying it reaches
`record()`, which the host runs inside the pass it opened with that batch; a
frame cancelled, abandoned, or failed before then leaves the upload outstanding
and the next `prepare()` enqueues it again. `atlas_upload_enqueues` counts those
offers, not executed uploads. Nothing in this component observes GPU execution,
presentation, or display, and it never claims to.

### Shaders

The component bakes its own `.qsb` artifacts covering SPIR-V, OpenGL ES 3.0,
desktop GL 3.3 and 4.1, HLSL 5.0, and Metal 1.2 and 2.1, for both the base and
the styled stages. The base fragment stage reconstructs coverage from the MTSDF
median as shown above, clamps sampling inside each glyph's UV rectangle, and
writes premultiplied colour; its pipeline blends `One` against
`OneMinusSrcAlpha`.

The styled fragment stage adds the optional capabilities. It writes straight
colour, because an LCD draw's one alpha stands for three channel coverages and
cannot premultiply them, so its pipeline blends `SrcAlpha` against
`OneMinusSrcAlpha` - the same `source over` composite by another route. Its LCD
decode thresholds, filter weights, tap windows, and subpixel step are the ones
`vnm_msdf_text::lcd_shader_reference` describes, and
`vnm_msdf_text_rhi_styled_shader_reference_tests` reads the shader source to
check they still are. The opacity and glow conditions that reference also states
are enforced where the draw is queued rather than in the shader, so a caller that
cannot have them is told rather than quietly given grayscale.

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

A build configured with `-DVNM_MSDF_TEXT_BUILD_RHI=ON` registers further tests.
`vnm_msdf_text_rhi_tests` covers the snapshot, batch, frame, and optional
draw-capability contracts, drives the Null QRhi backend, which proves resource
lifecycle and command recording, and checks that every compiled shader artifact
carries all seven backend profiles.
`vnm_msdf_text_rhi_styled_shader_reference_tests` checks the styled shader
source against the shared LCD reference; it needs neither Qt nor a device.
`vnm_msdf_text_rhi_real_backend_tests` renders text with a real backend and
inspects the rasterized result, including the subpixel orders, the glow, the
mask, and their combinations; it needs a working graphics device and is never a
substitute for the Null suite. Pass `--backend`, `--image`, and `--window` to
select the backend, save the rendered images, and additionally record frames
against a shown window. The registered Windows gate uses D3D11 by default, so
its raster proof is D3D11 evidence rather than a claim that an OpenGL backend
was exercised.

`tests/source_consumer` gates the way the component is actually consumed: it is
a project of its own that adds this repository with `add_subdirectory`, checks
`VNM_MSDF_TEXT_HAS_RHI`, includes only public headers, and links only
`vnm_msdf_text::rhi`. Configure, build, and run it like any other consumer:

```sh
cmake -S tests/source_consumer -B build-source-consumer -DVNM_MSDF_TEXT_FETCH_DEPS=ON
cmake --build build-source-consumer
./build-source-consumer/vnm_msdf_text_rhi_source_consumer
```

It is not registered with CTest. Configuring it detects a compiler and building
it invokes one, and a build a test run starts is a build nothing scheduled.

The `vnm_msdf_text_package_smoke` test installs the package and inspects it:
component and exact-version resolution, dependency-light degradation, and the
headers and targets the installed tree must not carry. It configures its
consumers as `LANGUAGES NONE` projects, so it detects no compiler and builds
nothing. `tests/package_consumer` covers the other half - that the installed
headers and imported targets really compile, link, and run - and is a project
of its own for the same reason `tests/source_consumer` is. Point it at the
prefix the smoke test installed and at the version this project declares, once
as the package stands and once as a dependency-light consumer sees it:

```sh
ctest --test-dir build -R vnm_msdf_text_package_smoke --output-on-failure

cmake -S tests/package_consumer -B build-package-consumer \
    -DVNM_MSDF_TEXT_PACKAGE_PREFIX=$PWD/build/package_smoke/install \
    -DVNM_MSDF_TEXT_PACKAGE_VERSION=0.2.0
cmake --build build-package-consumer
ctest --test-dir build-package-consumer --output-on-failure

cmake -S tests/package_consumer -B build-package-consumer-nodeps \
    -DVNM_MSDF_TEXT_PACKAGE_PREFIX=$PWD/build/package_smoke/install \
    -DVNM_MSDF_TEXT_PACKAGE_VERSION=0.2.0 \
    -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=TRUE \
    -DCMAKE_DISABLE_FIND_PACKAGE_msdfgen=TRUE
cmake --build build-package-consumer-nodeps
ctest --test-dir build-package-consumer-nodeps --output-on-failure
```
