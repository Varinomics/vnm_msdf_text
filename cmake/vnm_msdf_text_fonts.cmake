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

if(NOT VNM_FONTS_DIRECTORY)
  # Nothing in this tree has published the files yet, so this build obtains
  # vnm_fonts itself. Adding it sets VNM_FONTS_DIRECTORY below.
  include(FetchContent)

  get_filename_component(_vnm_msdf_text_vnm_fonts_sibling
    "${CMAKE_CURRENT_LIST_DIR}/../../vnm_fonts" ABSOLUTE)
  if(NOT EXISTS "${_vnm_msdf_text_vnm_fonts_sibling}/CMakeLists.txt")
    set(_vnm_msdf_text_vnm_fonts_sibling "")
  endif()

  set(VNM_MSDF_TEXT_VNM_FONTS_SOURCE_DIR "${_vnm_msdf_text_vnm_fonts_sibling}"
    CACHE PATH "Local vnm_fonts checkout; empty fetches vnm_fonts from GitHub")
  unset(_vnm_msdf_text_vnm_fonts_sibling)

  if(VNM_MSDF_TEXT_VNM_FONTS_SOURCE_DIR)
    message(STATUS
      "vnm_msdf_text: Using local vnm_fonts checkout: "
      "${VNM_MSDF_TEXT_VNM_FONTS_SOURCE_DIR}")
    FetchContent_Declare(vnm_fonts
      SOURCE_DIR "${VNM_MSDF_TEXT_VNM_FONTS_SOURCE_DIR}"
    )
  else()
    message(STATUS "vnm_msdf_text: Fetching vnm_fonts")
    FetchContent_Declare(vnm_fonts
      GIT_REPOSITORY https://github.com/Varinomics/vnm_fonts.git
      GIT_TAG        master
      GIT_SHALLOW    TRUE
    )
  endif()
  FetchContent_MakeAvailable(vnm_fonts)
endif()

set(VNM_MSDF_TEXT_FONT_FILE "${VNM_FONTS_DIRECTORY}/UbuntuSansMonoDerivativeVnm-Regular.ttf")

if(NOT EXISTS "${VNM_MSDF_TEXT_FONT_FILE}")
  message(FATAL_ERROR
    "vnm_msdf_text: ${VNM_MSDF_TEXT_FONT_FILE} does not exist. "
    "${VNM_FONTS_DIRECTORY} does not carry the font this repository bakes its "
    "atlases from.")
endif()
