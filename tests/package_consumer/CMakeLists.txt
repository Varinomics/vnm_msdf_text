# Installed-package consumer gate for vnm_msdf_text.
#
# tests/package_smoke.cmake installs the package and inspects what it exports:
# which components resolve, which versions are accepted, and which headers and
# targets the installed tree must not carry. Nothing there enables a language,
# so nothing there proves that an installed header is complete or that an
# imported target actually links. That is this project's job, and it is a
# project of its own because configuring it detects a compiler and building it
# invokes one, and this machine's build policy requires that every such command
# be leased explicitly instead of being started from inside a test run.
#
# Configure it twice against the prefix the smoke test installed: once as it
# stands, and once with FreeType and msdfgen disabled, which is how a
# dependency-light consumer sees the package. See the README section on
# building and testing for the exact commands.

cmake_minimum_required(VERSION 3.22)
project(vnm_msdf_text_package_consumer LANGUAGES CXX)

set(VNM_MSDF_TEXT_PACKAGE_PREFIX "" CACHE PATH
  "Prefix the vnm_msdf_text package under test was installed to")
set(VNM_MSDF_TEXT_PACKAGE_VERSION "" CACHE STRING
  "Version this consumer requests from the installed package, exactly")

if(NOT VNM_MSDF_TEXT_PACKAGE_PREFIX)
  message(FATAL_ERROR
    "Set VNM_MSDF_TEXT_PACKAGE_PREFIX to the prefix tests/package_smoke.cmake "
    "installed, which is <build-dir>/package_smoke/install.")
endif()
if(NOT VNM_MSDF_TEXT_PACKAGE_VERSION)
  message(FATAL_ERROR
    "Set VNM_MSDF_TEXT_PACKAGE_VERSION to the version the installed package "
    "must report, so a stale install cannot pass as the current one.")
endif()

# Resolve the package under test and the dependency stubs beside it, and
# nothing else: a consumer that silently found another copy of vnm_msdf_text
# would prove nothing about the one just installed.
list(APPEND CMAKE_PREFIX_PATH "${VNM_MSDF_TEXT_PACKAGE_PREFIX}")
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)

find_package(vnm_msdf_text ${VNM_MSDF_TEXT_PACKAGE_VERSION} EXACT CONFIG REQUIRED
  COMPONENTS lcd_contract lcd_shader_reference
  OPTIONAL_COMPONENTS atlas)

if(NOT vnm_msdf_text_lcd_contract_FOUND OR
   NOT vnm_msdf_text_lcd_shader_reference_FOUND)
  message(FATAL_ERROR "the installed package must report both dependency-light components")
endif()
if(NOT TARGET vnm_msdf_text::lcd_contract OR
   NOT TARGET vnm_msdf_text::lcd_shader_reference)
  message(FATAL_ERROR "the installed package must import both dependency-light targets")
endif()

# The QRhi component is a source-tree target with no installed artifact behind
# it. A consumer of the installed package must not be able to reach it through
# an imported target or through a header the package cannot satisfy.
if(TARGET vnm_msdf_text::rhi)
  message(FATAL_ERROR "the installed package must not import an rhi target")
endif()
if(EXISTS "${VNM_MSDF_TEXT_PACKAGE_PREFIX}/include/vnm_msdf_text/rhi")
  message(FATAL_ERROR "the installed include tree must not carry rhi headers")
endif()

enable_testing()

add_executable(lcd_contract_consumer lcd_contract_consumer.cpp)
target_link_libraries(lcd_contract_consumer PRIVATE vnm_msdf_text::lcd_contract)
add_test(NAME lcd_contract_consumer COMMAND lcd_contract_consumer)

add_executable(lcd_shader_reference_consumer lcd_shader_reference_consumer.cpp)
target_link_libraries(lcd_shader_reference_consumer
  PRIVATE vnm_msdf_text::lcd_shader_reference)
add_test(NAME lcd_shader_reference_consumer COMMAND lcd_shader_reference_consumer)

# FreeType and msdfgen decide whether the atlas component can be consumed at
# all, so what this project has to prove depends on how they resolved.
if(CMAKE_DISABLE_FIND_PACKAGE_Freetype OR CMAKE_DISABLE_FIND_PACKAGE_msdfgen)
  if(vnm_msdf_text_atlas_FOUND)
    message(FATAL_ERROR
      "the atlas component must degrade to not found when its dependencies are absent")
  endif()
  if(TARGET Freetype::Freetype OR
     TARGET msdfgen::msdfgen-core OR
     TARGET msdfgen::msdfgen-ext)
    message(FATAL_ERROR "a disabled dependency package must not define targets")
  endif()
elseif(vnm_msdf_text_atlas_FOUND)
  if(NOT TARGET vnm_msdf_text::vnm_msdf_text)
    message(FATAL_ERROR "a found atlas component must import the atlas target")
  endif()
  if(NOT TARGET Freetype::Freetype OR
     NOT TARGET msdfgen::msdfgen-core OR
     NOT TARGET msdfgen::msdfgen-ext)
    message(FATAL_ERROR "a found atlas component must bring its dependency targets with it")
  endif()

  add_executable(atlas_consumer atlas_consumer.cpp)
  target_link_libraries(atlas_consumer PRIVATE vnm_msdf_text::vnm_msdf_text)
  add_test(NAME atlas_consumer COMMAND atlas_consumer)
else()
  # A package installed from a build whose FreeType and msdfgen were built in
  # its own tree exports the dependency-light components only.
  if(TARGET vnm_msdf_text::vnm_msdf_text)
    message(FATAL_ERROR
      "a package that does not export the atlas component must not import its target")
  endif()
endif()
