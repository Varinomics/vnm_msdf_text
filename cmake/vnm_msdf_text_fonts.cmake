# The font this repository's tests and its source-consumer gate bake an atlas
# from: Ubuntu Sans Mono derivative vnm, taken from vnm_fonts, which ships
# every font Varinomics embeds byte-verbatim together with its licence and
# notice.
#
# vnm_fonts serves a file contract and a library contract. This repository uses
# only the first: VNM_FONTS_DIRECTORY names the verbatim files, and the library
# marks a family name on the way into QFontDatabase, which two files declaring
# one family need so they cannot merge into a single entry and serve glyph
# lookup and rasterisation from different files. Nothing here enters a font
# database - the atlas is baked straight from the file's bytes - so there is no
# family entry to collide with, and vnm::fonts is deliberately not linked.
# vnm_fonts configures without Qt for exactly this reason, so adding it does
# not put Qt in front of a build that is otherwise Qt-free.

include("${CMAKE_CURRENT_LIST_DIR}/vnm_cmake_dependency.cmake")
vnm_acquire_owned_dependency(NAME vnm_fonts
    GIT_REPOSITORY https://github.com/Varinomics/vnm_fonts.git
    FILES_VARIABLE VNM_FONTS_DIRECTORY)

set(VNM_MSDF_TEXT_FONT_FILE "${VNM_FONTS_DIRECTORY}/UbuntuSansMonoDerivativeVnm-Regular.ttf")

if(NOT EXISTS "${VNM_MSDF_TEXT_FONT_FILE}")
  message(FATAL_ERROR
    "vnm_msdf_text: ${VNM_MSDF_TEXT_FONT_FILE} does not exist. "
    "${VNM_FONTS_DIRECTORY} does not carry the font this repository bakes its "
    "atlases from.")
endif()
