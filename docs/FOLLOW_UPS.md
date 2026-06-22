# vnm_msdf_text Follow-Ups

This document tracks valid review items that are broader than the current
correctness and small API cleanup batches.

## Architecture

- Split runtime layout from atlas building when the dependency cost becomes a
  problem. A future `vnm_msdf_text_core` target would contain glyph maps,
  UTF-8 decoding, layout, bounds, and quad emission without FreeType or
  msdfgen. A `vnm_msdf_text_builder` target would contain font loading,
  glyph packing, MTSDF generation, and kerning extraction.
- Keep complex text shaping out of this library unless the scope changes
  intentionally. HarfBuzz shaping, bidirectional reordering, ligatures, and
  grapheme-cluster layout belong in a higher text stack.

## Atlas Building

- Replace the shelf packer with a skyline or rectangle-packing algorithm if
  larger or more varied glyph sets become common.
- Decide whether the default gutter should be derived from `atlas_px_range`
  for mipmapped renderers. The current option is explicit, but the default is
  still conservative for non-mipmapped, UV-clamped sampling.
- Add fallback-specific diagnostics if callers need to distinguish missing
  source coverage from drawable replacement aliases. A possible split is
  `missing_codepoints`, `fallback_codepoints`, and `unemitted_codepoints`.
- Consider storing an internal glyph id in glyph records so kerning can be
  keyed by glyph-id pairs instead of codepoint pairs. This would reduce
  repeated kerning queries for many codepoints aliased to one glyph.

## Fonts And Coverage

- Decide whether the bundled font is only a test fixture or a supported
  default. It does not cover the full `default_codepoints()` set.
- If a supported default font is desired, either narrow `default_codepoints()`
  to that font's coverage or bundle/document a font with matching coverage.
- Add a small purpose-built test font if future tests need exact coverage
  guarantees beyond the current dynamic missing-codepoint probe.

## Rendering Contract

- Add a visual/golden test for the atlas range and coordinate contract. The
  downstream terminal-surface tests exercise this path today, but a local
  renderer-independent fixture would make regressions easier to isolate.
- Document or support derivative-based screen pixel range calculation if
  callers scale generated quads after layout.

## Build And Packaging

- Consider respecting `BUILD_SHARED_LIBS` instead of forcing a static library.
- Prefer release archives with hashes over Git fetches if reproducibility or
  locked-down build environments become a requirement.
- Revisit install/export behavior if the library is packaged independently.
  The current export intentionally depends on whether dependencies are imported
  package targets or locally built targets.
