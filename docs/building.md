# Building and testing

Source builds require CMake 3.22 or newer and a C++20 compiler for the atlas
and QRhi components. FreeType and msdfgen supply font loading and distance-field
generation; Qt is optional. Start with the commands below, then enable the
components and tests needed by your consumer.

## Configuring and building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To require system or pre-provided FreeType/msdfgen targets without network
fetching:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVNM_MSDF_TEXT_FETCH_DEPS=OFF
cmake --build build
```

For offline builds with fetched dependencies already checked out, pass
`FETCHCONTENT_SOURCE_DIR_FREETYPE` and `FETCHCONTENT_SOURCE_DIR_MSDFGEN`, or set
`VNM_MSDF_TEXT_DEP_OVERRIDES_FILE` to a CMake file that defines those cache
entries.

## Optional components

Enable `VNM_MSDF_TEXT_BUILD_RHI=ON` for the [QRhi text renderer](rhi_usage.md).
It requires the atlas component, Qt 6.12 or newer, and Qt Shader Tools at build
time. `VNM_MSDF_TEXT_HAS_RHI` reports whether it was enabled.

The optional `qt_lcd` component resolves display subpixel order from `QScreen`
and the shared LCD request type. Enable `VNM_MSDF_TEXT_BUILD_QT_LCD` for source
builds or request the installed `qt_lcd` component. A resolver-only source build
may set `VNM_MSDF_TEXT_BUILD_ATLAS=OFF`; RHI requires the atlas component.
`VNM_MSDF_TEXT_SHADER_DIR` identifies the shared `lcd_filter.glsl` include for
source and installed shader consumers.

The dependency-light `lcd_contract` and `lcd_shader_reference` targets require
C++17 and neither FreeType, msdfgen nor Qt. Set `VNM_MSDF_TEXT_BUILD_ATLAS=OFF`
when only those components or the Qt LCD resolver are needed. Their rendering
and resolution semantics belong to the [LCD contract](lcd_msdf_commonization_contract.md).

## Installing a CMake package

```sh
cmake --install build --config Release --prefix install
```

An atlas package export requires imported, find-package-able FreeType and
msdfgen targets. Configure with `VNM_MSDF_TEXT_FETCH_DEPS=OFF` and make those
dependencies available to CMake to install the atlas component. Locally built
dependencies, including those fetched by this project, prevent atlas and RHI
exports; the dependency-light LCD targets and optional Qt LCD resolver remain
exportable. RHI exports additionally require `VNM_MSDF_TEXT_BUILD_RHI=ON`.

Installed consumers request `atlas`, `rhi`, `qt_lcd`, `lcd_contract`, or
`lcd_shader_reference` through `find_package(vnm_msdf_text CONFIG COMPONENTS ...)`.
Explicit component requests resolve only their own dependencies. A no-component
lookup imports the atlas when it was exported, otherwise it succeeds with the
dependency-light LCD targets. Versioned lookup requires the exact project
version; an unversioned lookup needs no version argument.

## Tests

When this project is configured as the top-level CMake project, tests are built
by default:

```bash
cmake --build build --target vnm_msdf_text_tests
ctest --test-dir build --output-on-failure
```

Set `-DVNM_MSDF_TEXT_BUILD_TESTS=OFF` to skip the test executable.

The tests bake their atlases from a font that lives in
[vnm_fonts](https://github.com/Varinomics/vnm_fonts), so a test build resolves
that repository. A tree that has already added vnm_fonts has published the
verbatim files in `VNM_FONTS_DIRECTORY`, and that is used as it stands;
otherwise a checkout beside this one is used when it exists and CMake fetches
`master` when it does not, and `FETCHCONTENT_SOURCE_DIR_VNM_FONTS` overrides
that pair.

Only the file contract is used. vnm_fonts configures without Qt and publishes
`VNM_FONTS_DIRECTORY` either way, so adding it puts no Qt in front of this
build. Atlases are baked from file bytes without using a font database or linking
`vnm::fonts`.

A build configured with `-DVNM_MSDF_TEXT_BUILD_RHI=ON` registers further tests.
`vnm_msdf_text_rhi_tests` covers the snapshot, batch, frame, and optional
draw-capability contracts using the Null QRhi backend.
`vnm_msdf_text_rhi_styled_shader_reference_tests` checks the styled shader
source against the shared LCD reference; it needs neither Qt nor a device.
`vnm_msdf_text_rhi_real_backend_tests` checks rendered output with a real backend
and needs a working graphics device. Pass `--backend`, `--image`, and `--window` to
select the backend, save the rendered images, and additionally record frames
against a shown window. The Windows CTest entry selects D3D11.

`tests/source_consumer` is a standalone consumer test that adds this repository
with `add_subdirectory`, checks `VNM_MSDF_TEXT_HAS_RHI`, includes only public
headers, and links only `vnm_msdf_text::rhi`. Configure, build, and run it like any
other consumer:

```sh
cmake -S tests/source_consumer -B build-source-consumer -DVNM_MSDF_TEXT_FETCH_DEPS=ON
cmake --build build-source-consumer
./build-source-consumer/vnm_msdf_text_rhi_source_consumer
```

It is not registered with CTest.

The `vnm_msdf_text_package_smoke` test installs and inspects the exported package.
`tests/package_consumer` checks that the installed headers and imported targets
compile, link, and run in a separate project. Point it at the prefix the smoke
test installed and at the version this project declares, once as the package
stands and once as a dependency-light consumer sees it:

```sh
ctest --test-dir build -R vnm_msdf_text_package_smoke --output-on-failure

cmake -S tests/package_consumer -B build-package-consumer \
    -DVNM_MSDF_TEXT_PACKAGE_PREFIX=$PWD/build/package_smoke/install \
    -DVNM_MSDF_TEXT_PACKAGE_VERSION=0.2.0
cmake --build build-package-consumer
ctest --test-dir build-package-consumer --output-on-failure

cmake -S tests/package_consumer -B build-package-consumer-nodeps \
    -DVNM_MSDF_TEXT_PACKAGE_PREFIX=$PWD/build/package_smoke/install \
    -DVNM_MSDF_TEXT_PACKAGE_VERSION=0.2.0 \
    -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=TRUE \
    -DCMAKE_DISABLE_FIND_PACKAGE_msdfgen=TRUE
cmake --build build-package-consumer-nodeps
ctest --test-dir build-package-consumer-nodeps --output-on-failure
```
