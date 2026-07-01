cmake_minimum_required(VERSION 3.16)

project(vnm_msdf_text
  VERSION 0.2.0
  DESCRIPTION "Small CPU-side MSDF text atlas builder"
  LANGUAGES CXX
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(VNM_MSDF_TEXT_IS_TOP_LEVEL ON)
else()
  set(VNM_MSDF_TEXT_IS_TOP_LEVEL OFF)
endif()

set(_vnm_msdf_text_fetch_deps_default ${VNM_MSDF_TEXT_IS_TOP_LEVEL})
if(DEFINED VNM_MSDF_TEXT_USE_SYSTEM_LIBS AND
   NOT DEFINED VNM_MSDF_TEXT_FETCH_DEPS)
  if(VNM_MSDF_TEXT_USE_SYSTEM_LIBS)
    set(_vnm_msdf_text_fetch_deps_default OFF)
  else()
    set(_vnm_msdf_text_fetch_deps_default ON)
  endif()
endif()

option(VNM_MSDF_TEXT_FETCH_DEPS
  "Fetch/build FreeType and msdfgen when they are not already available"
  ${_vnm_msdf_text_fetch_deps_default})
option(VNM_MSDF_TEXT_BUILD_TESTS
  "Build vnm_msdf_text tests"
  ${VNM_MSDF_TEXT_IS_TOP_LEVEL})

include(FetchContent)

add_library(vnm_msdf_text_lcd_contract INTERFACE)
add_library(vnm_msdf_text::lcd_contract ALIAS vnm_msdf_text_lcd_contract)

set_target_properties(vnm_msdf_text_lcd_contract PROPERTIES
  EXPORT_NAME lcd_contract
)

target_include_directories(vnm_msdf_text_lcd_contract
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(vnm_msdf_text_lcd_contract INTERFACE cxx_std_17)

add_library(vnm_msdf_text_lcd_shader_reference INTERFACE)
add_library(vnm_msdf_text::lcd_shader_reference ALIAS vnm_msdf_text_lcd_shader_reference)

set_target_properties(vnm_msdf_text_lcd_shader_reference PROPERTIES
  EXPORT_NAME lcd_shader_reference
)

target_include_directories(vnm_msdf_text_lcd_shader_reference
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(vnm_msdf_text_lcd_shader_reference INTERFACE cxx_std_17)

target_link_libraries(vnm_msdf_text_lcd_shader_reference
  INTERFACE
    vnm_msdf_text_lcd_contract
)

if(DEFINED VNM_MSDF_TEXT_USE_SYSTEM_LIBS)
  message(STATUS
    "VNM_MSDF_TEXT_USE_SYSTEM_LIBS is deprecated. Use "
    "VNM_MSDF_TEXT_FETCH_DEPS=OFF to require existing/find_package "
    "dependencies, or VNM_MSDF_TEXT_FETCH_DEPS=ON to allow FetchContent.")
endif()

function(vnm_msdf_text_set_fetchcontent_build_dir name dir_name)
  string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _fc_compiler_tag)
  set(_fc_config_suffix "-${_fc_compiler_tag}")
  if(CMAKE_BUILD_TYPE)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" _fc_bt_lower)
    string(APPEND _fc_config_suffix "-${_fc_bt_lower}")
  endif()

  set(FETCHCONTENT_BINARY_DIR_${name}
    "${FETCHCONTENT_BASE_DIR}/${dir_name}-build${_fc_config_suffix}"
    CACHE PATH "")
endfunction()

function(vnm_msdf_text_alias_freetype_target)
  if(TARGET freetype AND NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
  endif()
endfunction()

function(vnm_msdf_text_alias_msdfgen_targets)
  if(TARGET msdfgen-core AND NOT TARGET msdfgen::msdfgen-core)
    add_library(msdfgen::msdfgen-core ALIAS msdfgen-core)
  endif()
  if(TARGET msdfgen-ext AND NOT TARGET msdfgen::msdfgen-ext)
    add_library(msdfgen::msdfgen-ext ALIAS msdfgen-ext)
  endif()
endfunction()

if(DEFINED VNM_MSDF_TEXT_DEP_OVERRIDES_FILE AND
   NOT VNM_MSDF_TEXT_DEP_OVERRIDES_FILE STREQUAL "")
  if(EXISTS "${VNM_MSDF_TEXT_DEP_OVERRIDES_FILE}")
    message(STATUS
      "vnm_msdf_text: Loading dependency overrides from "
      "${VNM_MSDF_TEXT_DEP_OVERRIDES_FILE}")
    include("${VNM_MSDF_TEXT_DEP_OVERRIDES_FILE}")
  else()
    message(FATAL_ERROR
      "vnm_msdf_text: VNM_MSDF_TEXT_DEP_OVERRIDES_FILE was set to "
      "'${VNM_MSDF_TEXT_DEP_OVERRIDES_FILE}', but that file does not exist.")
  endif()
endif()

if(NOT TARGET Freetype::Freetype AND NOT TARGET freetype)
  find_package(Freetype CONFIG QUIET)
  if(NOT TARGET Freetype::Freetype AND NOT TARGET freetype AND
     NOT Freetype_FOUND AND NOT FREETYPE_FOUND)
    find_package(Freetype MODULE QUIET)
  endif()
endif()

vnm_msdf_text_alias_freetype_target()

if((Freetype_FOUND OR FREETYPE_FOUND) AND
   NOT TARGET Freetype::Freetype AND
   FREETYPE_INCLUDE_DIRS AND FREETYPE_LIBRARIES)
  add_library(Freetype::Freetype INTERFACE IMPORTED)
  set_target_properties(Freetype::Freetype PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${FREETYPE_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${FREETYPE_LIBRARIES}")
endif()

if(TARGET Freetype::Freetype)
  message(STATUS "vnm_msdf_text: Using Freetype::Freetype")
elseif(VNM_MSDF_TEXT_FETCH_DEPS)
  if(FETCHCONTENT_SOURCE_DIR_FREETYPE AND
     EXISTS "${FETCHCONTENT_SOURCE_DIR_FREETYPE}/CMakeLists.txt")
    message(STATUS
      "vnm_msdf_text: Using local freetype checkout: "
      "${FETCHCONTENT_SOURCE_DIR_FREETYPE}")
  else()
    message(STATUS "vnm_msdf_text: Fetching freetype")
  endif()
  vnm_msdf_text_set_fetchcontent_build_dir(freetype freetype)
  FetchContent_Declare(freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG VER-2-13-3
    GIT_SHALLOW TRUE
    BINARY_DIR "${FETCHCONTENT_BINARY_DIR_freetype}"
  )

  set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "")
  set(FT_DISABLE_BROTLI ON CACHE BOOL "")
  set(FT_DISABLE_BZIP2 ON CACHE BOOL "")
  set(FT_DISABLE_PNG ON CACHE BOOL "")
  set(FT_DISABLE_ZLIB ON CACHE BOOL "")
  set(FT_ENABLE_ERROR_STRINGS OFF CACHE BOOL "")

  FetchContent_MakeAvailable(freetype)
  vnm_msdf_text_alias_freetype_target()

  if(NOT TARGET Freetype::Freetype)
    message(FATAL_ERROR
      "vnm_msdf_text: FreeType was fetched, but target "
      "Freetype::Freetype was not created.")
  endif()
else()
  message(FATAL_ERROR
    "vnm_msdf_text: FreeType dependency not found. Provide an existing "
    "Freetype::Freetype target, install a find_package(Freetype)-compatible "
    "package, or configure with -DVNM_MSDF_TEXT_FETCH_DEPS=ON. For offline "
    "FetchContent builds, also set FETCHCONTENT_SOURCE_DIR_FREETYPE or "
    "VNM_MSDF_TEXT_DEP_OVERRIDES_FILE.")
endif()

vnm_msdf_text_alias_msdfgen_targets()

if(NOT TARGET msdfgen::msdfgen-core OR NOT TARGET msdfgen::msdfgen-ext)
  find_package(msdfgen CONFIG QUIET)
  vnm_msdf_text_alias_msdfgen_targets()
endif()

if(TARGET msdfgen::msdfgen-core AND TARGET msdfgen::msdfgen-ext)
  message(STATUS "vnm_msdf_text: Using msdfgen targets")
elseif(VNM_MSDF_TEXT_FETCH_DEPS)
  if(FETCHCONTENT_SOURCE_DIR_MSDFGEN AND
     EXISTS "${FETCHCONTENT_SOURCE_DIR_MSDFGEN}/CMakeLists.txt")
    message(STATUS
      "vnm_msdf_text: Using local msdfgen checkout: "
      "${FETCHCONTENT_SOURCE_DIR_MSDFGEN}")
  else()
    message(STATUS "vnm_msdf_text: Fetching msdfgen")
  endif()
  vnm_msdf_text_set_fetchcontent_build_dir(msdfgen msdfgen)
  FetchContent_Declare(msdfgen
    GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
    GIT_TAG v1.12.1
    GIT_SHALLOW TRUE
    BINARY_DIR "${FETCHCONTENT_BINARY_DIR_msdfgen}"
  )
  set(MSDFGEN_USE_VCPKG OFF CACHE BOOL "")
  set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "")
  set(MSDFGEN_DISABLE_SVG ON CACHE BOOL "")
  set(MSDFGEN_DISABLE_PNG ON CACHE BOOL "")
  set(MSDFGEN_USE_SKIA OFF CACHE BOOL "")
  set(MSDFGEN_CORE_ONLY OFF)
  set(MSDFGEN_CORE_ONLY OFF CACHE BOOL "" FORCE)
  if(MSVC)
    set(MSDFGEN_DYNAMIC_RUNTIME ON CACHE BOOL "")
  endif()
  FetchContent_MakeAvailable(msdfgen)
  vnm_msdf_text_alias_msdfgen_targets()

  if(NOT TARGET msdfgen::msdfgen-core OR NOT TARGET msdfgen::msdfgen-ext)
    message(FATAL_ERROR
      "vnm_msdf_text: msdfgen was fetched, but targets "
      "msdfgen::msdfgen-core and msdfgen::msdfgen-ext were not created.")
  endif()
else()
  message(FATAL_ERROR
    "vnm_msdf_text: msdfgen dependency not found. Provide existing "
    "msdfgen::msdfgen-core and msdfgen::msdfgen-ext targets, install a "
    "find_package(msdfgen)-compatible package, or configure with "
    "-DVNM_MSDF_TEXT_FETCH_DEPS=ON. For offline FetchContent builds, also "
    "set FETCHCONTENT_SOURCE_DIR_MSDFGEN or VNM_MSDF_TEXT_DEP_OVERRIDES_FILE.")
endif()

# Intentionally static: the public API exposes STL types and is meant for
# source-level embedding, not as a stable shared-library ABI.
add_library(vnm_msdf_text STATIC
  src/msdf_text.cpp
  include/vnm_msdf_text/msdf_text.h
)

add_library(vnm_msdf_text::vnm_msdf_text ALIAS vnm_msdf_text)

target_include_directories(vnm_msdf_text
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(vnm_msdf_text PUBLIC cxx_std_20)

target_link_libraries(vnm_msdf_text
  PUBLIC
    vnm_msdf_text_lcd_contract
    msdfgen::msdfgen-ext
    Freetype::Freetype
    msdfgen::msdfgen-core
)

if(WIN32)
  target_compile_definitions(vnm_msdf_text PRIVATE NOMINMAX)
endif()

if(MSVC)
  target_compile_options(vnm_msdf_text PRIVATE /W4)
else()
  target_compile_options(vnm_msdf_text PRIVATE -Wall -Wextra)
endif()

# install(EXPORT) requires every PUBLIC link dependency to be either IMPORTED
# or part of the installed export set. Locally-built dependencies from
# FetchContent or a parent project do not satisfy that here, so only the
# dependency-light components are exported in that configuration.
set(_vnm_msdf_text_export_ok TRUE)
foreach(_dep IN ITEMS Freetype::Freetype msdfgen::msdfgen-core msdfgen::msdfgen-ext)
  if(TARGET ${_dep})
    get_target_property(_dep_imported ${_dep} IMPORTED)
    if(NOT _dep_imported)
      set(_vnm_msdf_text_export_ok FALSE)
    endif()
  endif()
endforeach()
set(VNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT ${_vnm_msdf_text_export_ok})

if(VNM_MSDF_TEXT_BUILD_TESTS)
  enable_testing()

  add_executable(vnm_msdf_text_tests
    tests/msdf_text_tests.cpp
  )
  target_link_libraries(vnm_msdf_text_tests
    PRIVATE
      vnm_msdf_text
  )
  target_compile_features(vnm_msdf_text_tests PRIVATE cxx_std_20)
  target_compile_definitions(vnm_msdf_text_tests PRIVATE
    VNM_MSDF_TEXT_TEST_FONT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/fonts/monospace.ttf"
  )

  if(MSVC)
    target_compile_options(vnm_msdf_text_tests PRIVATE /W4)
  else()
    target_compile_options(vnm_msdf_text_tests PRIVATE -Wall -Wextra)
  endif()

  add_test(NAME vnm_msdf_text_tests COMMAND vnm_msdf_text_tests)

  add_executable(vnm_msdf_text_lcd_contract_tests
    tests/lcd_contract_tests.cpp
  )
  target_link_libraries(vnm_msdf_text_lcd_contract_tests
    PRIVATE
      vnm_msdf_text_lcd_shader_reference
  )
  target_compile_features(vnm_msdf_text_lcd_contract_tests PRIVATE cxx_std_17)

  if(MSVC)
    target_compile_options(vnm_msdf_text_lcd_contract_tests PRIVATE /W4)
  else()
    target_compile_options(vnm_msdf_text_lcd_contract_tests PRIVATE -Wall -Wextra)
  endif()

  add_test(
    NAME vnm_msdf_text_lcd_contract_tests
    COMMAND vnm_msdf_text_lcd_contract_tests)

  if(VNM_MSDF_TEXT_IS_TOP_LEVEL)
    add_test(
      NAME vnm_msdf_text_package_smoke
      COMMAND ${CMAKE_COMMAND}
        "-DVNM_MSDF_TEXT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DVNM_MSDF_TEXT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DVNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT=${VNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT}"
        "-DVNM_MSDF_TEXT_TEST_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DVNM_MSDF_TEXT_TEST_CONFIG=$<CONFIG>"
        "-DVNM_MSDF_TEXT_TEST_GENERATOR=${CMAKE_GENERATOR}"
        "-DVNM_MSDF_TEXT_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DVNM_MSDF_TEXT_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DVNM_MSDF_TEXT_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_smoke.cmake")
  endif()
endif()

if(VNM_MSDF_TEXT_IS_TOP_LEVEL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  install(DIRECTORY include/vnm_msdf_text
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
  install(FILES
    LICENSE.txt
    README.md
    THIRD_PARTY_NOTICES.md
    docs/lcd_msdf_commonization_contract.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
  )

  install(TARGETS
    vnm_msdf_text_lcd_contract
    vnm_msdf_text_lcd_shader_reference
    EXPORT vnm_msdf_textTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  if(_vnm_msdf_text_export_ok)
    set(VNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT TRUE)

    install(TARGETS vnm_msdf_text
      EXPORT vnm_msdf_textTargets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

  else()
    set(VNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT FALSE)

    message(STATUS
      "vnm_msdf_text: Skipping find_package export because Freetype/msdfgen are "
      "built locally. Configure with -DVNM_MSDF_TEXT_FETCH_DEPS=OFF and "
      "provide find_package-able dependencies to install a find_package-ready "
      "vnm_msdf_text atlas component. Dependency-light lcd_contract and "
      "lcd_shader_reference package components will still be exported.")
  endif()

  install(EXPORT vnm_msdf_textTargets
    NAMESPACE vnm_msdf_text::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vnm_msdf_text
  )

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_msdf_text-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vnm_msdf_text-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_msdf_text-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vnm_msdf_text
  )

  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_msdf_text-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_msdf_text-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vnm_msdf_text
  )
endif()
