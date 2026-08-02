cmake_minimum_required(VERSION 3.16)

foreach(_required_var IN ITEMS
  VNM_MSDF_TEXT_SOURCE_DIR
  VNM_MSDF_TEXT_BINARY_DIR
  VNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT
  VNM_MSDF_TEXT_PROJECT_VERSION
  VNM_MSDF_TEXT_CTEST_COMMAND)
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

  if(DEFINED VNM_MSDF_TEXT_TEST_BUILD_TYPE AND
     NOT VNM_MSDF_TEXT_TEST_BUILD_TYPE STREQUAL "")
    list(APPEND _command "-DCMAKE_BUILD_TYPE=${VNM_MSDF_TEXT_TEST_BUILD_TYPE}")
  endif()
  if(DEFINED VNM_MSDF_TEXT_TEST_GENERATOR AND
     NOT VNM_MSDF_TEXT_TEST_GENERATOR STREQUAL "")
    list(APPEND _command -G "${VNM_MSDF_TEXT_TEST_GENERATOR}")
  endif()
  if(DEFINED VNM_MSDF_TEXT_TEST_MAKE_PROGRAM AND
     NOT VNM_MSDF_TEXT_TEST_MAKE_PROGRAM STREQUAL "")
    list(APPEND _command
      "-DCMAKE_MAKE_PROGRAM=${VNM_MSDF_TEXT_TEST_MAKE_PROGRAM}")
  endif()
  if(DEFINED VNM_MSDF_TEXT_TEST_GENERATOR_PLATFORM AND
     NOT VNM_MSDF_TEXT_TEST_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND _command -A "${VNM_MSDF_TEXT_TEST_GENERATOR_PLATFORM}")
  endif()
  if(DEFINED VNM_MSDF_TEXT_TEST_GENERATOR_TOOLSET AND
     NOT VNM_MSDF_TEXT_TEST_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND _command -T "${VNM_MSDF_TEXT_TEST_GENERATOR_TOOLSET}")
  endif()
  if(_consumer_EXTRA_ARGS)
    list(APPEND _command ${_consumer_EXTRA_ARGS})
  endif()

  set(${out_var} ${_command} PARENT_SCOPE)
endfunction()

function(vnm_msdf_text_run_success_consumer name cmake_var source_var)
  cmake_parse_arguments(PARSE_ARGV 3 _consumer "" "" "EXTRA_ARGS")
  set(_consumer_source_dir "${_work_dir}/${name}")
  set(_consumer_build_dir "${_work_dir}/${name}-build")
  file(MAKE_DIRECTORY "${_consumer_source_dir}")
  file(WRITE "${_consumer_source_dir}/CMakeLists.txt" "${${cmake_var}}")
  file(WRITE "${_consumer_source_dir}/main.cpp" "${${source_var}}")

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
      "Configuring package smoke consumer '${name}' failed.\n"
      "${_configure_output}\n${_configure_error}")
  endif()

  set(_build_command "${CMAKE_COMMAND}" --build "${_consumer_build_dir}")
  if(DEFINED VNM_MSDF_TEXT_TEST_CONFIG AND
     NOT VNM_MSDF_TEXT_TEST_CONFIG STREQUAL "")
    list(APPEND _build_command --config "${VNM_MSDF_TEXT_TEST_CONFIG}")
  endif()
  execute_process(
    COMMAND ${_build_command}
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE  _build_error)
  if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
      "Building package smoke consumer '${name}' failed.\n"
      "${_build_output}\n${_build_error}")
  endif()

  set(_test_command
    "${VNM_MSDF_TEXT_CTEST_COMMAND}"
    --test-dir "${_consumer_build_dir}"
    --output-on-failure)
  if(DEFINED VNM_MSDF_TEXT_TEST_CONFIG AND
     NOT VNM_MSDF_TEXT_TEST_CONFIG STREQUAL "")
    list(APPEND _test_command -C "${VNM_MSDF_TEXT_TEST_CONFIG}")
  endif()
  execute_process(
    COMMAND ${_test_command}
    RESULT_VARIABLE _test_result
    OUTPUT_VARIABLE _test_output
    ERROR_VARIABLE  _test_error)
  if(NOT _test_result EQUAL 0)
    message(FATAL_ERROR
      "Running package smoke consumer '${name}' failed.\n"
      "${_test_output}\n${_test_error}")
  endif()
endfunction()

function(vnm_msdf_text_run_required_atlas_negative name cmake_var)
  cmake_parse_arguments(PARSE_ARGV 2 _consumer "" "" "EXTRA_ARGS")
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
]=])

set(_no_component_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(no_component_success LANGUAGES CXX)

find_package(vnm_msdf_text CONFIG REQUIRED)

if(NOT TARGET vnm_msdf_text::lcd_contract)
  message(FATAL_ERROR "dependency-light package must export lcd_contract")
endif()
if(TARGET vnm_msdf_text::vnm_msdf_text)
  message(FATAL_ERROR "dependency-light package must not export atlas target")
endif()

add_executable(no_component_success main.cpp)
target_link_libraries(no_component_success PRIVATE vnm_msdf_text::lcd_contract)
enable_testing()
add_test(NAME no_component_success COMMAND no_component_success)
]=])
set(_no_component_source [=[
#include <vnm_msdf_text/lcd_contract.h>

int main()
{
    using order_t = vnm::msdf_text::lcd::Resolved_lcd_subpixel_order;
    return vnm::msdf_text::lcd::resolved_order_value(order_t::RGB) == 1 ? 0 : 1;
}
]=])

set(_component_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(component_success LANGUAGES CXX)

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

add_executable(component_success main.cpp)
target_link_libraries(component_success PRIVATE vnm_msdf_text::lcd_shader_reference)
enable_testing()
add_test(NAME component_success COMMAND component_success)
]=])
set(_component_source [=[
#include <vnm_msdf_text/lcd_shader_reference.h>

int main()
{
    namespace lcd_ref = vnm::msdf_text::lcd::shader_reference;
    return lcd_ref::k_lcd_decode_rgb_min == 0.5f &&
           lcd_ref::k_lcd_decode_rgb_max == 1.5f ? 0 : 1;
}
]=])

set(_lcd_contract_deps_disabled_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(lcd_contract_deps_disabled LANGUAGES CXX)

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

add_executable(lcd_contract_deps_disabled main.cpp)
target_link_libraries(lcd_contract_deps_disabled PRIVATE vnm_msdf_text::lcd_contract)
enable_testing()
add_test(NAME lcd_contract_deps_disabled COMMAND lcd_contract_deps_disabled)
]=])
set(_lcd_contract_deps_disabled_source [=[
#include <vnm_msdf_text/lcd_contract.h>

int main()
{
    using order_t = vnm::msdf_text::lcd::Resolved_lcd_subpixel_order;
    return vnm::msdf_text::lcd::resolved_order_value(order_t::BGR) == 2 ? 0 : 1;
}
]=])

set(_lcd_shader_reference_deps_disabled_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(lcd_shader_reference_deps_disabled LANGUAGES CXX)

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

add_executable(lcd_shader_reference_deps_disabled main.cpp)
target_link_libraries(lcd_shader_reference_deps_disabled PRIVATE vnm_msdf_text::lcd_shader_reference)
enable_testing()
add_test(NAME lcd_shader_reference_deps_disabled COMMAND lcd_shader_reference_deps_disabled)
]=])
set(_lcd_shader_reference_deps_disabled_source [=[
#include <vnm_msdf_text/lcd_shader_reference.h>

int main()
{
    namespace lcd_ref = vnm::msdf_text::lcd::shader_reference;
    return lcd_ref::k_lcd_order_vrgb_uniform == 3.0f &&
           lcd_ref::k_lcd_filter_center == 0.3359375f ? 0 : 1;
}
]=])

set(_full_no_component_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(full_no_component_success LANGUAGES CXX)

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

add_executable(full_no_component_success main.cpp)
target_link_libraries(full_no_component_success PRIVATE vnm_msdf_text::vnm_msdf_text)
enable_testing()
add_test(NAME full_no_component_success COMMAND full_no_component_success)
]=])
set(_full_no_component_source [=[
#include <vnm_msdf_text/msdf_text.h>

int main()
{
    vnm::msdf_text::options_t options;
    return options.atlas_size > 0 ? 0 : 1;
}
]=])

set(_required_atlas_success_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(required_atlas_success LANGUAGES CXX)

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

add_executable(required_atlas_success main.cpp)
target_link_libraries(required_atlas_success PRIVATE vnm_msdf_text::vnm_msdf_text)
enable_testing()
add_test(NAME required_atlas_success COMMAND required_atlas_success)
]=])
set(_required_atlas_success_source [=[
#include <vnm_msdf_text/msdf_text.h>

int main()
{
    return sizeof(vnm::msdf_text::glyph_t) > 0 ? 0 : 1;
}
]=])

set(_optional_atlas_missing_deps_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(optional_atlas_missing_deps LANGUAGES CXX)

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

add_executable(optional_atlas_missing_deps main.cpp)
target_link_libraries(optional_atlas_missing_deps PRIVATE vnm_msdf_text::lcd_contract)
enable_testing()
add_test(NAME optional_atlas_missing_deps COMMAND optional_atlas_missing_deps)
]=])
set(_optional_atlas_missing_deps_source [=[
#include <vnm_msdf_text/lcd_contract.h>

int main()
{
    using order_t = vnm::msdf_text::lcd::Resolved_lcd_subpixel_order;
    return vnm::msdf_text::lcd::is_display_specific(order_t::VRGB) ? 0 : 1;
}
]=])

set(_required_lcd_optional_atlas_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(required_lcd_optional_atlas LANGUAGES CXX)

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

add_executable(required_lcd_optional_atlas main.cpp)
target_link_libraries(required_lcd_optional_atlas PRIVATE vnm_msdf_text::lcd_contract)
enable_testing()
add_test(NAME required_lcd_optional_atlas COMMAND required_lcd_optional_atlas)
]=])
set(_required_lcd_optional_atlas_source [=[
#include <vnm_msdf_text/lcd_contract.h>

int main()
{
    using order_t = vnm::msdf_text::lcd::Resolved_lcd_subpixel_order;
    return vnm::msdf_text::lcd::shader_uniform_value(order_t::VBGR) == 4.0f ? 0 : 1;
}
]=])

set(_required_atlas_negative_cmake [=[
cmake_minimum_required(VERSION 3.16)
project(required_atlas_negative LANGUAGES CXX)

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

set(_version_resolution_source_dir "${_work_dir}/version_resolution")
set(_version_resolution_build_dir "${_work_dir}/version_resolution-build")
file(MAKE_DIRECTORY "${_version_resolution_source_dir}")
file(WRITE
  "${_version_resolution_source_dir}/CMakeLists.txt"
  "${_version_resolution_cmake}")
vnm_msdf_text_consumer_configure_command(
  _version_resolution_command
  "${_version_resolution_source_dir}"
  "${_version_resolution_build_dir}"
  EXTRA_ARGS
    "-DVNM_MSDF_TEXT_PACKAGE_PREFIX=${_install_prefix}"
    "-DVNM_MSDF_TEXT_CURRENT_VERSION=${VNM_MSDF_TEXT_PROJECT_VERSION}"
    "-DVNM_MSDF_TEXT_NON_CURRENT_VERSION=${_non_current_version_request}")
execute_process(
  COMMAND ${_version_resolution_command}
  RESULT_VARIABLE _version_resolution_result
  OUTPUT_VARIABLE _version_resolution_output
  ERROR_VARIABLE  _version_resolution_error)
if(NOT _version_resolution_result EQUAL 0)
  message(FATAL_ERROR
    "Configuring package version resolution smoke failed.\n"
    "${_version_resolution_output}\n${_version_resolution_error}")
endif()

vnm_msdf_text_run_success_consumer(
  component_success
  _component_cmake
  _component_source)
vnm_msdf_text_run_success_consumer(
  lcd_contract_deps_disabled
  _lcd_contract_deps_disabled_cmake
  _lcd_contract_deps_disabled_source
  EXTRA_ARGS ${_deps_disabled_args})
vnm_msdf_text_run_success_consumer(
  lcd_shader_reference_deps_disabled
  _lcd_shader_reference_deps_disabled_cmake
  _lcd_shader_reference_deps_disabled_source
  EXTRA_ARGS ${_deps_disabled_args})

if(VNM_MSDF_TEXT_PACKAGE_HAS_ATLAS_EXPORT)
  vnm_msdf_text_run_success_consumer(
    full_no_component_success
    _full_no_component_cmake
    _full_no_component_source)
  vnm_msdf_text_run_success_consumer(
    required_atlas_success
    _required_atlas_success_cmake
    _required_atlas_success_source)
  vnm_msdf_text_run_success_consumer(
    optional_atlas_missing_deps
    _optional_atlas_missing_deps_cmake
    _optional_atlas_missing_deps_source
    EXTRA_ARGS ${_deps_disabled_args})
  vnm_msdf_text_run_required_atlas_negative(
    required_atlas_missing_deps_negative
    _required_atlas_negative_cmake
    EXTRA_ARGS ${_deps_disabled_args})
else()
  vnm_msdf_text_run_success_consumer(
    no_component_success
    _no_component_cmake
    _no_component_source)
  vnm_msdf_text_run_success_consumer(
    required_lcd_optional_atlas
    _required_lcd_optional_atlas_cmake
    _required_lcd_optional_atlas_source)
  vnm_msdf_text_run_required_atlas_negative(
    required_atlas_negative
    _required_atlas_negative_cmake)
endif()
