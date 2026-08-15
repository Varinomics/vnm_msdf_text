# Source-tree consumer gate for vnm_msdf_text::rhi.
#
# The QRhi component is consumed through add_subdirectory or FetchContent, so
# the things that can break it are not the ones building it in its own tree
# covers: the capability signal an external project reads, the namespaced alias,
# the public include directories, and whether the public headers are enough to
# compile and link against. This is a project of its own, configured and built
# like any other consumer, because both of those steps invoke a compiler and
# this machine's build policy requires each such command to be leased
# explicitly. The parent tree therefore does not register it as a test.
#
# See the README section on the QRhi component for the exact commands.

cmake_minimum_required(VERSION 3.22)
project(vnm_msdf_text_rhi_source_consumer LANGUAGES CXX)

get_filename_component(VNM_MSDF_TEXT_CONSUMED_SOURCE_DIR
  "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(VNM_MSDF_TEXT_CONSUMER_FONT_FILE
  "${VNM_MSDF_TEXT_CONSUMED_SOURCE_DIR}/fonts/monospace.ttf"
  CACHE FILEPATH "Font the source consumer bakes a snapshot from")

# The opt-in the component documents, and nothing else. How the consumed tree
# resolves FreeType and msdfgen stays the configuring caller's choice, exactly
# as it is for any other consumer.
set(VNM_MSDF_TEXT_BUILD_RHI ON CACHE BOOL "" FORCE)
set(VNM_MSDF_TEXT_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory("${VNM_MSDF_TEXT_CONSUMED_SOURCE_DIR}" vnm_msdf_text EXCLUDE_FROM_ALL)

if(NOT VNM_MSDF_TEXT_HAS_RHI)
  message(FATAL_ERROR
    "a source consumer must be able to read VNM_MSDF_TEXT_HAS_RHI to decide "
    "whether real QRhi text is available")
endif()
if(NOT TARGET vnm_msdf_text::rhi)
  message(FATAL_ERROR "the source tree must provide the vnm_msdf_text::rhi alias")
endif()

# Only the namespaced alias. Nothing here names the concrete target, the private
# source directory, or a Qt component: linking the alias has to be enough,
# because that is the whole promise a source consumer is given.
add_executable(vnm_msdf_text_rhi_source_consumer main.cpp)
target_link_libraries(vnm_msdf_text_rhi_source_consumer PRIVATE vnm_msdf_text::rhi)
target_compile_features(vnm_msdf_text_rhi_source_consumer PRIVATE cxx_std_20)
target_compile_definitions(vnm_msdf_text_rhi_source_consumer PRIVATE
  VNM_MSDF_TEXT_CONSUMER_FONT_FILE="${VNM_MSDF_TEXT_CONSUMER_FONT_FILE}")
