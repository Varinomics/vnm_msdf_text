# Third-Party Notices

The source code in this repository is licensed under the BSD 2-Clause License
in `LICENSE.txt`. This repository redistributes no third-party asset.

## The font the tests bake

`vnm_msdf_text_tests`, the QRhi test suites, and `tests/source_consumer` bake
their atlases from Ubuntu Sans Mono derivative vnm, which comes from
[vnm_fonts](https://github.com/Varinomics/vnm_fonts) as
`fonts/UbuntuSansMonoDerivativeVnm-Regular.ttf`. That repository ships the file
byte-verbatim and carries its notice and the Ubuntu Font Licence 1.0 text in
`THIRD_PARTY_NOTICES.md` and `LICENSES/Ubuntu-Font-Licence-1.0.txt`. The font is
a build input to the tests; it is neither checked in here nor embedded in
anything this repository produces, so no font bytes are distributed with
`vnm_msdf_text`.

A consumer that embeds a font in its own binary carries that font's notice and
licence itself. `build_font_atlas` takes font bytes from its caller and has no
font of its own.

## Why the font is not registered through vnm_fonts' library

vnm_fonts serves a file contract and a library contract, and only the file is
used here. Its `vnm::fonts` library marks a family name on the way into
`QFontDatabase`, because two files declaring one family name merge there into a
single entry and glyph lookup and rasterisation can then be served from
different files. Nothing in this repository enters a font database: an atlas is
baked straight from the file's bytes. There is no family entry to collide with
and nothing for the mark to do, so `vnm::fonts` is deliberately not linked.
