cmake_minimum_required(VERSION 3.16)

# Installs the project and inspects the package it produced: which components
# resolve, which versions are accepted, which dependencies a component needs,
# and which headers and targets the installed tree must not carry.
#
# Nothing here enables a language. Every consumer this script configures is a
# LANGUAGES NONE project, so CMake neither detects nor invokes a compiler for
# it, which is what makes this gate safe to register as an ordinary test on a
# machine whose build policy requires every compiler-invoking command to be
# leased explicitly. Proving that the installed headers and imported targets
# actually compile and link is the job of tests/package_consumer, which is
# configured and built as a project of its own.

foreach(_required_var IN ITEMS
  VNM_MSDF_TEXT_SOURCE_DIR
  VNM_MSDF_TEXT_BINARY_DIR
  VNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT
  VNM_MSDF_TEXT_PROJECT_VERSION)
  if(NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
    message(FATAL_ERROR "tests/package_smoke.cmake requires ${_required_var}.")
  endif()
endforeach()

get_filename_component(_source_dir "${VNM_MSDF_TEXT_SOURCE_DIR}" ABSOLUTE)
get_filename_component(_binary_dir "${VNM_MSDF_TEXT_BINARY_DIR}" ABSOLUTE)
set(_work_dir "${_binary_dir}/package_smoke")
set(_install_prefix "${_work_dir}/install")

file(TO_CMAKE_PATH "${_binary_dir}" _binary_dir_cmake)
file(TO_CMAKE_PATH "${_work_dir}" _work_dir_cmake)
string(FIND "${_work_dir_cmake}/" "${_binary_dir_cmake}/" _work_dir_prefix)
if(NOT _work_dir_prefix EQUAL 0)
  message(FATAL_ERROR "Refusing to clean package smoke directory outside the build tree.")
endif()

file(REMOVE_RECURSE "${_work_dir}")
file(MAKE_DIRECTORY "${_work_dir}")

set(_stub_deps_prefix "${_install_prefix}")
file(MAKE_DIRECTORY
  "${_stub_deps_prefix}/lib/cmake/Freetype"
  "${_stub_deps_prefix}/lib/cmake/msdfgen")
file(WRITE "${_stub_deps_prefix}/lib/cmake/Freetype/FreetypeConfig.cmake" [=[
if(NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype INTERFACE IMPORTED)
endif()
set(Freetype_FOUND TRUE)
set(FREETYPE_FOUND TRUE)
]=])
file(WRITE "${_stub_deps_prefix}/lib/cmake/msdfgen/msdfgenConfig.cmake" [=[
if(NOT TARGET msdfgen::msdfgen-core)
  add_library(msdfgen::msdfgen-core INTERFACE IMPORTED)
endif()
if(NOT TARGET msdfgen::msdfgen-ext)
  add_library(msdfgen::msdfgen-ext INTERFACE IMPORTED)
endif()
set(msdfgen_FOUND TRUE)
]=])

set(_install_command
  "${CMAKE_COMMAND}" --install "${_binary_dir}"
  --prefix "${_install_prefix}")
if(DEFINED VNM_MSDF_TEXT_TEST_CONFIG AND
   NOT VNM_MSDF_TEXT_TEST_CONFIG STREQUAL "")
  list(APPEND _install_command --config "${VNM_MSDF_TEXT_TEST_CONFIG}")
endif()
execute_process(
  COMMAND ${_install_command}
  RESULT_VARIABLE _install_result
  OUTPUT_VARIABLE _install_output
  ERROR_VARIABLE  _install_error)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR
    "Installing vnm_msdf_text for package smoke failed.\n"
    "${_install_output}\n${_install_error}")
endif()

# The QRhi component is a source-tree target with no installed artifact behind
# it, so the installed tree must not carry its headers either: a readable public
# header that no configuration of this package can link is a contract the
# package does not have.
if(EXISTS "${_install_prefix}/include/vnm_msdf_text/rhi")
  message(FATAL_ERROR
    "The installed package must not contain vnm_msdf_text/rhi headers, because "
    "no installed artifact provides the component that satisfies them.")
endif()
file(GLOB_RECURSE _installed_rhi_headers
  "${_install_prefix}/include/vnm_msdf_text/rhi*")
if(_installed_rhi_headers)
  message(FATAL_ERROR
    "The installed package contains unsupported RHI headers: ${_installed_rhi_headers}")
endif()

file(GLOB _installed_target_files
  "${_install_prefix}/lib/cmake/vnm_msdf_text/vnm_msdf_textTargets*.cmake")
foreach(_target_file IN LISTS _installed_target_files)
  file(READ "${_target_file}" _target_file_text)
  string(FIND "${_target_file_text}" "vnm_msdf_text::rhi" _rhi_target_index)
  if(NOT _rhi_target_index EQUAL -1)
    message(FATAL_ERROR
      "The installed export must not define vnm_msdf_text::rhi (${_target_file}).")
  endif()
endforeach()

# Keeps this gate compiler-free by construction rather than by convention: a
# consumer added here later cannot quietly reintroduce compiler detection or a
# nested build.
function(vnm_msdf_text_assert_language_free name text)
  if(NOT "${text}" MATCHES "project\\([A-Za-z0-9_]+ LANGUAGES NONE\\)")
    message(FATAL_ERROR
      "Package smoke consumer '${name}' must declare LANGUAGES NONE. A consumer "
      "that compiles belongs in tests/package_consumer.")
  endif()
  foreach(_compiling_command IN ITEMS
    "enable_language("
    "add_executable("
    "add_library("
    "try_compile("
    "try_run(")
    string(FIND "${text}" "${_compiling_command}" _found_index)
    if(NOT _found_index EQUAL -1)
      message(FATAL_ERROR
        "Package smoke consumer '${name}' must not use ${_compiling_command}. A "
        "consumer that compiles belongs in tests/package_consumer.")
    endif()
  endforeach()
endfunction()

function(vnm_msdf_text_consumer_configure_command out_var consumer_source_dir consumer_build_dir)
  cmake_parse_arguments(PARSE_ARGV 3 _consumer "" "" "EXTRA_ARGS")

  set(_command
    "${CMAKE_COMMAND}"
    -S "${consumer_source_dir}"
    -B "${consumer_build_dir}"
    "-DCMAKE_PREFIX_PATH=${_install_prefix}"
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE)

  if(DEFINED VNM_MSDF_TEXT_TEST_GENERATOR AND
     NOT VNM_MSDF_TEXT_TEST_GENERATOR STREQUAL "")
    list(APPEND _command -G "${VNM_MSDF_TEXT_TEST_GENERATOR}")
  endif()
  if(DEFINED VNM_MSDF_TEXT_TEST_MAKE_PROGRAM AND
     NOT VNM_MSDF_TEXT_TEST_MAKE_PROGRAM STREQUAL "")
    list(APPEND _command
      "-DCMAKE_MAKE_PROGRAM=${VNM_MSDF_TEXT_TEST_MAKE_PROGRAM}")
  endif()
  if(_consumer_EXTRA_ARGS)
    list(APPEND _command ${_consumer_EXTRA_ARGS})
  endif()

  set(${out_var} ${_command} PARENT_SCOPE)
endfunction()

function(vnm_msdf_text_inspect_package name cmake_var)
  cmake_parse_arguments(PARSE_ARGV 2 _consumer "" "" "EXTRA_ARGS")
  vnm_msdf_text_assert_language_free("${name}" "${${cmake_var}}")

  set(_consumer_source_dir "${_work_dir}/${name}")
  set(_consumer_build_dir "${_work_dir}/${name}-build")
  file(MAKE_DIRECTORY "${_consumer_source_dir}")
  file(WRITE "${_consumer_source_dir}/CMakeLists.txt" "${${cmake_var}}")

  vnm_msdf_text_consumer_configure_command(
    _configure_command
    "${_consumer_source_dir}"
    "${_consumer_build_dir}"
    EXTRA_ARGS ${_consumer_EXTRA_ARGS})
  execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE  _configure_error)
  if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
      "Package smoke inspection '${name}' failed.\n"
      "${_configure_output}\n${_configure_error}")
  endif()
endfunction()

function(vnm_msdf_text_run_required_atlas_negative name cmake_var)
  cmake_parse_arguments(PARSE_ARGV 2 _consumer "" "" "EXTRA_ARGS")
  vnm_msdf_text_assert_language_free("${name}" "${${cmake_var}}")

  set(_consumer_source_dir "${_work_dir}/${name}")
  set(_consumer_build_dir "${_work_dir}/${name}-build")
  file(MAKE_DIRECTORY "${_consumer_source_dir}")
  file(WRITE "${_consumer_source_dir}/CMakeLists.txt" "${${cmake_var}}")

  vnm_msdf_text_consumer_configure_command(
    _configure_command
    "${_consumer_source_dir}"
    "${_consumer_build_dir}"
    EXTRA_ARGS ${_consumer_EXTRA_ARGS})
  execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE  _configure_error)

  set(_combined_output "${_configure_output}\n${_configure_error}")
  if(_configure_result EQUAL 0)
    message(FATAL_ERROR "Required atlas package smoke unexpectedly configured.")
  endif()

  string(FIND "${_combined_output}" "required atlas unexpectedly configured" _unexpected_index)
  if(NOT _unexpected_index EQUAL -1)
    message(FATAL_ERROR
      "Required atlas package smoke reached the post-find_package failure.\n"
      "${_combined_output}")
  endif()

  if(NOT EXISTS "${_consumer_build_dir}/lcd_only_precheck_passed.txt")
    message(FATAL_ERROR
      "Required atlas package smoke failed before proving LCD-only component success.\n"
      "${_combined_output}")
  endif()
endfunction()

set(_deps_disabled_args
  -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=TRUE
  -DCMAKE_DISABLE_FIND_PACKAGE_msdfgen=TRUE)

if(NOT VNM_MSDF_TEXT_PROJECT_VERSION MATCHES
   "^([0-9]+)\\.([0-9]+)\\.[0-9]+$")
  message(FATAL_ERROR
    "Package smoke requires a major.minor.patch project version.")
endif()
set(_non_current_version_request
  "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")

set(_version_resolution_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(version_resolution LANGUAGES NONE)

if(NOT DEFINED VNM_MSDF_TEXT_PACKAGE_PREFIX OR
   VNM_MSDF_TEXT_PACKAGE_PREFIX STREQUAL "")
  message(FATAL_ERROR "version resolution smoke requires the installed package prefix")
endif()
if(NOT DEFINED VNM_MSDF_TEXT_CURRENT_VERSION OR
   VNM_MSDF_TEXT_CURRENT_VERSION STREQUAL "" OR
   NOT DEFINED VNM_MSDF_TEXT_NON_CURRENT_VERSION OR
   VNM_MSDF_TEXT_NON_CURRENT_VERSION STREQUAL "")
  message(FATAL_ERROR "version resolution smoke requires current and non-current versions")
endif()

find_package(vnm_msdf_text ${VNM_MSDF_TEXT_NON_CURRENT_VERSION}
  CONFIG QUIET COMPONENTS lcd_contract
  PATHS "${VNM_MSDF_TEXT_PACKAGE_PREFIX}"
  NO_DEFAULT_PATH)
if(vnm_msdf_text_FOUND)
  message(FATAL_ERROR
    "vnm_msdf_text must reject a non-current version request")
endif()
if(TARGET vnm_msdf_text::lcd_contract)
  message(FATAL_ERROR
    "rejected non-current version request must not import targets")
endif()

find_package(vnm_msdf_text ${VNM_MSDF_TEXT_CURRENT_VERSION} EXACT
  CONFIG REQUIRED COMPONENTS lcd_contract
  PATHS "${VNM_MSDF_TEXT_PACKAGE_PREFIX}"
  NO_DEFAULT_PATH)
if(NOT vnm_msdf_text_lcd_contract_FOUND)
  message(FATAL_ERROR
    "vnm_msdf_text must accept the exact current version")
endif()
if(NOT TARGET vnm_msdf_text::lcd_contract)
  message(FATAL_ERROR
    "accepted exact-current request must import lcd_contract")
endif()

find_package(vnm_msdf_text ${VNM_MSDF_TEXT_CURRENT_VERSION} EXACT
  CONFIG QUIET COMPONENTS rhi
  PATHS "${VNM_MSDF_TEXT_PACKAGE_PREFIX}"
  NO_DEFAULT_PATH)
if(vnm_msdf_text_rhi_FOUND)
  message(FATAL_ERROR
    "the installed package must not report an rhi component")
endif()
if(TARGET vnm_msdf_text::rhi)
  message(FATAL_ERROR
    "the installed package must not import an rhi target")
endif()
]=])

set(_no_component_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(no_component_success LANGUAGES NONE)

find_package(vnm_msdf_text CONFIG REQUIRED)

if(NOT TARGET vnm_msdf_text::lcd_contract)
  message(FATAL_ERROR "dependency-light package must export lcd_contract")
endif()
if(TARGET vnm_msdf_text::vnm_msdf_text)
  message(FATAL_ERROR "dependency-light package must not export atlas target")
endif()
]=])

set(_component_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(component_success LANGUAGES NONE)

find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS lcd_contract lcd_shader_reference)

if(NOT vnm_msdf_text_lcd_contract_FOUND)
  message(FATAL_ERROR "lcd_contract component must be found")
endif()
if(NOT vnm_msdf_text_lcd_shader_reference_FOUND)
  message(FATAL_ERROR "lcd_shader_reference component must be found")
endif()
if(NOT TARGET vnm_msdf_text::lcd_shader_reference)
  message(FATAL_ERROR "dependency-light package must export lcd_shader_reference")
endif()
if(TARGET vnm_msdf_text::rhi)
  message(FATAL_ERROR "installed package must not export an rhi target")
endif()
if(EXISTS "${vnm_msdf_text_DIR}/../../../include/vnm_msdf_text/rhi")
  message(FATAL_ERROR "installed include tree must not carry rhi headers")
endif()
]=])

set(_lcd_contract_deps_disabled_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(lcd_contract_deps_disabled LANGUAGES NONE)

find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS lcd_contract)

if(NOT vnm_msdf_text_lcd_contract_FOUND)
  message(FATAL_ERROR "lcd_contract component must be found")
endif()
if(NOT TARGET vnm_msdf_text::lcd_contract)
  message(FATAL_ERROR "package must export lcd_contract")
endif()
if(vnm_msdf_text_atlas_FOUND)
  message(FATAL_ERROR "LCD-only lookup must not report atlas found")
endif()
if(TARGET Freetype::Freetype)
  message(FATAL_ERROR "disabled Freetype package must not define a target")
endif()
if(TARGET msdfgen::msdfgen-core OR TARGET msdfgen::msdfgen-ext)
  message(FATAL_ERROR "disabled msdfgen package must not define targets")
endif()
]=])

set(_lcd_shader_reference_deps_disabled_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(lcd_shader_reference_deps_disabled LANGUAGES NONE)

find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS lcd_shader_reference)

if(NOT vnm_msdf_text_lcd_shader_reference_FOUND)
  message(FATAL_ERROR "lcd_shader_reference component must be found")
endif()
if(NOT TARGET vnm_msdf_text::lcd_shader_reference)
  message(FATAL_ERROR "package must export lcd_shader_reference")
endif()
if(vnm_msdf_text_atlas_FOUND)
  message(FATAL_ERROR "LCD-only lookup must not report atlas found")
endif()
if(TARGET Freetype::Freetype)
  message(FATAL_ERROR "disabled Freetype package must not define a target")
endif()
if(TARGET msdfgen::msdfgen-core OR TARGET msdfgen::msdfgen-ext)
  message(FATAL_ERROR "disabled msdfgen package must not define targets")
endif()
]=])

set(_full_no_component_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(full_no_component_success LANGUAGES NONE)

find_package(vnm_msdf_text CONFIG REQUIRED)

if(NOT vnm_msdf_text_atlas_FOUND)
  message(FATAL_ERROR "full-export package must report atlas found for no-component lookup")
endif()
if(NOT TARGET vnm_msdf_text::vnm_msdf_text)
  message(FATAL_ERROR "full-export package must export atlas target")
endif()
if(NOT TARGET Freetype::Freetype)
  message(FATAL_ERROR "full-export package must find Freetype target")
endif()
if(NOT TARGET msdfgen::msdfgen-core OR NOT TARGET msdfgen::msdfgen-ext)
  message(FATAL_ERROR "full-export package must find msdfgen targets")
endif()
]=])

set(_required_atlas_success_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(required_atlas_success LANGUAGES NONE)

find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS atlas)

if(NOT vnm_msdf_text_atlas_FOUND)
  message(FATAL_ERROR "required atlas component must be found")
endif()
if(NOT TARGET vnm_msdf_text::vnm_msdf_text)
  message(FATAL_ERROR "required atlas component must export atlas target")
endif()
if(NOT TARGET Freetype::Freetype)
  message(FATAL_ERROR "required atlas component must find Freetype target")
endif()
if(NOT TARGET msdfgen::msdfgen-core OR NOT TARGET msdfgen::msdfgen-ext)
  message(FATAL_ERROR "required atlas component must find msdfgen targets")
endif()
]=])

set(_optional_atlas_missing_deps_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(optional_atlas_missing_deps LANGUAGES NONE)

find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS lcd_contract OPTIONAL_COMPONENTS atlas)

if(NOT vnm_msdf_text_lcd_contract_FOUND)
  message(FATAL_ERROR "lcd_contract component must be found")
endif()
if(vnm_msdf_text_atlas_FOUND)
  message(FATAL_ERROR "optional atlas must degrade to not found when dependencies are absent")
endif()
if(NOT TARGET vnm_msdf_text::lcd_contract)
  message(FATAL_ERROR "full-export package must still expose lcd_contract")
endif()
if(NOT TARGET vnm_msdf_text::vnm_msdf_text)
  message(FATAL_ERROR "full-export package must still contain the atlas target")
endif()
if(TARGET Freetype::Freetype)
  message(FATAL_ERROR "disabled Freetype package must not define a target")
endif()
if(TARGET msdfgen::msdfgen-core OR TARGET msdfgen::msdfgen-ext)
  message(FATAL_ERROR "disabled msdfgen package must not define targets")
endif()
]=])

set(_required_lcd_optional_atlas_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(required_lcd_optional_atlas LANGUAGES NONE)

find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS lcd_contract OPTIONAL_COMPONENTS atlas)

if(NOT vnm_msdf_text_lcd_contract_FOUND)
  message(FATAL_ERROR "lcd_contract component must be found")
endif()
if(vnm_msdf_text_atlas_FOUND)
  message(FATAL_ERROR "dependency-light package must report atlas absent")
endif()
if(TARGET vnm_msdf_text::vnm_msdf_text)
  message(FATAL_ERROR "dependency-light package must not export atlas target")
endif()
]=])

set(_required_atlas_negative_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(required_atlas_negative LANGUAGES NONE)

find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS lcd_contract)
if(NOT vnm_msdf_text_lcd_contract_FOUND)
  message(FATAL_ERROR "lcd_contract component must be found before required atlas fails")
endif()
if(NOT TARGET vnm_msdf_text::lcd_contract)
  message(FATAL_ERROR "lcd_contract target must exist before required atlas fails")
endif()
if(TARGET Freetype::Freetype)
  message(FATAL_ERROR "Freetype target must be absent before required atlas lookup")
endif()
if(TARGET msdfgen::msdfgen-core OR TARGET msdfgen::msdfgen-ext)
  message(FATAL_ERROR "msdfgen targets must be absent before required atlas lookup")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/lcd_only_precheck_passed.txt" "ok\n")

find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS atlas)
message(FATAL_ERROR "required atlas unexpectedly configured")
]=])

vnm_msdf_text_inspect_package(
  version_resolution
  _version_resolution_cmake
  EXTRA_ARGS
    "-DVNM_MSDF_TEXT_PACKAGE_PREFIX=${_install_prefix}"
    "-DVNM_MSDF_TEXT_CURRENT_VERSION=${VNM_MSDF_TEXT_PROJECT_VERSION}"
    "-DVNM_MSDF_TEXT_NON_CURRENT_VERSION=${_non_current_version_request}")

vnm_msdf_text_inspect_package(
  component_success
  _component_cmake)
vnm_msdf_text_inspect_package(
  lcd_contract_deps_disabled
  _lcd_contract_deps_disabled_cmake
  EXTRA_ARGS ${_deps_disabled_args})
vnm_msdf_text_inspect_package(
  lcd_shader_reference_deps_disabled
  _lcd_shader_reference_deps_disabled_cmake
  EXTRA_ARGS ${_deps_disabled_args})

if(VNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT)
  vnm_msdf_text_inspect_package(
    full_no_component_success
    _full_no_component_cmake)
  vnm_msdf_text_inspect_package(
    required_atlas_success
    _required_atlas_success_cmake)
  vnm_msdf_text_inspect_package(
    optional_atlas_missing_deps
    _optional_atlas_missing_deps_cmake
    EXTRA_ARGS ${_deps_disabled_args})
  vnm_msdf_text_run_required_atlas_negative(
    required_atlas_missing_deps_negative
    _required_atlas_negative_cmake
    EXTRA_ARGS ${_deps_disabled_args})
else()
  vnm_msdf_text_inspect_package(
    no_component_success
    _no_component_cmake)
  vnm_msdf_text_inspect_package(
    required_lcd_optional_atlas
    _required_lcd_optional_atlas_cmake)
  vnm_msdf_text_run_required_atlas_negative(
    required_atlas_negative
    _required_atlas_negative_cmake)
endif()
