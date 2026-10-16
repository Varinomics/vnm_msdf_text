# QRhi text rendering

`vnm_msdf_text::rhi` draws the [CPU atlas](atlas_usage.md) with
Qt's QRhi. It is a compositional layer over the CPU API: it owns device-local
texture, buffer, sampler, pipeline, and binding resources, the per-frame upload
and recording, and the status of both. Where text goes, what it says, how it is
coloured, and when it is worth drawing stay with the consumer.

The component is off by default. Configure with `-DVNM_MSDF_TEXT_BUILD_RHI=ON`
to build it; Qt is located only when `VNM_MSDF_TEXT_BUILD_RHI` or
`VNM_MSDF_TEXT_BUILD_QT_LCD` is on. A consumer that requires GPU text can read
`VNM_MSDF_TEXT_HAS_RHI` and fail its own configure when RHI is unavailable. The
component requires C++20, Qt 6.12 or newer, and Qt Shader Tools at build time
only.

Consume `vnm_msdf_text::rhi` through `add_subdirectory`, `FetchContent`, or
`find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS rhi)`. An installation
built with RHI enabled and find-package-able atlas dependencies exports its
headers, static library and shader resource objects. Qt is resolved only when
a Qt component is requested; dependency-light LCD clients need no Qt.

Public headers live under `vnm_msdf_text/rhi/` and everything is in namespace
`vnm::msdf_text::rhi`. The public headers only forward-declare the QRhi types
they take pointers to, so Qt's private QRhi headers stay inside the
implementation.

The public [RHI headers](../include/vnm_msdf_text/rhi/) define signatures, result
codes and optional records. Use the [LCD contract](lcd_msdf_commonization_contract.md)
for display-order resolution and the shared filter reference.

## Font snapshots and cached atlases

`build_font_snapshot(font_bytes, draw_pixel_height, codepoints, options)` builds
an immutable `Font_snapshot` from caller-supplied bytes. The component never
reads a file or resolves an asset: font acquisition belongs to the consumer.

The result carries a status and, on success, a `shared_ptr` to the snapshot:

- `INVALID_ARGUMENT` for empty bytes or a non-positive draw pixel height.
- `FONT_BUILD_FAILED` when no usable atlas could be produced; no snapshot.
- `OK` otherwise. A partially built atlas is an `OK` result with a usable
  snapshot whose `build_result().status` is `PARTIAL_SUCCESS` and whose
  diagnostic vectors name the affected codepoints.

A snapshot exposes the CPU data it was built from rather than restating it:
`atlas()`, `draw_pixel_height()`, and the full `build_result()`. Measurement,
bounds, positioned glyphs, and quad emission are the existing free functions in
`msdf_text.h` applied to those two values, so there is one set of metrics.

`identity()` is a digest of the complete drawable atlas content and draw size.
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
object. `Text_renderer` retains the immutable `Baked_font` instead of the
draw-size snapshot, so changing draw size can reuse the uploaded bitmap.

`build_baked_font()` produces that shared owner, and `make_font_snapshot()`
creates inexpensive draw-size views over it. `adopt_baked_font()` validates a
complete cached build result and derives its identity from the actual data.
Cache owners retain status and coverage diagnostics, include the producer's
`k_font_bake_compatibility_version` in their key, and retain ownership of disk
locations, size budgets and eviction. Invalid cached data is rejected.

## Batches and draw states

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
text itself is laid out in. Negative `x` or `y` and partially out-of-bounds
rectangles are clamped by QRhi, and a zero width or height clips the draw away.
A negative width or height is not a rectangle QRhi can express -
it drops such a scissor and leaves the previous one in force - so `queue()`
rejects an enabled clip with one instead of recording a draw under someone
else's clip.

## Frame sequence and resource lifetime

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

`queue()`, `prepare()`, and `record()` return a `text_result_t`. `record()` reports
`NOT_PREPARED` when text was queued but preparation did not run or did not
succeed; `diagnostics()` reports what the call issued. A `queue()` that fails - a
rejected clip, a batch from another font, a geometry limit, or `OUT_OF_MEMORY` -
leaves the frame exactly as it was rather than half-queued. The renderer sets a
scissor for every draw and leaves the viewport as the host set it.

The frame's first batch with geometry fixes the snapshot the whole frame uses.
Every later batch must be laid out against that same snapshot, and the atlas
`prepare()` offers and the smoothing data `record()` draws with come from it, so
a frame can never combine one snapshot's geometry with another's atlas. A
`set_font()` during a frame that already holds text therefore applies to the
next frame that queues its first batch, not to the one in flight.

The resource set is rebuilt when the QRhi changes and the pipeline when the
render-pass descriptor or sample count changes. Draw-size snapshots over the same
`Baked_font` reuse its uploaded atlas.

`prepare()` enqueues its uploads; it does not perform them. QRhi runs the
commands in a `QRhiResourceUpdateBatch` only when the host submits the batch
through `beginPass()`, `endPass()`, or `QRhiCommandBuffer::resourceUpdate()`,
and a host may release or abandon a batch instead, in which case nothing runs.
So an enqueued atlas upload stays outstanding until a frame carrying it reaches
`record()`, which the host runs inside the pass it opened with that batch; a
frame cancelled, abandoned, or failed before then leaves the upload outstanding
and the next `prepare()` enqueues it again. See the
[Text_renderer API](../include/vnm_msdf_text/rhi/text_renderer.h) for resource and
diagnostic details.

## Optional draw capabilities

[draw_capabilities.h](../include/vnm_msdf_text/rhi/draw_capabilities.h) defines the
optional capabilities. Each is a presence-tagged record with a version of its own,
and each is absent by default. A draw state that carries none of them takes the
base path described above: the same geometry, uniform block, shader, and pipeline
a build without that header uses. A draw state that carries any of them is a
*styled* draw and takes a second path, created only once a frame queues one.

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
resources or enqueues updates.

`lcd_style_t::order` uses the shared
[LCD contract and filter reference](lcd_msdf_commonization_contract.md).
A display-specific order requires the destination to match `background_color`.
`queue()` rejects a display-specific order unless the draw colour and the
background are both opaque within
`lcd::shader_reference::k_lcd_opaque_alpha_cutoff` and no glow is present.
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

A frame that queues no styled draw builds no styled pipeline and allocates no
styled buffers. An unsupported record version returns `CAPABILITY_UNSUPPORTED`;
invalid values or combinations return `INVALID_ARGUMENT`. Both leave the frame
unchanged, including its other draws.

## Shader output and blending

The component bakes its own `.qsb` artifacts covering SPIR-V, OpenGL ES 3.0,
desktop GL 3.3 and 4.1, HLSL 5.0, and Metal 1.2 and 2.1, for both the base and
the styled stages. The base fragment stage reconstructs coverage from the MTSDF
median described in the [atlas guide](atlas_usage.md#layout-and-rendering),
clamps sampling inside each glyph's UV rectangle, and writes premultiplied
colour; its pipeline blends `One` against
`OneMinusSrcAlpha`.

The styled fragment stage adds the optional capabilities. It writes straight
colour, because an LCD draw's one alpha stands for three channel coverages and
cannot premultiply them, so its pipeline blends `SrcAlpha` against
`OneMinusSrcAlpha` - the same `source over` composite by another route. Its LCD
decode thresholds, filter weights, tap windows, and subpixel step are the ones
`vnm_msdf_text::lcd_shader_reference` describes. The renderer enforces the
opacity and glow conditions at `queue()` and reports invalid combinations.
