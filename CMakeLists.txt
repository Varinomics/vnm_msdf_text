# 3.22 is the floor because the Windows test registration below relies on the
# ENVIRONMENT_MODIFICATION test property, which older CMake stores and ignores.
# The installed package itself imposes no such requirement, so the generated
# package-smoke consumers keep declaring the older floor a consumer may use.
cmake_minimum_required(VERSION 3.22)

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
option(VNM_MSDF_TEXT_BUILD_RHI
  "Build the opt-in Qt QRhi text component (vnm_msdf_text::rhi)"
  OFF)

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

set_target_properties(vnm_msdf_text PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

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

# -----------------------------------------------------------------------------
# Opt-in QRhi text component
#
# Qt is located only inside this block. A CPU-only or editor-style consumer that
# leaves VNM_MSDF_TEXT_BUILD_RHI OFF never runs find_package(Qt6), so it can
# neither find nor link Qt through this project. The component is a source-tree
# target: it is deliberately not part of the installed package export, because
# an exported static library carrying Qt private headers and Qt resource objects
# would promise an installed-consumer contract no consumer has adopted yet.
# -----------------------------------------------------------------------------
if(VNM_MSDF_TEXT_BUILD_RHI)
  find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui GuiPrivate ShaderTools)

  add_library(vnm_msdf_text_rhi STATIC
    src/rhi/font_snapshot.cpp
    src/rhi/text_batch.cpp
    src/rhi/text_renderer.cpp
    include/vnm_msdf_text/rhi/font_snapshot.h
    include/vnm_msdf_text/rhi/status.h
    include/vnm_msdf_text/rhi/text_batch.h
    include/vnm_msdf_text/rhi/text_renderer.h
  )

  add_library(vnm_msdf_text::rhi ALIAS vnm_msdf_text_rhi)

  set_target_properties(vnm_msdf_text_rhi PROPERTIES
    EXPORT_NAME rhi
    POSITION_INDEPENDENT_CODE ON
  )

  target_include_directories(vnm_msdf_text_rhi
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  )

  target_compile_features(vnm_msdf_text_rhi PUBLIC cxx_std_20)

  # Qt private QRhi headers stay private to this implementation: the public
  # headers only forward-declare the QRhi types they take pointers to.
  target_link_libraries(vnm_msdf_text_rhi
    PUBLIC
      vnm_msdf_text
      Qt6::Core
      Qt6::Gui
    PRIVATE
      Qt6::GuiPrivate
  )

  if(MSVC)
    target_compile_options(vnm_msdf_text_rhi PRIVATE /W4)
  else()
    target_compile_options(vnm_msdf_text_rhi PRIVATE -Wall -Wextra)
  endif()

  # One .qsb artifact per stage, covering the backend profiles the Varinomics
  # QRhi consumers support: SPIR-V (always emitted), OpenGL ES 3.0, desktop
  # GL 3.3 and 4.1, HLSL 5.0, and Metal 1.2 and 2.1. ShaderTools is used here
  # only at build time; the component links no ShaderTools library.
  qt_add_shaders(vnm_msdf_text_rhi "vnm_msdf_text_rhi_shaders"
    PREFIX "/vnm_msdf_text"
    BASE   "${CMAKE_CURRENT_SOURCE_DIR}"
    GLSL   "300 es,330,410"
    HLSL   "50"
    MSL    "12,21"
    FILES
      shaders/rhi/msdf_text.vert
      shaders/rhi/msdf_text.frag
  )
endif()

# Configure-time capability signal, so an add_subdirectory or FetchContent
# consumer that requires real QRhi text can fail its own configure instead of
# linking a build that does not have the component. Forced on every reconfigure
# so it can never describe a capability this build tree does not have.
set(VNM_MSDF_TEXT_HAS_RHI ${VNM_MSDF_TEXT_BUILD_RHI}
  CACHE INTERNAL "Whether this vnm_msdf_text build provides the QRhi text component" FORCE)

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

  if(VNM_MSDF_TEXT_BUILD_RHI)
    add_executable(vnm_msdf_text_rhi_tests
      tests/rhi_text_tests.cpp
    )
    target_link_libraries(vnm_msdf_text_rhi_tests
      PRIVATE
        vnm_msdf_text_rhi
        Qt6::GuiPrivate
    )
    target_compile_features(vnm_msdf_text_rhi_tests PRIVATE cxx_std_20)
    target_compile_definitions(vnm_msdf_text_rhi_tests PRIVATE
      VNM_MSDF_TEXT_TEST_FONT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/fonts/monospace.ttf"
    )

    if(MSVC)
      target_compile_options(vnm_msdf_text_rhi_tests PRIVATE /W4)
    else()
      target_compile_options(vnm_msdf_text_rhi_tests PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME vnm_msdf_text_rhi_tests COMMAND vnm_msdf_text_rhi_tests)

    # On Windows the first Qt of the right major version on PATH wins, and an
    # unrelated application's bundled Qt loads into these tests and crashes
    # them. Put the Qt this build linked against in front for the test run.
    if(WIN32)
      get_target_property(_vnm_msdf_text_qt_core_dll Qt6::Core IMPORTED_LOCATION_RELEASE)
      if(NOT _vnm_msdf_text_qt_core_dll)
        get_target_property(_vnm_msdf_text_qt_core_dll Qt6::Core IMPORTED_LOCATION)
      endif()
      if(_vnm_msdf_text_qt_core_dll)
        get_filename_component(_vnm_msdf_text_qt_bin_dir
          "${_vnm_msdf_text_qt_core_dll}" DIRECTORY)
        file(TO_NATIVE_PATH "${_vnm_msdf_text_qt_bin_dir}" _vnm_msdf_text_qt_bin_native)
        set(_vnm_msdf_text_qt_path_env
          "PATH=path_list_prepend:${_vnm_msdf_text_qt_bin_native}")
      else()
        message(WARNING
          "vnm_msdf_text: the Qt 6 runtime directory could not be derived from "
          "Qt6::Core, so the QRhi tests run against whichever Qt is on PATH.")
      endif()
    endif()

    if(_vnm_msdf_text_qt_path_env)
      set_tests_properties(vnm_msdf_text_rhi_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "${_vnm_msdf_text_qt_path_env}")
    endif()

    # The real-backend gate needs a working graphics device, so it is registered
    # separately from the Null-backend suite and is never a substitute for it.
    add_executable(vnm_msdf_text_rhi_real_backend_tests
      tests/rhi_real_backend_tests.cpp
    )
    target_link_libraries(vnm_msdf_text_rhi_real_backend_tests
      PRIVATE
        vnm_msdf_text_rhi
        Qt6::GuiPrivate
    )
    target_compile_features(vnm_msdf_text_rhi_real_backend_tests PRIVATE cxx_std_20)
    target_compile_definitions(vnm_msdf_text_rhi_real_backend_tests PRIVATE
      VNM_MSDF_TEXT_TEST_FONT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/fonts/monospace.ttf"
    )

    if(MSVC)
      target_compile_options(vnm_msdf_text_rhi_real_backend_tests PRIVATE /W4)
    else()
      target_compile_options(vnm_msdf_text_rhi_real_backend_tests PRIVATE -Wall -Wextra)
    endif()

    if(WIN32)
      add_test(
        NAME vnm_msdf_text_rhi_real_backend_tests
        COMMAND vnm_msdf_text_rhi_real_backend_tests
          --backend d3d11
          --image "${CMAKE_CURRENT_BINARY_DIR}/vnm_msdf_text_rhi_real_backend.png")

      if(_vnm_msdf_text_qt_path_env)
        set_tests_properties(vnm_msdf_text_rhi_real_backend_tests PROPERTIES
          ENVIRONMENT_MODIFICATION "${_vnm_msdf_text_qt_path_env}")
      endif()
    endif()

    # The source-consumer gate lives in tests/source_consumer as a project of
    # its own and is configured, built, and run as one. It is deliberately not
    # registered here: configuring it detects a compiler and building it invokes
    # one, and this machine's build policy requires that every such command be
    # leased explicitly instead of being started from inside a test run.

    unset(_vnm_msdf_text_qt_bin_dir)
    unset(_vnm_msdf_text_qt_bin_native)
    unset(_vnm_msdf_text_qt_core_dll)
    unset(_vnm_msdf_text_qt_path_env)
  endif()

  if(VNM_MSDF_TEXT_IS_TOP_LEVEL)
    # Installs the package and inspects what it exports. Every consumer it
    # configures is a LANGUAGES NONE project, so this test detects no compiler
    # and starts no build. Whether the installed headers and imported targets
    # actually compile and link is proven by tests/package_consumer, which is
    # configured and built as a project of its own for the same reason
    # tests/source_consumer is.
    add_test(
      NAME vnm_msdf_text_package_smoke
      COMMAND ${CMAKE_COMMAND}
        "-DVNM_MSDF_TEXT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DVNM_MSDF_TEXT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DVNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT=${VNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT}"
        "-DVNM_MSDF_TEXT_PROJECT_VERSION=${PROJECT_VERSION}"
        "-DVNM_MSDF_TEXT_TEST_CONFIG=$<CONFIG>"
        "-DVNM_MSDF_TEXT_TEST_GENERATOR=${CMAKE_GENERATOR}"
        "-DVNM_MSDF_TEXT_TEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_smoke.cmake")
  endif()
endif()

if(VNM_MSDF_TEXT_IS_TOP_LEVEL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  # The QRhi component is not part of the installed export, so its headers stay
  # out of the installed tree too. They are Qt-free and would compile against an
  # installed package that has no library to satisfy them, which is a link error
  # discovered by a consumer instead of a boundary stated by the package.
  install(DIRECTORY include/vnm_msdf_text
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PATTERN "rhi" EXCLUDE
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
    COMPATIBILITY ExactVersion
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
